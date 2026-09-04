/*  Copyright (C) 2023-2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.  */

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
