/*  fastent: AVX-512 + BITALG + VNNI analyse variant.
      BITALG => VPOPCNTB byte popcount (bit-mode body).
      VNNI   => VPDPBUSD fused mul+horiz-sum (byte-mode SCC).

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#define FASTENT_VARIANT_AVX512
#define FASTENT_AVX512_HAVE_BITALG
#define FASTENT_AVX512_HAVE_VNNI
#define FASTENT_HAVE_SIMD
#include "analyze-impl.h"
