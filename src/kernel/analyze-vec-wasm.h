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

#ifndef FASTENT_ANALYZE_VEC_WASM_H
#define FASTENT_ANALYZE_VEC_WASM_H

#include <wasm_simd128.h>
#include "common.h"

#define FASTENT_VAR_SUFFIX _wasm128
#define FASTENT_SIMD_VEC   v128_t
#define FASTENT_SIMD_VLEN  16
#define V_SET1_EPI8(x)       wasm_i8x16_splat((int8_t)(x))
#define V_SETZERO()          wasm_i64x2_splat(0)
#define V_LOAD(p)            wasm_v128_load((const void *)(p))
#define V_STORE(p, v)        wasm_v128_store((void *)(p), (v))
#define V_AND(a, b)          wasm_v128_and((a), (b))
#define V_OR(a, b)           wasm_v128_or((a), (b))
/*  wasm_v128_andnot(x, y) = x & ~y; x86 V_ANDNOT(a, b) = ~a & b.  */
#define V_ANDNOT(a, b)       wasm_v128_andnot((b), (a))
#define V_ADD_EPI8(a, b)     wasm_i8x16_add((a), (b))
#define V_ADD_EPI64(a, b)    wasm_i64x2_add((a), (b))
#define V_SUBS_EPU8(a, b)    wasm_u8x16_sub_sat((a), (b))
#define V_CMPEQ_EPI8(a, b)   wasm_i8x16_eq((a), (b))
#define V_SRLI_EPI16(a, n)   wasm_u16x8_shr((a), (n))
#define V_SHUFFLE_EPI8(t, i) wasm_i8x16_swizzle((t), (i))

/*  PSADBW has no direct WASM equivalent; emulate via the same
    extadd-pairwise ladder NEON uses.  */
static __attribute__((always_inline)) inline v128_t
fastent_wasm_sad_zero(v128_t a) {
  v128_t s16 = wasm_i16x8_extadd_pairwise_u8x16(a);
  v128_t s32 = wasm_i32x4_extadd_pairwise_u16x8(s16);
  /*  4 u32 -> 2 u64 via two extmul-by-1 widenings and an add.  */
  v128_t ones = wasm_i32x4_splat(1);
  v128_t s64_lo = wasm_u64x2_extmul_low_u32x4(s32, ones);
  v128_t s64_hi = wasm_u64x2_extmul_high_u32x4(s32, ones);
  return wasm_i64x2_add(s64_lo, s64_hi);
}
#define V_SAD_EPU8(a, b)     (((void)(b)), fastent_wasm_sad_zero((a)))

#endif
