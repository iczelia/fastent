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

#ifndef FASTENT_MRANK_H
#define FASTENT_MRANK_H

#include "common.h"
#include "lzest.h"   /*  FASTENT_LZ_GRID: the shared absolute grid  */

/*  32x32 GF(2) matrix per test unit: 32 rows of 32 bits = 1024 bits = 128
    bytes.  */
#define FASTENT_MRANK_M  32u
#define FASTENT_MRANK_Q  32u
#define FASTENT_MRANK_MB 128u   /*  bytes per matrix  */

typedef char fastent_mrank_grid_assert_[
    (FASTENT_LZ_GRID % FASTENT_MRANK_MB == 0) ? 1 : -1];

/*  Three pooled bins: rank == 32, rank == 31, rank <= 30.  The
    integrator copies these into the inline result histogram.  */
#define FASTENT_MRANK_BINS 3

/*  Per-thread accumulator: buffers one absolute grid block, scores
    every whole 128-byte matrix with a fresh row word load, sum-merges
    the three bin counts.  */
typedef struct {
  u64 matrices;                       /*  total full matrices scored  */
  u64 bins[FASTENT_MRANK_BINS];       /*  [0]=r32, [1]=r31, [2]=rlo  */

  u64 abs_base, abs_pos;
  u8 * blk;
  sz   blk_len;
  u64  blk_off;
  int  oom;
} fastent_mrank_acc;

/*  feed takes any-size chunks (split at grid lines); merge is order
    independent; flush scores the tail; -1 = OOM.  */
void fastent_mrank_acc_init(fastent_mrank_acc * a, u64 abs_base);
void fastent_mrank_acc_reset(fastent_mrank_acc * a, u64 abs_base);
int  fastent_mrank_acc_feed(fastent_mrank_acc * a, const u8 * buf, sz len);
int  fastent_mrank_acc_flush(fastent_mrank_acc * a);
void fastent_mrank_acc_merge(
    fastent_mrank_acc * dst, const fastent_mrank_acc * src);
void fastent_mrank_acc_free(fastent_mrank_acc * a);

struct fastent_result;  /*  fwd: analyze.h includes this header  */

/*  NIST sec 2.5 minimum: skip the chi-square verdict below this many
    matrices; the headline is NaN with mrank_underpowered = 1.  */
#define FASTENT_MRANK_MIN 16u

/*  Bench-side compact summary, mirror of the fastent_result fields the
    integrator plumbs.  All numbers here are total functions of the
    merged acc; the headline is NaN when underpowered.  */
typedef struct {
  u64 matrices;                  /*  total full 32x32 matrices  */
  u32 hist[FASTENT_MRANK_BINS];  /*  saturating u32 copy of bin counts  */
  f64 chi2;                      /*  chi-square statistic, df = 2  */
  f64 mrank_dev;                 /*  headline = sqrt(chi2)  */
  f64 p_r32;                     /*  NIST expected proportions  */
  f64 p_r31;
  f64 p_rlo;
  i32 mrank_underpowered;        /*  1 if matrices < FASTENT_MRANK_MIN  */
} fastent_mrank_summary;

/*  Pure summary computation; used by the standalone bench.  Finalize
    inlines the same math directly into fastent_result.  */
void fastent_mrank_compute(
    const fastent_mrank_acc * a, fastent_mrank_summary * s);

/*  Write the mrank_* fields of fastent_result unconditionally; NaN
    sentinels survive when matrices < FASTENT_MRANK_MIN.  */
void fastent_mrank_finalize(
    const fastent_mrank_acc * a, u64 n, struct fastent_result * out);

#endif
