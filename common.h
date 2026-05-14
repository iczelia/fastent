/*  fastent: entropy and randomness tester for byte streams.

    Copyright (C) 2026 Kamila Szewczyk.

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
#else
  #define LIKELY(x)   (x)
  #define UNLIKELY(x) (x)
  #define FASTENT_PREFETCH(p) ((void) 0)
  #define FASTENT_NOINLINE
  #define FASTENT_HOT
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
  #define FASTENT_MINOR 0
#endif

#define FASTENT_STR_(x) #x
#define FASTENT_STR(x)  FASTENT_STR_(x)
#define FASTENT_VERSION_STRING FASTENT_STR(FASTENT_MAJOR) "." FASTENT_STR(FASTENT_MINOR)

#endif
