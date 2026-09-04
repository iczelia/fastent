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

#ifndef FASTENT_MATH_H
#define FASTENT_MATH_H

#include "common.h"

/*  p * log2(1/p), faithfully rounded.  +0 at the domain endpoints,
    qNaN outside [0, 1].  No libm log.  */
f64 fastent_entropy_term(f64 p);

/*  Faithfully-rounded libm-free log2, bit-identical across hosts (RN), shared
    DD core + 128-entry table.  */
f64 fastent_log2(f64 x);
f64 fastent_log2_ge1(f64 x);
f64 fastent_log2_ratio(u64 a, u64 b);
f64 fastent_log2_fast(f64 x);

#endif
