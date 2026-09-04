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

#ifndef FASTENT_LZEST_H
#define FASTENT_LZEST_H

#include "common.h"

/*  Block grid on the absolute file offset (block = abs/4 MiB, fresh
    table per block).  Tables are additive, so the integer sum-merge
    is order-independent: bit-identical for any -j, driver, host.  */
#define FASTENT_LZ_GRID  (4u * 1024u * 1024u)

/*  LZ77F match-finder constants (acceleration 1).  */
#define FASTENT_LZ_HLOG     13            /*  1<<13 u32 = 32 KiB table  */
#define FASTENT_LZ_HSZ      (1u << FASTENT_LZ_HLOG)
#define FASTENT_LZ_MINMATCH 4
#define FASTENT_LZ_MFLIMIT  12
#define FASTENT_LZ_LASTLIT  5
#define FASTENT_LZ_SKIP     6             /*  skip trigger              */
#define FASTENT_LZ_DISTMAX  65535
#define FASTENT_LZ_OFFFLUSH 60000         /*  < 65535: u16 cannot wrap  */

/*  The three raw LZ77F tables, heap-allocated only under -eee (one
    block per fastent_result, never inlined into the result struct).  */
struct fastent_lz77f_tables {
  u64 off_par[65536];     /*  per-offset match count                   */
  u64 mlen_full[256];     /*  per-match-length count (255 = sat cap)   */
  u64 lit_byte[256];      /*  LZ77-unmatched byte-value histogram      */
};

/*  Per-thread LZ77F accumulator: buffers an absolute range one grid
    block at a time, parses each with a fresh table, sum-merges into
    the integer tables (off_blk u16 folded into off_par periodically). */
typedef struct {
  u64 outsz;              /*  exact count-only encoded size            */
  u64 nmatch, nlit_run, lit_bytes, match_bytes;
  u64 off_par[65536];
  u64 mlen_full[256];
  u64 lit_byte[256];
  u16 off_blk[65536];     /*  hot per-window offset block              */
  u32 since_flush;        /*  matches since the last off flush         */

  /*  Absolute grid bookkeeping (blk has +32 slack for count_eq's
      over-read; the final tail clamps to the scalar loop).  */
  u64 abs_base, abs_pos;
  u8 * blk;
  sz   blk_len;           /*  bytes currently buffered in blk          */
  u64  blk_off;           /*  absolute offset of blk[0]                */
  u32 * tbl;              /*  FASTENT_LZ_HSZ u32, lazily allocated     */
  int  oom;               /*  set if a buffer/table alloc failed       */
} fastent_lz_acc;

/*  init binds the acc to its absolute base; feed takes any-size
    chunks (split at grid lines); merge sum-folds one acc into another
    (order independent); flush parses the tail; free releases.  -1=OOM. */
void fastent_lz_acc_init(fastent_lz_acc * a, u64 abs_base);
int  fastent_lz_acc_feed(fastent_lz_acc * a, const u8 * buf, sz len);
int  fastent_lz_acc_flush(fastent_lz_acc * a);
void fastent_lz_acc_merge(fastent_lz_acc * dst, const fastent_lz_acc * src);
void fastent_lz_acc_free(fastent_lz_acc * a);
void fastent_lz_acc_reset(fastent_lz_acc * a, u64 abs_base);

struct fastent_result;  /*  fwd: analyze.h includes this header  */

/*  Heap allocation of the 3 result tables (zeroed); NULL on OOM.  */
struct fastent_lz77f_tables * fastent_lz77f_tables_alloc(void);
void fastent_lz77f_tables_free(struct fastent_lz77f_tables * t);

/*  Fill the LZ77F result fields from a merged acc (n = total bytes):
    scalar stats into *out, 3 tables too if out->lz != NULL.  The
    nmatch<2 limits make every stat a NaN-free total function.  */
void fastent_lz_finalize(
    const fastent_lz_acc * a, u64 n, struct fastent_result * out);

#endif
