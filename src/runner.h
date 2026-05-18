/*  fastent: analysis driver.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_RUNNER_H
#define FASTENT_RUNNER_H

#include "common.h"
#include "analyze.h"
#include "fastent-options.h"
#include "lzest.h"
#include "port-io.h"

void fastent_run_mmap(
    fastent_chunk_state * st, const fastent_options * o,
    fastent_analyze_fn fn_byte, fastent_analyze_fn fn_bits,
    fastent_analyze_fn fn_byte_fold, fastent_analyze_fn fn_bits_fold,
    const u8 * data, u64 size);

void fastent_run_stream(
    fastent_chunk_state * st, const fastent_options * o,
    fastent_analyze_fn fn_byte, fastent_analyze_fn fn_bits,
    fastent_analyze_fn fn_byte_fold, fastent_analyze_fn fn_bits_fold,
    fastent_source * src);

/*  -eee non-mmap tee: one bounded pass feeding both the order-0/-ee
    analyzer (st) and the LZ77F acc (acc, init'd).  O(chunk + grid
    block), serial on -j, bit-identical to -j1/mmap.  */
void fastent_run_stream_lz_tee(
    fastent_chunk_state * st, fastent_lz_acc * acc,
    const fastent_options * o, fastent_analyze_fn fn_byte,
    fastent_analyze_fn fn_bits, fastent_analyze_fn fn_byte_fold,
    fastent_analyze_fn fn_bits_fold, fastent_source * src);

typedef struct {
  char *          path;
  fastent_result  result;
} fastent_recursive_row;

/*  Walk `root` recursively, analysing every regular file.  Allocates
    *out_rows on success; caller frees via fastent_rows_free.  Returns
    0 on success, -1 on walk error (errno set).  Analysis failures on
    individual files emit a stderr warning and skip.  */
int fastent_run_recursive(
    const char * root, const fastent_options * o, fastent_analyze_fn fn_byte,
    fastent_analyze_fn fn_bits, fastent_analyze_fn fn_byte_fold,
    fastent_analyze_fn fn_bits_fold, fastent_recursive_row ** out_rows,
    sz * out_n);

/*  LZ77F (-eee) driver.  Runs the absolute 4 MiB-grid LZ77F parse
    over `src` and writes the merged accumulator into *acc (already
    init'd).  Result is bit-identical for any -j / driver / host.  */
void fastent_run_lz(
    fastent_lz_acc * acc, const fastent_options * o, fastent_source * src);

void fastent_rows_free(fastent_recursive_row * rows, sz n);

void fastent_rows_sort(
    fastent_recursive_row * rows, sz n, const fastent_options * o);

#endif
