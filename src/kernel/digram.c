/*  Copyright (C) 2023-2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.  */

#include "analyze.h"

#include <string.h>

#if FASTENT_BG_NB < 2
#error "fastent_digram_count unrolls 2 shadow tables; FASTENT_BG_NB >= 2"
#endif

/*  -f scratch chunk; sized to stay L1/L2-resident across fold+scan.  */
#define FASTENT_DG_FOLD_CHUNK 32768

/*  Cached vectorised fold variant.  Pick is process-stable, so the
    lazy-init race stores the same pointer.  */
static fastent_fold_fn dg_fold_fn_(void) {
  static fastent_fold_fn ff = NULL;
  fastent_fold_fn f = ff;
  if (!f) { f = fastent_pick_fold_variant(NULL);  ff = f; }
  return f;
}

/*  Longest-run helpers are shared (fastent_lr_one / fastent_lr_run in
    analyze.c).  */

/*  Cached byte digram + longest-run kernel.  */
static fastent_digram_byte_fn dg_byte_fn_(void) {
  static fastent_digram_byte_fn df = NULL;
  fastent_digram_byte_fn f = df;
  if (!f) { f = fastent_pick_digram_byte_variant(NULL);  df = f; }
  return f;
}

#define FASTENT_DG_BITS_CHUNK 65536u                   /*  bytes  */
#define FASTENT_DG_BITS_WORDS (FASTENT_DG_BITS_CHUNK / 8u)


/*  Cached per-ISA bit -ee fused block kernel.  */
#ifndef FASTENT_DG_BITS_REF
static fastent_digram_bits_fn dg_bits_fn_(void) {
  static fastent_digram_bits_fn bf = NULL;
  fastent_digram_bits_fn f = bf;
  if (!f) { f = fastent_pick_digram_bits_variant(NULL);  bf = f; }
  return f;
}
#endif

#ifdef FASTENT_DG_BITS_REF
/*  Reference (oracle) block: per-byte pack + per-transition
    fastent_lr_run loop.  Compiled only for the differential gate; the
    default build dispatches to the templated per-ISA kernel.  */
static void digram_bits_blk_(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz cl,
    const i32 * cs_mn, const i32 * cs_mx, const i32 * cs_net) {
  u64 W[FASTENT_DG_BITS_WORDS];
  const sz M  = cl * 8;
  const sz NW = (M + 63) / 64;
  sz w, i;

  memset(W, 0, NW * sizeof (u64));
  for (i = 0; i < cl; i++)
    W[i >> 3] |= (u64) fastent_bitrev8_(buf[i]) << ((u32) (i & 7) * 8u);

  const u32 b0    = (u32) (W[0] & 1u);
  const sz  lbpos = M - 1;
  const u32 lbit  = (u32) ((W[lbpos >> 6] >> (lbpos & 63)) & 1u);

  u64 n11 = 0, ntr = 0, n10 = 0;
  for (w = 0; w < NW; w++) {
    const u64 a  = W[w];
    const u64 nx = (w + 1 < NW) ? W[w + 1] : 0ull;
    const u64 sa = (a >> 1) | (nx << 63);
    n11 += FASTENT_POPCOUNT64(a & sa);
    ntr += FASTENT_POPCOUNT64(a ^ sa);
    n10 += FASTENT_POPCOUNT64(a & ~sa);
  }
  ntr -= lbit;  n10 -= lbit;
  const u64 n01 = ntr - n10;
  const u64 n00 = (u64) (M - 1) - ntr - n11;

  if (st->dg_have) st->bit_bigram[st->dg_prev & 1u][b0]++;
  st->bit_bigram[0][0] += n00;  st->bit_bigram[0][1] += n01;
  st->bit_bigram[1][0] += n10;  st->bit_bigram[1][1] += n11;
  st->dg_prev = (u8) lbit;  st->dg_have = 1;

  if (!st->rn_have) { st->rn_have = 1;  st->rn_count = 1 + ntr; }
  else {
    if (b0 != st->rn_last) st->rn_count++;
    st->rn_count += ntr;
  }
  st->rn_last = (u8) lbit;

  {
    i64 o = 0, gmn = ((i64) 1 << 60), gmx = -((i64) 1 << 60);
    for (i = 0; i < cl; i++) {
      const u32 v = buf[i];
      if (o + cs_mn[v] < gmn) gmn = o + cs_mn[v];
      if (o + cs_mx[v] > gmx) gmx = o + cs_mx[v];
      o += cs_net[v];
    }
    const i64 cs0 = st->cs_sum;
    if (cs0 + gmn < st->cs_min) st->cs_min = cs0 + gmn;
    if (cs0 + gmx > st->cs_max) st->cs_max = cs0 + gmx;
    st->cs_sum = cs0 + o;
  }

  {
    const sz  wl = lbpos >> 6;
    const u32 bl = (u32) (lbpos & 63);
    const u64 lmask = bl ? ((1ull << bl) - 1ull) : 0ull;
    i64 lastp = -1;
    u32 sym = b0;
    for (w = 0; w < NW; w++) {
      const u64 a  = W[w];
      const u64 nx = (w + 1 < NW) ? W[w + 1] : 0ull;
      u64 tw = a ^ ((a >> 1) | (nx << 63));
      if (w == wl) tw &= lmask;
      while (tw) {
        const i64 p = (i64) w * 64 + (i64) FASTENT_CTZ64(tw);
        fastent_lr_run(st, sym, (u64) (p - lastp));
        lastp = p;  sym ^= 1u;
        tw &= tw - 1;
      }
    }
    fastent_lr_run(st, sym, (u64) ((i64) lbpos - lastp));
  }
}
#endif


/*  Bit mode: every adjacent bit pair (MSB->LSB, across byte and chunk
    boundaries via the carried previous bit) feeds the 2x2 bigram, longest
    run, 0/1 run count and the +-1 cusum walk.  */
static void digram_bits_(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len) {
  i32 cs_mn[256], cs_mx[256], cs_net[256];
  i32 v, k;
  for (v = 0; v < 256; v++) {
    i32 s = 0, mn = 0, mx = 0;
#ifdef FASTENT_DG_BITS_REF
    /*  REF walks raw bytes MSB-first.  */
    for (k = 7; k >= 0; k--) {
#else
    /*  Fused path indexes the packed (bit-reversed) word byte; its
        LSB-first walk equals the raw byte's MSB-first +-1 walk.  */
    for (k = 0; k <= 7; k++) {
#endif
      s += ((v >> k) & 1) ? 1 : -1;
      if (s < mn) mn = s;
      if (s > mx) mx = s;
    }
    cs_mn[v] = mn;  cs_mx[v] = mx;  cs_net[v] = s;
  }
#ifndef FASTENT_DG_BITS_REF
  fastent_digram_bits_fn blk = dg_bits_fn_();
#endif
  { sz off;
    for (off = 0; off < len; off += FASTENT_DG_BITS_CHUNK) {
      sz cl = len - off;
      if (cl > FASTENT_DG_BITS_CHUNK) cl = FASTENT_DG_BITS_CHUNK;
#ifdef FASTENT_DG_BITS_REF
      digram_bits_blk_(st, buf + off, cl, cs_mn, cs_mx, cs_net);
#else
      blk(st, buf + off, cl, cs_mn, cs_mx, cs_net);
#endif
    }
  }
}

/*  Byte scan with deterministic u32->u64 drains.  */
static void digram_bytes_drained_(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len) {
  fastent_digram_byte_fn fn = dg_byte_fn_();
  sz off = 0;
  while (off < len) {
    if (st->dg_chunk_bytes >= FASTENT_DG_U32_CHUNK)
      fastent_dg_drain(st);
    sz room = (sz) (FASTENT_DG_U32_CHUNK - st->dg_chunk_bytes);
    sz n = len - off;
    if (n > room) n = room;
    fn(st, buf + off, n);
    st->dg_chunk_bytes += n;
    off += n;
  }
}

void fastent_digram_count(
    fastent_chunk_state * st, const u8 * buf, sz len, int binary, int fold) {
  sz off;
  if (len == 0) return;
  if (!binary && !st->bigram) return;   /*  byte mode without -ee table  */

  if (!fold) {
    if (binary) digram_bits_(st, buf, len);
    else        digram_bytes_drained_(st, buf, len);
    return;
  }

  /*  -f: prefold per chunk (mmap is read-only/shared, no in-place
      fold), then fold-free scan.  st->dg_* and the lr/rn/cs
      accumulators carry across chunks, so boundaries do not matter.  */
  fastent_fold_fn ff = dg_fold_fn_();
  u8 scratch[FASTENT_DG_FOLD_CHUNK];
  for (off = 0; off < len; off += FASTENT_DG_FOLD_CHUNK) {
    sz cl = len - off;
    if (cl > FASTENT_DG_FOLD_CHUNK) cl = FASTENT_DG_FOLD_CHUNK;
    memcpy(scratch, buf + off, cl);
    ff(scratch, cl);
    if (binary) digram_bits_(st, scratch, cl);
    else        digram_bytes_drained_(st, scratch, cl);
  }
}
