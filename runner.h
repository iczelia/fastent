/*  fastent: analysis driver.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_RUNNER_H
#define FASTENT_RUNNER_H

#include "common.h"
#include "analyze.h"
#include "fastent-options.h"
#include "port-io.h"

void fastent_run_mmap(fastent_chunk_state * st, const fastent_options * o,
                      fastent_analyze_fn fn_byte,
                      fastent_analyze_fn fn_bits,
                      fastent_analyze_fn fn_byte_fold,
                      fastent_analyze_fn fn_bits_fold,
                      const u8 * data, u64 size);

void fastent_run_stream(fastent_chunk_state * st, const fastent_options * o,
                        fastent_analyze_fn fn_byte,
                        fastent_analyze_fn fn_bits,
                        fastent_analyze_fn fn_byte_fold,
                        fastent_analyze_fn fn_bits_fold,
                        fastent_source * src);

typedef struct {
  char *          path;
  fastent_result  result;
} fastent_recursive_row;

/*  Walk `root` recursively, analysing every regular file.  Allocates
    *out_rows on success; caller frees via fastent_rows_free.  Returns
    0 on success, -1 on walk error (errno set).  Analysis failures on
    individual files emit a stderr warning and skip.  */
int  fastent_run_recursive(const char * root, const fastent_options * o,
                           fastent_analyze_fn fn_byte,
                           fastent_analyze_fn fn_bits,
                           fastent_analyze_fn fn_byte_fold,
                           fastent_analyze_fn fn_bits_fold,
                           fastent_recursive_row ** out_rows,
                           sz * out_n);

void fastent_rows_free(fastent_recursive_row * rows, sz n);

void fastent_rows_sort(fastent_recursive_row * rows, sz n,
                       const fastent_options * o);

#endif
