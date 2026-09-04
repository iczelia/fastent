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

#ifndef FASTENT_OUTPUT_H
#define FASTENT_OUTPUT_H

#include "common.h"
#include "analyze.h"
#include "fastent-options.h"
#include "runner.h"

void fastent_print_default(
    const fastent_result * r, const fastent_options * o);
void fastent_print_terse(const fastent_result * r, const fastent_options * o);
void fastent_print_json(const fastent_result * r, const fastent_options * o);
void fastent_print_histogram(
    const fastent_result * r, const fastent_options * o);
void fastent_print_annotated(
    const fastent_result * r, const fastent_options * o);

void fastent_print_recursive_csv(
    const fastent_recursive_row * rows, sz n, const fastent_options * o);
void fastent_print_recursive_json(
    const fastent_recursive_row * rows, sz n, const fastent_options * o);

#endif
