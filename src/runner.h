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

/*  Feed the histogram, -ee tests and active grid estimators in one stream
    pass.  Estimator pointers may be NULL; st is required.  */
void fastent_run_stream_lz_tee(
    fastent_chunk_state * st, fastent_lz_acc * lz, fastent_bm_acc * bm,
    fastent_maurer_acc * ma, fastent_mrank_acc * mr,
    fastent_perment_acc * pe, const fastent_options * o,
    fastent_analyze_fn fn_byte, fastent_analyze_fn fn_bits,
    fastent_analyze_fn fn_byte_fold, fastent_analyze_fn fn_bits_fold,
    fastent_source * src);

typedef struct {
  char * path;
  fastent_result result;
} fastent_recursive_row;

/*  Walk `root` recursively, analysing every regular file.  */
int fastent_run_recursive(
    const char * root, const fastent_options * o, fastent_analyze_fn fn_byte,
    fastent_analyze_fn fn_bits, fastent_analyze_fn fn_byte_fold,
    fastent_analyze_fn fn_bits_fold, fastent_recursive_row ** out_rows,
    sz * out_n);

/*  The grid drivers require a mapped source and an initialised accumulator;
    streams use the tee above.  Each driver scores absolute 4 MiB blocks.
    LZ77F (-e).  */
void fastent_run_lz(
    fastent_lz_acc * acc, const fastent_options * o, fastent_source * src);

/*  Linear complexity (-eee).  The 64-byte windows divide the grid
    exactly, so no window crosses a block boundary.  */
void fastent_run_bm(
    fastent_bm_acc * acc, const fastent_options * o, fastent_source * src);

/*  Maurer universal test (-eee).  */
void fastent_run_maurer(
    fastent_maurer_acc * acc, const fastent_options * o,
    fastent_source * src);

/*  Binary matrix rank (-eee).  The 128-byte matrices divide the grid
    exactly, so no matrix crosses a block boundary.  */
void fastent_run_mrank(
    fastent_mrank_acc * acc, const fastent_options * o,
    fastent_source * src);

/*  Bandt-Pompe permutation entropy (-e).  */
void fastent_run_perment(
    fastent_perment_acc * acc, const fastent_options * o,
    fastent_source * src);

void fastent_rows_free(fastent_recursive_row * rows, sz n);

void fastent_rows_sort(
    fastent_recursive_row * rows, sz n, const fastent_options * o);

#endif
