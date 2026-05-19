/*  fastent: windowed Maurer universal statistical test (count-only).

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

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

/*  Memoised log2(d) for the small distances that dominate (d < this
    bound covers ~99.95% of test blocks); larger d recomputes.  The
    cached value is the same pure fastent_log2_ratio result, so the
    in-order sum stays bit-identical.  512 KiB, lazily allocated.  */
#define FASTENT_MA_LOGT  65536

/*  One absolute grid block's deterministic partial: its block index,
    its in-order Sum log2(distance) and its test-block count K.  */
typedef struct {
  u64 idx;
  f64 sum;
  u64 k;
} fastent_maurer_part;

/*  Per-thread accumulator: buffers one absolute grid block, runs a
    fresh-table Maurer over it and appends one partial record.  merge
    concatenates the records; finalize reduces them in absolute block
    order so the f64 sum is bit-identical for any -j / driver / host.
    The 2^L T table, the small log2 memo and the 4 MiB blk are heap
    (lazy malloc); the inline state is a few scalars plus the 512-byte
    log2-distance histogram (sizeof ~600 bytes), so a stack-local acc
    is safe yet every path still heap-allocates it for the wasm / DOS
    small-stack guarantee. */
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
