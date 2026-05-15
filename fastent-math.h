/*  fastent: scalar math shims.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_MATH_H
#define FASTENT_MATH_H

#include "common.h"

/*  p * log2(1/p), faithfully rounded.  +0 at the domain endpoints,
    qNaN outside [0, 1].  No libm log.  */
f64 fastent_entropy_term(f64 p);

#endif
