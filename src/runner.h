/*  fastent: analysis driver.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_RUNNER_H
#define FASTENT_RUNNER_H

#include "common.h"
#include "analyze.h"
#include "fastent-options.h"
#include "lzest.h"
#include "bm.h"
#include "maurer.h"
#include "mrank.h"
#include "perment.h"
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

/*  -eee non-mmap tee: one bounded pass feeding the order-0/-ee
    analyzer (st), the LZ77F acc (lz), the linear-complexity acc (bm),
    the Maurer acc (ma), the binary matrix-rank acc (mr) and the
    permutation-entropy acc (pe); each may be NULL.  O(chunk + grid
    block), serial on -j, bit-identical to -j1/mmap.  */
void fastent_run_stream_lz_tee(
    fastent_chunk_state * st, fastent_lz_acc * lz, fastent_bm_acc * bm,
    fastent_maurer_acc * ma, fastent_mrank_acc * mr,
    fastent_perment_acc * pe, const fastent_options * o,
    fastent_analyze_fn fn_byte, fastent_analyze_fn fn_bits,
    fastent_analyze_fn fn_byte_fold, fastent_analyze_fn fn_bits_fold,
    fastent_source * src);

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

/*  LZ77F (-e) driver.  Runs the absolute 4 MiB-grid LZ77F parse
    over `src` and writes the merged accumulator into *acc (already
    init'd).  Result is bit-identical for any -j / driver / host.  */
void fastent_run_lz(
    fastent_lz_acc * acc, const fastent_options * o, fastent_source * src);

/*  Linear-complexity (-eee) driver.  Same absolute 4 MiB-grid scheme
    as fastent_run_lz; 64 divides the grid so windows never straddle,
    giving zero-drift bit-identical output for any -j / driver / host. */
void fastent_run_bm(
    fastent_bm_acc * acc, const fastent_options * o, fastent_source * src);

/*  Maurer universal test (-eee, alongside LZ77F / BM) driver.  Same
    absolute 4 MiB-grid scheme; per-block partials reduce in absolute
    block order so the f64 statistic is bit-identical for any -j /
    driver / host.  */
void fastent_run_maurer(
    fastent_maurer_acc * acc, const fastent_options * o,
    fastent_source * src);

/*  Binary matrix-rank (-eee) driver.  Same absolute 4 MiB-grid scheme
    as fastent_run_lz; 128 divides the grid so matrices never straddle,
    giving bit-identical integer output for any -j / driver / host.  */
void fastent_run_mrank(
    fastent_mrank_acc * acc, const fastent_options * o,
    fastent_source * src);

/*  Bandt-Pompe permutation entropy (-e) driver.  Same absolute
    4 MiB-grid scheme; the (m - 1) windows that span a grid boundary
    are dropped (bounded drift, verdict-neutral); bit-identical for
    any -j / driver / host.  */
void fastent_run_perment(
    fastent_perment_acc * acc, const fastent_options * o,
    fastent_source * src);

void fastent_rows_free(fastent_recursive_row * rows, sz n);

void fastent_rows_sort(
    fastent_recursive_row * rows, sz n, const fastent_options * o);

#endif
