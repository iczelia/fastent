/*  fastent: the -ee level-2 extra pass.

    One scalar pass over the same bytes the SIMD analyser already
    consumed (order-0 keeps full SIMD throughput), folding every
    per-symbol level-2 reduction into a single scan:

      digram     : order-0 histogram of the 16-bit key (prev<<8)|cur
                   over FASTENT_BG_NB round-robin shadow tables, so
                   the store-to-load-forward dependency does not
                   serialise; the planes are summed at finalize.  Bit
                   mode uses the 2x2 bit_bigram.
      longest run: longest identical-symbol run (bytes or bits).
      runs       : number of 0/1 runs                (bit mode).
      cusum_max  : extent of the +-1 bit walk         (bit mode).

    Streams call it per chunk; with -j each worker scans its slab and
    run_mmap_mt_ merges the per-slab reductions with a boundary
    stitch, so the result is bit-identical for any thread count.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

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
    analyze.c).  The bit-mode scan below uses fastent_lr_run; the byte
    digram + run kernel is templatized per ISA (analyze-impl.h) and
    dispatched via dg_byte_fn_().  */

/*  Cached byte digram + longest-run kernel.  Process-stable pick so
    the lazy-init race stores the same pointer (as dg_fold_fn_).  The
    always-built scalar variant is the reference; the chosen SIMD
    variant reproduces its counters bit-for-bit.  */
static fastent_digram_byte_fn dg_byte_fn_(void) {
  static fastent_digram_byte_fn df = NULL;
  fastent_digram_byte_fn f = df;
  if (!f) { f = fastent_pick_digram_byte_variant(NULL);  df = f; }
  return f;
}

#define FASTENT_DG_BITS_CHUNK 65536u                   /*  bytes  */
#define FASTENT_DG_BITS_WORDS (FASTENT_DG_BITS_CHUNK / 8u)

/*  One <= 64 KiB sub-block; state carries across calls so a sub-block
    is a contiguous continuation (boundary pair, run, cusum offset,
    rn_last all thread through st as the per-bit scan).  cs LUT holds
    min/max prefix offset and net of a byte's MSB-first +-1 walk.  */
static void digram_bits_blk_(fastent_chunk_state * st,
                             const u8 * FASTENT_RESTRICT buf, sz cl,
                             const i32 * cs_mn, const i32 * cs_mx,
                             const i32 * cs_net) {
  u64 W[FASTENT_DG_BITS_WORDS];
  const sz M  = cl * 8;
  const sz NW = (M + 63) / 64;
  sz w, i;

  memset(W, 0, NW * sizeof(u64));
  for (i = 0; i < cl; i++)
    W[i >> 3] |= (u64) fastent_bitrev8_(buf[i]) << ((u32)(i & 7) * 8u);

  const u32 b0    = (u32)(W[0] & 1u);
  const sz  lbpos = M - 1;
  const u32 lbit  = (u32)((W[lbpos >> 6] >> (lbpos & 63)) & 1u);

  /*  bit_bigram: n11 / transitions / n10 over the M-1 internal
      pairs.  succ(W)[p] = W[p+1]; out-of-block bits are zero, so
      only the spurious p == M-1 term (= lbit) needs correcting.  */
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
  const u64 n00 = (u64)(M - 1) - ntr - n11;

  if (st->dg_have) st->bit_bigram[st->dg_prev & 1u][b0]++;
  st->bit_bigram[0][0] += n00;  st->bit_bigram[0][1] += n01;
  st->bit_bigram[1][0] += n10;  st->bit_bigram[1][1] += n11;
  st->dg_prev = (u8) lbit;  st->dg_have = 1;

  /*  0/1 run count = prior runs + boundary flip + internal flips.  */
  if (!st->rn_have) { st->rn_have = 1;  st->rn_count = 1 + ntr; }
  else {
    if (b0 != st->rn_last) st->rn_count++;
    st->rn_count += ntr;
  }
  st->rn_last = (u8) lbit;

  /*  cusum: per-byte LUT, running offset, fold into the carried
      walk.  */
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

  /*  Longest run: enumerate runs via the transition bitmap and feed
      each to fastent_lr_run (exactly the scalar run sequence).  */
  {
    const sz  wl = lbpos >> 6;
    const u32 bl = (u32)(lbpos & 63);
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
        fastent_lr_run(st, sym, (u64)(p - lastp));
        lastp = p;  sym ^= 1u;
        tw &= tw - 1;
      }
    }
    fastent_lr_run(st, sym, (u64)((i64) lbpos - lastp));
  }
}

/*  Bit mode: every adjacent bit pair (MSB->LSB, across byte and
    chunk boundaries via the carried previous bit) feeds the 2x2
    bigram, longest run, 0/1 run count and the +-1 cusum walk.
    Word-parallel; dg_prev holds the previous bit (0/1).  */
static void digram_bits_(fastent_chunk_state * st,
                         const u8 * FASTENT_RESTRICT buf, sz len) {
  i32 cs_mn[256], cs_mx[256], cs_net[256];
  i32 v, k;
  for (v = 0; v < 256; v++) {
    i32 s = 0, mn = 0, mx = 0;
    for (k = 7; k >= 0; k--) {
      s += ((v >> k) & 1) ? 1 : -1;
      if (s < mn) mn = s;
      if (s > mx) mx = s;
    }
    cs_mn[v] = mn;  cs_mx[v] = mx;  cs_net[v] = s;
  }
  { sz off;
    for (off = 0; off < len; off += FASTENT_DG_BITS_CHUNK) {
      sz cl = len - off;
      if (cl > FASTENT_DG_BITS_CHUNK) cl = FASTENT_DG_BITS_CHUNK;
      digram_bits_blk_(st, buf + off, cl, cs_mn, cs_mx, cs_net);
    }
  }
}

/*  Byte scan with deterministic u32->u64 drains.  Drain fires when
    dg_chunk_bytes would cross FASTENT_DG_U32_CHUNK; split calls are
    bit-identical to one (dg_prev/lr carry), so the drain point depends
    only on byte count (any -j).  Draining first keeps cells < 2^31.  */
static void digram_bytes_drained_(fastent_chunk_state * st,
                                  const u8 * FASTENT_RESTRICT buf, sz len) {
  fastent_digram_byte_fn fn = dg_byte_fn_();
  sz off = 0;
  while (off < len) {
    if (st->dg_chunk_bytes >= FASTENT_DG_U32_CHUNK)
      fastent_dg_drain(st);
    sz room = (sz)(FASTENT_DG_U32_CHUNK - st->dg_chunk_bytes);
    sz n = len - off;
    if (n > room) n = room;
    fn(st, buf + off, n);
    st->dg_chunk_bytes += n;
    off += n;
  }
}

void fastent_digram_count(fastent_chunk_state * st, const u8 * buf,
                          sz len, int binary, int fold) {
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
  for (sz off = 0; off < len; off += FASTENT_DG_FOLD_CHUNK) {
    sz cl = len - off;
    if (cl > FASTENT_DG_FOLD_CHUNK) cl = FASTENT_DG_FOLD_CHUNK;
    memcpy(scratch, buf + off, cl);
    ff(scratch, cl);
    if (binary) digram_bits_(st, scratch, cl);
    else        digram_bytes_drained_(st, scratch, cl);
  }
}
