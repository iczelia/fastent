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

#ifndef FASTENT_MAURER_H
#define FASTENT_MAURER_H

#include "common.h"
#include "lzest.h"   /*  FASTENT_LZ_GRID: the shared absolute grid  */

/*  L = 8 bits/block: table 2^8, L/8 = 1 divides the 4 MiB grid so a
    block never straddles a grid line.  Q = 10*2^L init blocks.  */
#define FASTENT_MA_L     8
#define FASTENT_MA_TSZ   (1u << FASTENT_MA_L)
#define FASTENT_MA_Q     (10u * FASTENT_MA_TSZ)

typedef char fastent_maurer_grid_assert_[
    (FASTENT_LZ_GRID % (FASTENT_MA_L / 8u) == 0) ? 1 : -1];

/*  log2-distance histogram: 64 bins, inline in fastent_result.  */
#define FASTENT_MA_LBINS 64

/*  Memoised log2(d) for the small distances that dominate (d < this bound
    covers ~99.95% of test blocks); larger d recomputes.  */
#define FASTENT_MA_LOGT  65536

/*  One absolute grid block's deterministic partial: its block index,
    its in-order Sum log2(distance) and its test-block count K.  */
typedef struct {
  u64 idx;
  f64 sum;
  u64 k;
} fastent_maurer_part;

/*  Per-thread accumulator: buffers one absolute grid block, runs a
    fresh-table Maurer over it and appends one partial record.  */
typedef struct {
  fastent_maurer_part * parts;  /*  one per scored grid block  */
  sz   nparts, cap;
  u64  lhist[FASTENT_MA_LBINS];

  u64   abs_base, abs_pos;
  u8 *  blk;
  sz    blk_len;
  u64   blk_off;
  u32 * tbl;                    /*  2^L block indices, lazily allocated  */
  f64 * logt;                   /*  memoised log2(d), lazily allocated  */
  int   oom;
} fastent_maurer_acc;

/*  feed takes any-size chunks (split at grid lines); merge concatenates
    the per-block partials; flush scores the tail; -1 = OOM.  */
void fastent_maurer_acc_init(fastent_maurer_acc * a, u64 abs_base);
void fastent_maurer_acc_reset(fastent_maurer_acc * a, u64 abs_base);
int  fastent_maurer_acc_feed(fastent_maurer_acc * a, const u8 * buf, sz len);
int  fastent_maurer_acc_flush(fastent_maurer_acc * a);
void fastent_maurer_acc_merge(
    fastent_maurer_acc * dst, const fastent_maurer_acc * src);
void fastent_maurer_acc_free(fastent_maurer_acc * a);

struct fastent_result;  /*  fwd: analyze.h includes this header  */

/*  No test block anywhere -> z = 0.0; NaN is the not-computed
    sentinel.  */
void fastent_maurer_finalize(
    fastent_maurer_acc * a, u64 n, struct fastent_result * out);

#endif
