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

#if defined(__i386__) || defined(__x86_64__) \
 || defined(_M_IX86) || defined(_M_X64)

#if defined(_MSC_VER)

/*  MSVC: CPUID / XGETBV via intrinsics (cl targets Pentium+, so
    CPUID always exists).  */
#include <intrin.h>
static inline int has_cpuid(void) { return 1; }
static inline void cpuid_(
    u32 leaf, u32 subleaf, u32 * a, u32 * b, u32 * c, u32 * d) {
  int r[4];
  __cpuidex(r, (int) leaf, (int) subleaf);
  *a = (u32) r[0];  *b = (u32) r[1];
  *c = (u32) r[2];  *d = (u32) r[3];
}
static inline u64 xgetbv0_(void) { return (u64) _xgetbv(0); }

#else

/*  EFLAGS bit 21 toggles only on Pentium-class+; stuck on 386/486
    means no CPUID and we bail to scalar.  */
#if defined(__x86_64__)
static inline int has_cpuid(void) { return 1; }
#else
static int has_cpuid(void) {
  u32 x, y;
  __asm__ volatile (
    "pushfl\n\t"
    "pushfl\n\t"
    "popl %0\n\t"
    "movl %0, %1\n\t"
    "xorl $0x200000, %1\n\t"
    "pushl %1\n\t"
    "popfl\n\t"
    "pushfl\n\t"
    "popl %1\n\t"
    "popfl\n\t"
    : "=&r"(x), "=&r"(y));
  return ((x ^ y) & 0x200000u) != 0;
}
#endif

/*  i386 -fPIC reserves EBX as the PIC base, so xchg it via a scratch
    instead of clobbering it.  */
static inline void cpuid_(
    u32 leaf, u32 subleaf, u32 * a, u32 * b, u32 * c, u32 * d) {
#if defined(__i386__) && defined(__PIC__)
  __asm__ volatile (
    "xchgl %%ebx, %1\n\t"
    "cpuid\n\t"
    "xchgl %%ebx, %1"
    : "=a"(*a), "=r"(*b), "=c"(*c), "=d"(*d)
    : "0"(leaf), "2"(subleaf));
#else
  __asm__ volatile ("cpuid"
    : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
    : "0"(leaf), "2"(subleaf));
#endif
}

/*  Caller must gate on OSXSAVE (CPUID.1.ECX[27]) before invoking.
    Encoded as raw bytes so we don't need -mxsave.  */
static inline u64 xgetbv0_(void) {
  unsigned int lo, hi;
  __asm__ volatile (".byte 0x0f, 0x01, 0xd0"
    : "=a"(lo), "=d"(hi) : "c"(0));
  return ((u64) hi << 32) | (u64) lo;
}

#endif  /*  _MSC_VER  */

static fastent_cpu_features cache_;
static i32                  cache_done_ = 0;

static fastent_cpu_features probe_(void) {
  fastent_cpu_features f;
  u32 a = 0, b = 0, c = 0, d = 0;
  u32 max_leaf = 0;
  i32 osxsave = 0, avx_os_ok = 0, avx512_os_ok = 0;

  f.ssse3 = f.sse41 = f.sse42 = 0;
  f.avx = f.avx2 = 0;
  f.avx512f = f.avx512bw = f.avx512bitalg = f.avx512vnni = 0;
  f.avx512vpopcntdq = 0;
  f.neon = f.sve2 = f.wasm128 = 0;

  if (!has_cpuid()) return f;
  cpuid_(0, 0, &a, &b, &c, &d);
  max_leaf = a;
  if (max_leaf < 1) return f;

  cpuid_(1, 0, &a, &b, &c, &d);
  f.ssse3 = !!(c & (1u <<  9));
  f.sse41 = !!(c & (1u << 19));
  f.sse42 = !!(c & (1u << 20));
  osxsave = !!(c & (1u << 27));
  /*  AVX needs both CPUID.1.ECX[28] and OS-enabled XCR0 state, else
      the CPU #UDs on AVX instructions.  */
  if (osxsave && (c & (1u << 28))) {
    u64 xcr0 = xgetbv0_();
    if ((xcr0 & 0x6u) == 0x6u) {
      f.avx = 1;
      avx_os_ok = 1;
      /*  Bits 5,6,7 cover opmask, ZMM_Hi256, Hi16_ZMM for AVX-512.  */
      if ((xcr0 & 0xE0u) == 0xE0u) avx512_os_ok = 1;
    }
  }

  if (max_leaf < 7) return f;
  cpuid_(7, 0, &a, &b, &c, &d);
  if (avx_os_ok && (b & (1u <<  5))) f.avx2 = 1;
  if (avx512_os_ok) {
    if (b & (1u << 16)) f.avx512f      = 1;
    if (b & (1u << 30)) f.avx512bw     = 1;
    /*  CPUID leaf 7,0 ECX[11]: AVX-512 VNNI (VPDPBUSD).  */
    if (c & (1u << 11)) f.avx512vnni   = 1;
    if (c & (1u << 12)) f.avx512bitalg = 1;
    /*  CPUID leaf 7,0 ECX[14]: VPOPCNTDQ (wide VPOPCNTQ).  */
    if (c & (1u << 14)) f.avx512vpopcntdq = 1;
  }
  return f;
}

const fastent_cpu_features * fastent_cpu_get(void) {
  if (!cache_done_) {
    cache_ = probe_();
    cache_done_ = 1;
  }
  return &cache_;
}

#endif  /*  __i386__ || __x86_64__  */
