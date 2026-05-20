/*  fastent: Bandt-Pompe permutation entropy estimator (count-only).

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_PERMENT_H
#define FASTENT_PERMENT_H

#include "common.h"
#include "lzest.h"   /*  FASTENT_LZ_GRID: the shared absolute grid  */

/*  Embedding dimension m and delay tau = 1 (Bandt-Pompe 2002).  m = 4
    gives 24 ordinal patterns, a Lehmer-code id in 6 strict comparisons
    per window, and a chi-square df of 23 (odd: fastent_chisq_tail_df
    consumes it directly).  Single-byte windows always divide the 4 MiB
    grid; bounded boundary drift is documented in perment.c.  */
#define FASTENT_PERMENT_M     4
#define FASTENT_PERMENT_BINS  24

typedef char fastent_perment_grid_assert_[
    (FASTENT_LZ_GRID % 1u == 0) ? 1 : -1];

/*  Per-thread accumulator: buffers one absolute grid block, parses each
    with a fresh hot loop (n - (m - 1) ordinal windows), sum-merges the
    24-bin histogram.  The merge is integer-additive across blocks; the
    (m - 1) windows that would span a grid boundary are dropped, capping
    drift at (nblocks - 1) * 3 windows out of ~n total (verdict-neutral
    at the 4 MiB grid, mirrors LZ77F's bounded-drift precedent).  */
typedef struct {
  u64 hist[FASTENT_PERMENT_BINS];
  u64 windows;                /*  full m-tuples scored (no boundary)   */

  u64 abs_base, abs_pos;
  u8 * blk;
  sz   blk_len;
  u64  blk_off;
  int  oom;
} fastent_perment_acc;

/*  feed takes any-size chunks (split at grid lines); merge sum-folds
    histograms (order independent); flush parses the tail; -1 = OOM.  */
void fastent_perment_acc_init(fastent_perment_acc * a, u64 abs_base);
void fastent_perment_acc_reset(fastent_perment_acc * a, u64 abs_base);
int  fastent_perment_acc_feed(
    fastent_perment_acc * a, const u8 * buf, sz len);
int  fastent_perment_acc_flush(fastent_perment_acc * a);
void fastent_perment_acc_merge(
    fastent_perment_acc * dst, const fastent_perment_acc * src);
void fastent_perment_acc_free(fastent_perment_acc * a);

struct fastent_result;  /*  fwd: analyze.h includes this header  */

/*  Write the perment_* fields of fastent_result unconditionally; the
    headline z stays NaN only when no windows were scored.  */
void fastent_perment_finalize(
    const fastent_perment_acc * a, u64 n, struct fastent_result * out);

#endif
