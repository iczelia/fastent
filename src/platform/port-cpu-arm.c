/*  fastent: AArch64 / ARMv7-A CPU feature probe.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "port-cpu.h"

#if defined(__aarch64__) || defined(__arm__)

static fastent_cpu_features cache_;
static int                  cache_done_ = 0;

#if defined(__aarch64__)
  /*  NEON is mandatory per the AArch64 procedure call standard.  */
  static inline int probe_neon_(void) { return 1; }
#elif defined(__arm__) && defined(HAVE_SYS_AUXV_H) && defined(HAVE_GETAUXVAL)
  #include <sys/auxv.h>
  #ifndef HWCAP_NEON
    #define HWCAP_NEON (1u << 12)
  #endif
  static int probe_neon_(void) {
    return (getauxval(AT_HWCAP) & HWCAP_NEON) != 0;
  }
#else
  static inline int probe_neon_(void) { return 1; }
#endif

#if defined(__aarch64__) && defined(__linux__) \
    && defined(HAVE_SYS_AUXV_H) && defined(HAVE_GETAUXVAL)
  #include <sys/auxv.h>
  #ifndef HWCAP2_SVE2
    #define HWCAP2_SVE2 (1u << 1)
  #endif
  static int probe_sve2_(void) {
    return (getauxval(AT_HWCAP2) & HWCAP2_SVE2) != 0;
  }
#elif defined(__aarch64__) && defined(__FreeBSD__)
  #include <sys/auxv.h>
  #ifndef HWCAP2_SVE2
    #define HWCAP2_SVE2 (1u << 1)
  #endif
  static int probe_sve2_(void) {
    unsigned long hwcap2 = 0;
    if (elf_aux_info(AT_HWCAP2, &hwcap2, sizeof(hwcap2)) != 0) return 0;
    return (hwcap2 & HWCAP2_SVE2) != 0;
  }
#elif defined(__aarch64__) && defined(__APPLE__)
  #include <sys/sysctl.h>
  #include <stddef.h>
  static int probe_sve2_(void) {
    int    v   = 0;
    sz     len = sizeof(v);
    if (sysctlbyname("hw.optional.arm.FEAT_SVE2", &v, &len, NULL, 0) != 0)
      return 0;
    return v != 0;
  }
#else
  static inline int probe_sve2_(void) { return 0; }
#endif

const fastent_cpu_features * fastent_cpu_get(void) {
  if (!cache_done_) {
    fastent_cpu_features f;
    f.ssse3 = f.sse41 = f.sse42 = 0;
    f.avx = f.avx2 = 0;
    f.avx512f = f.avx512bw = f.avx512bitalg = 0;
    f.wasm128 = 0;
    f.neon = probe_neon_() ? 1 : 0;
    f.sve2 = probe_sve2_() ? 1 : 0;
    cache_ = f;
    cache_done_ = 1;
  }
  return &cache_;
}

#endif  /*  __aarch64__ || __arm__  */
