/*  fastent: entropy and randomness tester for byte streams.

    Copyright (C) 2023-2026 Kamila Szewczyk.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.  */

#ifndef FASTENT_COMMON_H
#define FASTENT_COMMON_H

/*  Feature test macros: enable POSIX 2008 + BSD extensions so that
    posix_memalign / madvise / posix_fadvise / isascii are visible.
    Must be defined before any system header is included.  */
#ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
#endif
#ifndef _XOPEN_SOURCE
  #define _XOPEN_SOURCE 700
#endif

#ifdef __DJGPP__
  #undef __STRICT_ANSI__
#endif

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef size_t   sz;
typedef double   f64;

#if defined(__GNUC__) || defined(__clang__)
  #define LIKELY(x)   __builtin_expect(!!(x), 1)
  #define UNLIKELY(x) __builtin_expect(!!(x), 0)
  #define FASTENT_PREFETCH(p) __builtin_prefetch((p))
  #define FASTENT_NOINLINE __attribute__((noinline))
  #define FASTENT_HOT __attribute__((hot))
  #define FASTENT_ALWAYS_INLINE __attribute__((always_inline)) inline
  #define FASTENT_PREFETCH_R(p) __builtin_prefetch((p), 0, 1)
  #define FASTENT_ALIGN(n) __attribute__((aligned(n)))
#elif defined(_MSC_VER)
  #define LIKELY(x)   (x)
  #define UNLIKELY(x) (x)
  #define FASTENT_PREFETCH(p) ((void) 0)
  #define FASTENT_NOINLINE __declspec(noinline)
  #define FASTENT_HOT
  #define FASTENT_ALWAYS_INLINE __forceinline
  /*  _mm_prefetch / _MM_HINT_* come from <immintrin.h>, already
      included by the SIMD TUs where FASTENT_PREFETCH_R is used.  */
  #define FASTENT_PREFETCH_R(p) _mm_prefetch((const char *)(p), _MM_HINT_T1)
  #define FASTENT_ALIGN(n) __declspec(align(n))
#else
  #define LIKELY(x)   (x)
  #define UNLIKELY(x) (x)
  #define FASTENT_PREFETCH(p) ((void) 0)
  #define FASTENT_NOINLINE
  #define FASTENT_HOT
  #define FASTENT_ALWAYS_INLINE inline
  #define FASTENT_PREFETCH_R(p) ((void) 0)
  #define FASTENT_ALIGN(n)
#endif

#ifdef restrict
  #define FASTENT_RESTRICT restrict
#elif defined(__GNUC__) || defined(__clang__)
  #define FASTENT_RESTRICT __restrict__
#else
  #define FASTENT_RESTRICT
#endif

#ifndef MIN
  #define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
  #define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/*  popcount: the SWAR fallback exists because on x86 without a
    POPCNT-enabling flag (no __POPCNT__) __builtin_popcount goes
    out-of-line to libgcc __popcountdi2, far costlier than inline
    bit-twiddling, and that hits the hot bit-mode walker in
    analyze-scalar.c (no -m flags) and analyze-ssse3.c (-mssse3
    only).  TCC/non-GCC compilers also take SWAR (__TINYC__ also
    defines __GNUC__, hence the explicit exclusion).  Other targets
    (e.g. AArch64 CNT) trust the builtin.  */
#if defined(__GNUC__) && !defined(__TINYC__) \
    && !((defined(__i386__) || defined(__x86_64__)) && !defined(__POPCNT__))
  #define FASTENT_POPCOUNT32(x) ((unsigned) __builtin_popcount((unsigned)(x)))
#else
  static inline unsigned fastent_popcount32_(unsigned x) {
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0f0f0f0fu;
    return (x * 0x01010101u) >> 24;
  }
  #define FASTENT_POPCOUNT32(x) fastent_popcount32_((unsigned)(x))
#endif

/*  64-bit popcount: same builtin/SWAR rationale as the 32-bit form.  */
#if defined(__GNUC__) && !defined(__TINYC__) \
    && !((defined(__i386__) || defined(__x86_64__)) && !defined(__POPCNT__))
  #define FASTENT_POPCOUNT64(x) ((u32) __builtin_popcountll((u64)(x)))
#else
  static inline u32 fastent_popcount64_(u64 x) {
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return (u32) ((x * 0x0101010101010101ull) >> 56);
  }
  #define FASTENT_POPCOUNT64(x) fastent_popcount64_((u64)(x))
#endif

/*  64-bit count-trailing-zeros, non-zero input only.  */
#if defined(__GNUC__) && !defined(__TINYC__)
  #define FASTENT_CTZ64(x) ((u32) __builtin_ctzll((u64)(x)))
#else
  static inline u32 fastent_ctz64_(u64 x) {
    return FASTENT_POPCOUNT64((x & (0ull - x)) - 1ull);
  }
  #define FASTENT_CTZ64(x) fastent_ctz64_((u64)(x))
#endif

/*  Reverse the 8 bits of a byte (no standard builtin).  */
static inline u32 fastent_bitrev8_(u32 x) {
  u64 v = (u64)(x & 0xFFu);
  v = ((v * 0x0802ull & 0x22110ull) | (v * 0x8020ull & 0x88440ull))
      * 0x10101ull >> 16;
  return (u32)(v & 0xFFu);
}

/*  Tight int-counter for-loop macros, C89-compliant.  */
#define Fi(n, body)        { int i; for (i = 0; i < (n); i++) { body; } }
#define Fj(n, body)        { int j; for (j = 0; j < (n); j++) { body; } }
#define Fk(n, body)        { int k; for (k = 0; k < (n); k++) { body; } }
#define Fi0(n, s, body)    { int i; for (i = (s); i < (n); i++) { body; } }
#define Fj0(n, s, body)    { int j; for (j = (s); j < (n); j++) { body; } }
#define Fk0(n, s, body)    { int k; for (k = (s); k < (n); k++) { body; } }

#ifndef FASTENT_MAJOR
  #define FASTENT_MAJOR 1
#endif
#ifndef FASTENT_MINOR
  #define FASTENT_MINOR 2
#endif

#define FASTENT_STR_(x) #x
#define FASTENT_STR(x)  FASTENT_STR_(x)
#define FASTENT_VERSION_STRING \
  FASTENT_STR(FASTENT_MAJOR) "." FASTENT_STR(FASTENT_MINOR)

#endif
