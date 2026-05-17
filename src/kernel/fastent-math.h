/*  fastent: scalar math shims.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_MATH_H
#define FASTENT_MATH_H

#include "common.h"

/*  p * log2(1/p), faithfully rounded.  +0 at the domain endpoints,
    qNaN outside [0, 1].  No libm log.  */
f64 fastent_entropy_term(f64 p);

/*  Faithfully-rounded libm-free log2, bit-identical across hosts (RN),
    shared DD core + 128-entry table.  _ge1 needs x>=1 (no near-1/
    denormal); _ratio log2(a/b) a>=b>=1 exact for a,b<2^53; _fast
    ~1e-9 double, NOT faithful, never on the deterministic path.  */
f64 fastent_log2(f64 x);
f64 fastent_log2_ge1(f64 x);
f64 fastent_log2_ratio(u64 a, u64 b);
f64 fastent_log2_fast(f64 x);

#endif
