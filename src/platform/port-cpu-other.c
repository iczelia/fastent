/*  fastent: CPU feature probe for non-x86, non-ARM targets.
    WebAssembly hosts also land here; SIMD128 is module-load-gated, so
    a build that linked -msimd128 has the feature unconditionally.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "port-cpu.h"

#if !defined(__i386__) && !defined(__x86_64__) \
    && !defined(_M_IX86) && !defined(_M_X64) \
    && !defined(__aarch64__) && !defined(__arm__)

static fastent_cpu_features cache_;
static int                  cache_done_ = 0;

const fastent_cpu_features * fastent_cpu_get(void) {
  if (!cache_done_) {
    fastent_cpu_features f;
    f.ssse3 = f.sse41 = f.sse42 = 0;
    f.avx = f.avx2 = 0;
    f.avx512f = f.avx512bw = f.avx512bitalg = 0;
    f.neon = f.sve2 = 0;
#if defined(HAVE_WASM128)
    f.wasm128 = 1;
#else
    f.wasm128 = 0;
#endif
    cache_ = f;
    cache_done_ = 1;
  }
  return &cache_;
}

#endif
