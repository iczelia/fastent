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

#ifndef FASTENT_BM_H
#define FASTENT_BM_H

#include "common.h"
#include "lzest.h"   /*  FASTENT_LZ_GRID: the shared absolute grid  */

/*  512-bit (64-byte) window; 64 divides the 4 MiB grid so windows
    never straddle a block (exact additive sum-merge, zero drift).  */
#define FASTENT_BM_M    512
#define FASTENT_BM_WB   (FASTENT_BM_M / 8)
#define FASTENT_BM_W64  (FASTENT_BM_M / 64)

typedef char fastent_bm_grid_assert_[
    (FASTENT_LZ_GRID % FASTENT_BM_WB == 0) ? 1 : -1];

/*  L histogram: 64 bins over [0, M], inline in fastent_result.  */
#define FASTENT_BM_LBINS 64

/*  Window batch slab for the vector scorers (multiple of 8 so it is a
    whole multiple of every lane count: 2/4/8 and any SVE VL/64).  */
#define FASTENT_BM_BATCH 4096

/*  Per-thread accumulator: buffers one absolute grid block, scores
    each whole window with a fresh GF(2) pass, sum-merges integers.  */
typedef struct {
  u64 windows;
  u64 meanl_sum;
  u64 lhist[FASTENT_BM_LBINS];
  u64 tbin[6];  /*  NIST T-class histogram, df = 5  */

  /*  At most one partial tail window per stream; merged by a
      fixed-order pick on tail_abs.  */
  u64 tail_abs;
  u32 tail_bits;  /*  m' (0 = none)  */
  u32 tail_l;
  u8 have_tail;

  u64 abs_base, abs_pos;
  u8 * blk;
  sz blk_len;
  u64 blk_off;
  int oom;
} fastent_bm_acc;

/*  feed takes any-size chunks (split at grid lines); merge is order
    independent; flush scores the tail; -1 = OOM.  */
void fastent_bm_acc_init(fastent_bm_acc * a, u64 abs_base);
int  fastent_bm_acc_feed(fastent_bm_acc * a, const u8 * buf, sz len);
int  fastent_bm_acc_flush(fastent_bm_acc * a);
void fastent_bm_acc_merge(fastent_bm_acc * dst, const fastent_bm_acc * src);
void fastent_bm_acc_free(fastent_bm_acc * a);
void fastent_bm_acc_reset(fastent_bm_acc * a, u64 abs_base);

/*  Batched scorers: each scores its lane count of windows per pass with the
    same per-window L as the scalar reference, and returns the count scored
    (the largest whole multiple of its lane width <= nfull).  */
#ifdef HAVE_SSE41
sz fastent_bm_windows_sse(const u8 * src, sz nfull, u32 * Lout);
#endif
#ifdef HAVE_AVX2
sz fastent_bm_windows_avx2(const u8 * src, sz nfull, u32 * Lout);
#endif
#ifdef HAVE_AVX512
sz fastent_bm_windows_avx512(const u8 * src, sz nfull, u32 * Lout);
#endif
#ifdef HAVE_NEON
sz fastent_bm_windows_neon(const u8 * src, sz nfull, u32 * Lout);
#endif
#ifdef HAVE_SVE2
sz fastent_bm_windows_sve(const u8 * src, sz nfull, u32 * Lout);
#endif

struct fastent_result;  /*  fwd: analyze.h includes this header  */

/*  Zero full windows -> z = 0.0; NaN is the not-computed sentinel.  */
void fastent_bm_finalize(
    const fastent_bm_acc * a, u64 n, struct fastent_result * out);

#endif
