/*  fastent: FIPS 140-2 RNG power-up self-tests.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_FIPS1402_H
#define FASTENT_FIPS1402_H

#include "common.h"

#include <stdio.h>

/*  Aggregate over every full 20000-bit (2500-byte) block.  Each
    block is tested independently per FIPS 140-2 4.9.1; trailing
    bytes that do not fill a block are not tested.  */
typedef struct {
  u64 blocks;        /*  full 2500-byte blocks tested        */
  u64 leftover;      /*  trailing untested bytes             */
  u64 monobit_fail;  /*  blocks failing each sub-test        */
  u64 poker_fail;
  u64 runs_fail;
  u64 longrun_fail;
  u64 blocks_pass;   /*  blocks passing all four sub-tests    */
} fastent_fips_report;

/*  Run the four FIPS 140-2 RNG tests over buf[0..len).  threads <= 1
    runs serially; otherwise the (independent) blocks are split
    across the worker pool.  Bit-identical regardless of thread
    count.  */
void fastent_fips140_run(const u8 * buf, sz len, int threads,
                         fastent_fips_report * out);

/*  Human-readable report.  Returns 1 if the input passes (>= 1 block
    and every block passed all four tests), else 0.  */
int  fastent_fips140_print(const fastent_fips_report * r, FILE * fp);

#endif
