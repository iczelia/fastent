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

#include "common.h"
#include "port-cpu.h"

#if !defined(__i386__) && !defined(__x86_64__) \
    && !defined(_M_IX86) && !defined(_M_X64)   \
    && !defined(__aarch64__) && !defined(__arm__)

static fastent_cpu_features cache_;
static int                  cache_done_ = 0;

const fastent_cpu_features * fastent_cpu_get(void) {
  if (!cache_done_) {
    fastent_cpu_features f;
    f.ssse3 = f.sse41 = f.sse42 = 0;
    f.avx = f.avx2 = 0;
    f.avx512f = f.avx512bw = f.avx512bitalg = 0;
    f.avx512vpopcntdq = 0;
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
