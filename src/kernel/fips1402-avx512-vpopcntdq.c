/*  fastent: AVX-512 + VPOPCNTDQ FIPS 140-2 variant.
      VPOPCNTDQ => _mm512_popcnt_epi64 (VPOPCNTQ) for the monobit and
      run-start population counts, replacing the PSHUFB-LUT path.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#define FASTENT_VARIANT_AVX512
#define FASTENT_AVX512_HAVE_VPOPCNTDQ
#define FASTENT_HAVE_SIMD
#include "fips1402-impl.h"
