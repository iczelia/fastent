/*  fastent: V_* macros for x86 SIMD variants (SSSE3, SSE4.1, AVX2,
    AVX-512 F+BW, AVX-512 F+BW+BITALG).  Selected by analyze-impl.h
    when FASTENT_VARIANT_{SSSE3,SSE41,AVX2,AVX512} is defined.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_ANALYZE_VEC_X86_H
#define FASTENT_ANALYZE_VEC_X86_H

#include <immintrin.h>

#if defined(FASTENT_VARIANT_AVX512)
  #if defined(FASTENT_AVX512_HAVE_BITALG)
    #define FASTENT_VAR_SUFFIX _avx512_bitalg
  #else
    #define FASTENT_VAR_SUFFIX _avx512
  #endif
  #define FASTENT_SIMD_VEC   __m512i
  #define FASTENT_SIMD_VLEN  64
  #define V_SET1_EPI8(x)       _mm512_set1_epi8((char)(x))
  #define V_SETZERO()          _mm512_setzero_si512()
  #define V_LOAD(p)            _mm512_loadu_si512((const void *)(p))
  #define V_STORE(p, v)        _mm512_storeu_si512((void *)(p), (v))
  #define V_AND(a, b)          _mm512_and_si512((a), (b))
  #define V_OR(a, b)           _mm512_or_si512((a), (b))
  #define V_ANDNOT(a, b)       _mm512_andnot_si512((a), (b))
  #define V_ADD_EPI8(a, b)     _mm512_add_epi8((a), (b))
  #define V_ADD_EPI64(a, b)    _mm512_add_epi64((a), (b))
  #define V_SUBS_EPU8(a, b)    _mm512_subs_epu8((a), (b))
  /*  CMPEQ on AVX-512 returns a mask register, not a vector; we
      synthesize a 0/-1 vector by masking a -1 splat so the fold helper
      can stay variant-agnostic.  */
  #define V_CMPEQ_EPI8(a, b)   _mm512_maskz_set1_epi8( \
                                  _mm512_cmpeq_epi8_mask((a), (b)), -1)
  #define V_SRLI_EPI16(a, n)   _mm512_srli_epi16((a), (n))
  #define V_SHUFFLE_EPI8(t, i) _mm512_shuffle_epi8((t), (i))
  #define V_SAD_EPU8(a, b)     _mm512_sad_epu8((a), (b))
#elif defined(FASTENT_VARIANT_AVX2)
  #define FASTENT_VAR_SUFFIX _avx2
  #define FASTENT_SIMD_VEC   __m256i
  #define FASTENT_SIMD_VLEN  32
  #define V_SET1_EPI8(x)       _mm256_set1_epi8((char)(x))
  #define V_SETZERO()          _mm256_setzero_si256()
  #define V_LOAD(p)            _mm256_loadu_si256((const __m256i *)(p))
  #define V_STORE(p, v)        _mm256_storeu_si256((__m256i *)(p), (v))
  #define V_AND(a, b)          _mm256_and_si256((a), (b))
  #define V_OR(a, b)           _mm256_or_si256((a), (b))
  #define V_ANDNOT(a, b)       _mm256_andnot_si256((a), (b))
  #define V_ADD_EPI8(a, b)     _mm256_add_epi8((a), (b))
  #define V_ADD_EPI64(a, b)    _mm256_add_epi64((a), (b))
  #define V_SUBS_EPU8(a, b)    _mm256_subs_epu8((a), (b))
  #define V_CMPEQ_EPI8(a, b)   _mm256_cmpeq_epi8((a), (b))
  #define V_SRLI_EPI16(a, n)   _mm256_srli_epi16((a), (n))
  #define V_SHUFFLE_EPI8(t, i) _mm256_shuffle_epi8((t), (i))
  #define V_SAD_EPU8(a, b)     _mm256_sad_epu8((a), (b))
#elif defined(FASTENT_VARIANT_SSE41)
  #define FASTENT_VAR_SUFFIX _sse41
  #define FASTENT_SIMD_VEC   __m128i
  #define FASTENT_SIMD_VLEN  16
  #define V_SET1_EPI8(x)       _mm_set1_epi8((char)(x))
  #define V_SETZERO()          _mm_setzero_si128()
  #define V_LOAD(p)            _mm_loadu_si128((const __m128i *)(p))
  #define V_STORE(p, v)        _mm_storeu_si128((__m128i *)(p), (v))
  #define V_AND(a, b)          _mm_and_si128((a), (b))
  #define V_OR(a, b)           _mm_or_si128((a), (b))
  #define V_ANDNOT(a, b)       _mm_andnot_si128((a), (b))
  #define V_ADD_EPI8(a, b)     _mm_add_epi8((a), (b))
  #define V_ADD_EPI64(a, b)    _mm_add_epi64((a), (b))
  #define V_SUBS_EPU8(a, b)    _mm_subs_epu8((a), (b))
  #define V_CMPEQ_EPI8(a, b)   _mm_cmpeq_epi8((a), (b))
  #define V_SRLI_EPI16(a, n)   _mm_srli_epi16((a), (n))
  #define V_SHUFFLE_EPI8(t, i) _mm_shuffle_epi8((t), (i))
  #define V_SAD_EPU8(a, b)     _mm_sad_epu8((a), (b))
#elif defined(FASTENT_VARIANT_SSSE3)
  #define FASTENT_VAR_SUFFIX _ssse3
  #define FASTENT_SIMD_VEC   __m128i
  #define FASTENT_SIMD_VLEN  16
  #define V_SET1_EPI8(x)       _mm_set1_epi8((char)(x))
  #define V_SETZERO()          _mm_setzero_si128()
  #define V_LOAD(p)            _mm_loadu_si128((const __m128i *)(p))
  #define V_STORE(p, v)        _mm_storeu_si128((__m128i *)(p), (v))
  #define V_AND(a, b)          _mm_and_si128((a), (b))
  #define V_OR(a, b)           _mm_or_si128((a), (b))
  #define V_ANDNOT(a, b)       _mm_andnot_si128((a), (b))
  #define V_ADD_EPI8(a, b)     _mm_add_epi8((a), (b))
  #define V_ADD_EPI64(a, b)    _mm_add_epi64((a), (b))
  #define V_SUBS_EPU8(a, b)    _mm_subs_epu8((a), (b))
  #define V_CMPEQ_EPI8(a, b)   _mm_cmpeq_epi8((a), (b))
  #define V_SRLI_EPI16(a, n)   _mm_srli_epi16((a), (n))
  #define V_SHUFFLE_EPI8(t, i) _mm_shuffle_epi8((t), (i))
  #define V_SAD_EPU8(a, b)     _mm_sad_epu8((a), (b))
#endif

#endif
