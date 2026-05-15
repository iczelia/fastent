/*  fastent: V_* macros for the NEON (AArch64 + ARMv7-A) variant.
    AArch64-only intrinsics (vqtbl1q_u8, vaddvq_u64/s64) are emulated
    on ARMv7-A via vtbl2_u8 + lane-extract.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_ANALYZE_VEC_NEON_H
#define FASTENT_ANALYZE_VEC_NEON_H

/*  Refuse big-endian NEON: vget_low_/vget_high_ return swapped halves
    on BE so the SCC and tbl1q paths would silently miscompute.  The
    configure probe already gates HAVE_NEON on BE; this is a backstop
    for direct -DFASTENT_VARIANT_NEON builds.  */
#if defined(__ARM_BIG_ENDIAN) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#error "fastent NEON variant is not supported on big-endian ARM"
#endif

#include <arm_neon.h>
#include "common.h"

#define FASTENT_VAR_SUFFIX _neon
#define FASTENT_SIMD_VEC   uint8x16_t
#define FASTENT_SIMD_VLEN  16
#define V_SET1_EPI8(x)       vdupq_n_u8((uint8_t)(x))
#define V_SETZERO()          vdupq_n_u8(0)
#define V_LOAD(p)            vld1q_u8((const uint8_t *)(p))
#define V_STORE(p, v)        vst1q_u8((uint8_t *)(p), (v))
#define V_AND(a, b)          vandq_u8((a), (b))
#define V_OR(a, b)           vorrq_u8((a), (b))
/*  x86 V_ANDNOT(a, b) = (~a) & b; NEON's BIC is b & (~a), so the
    argument order flips.  */
#define V_ANDNOT(a, b)       vbicq_u8((b), (a))
#define V_ADD_EPI8(a, b)     vaddq_u8((a), (b))
#define V_ADD_EPI64(a, b)    vreinterpretq_u8_u64(             \
                               vaddq_u64(vreinterpretq_u64_u8(a),\
                                         vreinterpretq_u64_u8(b)))
#define V_SUBS_EPU8(a, b)    vqsubq_u8((a), (b))
#define V_CMPEQ_EPI8(a, b)   vceqq_u8((a), (b))
#define V_SRLI_EPI16(a, n)   vreinterpretq_u8_u16(             \
                               vshrq_n_u16(vreinterpretq_u16_u8(a), (n)))

static __attribute__((always_inline)) inline uint8x16_t
fastent_neon_tbl1q_u8(uint8x16_t table, uint8x16_t idx) {
#ifdef __aarch64__
  return vqtbl1q_u8(table, idx);
#else
  uint8x8x2_t t;
  t.val[0] = vget_low_u8(table);
  t.val[1] = vget_high_u8(table);
  uint8x8_t lo = vtbl2_u8(t, vget_low_u8(idx));
  uint8x8_t hi = vtbl2_u8(t, vget_high_u8(idx));
  return vcombine_u8(lo, hi);
#endif
}
#define V_SHUFFLE_EPI8(t, i) fastent_neon_tbl1q_u8((t), (i))

static __attribute__((always_inline)) inline uint64_t
fastent_neon_addvq_u64(uint64x2_t v) {
#ifdef __aarch64__
  return vaddvq_u64(v);
#else
  return vgetq_lane_u64(v, 0) + vgetq_lane_u64(v, 1);
#endif
}
static __attribute__((always_inline)) inline int64_t
fastent_neon_addvq_s64(int64x2_t v) {
#ifdef __aarch64__
  return vaddvq_s64(v);
#else
  return vgetq_lane_s64(v, 0) + vgetq_lane_s64(v, 1);
#endif
}

/*  PSADBW emulation: the only call site passes zero for `b`, so we
    drop it.  Saturating-pairwise-add ladder u8 -> u16 -> u32 -> u64.  */
static __attribute__((always_inline)) inline uint8x16_t
fastent_neon_sad_zero(uint8x16_t a) {
  uint16x8_t s16 = vpaddlq_u8(a);
  uint32x4_t s32 = vpaddlq_u16(s16);
  uint64x2_t s64 = vpaddlq_u32(s32);
  return vreinterpretq_u8_u64(s64);
}
#define V_SAD_EPU8(a, b)     (((void)(b)), fastent_neon_sad_zero((a)))

#endif
