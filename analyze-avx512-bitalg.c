/*  fastent: AVX-512 + BITALG analyse variant (VPOPCNTB byte popcount).

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#define FASTENT_VARIANT_AVX512
#define FASTENT_AVX512_HAVE_BITALG
#define FASTENT_HAVE_SIMD
#include "analyze-impl.h"
