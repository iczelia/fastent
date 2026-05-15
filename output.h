/*  fastent: output renderers.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_OUTPUT_H
#define FASTENT_OUTPUT_H

#include "common.h"
#include "analyze.h"
#include "fastent-options.h"
#include "runner.h"

void fastent_print_default(const fastent_result * r, const fastent_options * o);
void fastent_print_terse  (const fastent_result * r, const fastent_options * o);
void fastent_print_json   (const fastent_result * r, const fastent_options * o);
void fastent_print_histogram(const fastent_result * r,
                             const fastent_options * o);

void fastent_print_recursive_csv (const fastent_recursive_row * rows, sz n,
                                  const fastent_options * o);
void fastent_print_recursive_json(const fastent_recursive_row * rows, sz n,
                                  const fastent_options * o);

#endif
