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
#include "bm.h"

#if defined(__ARM_BIG_ENDIAN) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#error "fastent NEON BM variant is not supported on big-endian ARM"
#endif

#include <arm_neon.h>
#include <string.h>

#ifdef __aarch64__

/*  Two 512-bit windows run in the 2 u64 lanes of one uint64x2_t; the eight
    polynomial words are eight vectors.  */

#define WB FASTENT_BM_W64   /*  8 polynomial words  */

static inline void bm_polx_(uint64x2_t v[WB]) {
  u32 w;
  for (w = WB; w-- > 1; )
    v[w] = vorrq_u64(vshrq_n_u64(v[w], 1), vshlq_n_u64(v[w - 1], 63));
  v[0] = vshrq_n_u64(v[0], 1);
}

/*  Per-lane parity of a u64 vector, broadcast to a full-lane mask
    (all-ones where parity is 1).  Fold 64->1 by xor-halving, then
    expand the low bit.  */
static inline uint64x2_t bm_parmask_(uint64x2_t a) {
  a = veorq_u64(a, vshrq_n_u64(a, 32));
  a = veorq_u64(a, vshrq_n_u64(a, 16));
  a = veorq_u64(a, vshrq_n_u64(a, 8));
  a = veorq_u64(a, vshrq_n_u64(a, 4));
  a = veorq_u64(a, vshrq_n_u64(a, 2));
  a = veorq_u64(a, vshrq_n_u64(a, 1));
  a = vandq_u64(a, vdupq_n_u64(1));
  return vsubq_u64(vdupq_n_u64(0), a);
}

/*  Pack one 64-byte window into 8 u64, MSB-first (byte i high to low,
    bit 7 of byte 0 is poly coefficient 0).  */
static inline void bm_pack_(const u8 * src, u64 s[WB]) {
  i32 i;
  Fi(WB, s[i] = 0);
  for (i = 0; i < FASTENT_BM_WB; i++)
    s[i >> 3] |= (u64) src[i] << (56u - 8u * (i & 7u));
}

/*  Process exactly 2 full M-bit windows; write their L into Lout.  */
static void bm_batch2_(const u8 * src, u32 * Lout) {
  u64 sp[2][WB];
  i32 i;
  u32 N, w;
  Fi(2, bm_pack_(src + (u32) i * FASTENT_BM_WB, sp[i]));

  uint64x2_t cv[WB], bsv[WB], rv[WB], tv[WB];
  for (w = 0; w < WB; w++) {
    cv[w] = vdupq_n_u64(0);
    bsv[w] = vdupq_n_u64(0);
    rv[w] = vdupq_n_u64(0);
  }
  uint64x2_t one63 = vdupq_n_u64((u64) 1 << 63);
  cv[0] = one63;
  bsv[0] = one63;
  bm_polx_(bsv);                 /*  bs_ at start of N=0 is x^1 * 1  */

  i64 L[2] = { 0, 0 };

  for (N = 0; N < FASTENT_BM_M; N++) {
    bm_polx_(rv);
    u32 wi = N >> 6, sh = 63u - (N & 63u);
    u64 b0 = (sp[0][wi] >> sh) & 1ull, b1 = (sp[1][wi] >> sh) & 1ull;
    u64 nb[2] = { b0 << 63, b1 << 63 };
    rv[0] = vorrq_u64(rv[0], vld1q_u64(nb));

    uint64x2_t acc = vdupq_n_u64(0);
    for (w = 0; w < WB; w++)
      acc = veorq_u64(acc, vandq_u64(cv[w], rv[w]));
    uint64x2_t dmask = bm_parmask_(acc);

    for (w = 0; w < WB; w++) tv[w] = cv[w];
    for (w = 0; w < WB; w++)
      cv[w] = veorq_u64(cv[w], vandq_u64(bsv[w], dmask));

    /*  Length change where d set and 2L <= N.  2L<=N is N-2L>=0;
        with L,N small the i64 lane compare is exact.  */
    int64x2_t Lv = vld1q_s64(L);
    int64x2_t twoL = vaddq_s64(Lv, Lv);
    int64x2_t Nv = vdupq_n_s64((i64) N);
    /*  2L <= N: a true le mask, intersected with the discrepancy.  */
    uint64x2_t le = vcleq_s64(twoL, Nv);
    uint64x2_t chg = vandq_u64(dmask, le);
    for (w = 0; w < WB; w++)
      bsv[w] = vbslq_u64(chg, tv[w], bsv[w]);
    /*  L <- N + 1 - L where chg.  */
    int64x2_t newL = vsubq_s64(vdupq_n_s64((i64) N + 1), Lv);
    Lv = vreinterpretq_s64_u64(
        vbslq_u64(chg, vreinterpretq_u64_s64(newL),
                  vreinterpretq_u64_s64(Lv)));
    vst1q_s64(L, Lv);

    bm_polx_(bsv);
  }
  Fi(2, Lout[i] = (u32) L[i]);
}

/*  Score nfull full windows; NEON in groups of 2.  Returns the count
    actually scored (the largest multiple of 2 <= nfull); the caller
    handles the 0..1 tail with the scalar reference.  */
sz fastent_bm_windows_neon(const u8 * src, sz nfull, u32 * Lout) {
  sz g = nfull & ~(sz) 1;
  sz i;
  for (i = 0; i < g; i += 2)
    bm_batch2_(src + i * FASTENT_BM_WB, Lout + i);
  return g;
}

#else

/*  ARMv7-A NEON: no 64-bit SIMD compare; defer to the scalar path.  */
sz fastent_bm_windows_neon(const u8 * src, sz nfull, u32 * Lout) {
  (void) src;  (void) Lout;
  return 0;
}

#endif
