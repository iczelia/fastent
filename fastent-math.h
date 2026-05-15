/*  fastent: scalar math shims.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_MATH_H
#define FASTENT_MATH_H

#include "common.h"

/*  p * log2(1/p), correctly rounded to nearest double for every input
    short of a handful of worst-case table-maker's-dilemma cases at
    log's 2^-65-of-half-ULP frontier; faithful (<= 1 ULP) on those.
    Pure FP internally - no libm log / log2 / log10.  Computed in
    double-double via a 128-entry table + degree-7 Taylor polynomial.

    Domain:
      p == 0      -> +0   (limit x log(1/x) -> 0 as x -> 0+)
      p == 1      -> +0   (exact)
      p in (0, 1) -> main path
      otherwise   -> qNaN  (including p < 0, p > 1, NaN, Inf)
    Replaces analyze.c's per-bin libm log10 call.  */
f64 fastent_entropy_term(f64 p);

#endif
