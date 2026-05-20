/*  fastent: Bandt-Pompe permutation entropy estimator.

    A count-only ordinal-pattern scorer: each window (x[i], x[i+1],
    x[i+2], x[i+3]) is reduced to one of 24 Lehmer-code ids via six
    strict byte comparisons (ties break by index, the standard Bandt-
    Pompe convention).  Per-block 24-bin histograms sum-merge on the
    shared 4 MiB absolute grid.  Drift versus a whole-stream reference
    is at most (nblocks - 1) * 3 dropped boundary windows out of ~n
    total (~7.2e-7 at 4 MiB blocks, verdict-neutral, mirrors LZ77F).
    Catches short-range monotone / structural correlations missed by
    order-0 / order-1; blind to long-range patterns by construction
    (m = 4).

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "perment.h"
#include "analyze.h"
#include "fastent-math.h"
#include "chisq.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*  IEEE sqrt is correctly rounded, so bit-identical across hosts.  */
static inline f64 fastent_perment_sqrt(f64 x) { return sqrt(x); }

/*  One block's hot loop: produce n - (m - 1) ordinal pattern ids and
    fold them into the 24-bin histogram.  Six strict comparisons + one
    increment per window; the (u32) cast forces 0/1 lane arithmetic so
    branch prediction never enters the inner sum.  */
static FASTENT_HOT void fastent_perment_block(
    fastent_perment_acc * FASTENT_RESTRICT a,
    const u8 * FASTENT_RESTRICT src, sz n) {
  if (n < FASTENT_PERMENT_M) return;
  sz nw = n - (FASTENT_PERMENT_M - 1u);
  u64 * FASTENT_RESTRICT h = a->hist;
  sz i = 0;
  for (; i + 8u <= nw; i += 8u) {
    /*  Unroll by 8 so the load/compare chains overlap; the trailing
        byte of one window is the leading byte of the next + 1, so the
        9-byte read pattern keeps the working set in one cache line.  */
    Fj(8,
       u32 a0 = src[i + (sz) j + 0];
       u32 a1 = src[i + (sz) j + 1];
       u32 a2 = src[i + (sz) j + 2];
       u32 a3 = src[i + (sz) j + 3];
       u32 c0 = (u32)(a0 > a1) + (u32)(a0 > a2) + (u32)(a0 > a3);
       u32 c1 = (u32)(a1 > a2) + (u32)(a1 > a3);
       u32 c2 = (u32)(a2 > a3);
       u32 id = c0 * 6u + c1 * 2u + c2;
       h[id]++)
  }
  for (; i < nw; i++) {
    u32 a0 = src[i + 0], a1 = src[i + 1];
    u32 a2 = src[i + 2], a3 = src[i + 3];
    u32 c0 = (u32)(a0 > a1) + (u32)(a0 > a2) + (u32)(a0 > a3);
    u32 c1 = (u32)(a1 > a2) + (u32)(a1 > a3);
    u32 c2 = (u32)(a2 > a3);
    u32 id = c0 * 6u + c1 * 2u + c2;
    h[id]++;
  }
  a->windows += (u64) nw;
}

static int fastent_perment_ensure(fastent_perment_acc * a) {
  if (a->oom) return -1;
  if (!a->blk) {
    a->blk = (u8 *) malloc((sz) FASTENT_LZ_GRID);
    if (!a->blk) { a->oom = 1;  return -1; }
  }
  return 0;
}

void fastent_perment_acc_init(fastent_perment_acc * a, u64 abs_base) {
  memset(a, 0, sizeof(*a));
  a->abs_base = abs_base;
  a->abs_pos  = abs_base;
  a->blk_off  = abs_base;
}

/*  Re-init for the next grid block, keeping the lazily-grown buffer.  */
void fastent_perment_acc_reset(fastent_perment_acc * a, u64 abs_base) {
  u8 * blk = a->blk;
  fastent_perment_acc_init(a, abs_base);
  a->blk = blk;
}

int fastent_perment_acc_feed(
    fastent_perment_acc * a, const u8 * buf, sz len) {
  if (len == 0) return 0;
  if (fastent_perment_ensure(a) != 0) return -1;
  sz pos = 0;
  while (pos < len) {
    u64 abs = a->abs_pos;
    u64 next_grid = (abs / FASTENT_LZ_GRID + 1) * (u64) FASTENT_LZ_GRID;
    sz  room = (sz)(next_grid - abs);
    sz  take = len - pos;
    if (take > room) take = room;
    memcpy(a->blk + a->blk_len, buf + pos, take);
    a->blk_len += take;
    a->abs_pos += take;
    pos        += take;
    if (a->abs_pos % FASTENT_LZ_GRID == 0) {
      fastent_perment_block(a, a->blk, a->blk_len);
      a->blk_len = 0;
      a->blk_off = a->abs_pos;
    }
  }
  return 0;
}

int fastent_perment_acc_flush(fastent_perment_acc * a) {
  if (a->oom) return -1;
  if (a->blk_len) {
    if (fastent_perment_ensure(a) != 0) return -1;
    fastent_perment_block(a, a->blk, a->blk_len);
    a->blk_len = 0;
  }
  return 0;
}

void fastent_perment_acc_merge(
    fastent_perment_acc * dst, const fastent_perment_acc * src) {
  if (src->oom) dst->oom = 1;
  dst->windows += src->windows;
  Fi(FASTENT_PERMENT_BINS, dst->hist[i] += src->hist[i])
}

void fastent_perment_acc_free(fastent_perment_acc * a) {
  free(a->blk);  a->blk = NULL;
}

void fastent_perment_finalize(
    const fastent_perment_acc * a, u64 n, struct fastent_result * out) {
  (void) n;
  /*  Sentinel rule: floats default to NaN, ints to 0; overwrite below
      when the merged acc carries any windows.  Histogram is copied
      saturating-to-u32 so the result struct stays scalar-sized.  */
  out->perment_h_norm   = (f64) NAN;
  out->perment_deviation = (f64) NAN;
  out->perment_chi      = (f64) NAN;
  out->perment_chi_p    = (f64) NAN;
  out->perment_windows  = a->windows;
  Fi(FASTENT_PERMENT_BINS, out->perment_hist[i] =
     (u32)(a->hist[i] > 0xffffffffu ? 0xffffffffu : a->hist[i]))

  u64 W = a->windows;
  if (W == 0) return;

  /*  H_p = -sum p_i log2(p_i) summed in a fixed bin order so the f64
      result is bit-identical across hosts.  Each term is
      (k/W) * (log2(W) - log2(k)) computed via libm-free log2 helpers.  */
  f64 logW = fastent_log2_ratio(W, 1);
  volatile f64 H = 0.0;
  Fi(FASTENT_PERMENT_BINS,
     u64 k = a->hist[i];
     if (k == 0) continue;
     f64 logk = fastent_log2_ratio(k, 1);
     volatile f64 term = ((f64) k * (logW - logk)) / (f64) W;
     H = H + term)

  f64 logBins = fastent_log2_ratio((u64) FASTENT_PERMENT_BINS, 1);
  volatile f64 hn = H / logBins;
  if (hn > 1.0) hn = 1.0;
  if (hn < 0.0) hn = 0.0;
  out->perment_h_norm = hn;

  /*  Advisory chi-square against the uniform null over BINS, df = 23
      (odd; fastent_chisq_tail_df consumes it directly).  For 8-bit
      data the null is not exactly uniform under Bandt-Pompe (ties
      break by index introduce a ~1e-3 per-bin bias), so chi-square /
      its p are advisory; the entropy-based deviation below is the
      verdict-bearing headline.  */
  f64 expected = (f64) W / (f64) FASTENT_PERMENT_BINS;
  volatile f64 chi = 0.0;
  Fi(FASTENT_PERMENT_BINS,
     volatile f64 e = expected;
     volatile f64 v = (f64) a->hist[i];
     volatile f64 d = v - e;
     volatile f64 term = (d * d) / e;
     chi = chi + term)
  out->perment_chi   = chi;
  out->perment_chi_p = fastent_chisq_tail_df(chi, FASTENT_PERMENT_BINS - 1);

  /*  Headline z: (1 - H_norm) * sqrt(W * 2 * ln 2).  Under H0 (IID)
      Var(H_norm) ~ 1 / (2 ln 2 * W) so the unsigned shortfall in
      H_norm scales to a z-equivalent that PASSes random sources
      (small (1 - H_norm), z << 1) and FAILs concentrated histograms
      (H_norm near 0, z huge).  Fused through the volatile barrier.  */
  const f64 two_ln2 = 1.3862943611198906;
  volatile f64 oneMinus = 1.0 - hn;
  if (oneMinus < 0.0) oneMinus = 0.0;
  f64 zscale = fastent_perment_sqrt((f64) W * two_ln2);
  out->perment_deviation = oneMinus * zscale;
}
