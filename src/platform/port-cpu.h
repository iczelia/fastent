/*  fastent: CPU feature detection port.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_PORT_CPU_H
#define FASTENT_PORT_CPU_H

#include "common.h"

typedef struct {
  /*  x86 / x86_64  */
  u32 ssse3        : 1;
  u32 sse41        : 1;
  u32 sse42        : 1;
  u32 avx          : 1;
  u32 avx2         : 1;
  u32 avx512f      : 1;
  u32 avx512bw     : 1;
  u32 avx512bitalg : 1;
  u32 avx512vnni   : 1;
  u32 avx512vpopcntdq : 1;
  /*  AArch64 / ARMv7-A  */
  u32 neon         : 1;
  u32 sve2         : 1;
  /*  WebAssembly  */
  u32 wasm128      : 1;
} fastent_cpu_features;

/*  Lazy-cached probe.  Idempotent; safe to call before threads spawn.  */
const fastent_cpu_features * fastent_cpu_get(void);

#endif
