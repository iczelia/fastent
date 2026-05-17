/*  fastent: AVX-512 + BITALG FIPS 140-2 variant.
      BITALG => VPOPCNTB byte popcount for monobit and run-start counts.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#define FASTENT_VARIANT_AVX512
#define FASTENT_AVX512_HAVE_BITALG
#define FASTENT_HAVE_SIMD
#include "fips1402-impl.h"
