m4_define([fastent_version_major], [1])
m4_define([fastent_version_minor], [2])
m4_define([fastent_version], [fastent_version_major.fastent_version_minor])

AC_PREREQ([2.69])
AC_INIT([fastent],[fastent_version],[https://github.com/iczelia/fastent])
AC_CONFIG_HEADERS([config.h])
AC_COPYRIGHT([Copyright (C) 2023-2026 Kamila Szewczyk])
AC_LANG([C])
AC_CONFIG_MACRO_DIR([m4])

AC_CANONICAL_TARGET
AM_INIT_AUTOMAKE([-Wall color-tests subdir-objects \
                  dist-bzip2 dist-xz dist-zip dist-bzip3])

AC_PROG_INSTALL
AC_PROG_MAKE_SET
AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS
# DJGPP ar trips AM_PROG_AR's interface probe; short-circuit it.
case $host_os in
  msdosdjgpp*|djgpp*) am_cv_ar_interface=ar ;;
esac
AM_PROG_AR
AC_PROG_RANLIB

AC_DEFINE([FASTENT_MAJOR], [fastent_version_major], [Major version number])
AC_DEFINE([FASTENT_MINOR], [fastent_version_minor], [Minor version number])

AC_C_RESTRICT
AC_C_INLINE
AC_CHECK_HEADERS([sys/mman.h sys/stat.h fcntl.h unistd.h immintrin.h sys/auxv.h linux/io_uring.h])

# io_uring: Linux 5.1+ async I/O.  We use raw syscalls (no liburing dep),
# but we need both <linux/io_uring.h> (for the SQE/CQE layouts) AND a
# __NR_io_uring_setup that the kernel headers expose.  At runtime
# io_uring_setup may still ENOSYS on older kernels; we fall back to the
# read(2) stream path in that case unless the user explicitly asked for
# --io=uring.
if test "x$ac_cv_header_linux_io_uring_h" = "xyes"; then
  AC_MSG_CHECKING([for __NR_io_uring_setup])
  AC_COMPILE_IFELSE(
    [AC_LANG_PROGRAM([[
      #include <sys/syscall.h>
      #include <linux/io_uring.h>
    ]], [[
      int n = __NR_io_uring_setup + __NR_io_uring_enter;
      return n != 0;
    ]])],
    [AC_MSG_RESULT([yes])
     AC_DEFINE([HAVE_IO_URING], [1], [Define if io_uring is available])],
    [AC_MSG_RESULT([no])])
fi

# WebAssembly hosts: emscripten's libc resolves mmap / madvise /
# posix_fadvise through JS-side `env::__syscall_*` imports that the
# wasm runtime can't supply in STANDALONE_WASM mode (module
# instantiation rejects unresolved imports even if the call site
# never fires).  Force-disable detection so the source falls back to
# the plain read(2) path on those targets.
case $host_os in
  *emscripten*|*wasi*)
    ac_cv_func_mmap=no
    ac_cv_func_madvise=no
    ac_cv_func_posix_fadvise=no
    ;;
esac
AC_CHECK_FUNCS([mmap madvise posix_fadvise posix_memalign aligned_alloc getauxval pledge])

# libm: log10 / sqrt / pow / fabs / lgamma all live here.
# DJGPP's libc.a has stub log10/sqrt that fool the probe; force -lm.
case "$host_os" in
  msdosdjgpp*|djgpp*) LIBS="-lm $LIBS" ;;
  *)                  AC_SEARCH_LIBS([log10], [m]) ;;
esac

# MS-DOS / DJGPP host (i586-pc-msdosdjgpp).
case "$host_os" in
  msdosdjgpp*|djgpp*) fastent_is_dos=yes ;;
  *)                  fastent_is_dos=no  ;;
esac
if test "x$fastent_is_dos" = "xyes"; then
  AC_DEFINE([FASTENT_DOS], [1], [MS-DOS / DJGPP target])
  AC_CHECK_PROGS([EXE2COFF], [${host_alias}-exe2coff ${host}-exe2coff exe2coff])
  if test -z "$EXE2COFF"; then
    AC_MSG_ERROR([exe2coff not found; ships with DJGPP binutils under <prefix>/i586-pc-msdosdjgpp/bin])
  fi
  AC_ARG_VAR([CWSDSTUB], [Path to CWSDSTUB.EXE (from csdpmi7b.zip on delorie.com)])
  if test -z "$CWSDSTUB"; then
    AC_MSG_CHECKING([for CWSDSTUB.EXE])
    for d in /usr/share/csdpmi /usr/local/share/csdpmi \
             "$HOME/csdpmi" "$HOME/csdpmi/bin" "$HOME/djgpp/bin" \
             "`dirname $EXE2COFF`"; do
      for f in CWSDSTUB.EXE cwsdstub.exe; do
        if test -f "$d/$f"; then CWSDSTUB="$d/$f"; break 2; fi
      done
    done
    if test -z "$CWSDSTUB"; then
      AC_MSG_RESULT([not found])
      AC_MSG_ERROR([CWSDSTUB.EXE not found; fetch csdpmi7b.zip from delorie.com and pass CWSDSTUB=...])
    fi
    AC_MSG_RESULT([$CWSDSTUB])
  fi
fi
AM_CONDITIONAL([FASTENT_DOS], [test "x$fastent_is_dos" = "xyes"])

AC_ARG_ENABLE([threads],
  [AS_HELP_STRING([--disable-threads],[build a single-threaded fastent])],
  [enable_threads=$enableval], [enable_threads=yes])
if test "x$fastent_is_dos" = "xyes"; then
  enable_threads=no
fi
# Thread backend selection is deferred to after Windows target detection.

# Windows host: cross-compile via mingw-w64 (any toolchain that
# defines _WIN32) needs the fastent-win32.c TU for Unicode-path /
# Unicode-console support.  Cygwin is excluded: its POSIX layer maps
# open() through cygwin1.dll which already handles UTF-8 paths via
# the cygwin DLL's internal narrowing, so our wrapper would be
# redundant there.
case "$host_os" in
  mingw*|msys*) fastent_is_windows=yes ;;
  *)            fastent_is_windows=no  ;;
esac
AM_CONDITIONAL([FASTENT_IS_WINDOWS], [test "x$fastent_is_windows" = "xyes"])

# Windows target: vista (wide API, default) or win95 (narrow, kernel32-only).
AC_ARG_WITH([windows-target],
  [AS_HELP_STRING([--with-windows-target=@<:@vista|win95@:>@],
    [target Windows version for native-Windows builds (default: vista)])],
  [fastent_win_target=$withval],
  [fastent_win_target=vista])
case $fastent_win_target in
  vista)
    fastent_win_cppflags="-D_WIN32_WINNT=0x0600 -DWINVER=0x0600"
    fastent_win_ldflags="-Wl,--major-subsystem-version=6,--minor-subsystem-version=0,--major-os-version=6,--minor-os-version=0"
    ;;
  win95)
    fastent_win_cppflags="-D_WIN32_WINNT=0x0400 -DWINVER=0x0400 -DFASTENT_WIN_LEGACY=1"
    fastent_win_ldflags="-Wl,--major-subsystem-version=4,--minor-subsystem-version=0,--major-os-version=4,--minor-os-version=0"
    ;;
  *)
    AC_MSG_ERROR([--with-windows-target must be "vista" or "win95" (got: $fastent_win_target)])
    ;;
esac
FASTENT_WIN_CPPFLAGS=""
FASTENT_WIN_LDFLAGS=""
if test "x$fastent_is_windows" = "xyes"; then
  FASTENT_WIN_CPPFLAGS="$fastent_win_cppflags"
  FASTENT_WIN_LDFLAGS="$fastent_win_ldflags"
  AC_MSG_NOTICE([Windows target: $fastent_win_target])
fi
AC_SUBST([FASTENT_WIN_CPPFLAGS])
AC_SUBST([FASTENT_WIN_LDFLAGS])

# Threads (pthread on POSIX, CreateThread + CONDITION_VARIABLE on
# Win32 Vista+).  When disabled, the threadpool TUs compile to nothing.
fastent_have_threads=no
if test "x$enable_threads" = "xyes"; then
  if test "x$fastent_is_windows" = "xyes"; then
    if test "x$fastent_win_target" = "xwin95"; then
      AC_MSG_NOTICE([threads disabled: Win95 lacks CONDITION_VARIABLE])
    else
      AC_DEFINE([FASTENT_HAVE_THREADS], [1], [Have a thread backend])
      fastent_have_threads=yes
    fi
  else
    AX_PTHREAD([
      AC_DEFINE([FASTENT_HAVE_THREADS], [1], [Have a thread backend])
      AC_DEFINE([FASTENT_HAVE_PTHREAD], [1], [Use pthread backend])
      LIBS="$PTHREAD_LIBS $LIBS"
      CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
      CC="$PTHREAD_CC"
      fastent_have_threads=yes
    ], [])
  fi
fi
AM_CONDITIONAL([FASTENT_THREADS], [test "x$fastent_have_threads" = "xyes"])

# SIMD probes. Save user CFLAGS, try each -m... in turn, restore, set
# AC_SUBST'd flags and HAVE_* defines + AM_CONDITIONAL.  DJGPP (i386
# baseline), Win95 (pre-SSE hardware), and WebAssembly targets bypass
# the probe block: on emscripten/wasi the x86 intrinsics either don't
# compile or get emulated through wasm SIMD128, and we'd rather let the
# scalar variant auto-vectorise under -msimd128.
fastent_skip_simd=no
if test "x$fastent_is_dos" = "xyes"; then
  fastent_skip_simd=yes
fi
if test "x$fastent_win_target" = "xwin95"; then
  fastent_skip_simd=yes
fi
case $host_os in
  *emscripten*|*wasi*) fastent_skip_simd=yes ;;
esac
SSSE3_CFLAGS=""
SSE41_CFLAGS=""
AVX2_CFLAGS=""
AVX512_CFLAGS=""
AVX512_BITALG_CFLAGS=""
NEON_CFLAGS=""
fastent_have_neon=no

save_CFLAGS="$CFLAGS"

if test "x$fastent_skip_simd" != "xyes"; then

# NEON.  Two ABIs to cover:
#   AArch64 (`__aarch64__`): NEON is mandatory, baseline.  No flag.
#   ARMv7-A (`__arm__`):     NEON is optional.  Needs `-mfpu=neon`
#                            and a float-ABI that matches the rest
#                            of the build (see softfp note below).
#
# Probe with no flag first.  If that fails, detect the base float-ABI
# and pick the right -mfpu/-mfloat-abi combination for the NEON TU.
# Note on armel (-mfloat-abi=soft) hosts: forcing only `-mfpu=neon`
# either fails outright or, on lenient toolchains, builds the NEON TU
# with a vector calling convention that differs from the non-NEON
# TUs (vectors in NEON regs vs. on the stack).  That mismatch is
# silent at compile time and breaks at runtime.  The cure is to also
# pass `-mfloat-abi=softfp` to the NEON TU: softfp uses the same
# call boundary as soft (FP/vector args in GPRs+stack) so a soft
# caller can invoke a softfp callee, while softfp still permits NEON
# instructions and intrinsics inside the function body.  On hardfp
# (__ARM_PCS_VFP) we must leave the ABI alone; switching that TU to
# softfp would mismatch the rest of the program.
# The probe exercises a few of the bread-and-butter intrinsics we
# rely on (vcntq_u8 for popcount, vpaddlq_s16 for SCC reduction).
# The AArch64-only ops (vqtbl1q_u8 / vaddvq_*) are emulated in
# analyze-impl.h via shims gated on __aarch64__, so we don't need
# them in the probe.

CFLAGS="$save_CFLAGS"
fastent_have_neon=no
NEON_CFLAGS=""

AC_MSG_CHECKING([for NEON intrinsics with $CC])
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[
    /*  Refuse big-endian NEON: lane numbering is reversed relative to
        memory, so vget_low_xxx / vget_high_xxx return swapped halves
        and the tbl1q emulation reads wrong table bytes.  Bailing out
        lets the build fall back to the scalar variant.  */
    #if defined(__ARM_BIG_ENDIAN) || \
        (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    #error big-endian NEON unsupported
    #endif
    #include <arm_neon.h>
  ]], [[
    uint8x16_t a = vdupq_n_u8(7);
    uint8x16_t b = vcntq_u8(a);
    int8x16_t  s = vreinterpretq_s8_u8(b);
    int16x8_t  w = vmovl_s8(vget_low_s8(s));
    int32x4_t  p = vpaddlq_s16(w);
    return (int) vgetq_lane_s32(p, 0);
  ]])],
  [AC_MSG_RESULT([yes (baseline; AArch64)])
   AC_DEFINE([HAVE_NEON], [1], [Define if NEON intrinsics are available])
   NEON_CFLAGS=""
   fastent_have_neon=yes],
  [AC_MSG_RESULT([no])])

if test "x$fastent_have_neon" = "xno"; then
  # 32-bit ARM only: -mfpu=neon is a no-op (and an error in newer
  # toolchains) on AArch64, x86, etc.  Skip the rest of the probe if
  # we're not actually targeting __arm__.
  CFLAGS="$save_CFLAGS"
  AC_COMPILE_IFELSE(
    [AC_LANG_SOURCE([[
      #ifndef __arm__
      #error not 32-bit ARM
      #endif
      int main(void) { return 0; }
    ]])],
    [fastent_is_arm32=yes],
    [fastent_is_arm32=no])
fi

if test "x$fastent_have_neon" = "xno" && test "x$fastent_is_arm32" = "xyes"; then
  # Detect base float-ABI: __ARM_PCS_VFP is set only for
  # -mfloat-abi=hard.  Soft and softfp leave it unset (and use the
  # same call boundary as each other).
  CFLAGS="$save_CFLAGS"
  AC_MSG_CHECKING([ARM float-ABI of the base CFLAGS])
  AC_COMPILE_IFELSE(
    [AC_LANG_SOURCE([[
      #ifndef __ARM_PCS_VFP
      #error not hard-float
      #endif
      int main(void) { return 0; }
    ]])],
    [fastent_arm_abi=hard],
    [fastent_arm_abi=soft])
  AC_MSG_RESULT([$fastent_arm_abi])

  if test "x$fastent_arm_abi" = "xhard"; then
    fastent_neon_try="-mfpu=neon"
    fastent_neon_label="ARMv7-A hardfp; -mfpu=neon"
  else
    # Force softfp on the NEON TU so its call boundary matches the
    # soft caller; without this the build is silently ABI-broken.
    fastent_neon_try="-mfpu=neon -mfloat-abi=softfp"
    fastent_neon_label="ARMv7-A softfp; -mfpu=neon -mfloat-abi=softfp"
  fi

  CFLAGS="$save_CFLAGS $fastent_neon_try"
  AC_MSG_CHECKING([for NEON intrinsics with $CC $fastent_neon_try])
  AC_COMPILE_IFELSE(
    [AC_LANG_PROGRAM([[
      #if defined(__ARM_BIG_ENDIAN) || \
          (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
      #error big-endian NEON unsupported
      #endif
      #include <arm_neon.h>
    ]], [[
      uint8x16_t a = vdupq_n_u8(7);
      uint8x16_t b = vcntq_u8(a);
      int8x16_t  s = vreinterpretq_s8_u8(b);
      int16x8_t  w = vmovl_s8(vget_low_s8(s));
      int32x4_t  p = vpaddlq_s16(w);
      return (int) vgetq_lane_s32(p, 0);
    ]])],
    [AC_MSG_RESULT([yes ($fastent_neon_label)])
     AC_DEFINE([HAVE_NEON], [1], [Define if NEON intrinsics are available])
     NEON_CFLAGS="$fastent_neon_try"
     fastent_have_neon=yes],
    [AC_MSG_RESULT([no])])
fi

# SVE2.  AArch64-only; ARMv9-A or ARMv8-A+sve2 hardware.  SVE2 strictly
# extends NEON (a CPU that has SVE2 always has NEON), so the SVE2 TU
# only gets built when NEON is also available - the dispatcher prefers
# SVE2 at runtime and falls back to NEON when HWCAP2_SVE2 is unset.
# Big-endian is refused for the same lane-ordering reasons as NEON.
# Probe order: -march=armv8-a+sve2 (GCC baseline), -march=armv9-a
# (some Apple/Linaro toolchains), then -mcpu=generic+sve2 (clang/zig,
# which reject the GCC -march=...+sve2 spelling).
SVE2_CFLAGS=""
fastent_have_sve2=no
if test "x$fastent_skip_simd" != "xyes" && test "x$fastent_have_neon" = "xyes"; then
  CFLAGS="$save_CFLAGS"
  AC_COMPILE_IFELSE(
    [AC_LANG_SOURCE([[
      #ifndef __aarch64__
      #error not AArch64
      #endif
      int main(void) { return 0; }
    ]])],
    [fastent_is_aarch64=yes],
    [fastent_is_aarch64=no])

  if test "x$fastent_is_aarch64" = "xyes"; then
    for fastent_sve2_try in "-march=armv8-a+sve2" "-march=armv9-a" \
                            "-mcpu=generic+sve2"; do
      test "x$SVE2_CFLAGS" != "x" && break
      CFLAGS="$save_CFLAGS $fastent_sve2_try"
      AC_MSG_CHECKING([for SVE2 intrinsics with $CC $fastent_sve2_try])
      AC_COMPILE_IFELSE(
        [AC_LANG_PROGRAM([[
          #if defined(__ARM_BIG_ENDIAN) || \
              (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
          #error big-endian SVE2 unsupported
          #endif
          #include <arm_sve.h>
        ]], [[
          svbool_t pg = svptrue_b8();
          svuint8_t a = svdup_n_u8(7);
          svuint8_t b = svcnt_u8_x(pg, a);
          /*  svtbl needs an SVE2 host; on SVE-only it'd still link but
              -march=armv8-a+sve2 won't accept it.  */
          svuint8_t c = svtbl_u8(b, svdup_n_u8(0));
          return (int) svaddv_u8(pg, c);
        ]])],
        [AC_MSG_RESULT([yes])
         AC_DEFINE([HAVE_SVE2], [1], [Define if SVE2 intrinsics are available])
         SVE2_CFLAGS="$fastent_sve2_try"
         fastent_have_sve2=yes],
        [AC_MSG_RESULT([no])])
    done
  fi
fi
CFLAGS="$save_CFLAGS"

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

# AVX-512 base path (F + BW).
#   AVX512F:  zmm registers, foundation 32/64-bit integer ops, horizontal
#             reduction helpers (_mm512_reduce_add_epi64).
#   AVX512BW: 8/16-bit ops on zmm (set1_epi8, add_epi8, subs_epu8,
#             cmpeq_epi8_mask, shuffle_epi8, madd_epi16, sad_epu8,
#             cvtep[iu]8_epi16, srli_epi16, mullo_epi16, ...).
# Byte popcount in the bit-mode body uses a zmm PSHUFB-LUT (BW only).
# A separate BITALG-augmented archive below replaces those lookups with
# VPOPCNTB when the host advertises it; the F+BW archive is the
# fallback when BITALG is missing.
# Clang 18+ / GCC 14+ need -mevex512 for 512-bit ZMM; probe with, fall back.
AVX512_CC_FLAGS_BASE="-mavx512f -mavx512bw"
AVX512_CFLAGS=""
for fastent_avx512_try in "-mevex512 $AVX512_CC_FLAGS_BASE" "$AVX512_CC_FLAGS_BASE"; do
  test "x$AVX512_CFLAGS" != "x" && break
  CFLAGS="$save_CFLAGS $fastent_avx512_try"
  AC_MSG_CHECKING([for AVX-512F+BW intrinsics with $CC $fastent_avx512_try])
  AC_COMPILE_IFELSE(
    [AC_LANG_PROGRAM([[
      #include <immintrin.h>
    ]], [[
      __m512i a = _mm512_set1_epi8(1);
      __m512i b = _mm512_madd_epi16(a, a);
      __m512i c = _mm512_shuffle_epi8(a, a);
      __m512i d = _mm512_sad_epu8(a, _mm512_setzero_si512());
      return (int) _mm512_reduce_add_epi64(
                     _mm512_add_epi64(_mm512_add_epi64(b, c), d));
    ]])],
    [AC_MSG_RESULT([yes])
     AC_DEFINE([HAVE_AVX512], [1], [Define if AVX-512 F+BW intrinsics are available])
     AVX512_CFLAGS="$fastent_avx512_try"],
    [AC_MSG_RESULT([no])])
done

# AVX-512 BITALG + VNNI augment.
#   BITALG enables _mm512_popcnt_epi8 (VPOPCNTB) for the bit-mode body.
#   VNNI   enables _mm512_dpbusd_epi32   (VPDPBUSD)  for the byte-mode SCC.
# We probe them together because every shipping chip that has BITALG also
# has VNNI (Ice Lake / Tiger Lake / Sapphire Rapids / Zen 4); collapsing
# them into a single tier keeps the variant table small.  The resulting
# TU exports _avx512_bitalg-suffixed symbols the dispatcher prefers on
# hosts with both features.
AVX512_BITALG_CFLAGS=""
if test "x$AVX512_CFLAGS" != "x"; then
  AVX512_BITALG_TRY="$AVX512_CFLAGS -mavx512bitalg -mavx512vnni"
  CFLAGS="$save_CFLAGS $AVX512_BITALG_TRY"
  AC_MSG_CHECKING([for AVX-512 BITALG + VNNI intrinsics with $CC])
  AC_COMPILE_IFELSE(
    [AC_LANG_PROGRAM([[
      #include <immintrin.h>
    ]], [[
      __m512i a = _mm512_set1_epi8(1);
      __m512i c = _mm512_popcnt_epi8(a);
      __m512i d = _mm512_dpbusd_epi32(c, a, a);
      return (int) _mm512_reduce_add_epi64(d);
    ]])],
    [AC_MSG_RESULT([yes])
     AC_DEFINE([HAVE_AVX512_BITALG], [1],
       [Define if AVX-512 BITALG + VNNI intrinsics are available])
     AVX512_BITALG_CFLAGS="$AVX512_BITALG_TRY"],
    [AC_MSG_RESULT([no])])
fi

# AVX-512 VPOPCNTDQ augment.
#   VPOPCNTDQ enables _mm512_popcnt_epi64 (VPOPCNTQ) for the FIPS
#   monobit / run-start population counts.  Independent of BITALG:
#   probe it on top of the F+BW base so a host with VPOPCNTDQ but
#   without BITALG still gets the wide popcount tier.  Mirrors the
#   HAVE_AVX512_BITALG machinery exactly; the resulting TU exports
#   _avx512_vpopcntdq-suffixed FIPS symbols the dispatcher prefers.
AVX512_VPOPCNTDQ_CFLAGS=""
if test "x$AVX512_CFLAGS" != "x"; then
  AVX512_VPOPCNTDQ_TRY="$AVX512_CFLAGS -mavx512vpopcntdq"
  CFLAGS="$save_CFLAGS $AVX512_VPOPCNTDQ_TRY"
  AC_MSG_CHECKING([for AVX-512 VPOPCNTDQ intrinsics with $CC])
  AC_COMPILE_IFELSE(
    [AC_LANG_PROGRAM([[
      #include <immintrin.h>
    ]], [[
      __m512i a = _mm512_set1_epi8(1);
      __m512i c = _mm512_popcnt_epi64(a);
      return (int) _mm512_reduce_add_epi64(c);
    ]])],
    [AC_MSG_RESULT([yes])
     AC_DEFINE([HAVE_AVX512_VPOPCNTDQ], [1],
       [Define if AVX-512 VPOPCNTDQ intrinsics are available])
     AVX512_VPOPCNTDQ_CFLAGS="$AVX512_VPOPCNTDQ_TRY"],
    [AC_MSG_RESULT([no])])
fi

CFLAGS="$save_CFLAGS"

fi  # end !fastent_skip_simd

# WebAssembly SIMD128 (wasm32/wasm64). Independent of the x86/NEON
# probe block; only fires when the host triple says emscripten/wasi.
# The wasm_simd128.h header is gated to those targets, so the probe
# naturally fails elsewhere.  Opt-out via --disable-wasm128 for hosts
# whose runtime doesn't implement the SIMD128 proposal (the scalar
# variant is still selected at dispatch time in that case).
AC_ARG_ENABLE([wasm128],
  [AS_HELP_STRING([--disable-wasm128],[skip the WebAssembly SIMD128 analyse body])],
  [enable_wasm128=$enableval], [enable_wasm128=yes])
WASM128_CFLAGS=""
fastent_have_wasm128=no
if test "x$enable_wasm128" = "xyes"; then
case $host_os in
  *emscripten*|*wasi*)
    CFLAGS="$save_CFLAGS -msimd128"
    AC_MSG_CHECKING([for WebAssembly SIMD128 intrinsics with $CC -msimd128])
    AC_COMPILE_IFELSE(
      [AC_LANG_PROGRAM([[
        #include <wasm_simd128.h>
      ]], [[
        v128_t a = wasm_i8x16_splat(1);
        v128_t b = wasm_i8x16_popcnt(a);
        v128_t c = wasm_i8x16_swizzle(a, b);
        v128_t d = wasm_i32x4_dot_i16x8(wasm_i16x8_splat(1),
                                        wasm_i16x8_splat(1));
        v128_t e = wasm_i16x8_extadd_pairwise_u8x16(a);
        v128_t f = wasm_i32x4_extadd_pairwise_u16x8(e);
        v128_t g = wasm_u64x2_extmul_low_u32x4(f, wasm_i32x4_splat(1));
        return (int) wasm_i8x16_extract_lane(c, 0)
             + (int) wasm_i32x4_extract_lane(d, 0)
             + (int) wasm_i64x2_extract_lane(g, 0);
      ]])],
      [AC_MSG_RESULT([yes])
       AC_DEFINE([HAVE_WASM128], [1], [Define if WebAssembly SIMD128 intrinsics are available])
       WASM128_CFLAGS="-msimd128"
       fastent_have_wasm128=yes],
      [AC_MSG_RESULT([no])])
    CFLAGS="$save_CFLAGS"
    ;;
esac
fi

AC_SUBST([SSSE3_CFLAGS])
AC_SUBST([SSE41_CFLAGS])
AC_SUBST([AVX2_CFLAGS])
AC_SUBST([AVX512_CFLAGS])
AC_SUBST([AVX512_BITALG_CFLAGS])
AC_SUBST([AVX512_VPOPCNTDQ_CFLAGS])
AC_SUBST([NEON_CFLAGS])
AC_SUBST([SVE2_CFLAGS])
AC_SUBST([WASM128_CFLAGS])
AM_CONDITIONAL([HAVE_SSSE3],         [test "x$SSSE3_CFLAGS"         != "x"])
AM_CONDITIONAL([HAVE_SSE41],         [test "x$SSE41_CFLAGS"         != "x"])
AM_CONDITIONAL([HAVE_AVX2],          [test "x$AVX2_CFLAGS"          != "x"])
AM_CONDITIONAL([HAVE_AVX512],        [test "x$AVX512_CFLAGS"        != "x"])
AM_CONDITIONAL([HAVE_AVX512_BITALG], [test "x$AVX512_BITALG_CFLAGS" != "x"])
AM_CONDITIONAL([HAVE_AVX512_VPOPCNTDQ],
               [test "x$AVX512_VPOPCNTDQ_CFLAGS" != "x"])
AM_CONDITIONAL([HAVE_NEON],          [test "x$fastent_have_neon"    = "xyes"])
AM_CONDITIONAL([HAVE_SVE2],          [test "x$fastent_have_sve2"    = "xyes"])
AM_CONDITIONAL([HAVE_WASM128],       [test "x$fastent_have_wasm128" = "xyes"])

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
