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

#ifndef FASTENT_FIPS1402_H
#define FASTENT_FIPS1402_H

#include "common.h"
#include "analyze.h"  /*  fastent_variant enum for the SIMD dispatch.  */

#include <stdio.h>

/*  Aggregate over every full 20000-bit (2500-byte) block.  Each
    block is tested independently per FIPS 140-2 4.9.1; trailing
    bytes that do not fill a block are not tested.  */
typedef struct {
  u64 blocks;  /*  full 2500-byte blocks tested  */
  u64 leftover;  /*  trailing untested bytes  */
  u64 monobit_fail;  /*  blocks failing each sub-test  */
  u64 poker_fail;
  u64 runs_fail;
  u64 longrun_fail;
  u64 blocks_pass;  /*  blocks passing all four sub-tests  */
} fastent_fips_report;

/*  Run the four FIPS 140-2 RNG tests over buf[0..len).  */
void fastent_fips140_run(
    const u8 * buf, sz len, int threads, fastent_fips_report * out);

/*  Batched block runner: tests nblocks consecutive 2500-byte blocks starting
    at buf and folds the verdicts into *r (integer, sum-mergeable).  */
typedef void (* fastent_fips_run_fn)(const u8 * buf, u64 nblocks,
                                     fastent_fips_report * r);

#define FASTENT_FIPS_BLOCK_BYTES 2500u

/*  Bounded streaming FIPS driver: init binds *out, push folds whole blocks
    via the per-ISA runner, finish reports the residue.  */
typedef struct {
  fastent_fips_report * out;
  fastent_fips_run_fn run;
  u8 carry[FASTENT_FIPS_BLOCK_BYTES];
  sz fill;  /*  bytes currently in carry  */
} fastent_fips_stream;

void fastent_fips140_stream_init(
    fastent_fips_stream * s, fastent_fips_report * out);
void fastent_fips140_stream_push(
    fastent_fips_stream * s, const u8 * buf, sz len);
int  fastent_fips140_stream_finish(fastent_fips_stream * s);

/*  Human-readable report.  Returns 1 if the input passes (>= 1 block
    and every block passed all four tests), else 0.  */
int  fastent_fips140_print(const fastent_fips_report * r, FILE * fp);

/*  Resolve the best available FIPS block runner for this CPU and, if
    `which` is non-NULL, report which variant was chosen.  Mirrors
    fastent_pick_variant() in analyze.c.  */
fastent_fips_run_fn fastent_pick_fips_variant(fastent_variant * which);

/*  Per-ISA batched runners (one per variant TU).  The scalar form is
    always linked; the rest exist only when their HAVE_* tier built.  */
void fastent_fips_run_blocks_scalar(const u8 *, u64, fastent_fips_report *);
void fastent_fips_run_blocks_ssse3(const u8 *, u64, fastent_fips_report *);
void fastent_fips_run_blocks_sse41(const u8 *, u64, fastent_fips_report *);
void fastent_fips_run_blocks_avx2(const u8 *, u64, fastent_fips_report *);
void fastent_fips_run_blocks_avx512(const u8 *, u64, fastent_fips_report *);
void fastent_fips_run_blocks_avx512_bitalg(
    const u8 *, u64, fastent_fips_report *);
void fastent_fips_run_blocks_avx512_vpopcntdq(
    const u8 *, u64, fastent_fips_report *);
void fastent_fips_run_blocks_neon(const u8 *, u64, fastent_fips_report *);
void fastent_fips_run_blocks_sve2(const u8 *, u64, fastent_fips_report *);
void fastent_fips_run_blocks_wasm128(const u8 *, u64, fastent_fips_report *);

#endif
