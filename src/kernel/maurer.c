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

#include "maurer.h"
#include "analyze.h"
#include "fastent-math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*  IEEE sqrt is correctly rounded, so bit-identical across hosts.  */
static inline f64 fastent_maurer_sqrt(f64 x) { return sqrt(x); }

/*  Libm-free 2^x for x <= 0 (the c factor needs K^(-3/L)).  */
static f64 fastent_maurer_exp2_neg(f64 x) {
  f64 fi = floor(x);
  int i = (int) fi;
  f64 f = x - fi;
  f64 p = 2.1646978346697556e-05;
  p = p * f + 1.4294854512282671e-04;
  p = p * f + 1.3431442313888724e-03;
  p = p * f + 9.6133392592351822e-03;
  p = p * f + 5.5505400873892531e-02;
  p = p * f + 2.4022632934519089e-01;
  p = p * f + 6.9314719076093056e-01;
  p = p * f + 9.9999999985822574e-01;
  return ldexp(p, i);
}

/*  Score one grid block [src,src+n): bits MSB-first, L-bit blocks,
    first Q set T, the rest add log2(i - T[pat]) fused through the
    volatile barrier.  Emits at most one partial record.  */
static void fastent_maurer_block(
    fastent_maurer_acc * a, const u8 * src, sz n) {
  u64 nbits = (u64) n * 8u;
  u64 nblk  = nbits / FASTENT_MA_L;
  u64 i;
  if (nblk < (u64) FASTENT_MA_Q + 1u) return;
  u64 K = nblk - FASTENT_MA_Q;

  u32 * T = a->tbl;
  memset(T, 0, (sz) FASTENT_MA_TSZ * sizeof (u32));
  volatile f64 sum = 0.0;

  const f64 * lt = a->logt;
  for (i = 1; i <= nblk; i++) {
    u32 pat;
#if FASTENT_MA_L == 8
    /*  L = 8: the block is exactly one byte, no bit assembly.  */
    pat = src[i - 1u];
#else
    u64 bp = (i - 1u) * FASTENT_MA_L;
    u32 b;
    pat = 0;
    for (b = 0; b < FASTENT_MA_L; b++) {
      u64 q = bp + b;
      pat = (pat << 1) | ((src[q >> 3] >> (7u - (q & 7u))) & 1u);
    }
#endif
    if (i <= (u64) FASTENT_MA_Q) { T[pat] = (u32) i; } else {
      u64 d = i - (u64) T[pat];
      T[pat] = (u32) i;
      f64 l2 = d < FASTENT_MA_LOGT ? lt[d] : fastent_log2_ratio(d, 1);
      sum = sum + l2;
      u32 bin = (u32) l2;
      if (bin >= FASTENT_MA_LBINS) bin = FASTENT_MA_LBINS - 1u;
      a->lhist[bin]++;
    }
  }

  if (a->nparts == a->cap) {
    sz nc = a->cap ? a->cap * 2u : 8u;
    fastent_maurer_part * np = (fastent_maurer_part *)
      realloc(a->parts, nc * sizeof (*np));
    if (!np) { a->oom = 1;  return; }
    a->parts = np;  a->cap = nc;
  }
  a->parts[a->nparts].idx = a->blk_off / (u64) FASTENT_LZ_GRID;
  a->parts[a->nparts].sum = sum;
  a->parts[a->nparts].k   = K;
  a->nparts++;
}

static int fastent_maurer_ensure(fastent_maurer_acc * a) {
  i32 i;
  if (a->oom) return -1;
  if (!a->tbl) {
    a->tbl = (u32 *) malloc((sz) FASTENT_MA_TSZ * sizeof (u32));
    if (!a->tbl) { a->oom = 1;  return -1; }
  }
  if (!a->blk) {
    a->blk = (u8 *) malloc((sz) FASTENT_LZ_GRID);
    if (!a->blk) { a->oom = 1;  return -1; }
  }
  if (!a->logt) {
    a->logt = (f64 *) malloc((sz) FASTENT_MA_LOGT * sizeof (f64));
    if (!a->logt) { a->oom = 1;  return -1; }
    for (i = 1; i < FASTENT_MA_LOGT; i++)
      a->logt[i] = fastent_log2_ratio((u64) i, 1);
  }
  return 0;
}

void fastent_maurer_acc_init(fastent_maurer_acc * a, u64 abs_base) {
  memset(a, 0, sizeof (*a));
  a->abs_base = abs_base;
  a->abs_pos  = abs_base;
  a->blk_off  = abs_base;
}

/*  Rebase for the next absolute grid block while keeping the heap scratch (T
    table, log2 memo, block buffer) so a worker that scores many blocks fills
    the memo once.  */
void fastent_maurer_acc_reset(fastent_maurer_acc * a, u64 abs_base) {
  a->nparts  = 0;
  a->blk_len = 0;
  a->oom     = 0;
  a->abs_base = abs_base;
  a->abs_pos  = abs_base;
  a->blk_off  = abs_base;
  i32 i;
  Fi(FASTENT_MA_LBINS, a->lhist[i] = 0);
}

int fastent_maurer_acc_feed(fastent_maurer_acc * a, const u8 * buf, sz len) {
  if (len == 0) return 0;
  if (fastent_maurer_ensure(a) != 0) return -1;
  sz pos = 0;
  while (pos < len) {
    u64 abs = a->abs_pos;
    u64 next_grid = (abs / FASTENT_LZ_GRID + 1) * (u64) FASTENT_LZ_GRID;
    sz  room = (sz) (next_grid - abs);
    sz  take = len - pos;
    if (take > room) take = room;
    memcpy(a->blk + a->blk_len, buf + pos, take);
    a->blk_len += take;
    a->abs_pos += take;
    pos        += take;
    if (a->abs_pos % FASTENT_LZ_GRID == 0) {
      fastent_maurer_block(a, a->blk, a->blk_len);
      if (a->oom) return -1;
      a->blk_len = 0;
      a->blk_off = a->abs_pos;
    }
  }
  return 0;
}

int fastent_maurer_acc_flush(fastent_maurer_acc * a) {
  if (a->oom) return -1;
  if (a->blk_len) {
    if (fastent_maurer_ensure(a) != 0) return -1;
    fastent_maurer_block(a, a->blk, a->blk_len);
    a->blk_len = 0;
    if (a->oom) return -1;
  }
  return 0;
}

void fastent_maurer_acc_merge(
    fastent_maurer_acc * dst, const fastent_maurer_acc * src) {
  i32 i;
  if (src->oom) dst->oom = 1;
  Fi(FASTENT_MA_LBINS, dst->lhist[i] += src->lhist[i]);
  if (src->nparts == 0) return;
  sz need = dst->nparts + src->nparts;
  if (need > dst->cap) {
    sz nc = dst->cap ? dst->cap : 8u;
    while (nc < need) nc *= 2u;
    fastent_maurer_part * np = (fastent_maurer_part *)
      realloc(dst->parts, nc * sizeof (*np));
    if (!np) { dst->oom = 1;  return; }
    dst->parts = np;  dst->cap = nc;
  }
  memcpy(dst->parts + dst->nparts, src->parts,
         src->nparts * sizeof (*src->parts));
  dst->nparts = need;
}

void fastent_maurer_acc_free(fastent_maurer_acc * a) {
  free(a->tbl);    a->tbl   = NULL;
  free(a->logt);   a->logt  = NULL;
  free(a->blk);    a->blk   = NULL;
  free(a->parts);  a->parts = NULL;
  a->nparts = a->cap = 0;
}

static int fastent_maurer_part_cmp(const void * x, const void * y) {
  u64 ax = ((const fastent_maurer_part *) x)->idx;
  u64 bx = ((const fastent_maurer_part *) y)->idx;
  return ax < bx ? -1 : (ax > bx ? 1 : 0);
}

/*  NIST SP800-22 sec 2.9 expected value and variance for L = 8.  */
#define FASTENT_MA_EXPECTED 7.1836656
#define FASTENT_MA_VARIANCE 3.238

void fastent_maurer_finalize(
    fastent_maurer_acc * a, u64 n, struct fastent_result * out) {
  i32 i;
  (void) n;
  out->maurer_fn       = 0.0;
  out->maurer_expected = FASTENT_MA_EXPECTED;
  out->maurer_dev      = 0.0;
  out->maurer_k        = 0;
  out->maurer_degenerate = 0;
  Fi(FASTENT_MA_LBINS, out->maurer_lhist[i] =
    (u32) (a->lhist[i] > 0xffffffffu ? 0xffffffffu : a->lhist[i]));

  if (a->nparts == 0) return;

  /*  Sort the per-block partials in place by absolute block index;
      no copy, so there is no allocation that could false-PASS.  */
  qsort(a->parts, a->nparts, sizeof (*a->parts), fastent_maurer_part_cmp);

  volatile f64 tsum = 0.0;
  u64 tk = 0;
  Fi((int) a->nparts, tsum = tsum + a->parts[i].sum;  tk += a->parts[i].k);

  out->maurer_k = tk;
  if (tk == 0) return;

  f64 fn = (f64) tsum / (f64) tk;
  out->maurer_fn = fn;

  /*  c = 0.7 - 0.8/L + (4 + 32/L) * K^(-3/L) / 15, libm-free.  */
  f64 lk  = fastent_log2_ratio(tk, 1);
  f64 km  = fastent_maurer_exp2_neg(-(3.0 / 8.0) * lk);
  f64 c   = 0.7 - 0.8 / 8.0 + (4.0 + 32.0 / 8.0) * km / 15.0;

  volatile f64 num = fn - FASTENT_MA_EXPECTED;
  f64 an = num < 0.0 ? -num : num;
  f64 sigma = c * fastent_maurer_sqrt(FASTENT_MA_VARIANCE / (f64) tk);
  out->maurer_dev = sigma > 0.0 ? an / sigma : 0.0;

  /*  fn far below expected: a near-constant / highly repetitive
      stream; the z still fails it.  */
  if (fn < FASTENT_MA_EXPECTED * 0.5) out->maurer_degenerate = 1;
}
