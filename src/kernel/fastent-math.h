/*  fastent: scalar math shims.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_MATH_H
#define FASTENT_MATH_H

#include "common.h"

/*  p * log2(1/p), faithfully rounded.  +0 at the domain endpoints,
    qNaN outside [0, 1].  No libm log.  */
f64 fastent_entropy_term(f64 p);

/*  Faithfully-rounded log2(x), libm-free, bit-identical across hosts
    (round-to-nearest assumed).  All four share one DD core and the
    same 128-entry table, differing only in how much a proven input
    domain lets them skip.

    fastent_log2        general x > 0.  qNaN for x <= 0 / NaN; keeps
                         the x-near-1 split for accuracy on (0.5, 1).
    fastent_log2_ge1    PRECONDITION x >= 1.  Drops the near-1 split
                         and denormal path (both unreachable).
    fastent_log2_ratio  log2(a / b), a >= b >= 1.  Exact argument when
                         a, b < 2^53; else the f64 divide is the only
                         rounding.  a == b yields exact 0.
    fastent_log2_fast   ~1e-9, plain double, no DD / denormal / sign
                         handling.  NOT faithful, NOT bit-identical:
                         never on the deterministic stats / display
                         path.  Caller guarantees finite normal x > 0.  */
f64 fastent_log2(f64 x);
f64 fastent_log2_ge1(f64 x);
f64 fastent_log2_ratio(u64 a, u64 b);
f64 fastent_log2_fast(f64 x);

#endif
