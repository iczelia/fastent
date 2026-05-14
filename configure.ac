m4_define([fastent_version_major], [1])
m4_define([fastent_version_minor], [0])
m4_define([fastent_version], [fastent_version_major.fastent_version_minor])

AC_PREREQ([2.69])
AC_INIT([fastent],[fastent_version],[https://github.com/iczelia/fastent])
AC_CONFIG_HEADERS([config.h])
AC_COPYRIGHT([Copyright (C) 2026 Kamila Szewczyk])
AC_LANG([C])
AC_CONFIG_MACRO_DIR([m4])

AC_CANONICAL_TARGET
AM_INIT_AUTOMAKE([-Wall foreign color-tests subdir-objects])

AC_PROG_INSTALL
AC_PROG_MAKE_SET
AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS
AM_PROG_AR
AC_PROG_RANLIB

AC_DEFINE([FASTENT_MAJOR], [fastent_version_major], [Major version number])
AC_DEFINE([FASTENT_MINOR], [fastent_version_minor], [Minor version number])

AC_C_RESTRICT
AC_C_INLINE
AC_CHECK_HEADERS([sys/mman.h sys/stat.h fcntl.h unistd.h immintrin.h])
AC_CHECK_FUNCS([mmap madvise posix_fadvise posix_memalign])

# libm: log10 / sqrt / pow / fabs / lgamma all live here.
AC_SEARCH_LIBS([log10], [m])

# pthread (optional). When disabled, the binary is single-threaded and
# never calls into the threadpool module.
AC_ARG_ENABLE([threads],
  [AS_HELP_STRING([--disable-threads],[build a single-threaded fastent (no pthread dep)])],
  [enable_threads=$enableval], [enable_threads=yes])
if test "x$enable_threads" = "xyes"; then
  AX_PTHREAD([
    AC_DEFINE([FASTENT_HAVE_PTHREAD], [1], [Have pthreads])
    LIBS="$PTHREAD_LIBS $LIBS"
    CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
    CC="$PTHREAD_CC"
    fastent_have_pthread=yes
  ], [fastent_have_pthread=no])
else
  fastent_have_pthread=no
fi
AM_CONDITIONAL([FASTENT_THREADS], [test "x$fastent_have_pthread" = "xyes"])

# SIMD probes. Save user CFLAGS, try each -m... in turn, restore, set
# AC_SUBST'd flags and HAVE_* defines + AM_CONDITIONAL.

save_CFLAGS="$CFLAGS"

CFLAGS="$save_CFLAGS -mssse3"
AC_MSG_CHECKING([for SSSE3 intrinsics with $CC])
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[
    #include <tmmintrin.h>
  ]], [[
    __m128i a = _mm_set1_epi8(1);
    __m128i b = _mm_shuffle_epi8(a, a);
    return _mm_extract_epi16(b, 0);
  ]])],
  [AC_MSG_RESULT([yes])
   AC_DEFINE([HAVE_SSSE3], [1], [Define if SSSE3 intrinsics are available])
   SSSE3_CFLAGS="-mssse3"],
  [AC_MSG_RESULT([no])
   SSSE3_CFLAGS=""])

# SSE4.1 (built with -msse4.2: analyze-impl.h's 128-bit MC residual
# uses _mm_cmpgt_epi64, which is SSE4.2. SSE4.2 is a strict superset
# present on Nehalem+; Penryn is the only chip with 4.1 but not 4.2).
CFLAGS="$save_CFLAGS -msse4.2"
AC_MSG_CHECKING([for SSE4.1/4.2 intrinsics with $CC])
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[
    #include <smmintrin.h>
    #include <nmmintrin.h>
  ]], [[
    __m128i a = _mm_set1_epi32(1);
    __m128i b = _mm_insert_epi64(a, 0, 1);
    __m128i c = _mm_cmpgt_epi64(a, b);
    return _mm_extract_epi32(c, 0);
  ]])],
  [AC_MSG_RESULT([yes])
   AC_DEFINE([HAVE_SSE41], [1], [Define if SSE4.1 intrinsics are available])
   SSE41_CFLAGS="-msse4.2"],
  [AC_MSG_RESULT([no])
   SSE41_CFLAGS=""])

# AVX2 (also exercises VPMADDUBSW, the SCC kernel)
CFLAGS="$save_CFLAGS -mavx2"
AC_MSG_CHECKING([for AVX2 intrinsics with $CC])
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[
    #include <immintrin.h>
  ]], [[
    __m256i a = _mm256_set1_epi8(1);
    __m256i b = _mm256_maddubs_epi16(a, a);
    __m256i c = _mm256_madd_epi16(b, _mm256_set1_epi16(1));
    return _mm256_extract_epi32(c, 0);
  ]])],
  [AC_MSG_RESULT([yes])
   AC_DEFINE([HAVE_AVX2], [1], [Define if AVX2 intrinsics are available])
   AVX2_CFLAGS="-mavx2"],
  [AC_MSG_RESULT([no])
   AVX2_CFLAGS=""])

# AVX-512 (F + BW + CD + DQ + VL + VPOPCNTDQ + BITALG)
# AVX512F+BW+VL: byte/word 512-bit ops + 128/256-bit EVEX encodings.
# AVX512CD: VPCONFLICTD/Q for the parallel-scatter histogram.
# AVX512VPOPCNTDQ + AVX512BITALG: VPOPCNTB byte-popcount for the
# bit-mode body (replaces the PSHUFB-LUT nibble popcount).
# All four extensions ship together on Zen 4, Intel Ice Lake-SP, and
# later, so a single compound probe is fine.
AVX512_CC_FLAGS="-mavx512f -mavx512bw -mavx512cd -mavx512dq -mavx512vl -mavx512vpopcntdq -mavx512bitalg"
CFLAGS="$save_CFLAGS $AVX512_CC_FLAGS"
AC_MSG_CHECKING([for AVX-512F+BW+CD+VPOPCNTDQ+BITALG intrinsics with $CC])
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[
    #include <immintrin.h>
  ]], [[
    __m512i a = _mm512_set1_epi8(1);
    __m512i b = _mm512_conflict_epi32(a);
    __m512i c = _mm512_popcnt_epi8(a);
    __m512i d = _mm512_madd_epi16(a, b);
    return (int) _mm512_reduce_add_epi32(_mm512_add_epi32(c, d));
  ]])],
  [AC_MSG_RESULT([yes])
   AC_DEFINE([HAVE_AVX512], [1], [Define if AVX-512 intrinsics are available])
   AVX512_CFLAGS="$AVX512_CC_FLAGS"],
  [AC_MSG_RESULT([no])
   AVX512_CFLAGS=""])

CFLAGS="$save_CFLAGS"

AC_SUBST([SSSE3_CFLAGS])
AC_SUBST([SSE41_CFLAGS])
AC_SUBST([AVX2_CFLAGS])
AC_SUBST([AVX512_CFLAGS])
AM_CONDITIONAL([HAVE_SSSE3],  [test "x$SSSE3_CFLAGS"  != "x"])
AM_CONDITIONAL([HAVE_SSE41],  [test "x$SSE41_CFLAGS"  != "x"])
AM_CONDITIONAL([HAVE_AVX2],   [test "x$AVX2_CFLAGS"   != "x"])
AM_CONDITIONAL([HAVE_AVX512], [test "x$AVX512_CFLAGS" != "x"])

# Optional optimisations.
AC_ARG_ENABLE([native],
  [AS_HELP_STRING([--enable-native],[-march=native -mtune=native])],
  [enable_native=$enableval], [enable_native=no])
if test "x$enable_native" = "xyes"; then
  AX_APPEND_COMPILE_FLAGS([-march=native -mtune=native])
fi

AC_ARG_ENABLE([lto],
  [AS_HELP_STRING([--enable-lto],[link-time optimisation])],
  [enable_lto=$enableval], [enable_lto=no])
if test "x$enable_lto" = "xyes"; then
  AX_APPEND_COMPILE_FLAGS([-flto])
  AX_APPEND_LINK_FLAGS([-flto])
fi

AX_APPEND_COMPILE_FLAGS([-Wall -Wextra -Wno-unused-parameter -Wno-unused-function])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
