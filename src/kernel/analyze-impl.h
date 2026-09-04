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

#include "analyze.h"

#if defined(FASTENT_VARIANT_NEON)
#include "analyze-vec-neon.h"
#elif defined(FASTENT_VARIANT_WASM128)
#include "analyze-vec-wasm.h"
#elif defined(FASTENT_VARIANT_AVX512) || defined(FASTENT_VARIANT_AVX2) \
   || defined(FASTENT_VARIANT_SSE41)  || defined(FASTENT_VARIANT_SSSE3)
#include "analyze-vec-x86.h"
#else
#define FASTENT_VAR_SUFFIX _scalar
#endif

#define FASTENT_CAT2(a, b) a##b
#define FASTENT_CAT(a, b)  FASTENT_CAT2(a, b)
#define FASTENT_FN(name)   FASTENT_CAT(name, FASTENT_VAR_SUFFIX)

/*  Stage-buffer pointer launder.  */
#if defined(__GNUC__) || defined(__clang__)
#define FASTENT_STAGE_PTR    const u8 *
#define FASTENT_LAUNDER(sp)  __asm__("" : "+r"(sp) :: "memory")
#else
#define FASTENT_STAGE_PTR    const volatile u8 *
#define FASTENT_LAUNDER(sp)  ((void) 0)
#endif

/*  In-register case fold: ASCII A-Z and Latin-1 0xC0-0xDE (except
    0xD7) lowered, other bytes unchanged. Used by the fused fold +
    analyse path so no staging copy of the input is needed.  */

static inline int FASTENT_FN(fold_is_upper_inline)(u32 c) {
  return ((u32) (c - 'A')   < 26u) ||
         ((u32) (c - 0xC0u) < 31u && c != 0xD7u);
}

static inline u8 FASTENT_FN(fold_byte_inline)(u8 b) {
  u32 c = b;
  if (FASTENT_FN(fold_is_upper_inline)(c)) return (u8) (c + 0x20u);
  return b;
}

#ifdef FASTENT_HAVE_SIMD
static INLINE FASTENT_SIMD_VEC
FASTENT_FN(fold_vec_inline)(FASTENT_SIMD_VEC c) {
  const FASTENT_SIMD_VEC zero    = V_SETZERO();
  const FASTENT_SIMD_VEC v_amin  = V_SET1_EPI8('A');
  const FASTENT_SIMD_VEC v_zmax  = V_SET1_EPI8('Z');
  const FASTENT_SIMD_VEC v_c0min = V_SET1_EPI8((char) 0xC0);
  const FASTENT_SIMD_VEC v_demax = V_SET1_EPI8((char) 0xDE);
  const FASTENT_SIMD_VEC v_d7    = V_SET1_EPI8((char) 0xD7);
  const FASTENT_SIMD_VEC v_0x20  = V_SET1_EPI8(0x20);

  FASTENT_SIMD_VEC s_ge_a  = V_SUBS_EPU8(v_amin, c);
  FASTENT_SIMD_VEC s_le_z  = V_SUBS_EPU8(c, v_zmax);
  FASTENT_SIMD_VEC m_ascii = V_CMPEQ_EPI8(V_OR(s_ge_a, s_le_z), zero);
  FASTENT_SIMD_VEC s_ge_c0 = V_SUBS_EPU8(v_c0min, c);
  FASTENT_SIMD_VEC s_le_de = V_SUBS_EPU8(c, v_demax);
  FASTENT_SIMD_VEC m_lat   = V_CMPEQ_EPI8(V_OR(s_ge_c0, s_le_de), zero);
  FASTENT_SIMD_VEC m_d7    = V_CMPEQ_EPI8(c, v_d7);
  m_lat = V_ANDNOT(m_d7, m_lat);
  FASTENT_SIMD_VEC mask  = V_OR(m_ascii, m_lat);
  FASTENT_SIMD_VEC delta = V_AND(mask, v_0x20);
  return V_ADD_EPI8(c, delta);
}
#endif

/*  HIST_N's landing bank per byte is irrelevant for correctness:
    finalize sums all FASTENT_BANKS banks per value.  */
#if FASTENT_BANKS == 8
#define FASTENT_HIST_DECL                                              \
  u32 * RESTRICT b0 = st->bank[0];                             \
  u32 * RESTRICT b1 = st->bank[1];                             \
  u32 * RESTRICT b2 = st->bank[2];                             \
  u32 * RESTRICT b3 = st->bank[3];                             \
  u32 * RESTRICT b4 = st->bank[4];                             \
  u32 * RESTRICT b5 = st->bank[5];                             \
  u32 * RESTRICT b6 = st->bank[6];                             \
  u32 * RESTRICT b7 = st->bank[7]
#define HIST_N(p, o)                                                   \
  do {                                                                 \
    u64 w_ = fastent_ld64_((const u8 *)(p) + (o));                     \
    b0[ w_        & 0xffu]++; b1[(w_ >>  8) & 0xffu]++;                 \
    b2[(w_ >> 16) & 0xffu]++; b3[(w_ >> 24) & 0xffu]++;                 \
    b4[(w_ >> 32) & 0xffu]++; b5[(w_ >> 40) & 0xffu]++;                 \
    b6[(w_ >> 48) & 0xffu]++; b7[ w_ >> 56        ]++;                  \
  } while (0)
#else
#define FASTENT_HIST_DECL                                              \
  u32 * RESTRICT b0 = st->bank[0];                             \
  u32 * RESTRICT b1 = st->bank[1];                             \
  u32 * RESTRICT b2 = st->bank[2];                             \
  u32 * RESTRICT b3 = st->bank[3 & (FASTENT_BANKS - 1)]
#define HIST_N(p, o)                                                   \
  do {                                                                 \
    u64 w_ = fastent_ld64_((const u8 *)(p) + (o));                     \
    b0[ w_        & 0xffu]++; b1[(w_ >>  8) & 0xffu]++;                 \
    b2[(w_ >> 16) & 0xffu]++; b3[(w_ >> 24) & 0xffu]++;                 \
    b0[(w_ >> 32) & 0xffu]++; b1[(w_ >> 40) & 0xffu]++;                 \
    b2[(w_ >> 48) & 0xffu]++; b3[ w_ >> 56        ]++;                  \
  } while (0)
#endif

/*  Scalar single-byte update: histogram + SCC + MC Pi + first/last.
    Used by head/tail of all variants and the whole scalar body.  */

static inline void FASTENT_FN(consume_byte)(
    fastent_chunk_state * st, u8 b, u32 bank_idx) {
  st->bank[bank_idx & (FASTENT_BANKS - 1)][b]++;
  if (st->have_carry) { st->cross_product += (i64) st->carry_byte * (i64) b; } else {
    st->first_byte = b;
    st->have_first = 1;
    st->have_carry = 1;
  }
  st->carry_byte = b;
  st->last_byte  = b;
  st->total_bytes++;

  /*  MC Pi ring update.  */
  st->mc_buf[st->mc_pos++] = b;
  if (st->mc_pos == 6) {
    u32 x = ((u32) st->mc_buf[0] << 16) | ((u32) st->mc_buf[1] << 8)
          |  (u32) st->mc_buf[2];
    u32 y = ((u32) st->mc_buf[3] << 16) | ((u32) st->mc_buf[4] << 8)
          |  (u32) st->mc_buf[5];
    u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
    st->mc_count++;
    st->mc_inside += (d <= FASTENT_INCIRC);
    st->mc_pos = 0;
  }
}

/*  Scalar histogram + SCC body (fallback and chunk edges). Templated
    on the compile-time `fold` constant; the branch dead-eliminates.  */

static INLINE sz
FASTENT_FN(scalar_body_impl)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len,
    sz start_bank, int fold) {
  sz i;
  for (i = 0; i < len; i++) {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[i]) : buf[i];
    FASTENT_FN(consume_byte)(st, b, start_bank + i);
  }
  return i;
}

static inline sz FASTENT_FN(scalar_body)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len,
    sz start_bank) {
  return FASTENT_FN(scalar_body_impl)(st, buf, len, start_bank, 0);
}

/*  SIMD body: 4-bank histogram + sign-corrected SCC accumulator,
    one variant per ISA below.  */

#if defined(FASTENT_VARIANT_AVX2)

static INLINE sz
FASTENT_FN(simd_body_impl)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len,
    int fold) {
  sz i;
  /*  Stride 64 (two 32-byte vectors). SCC needs byte +1 readable, so
      the body stops at len - 32 (32-byte read-ahead margin).  */
  if (len < 65) return 0;

  const sz body_max = len - 32;
  sz iters = body_max / 64;
  if (iters == 0) return 0;
  const sz body_end = iters * 64;

  /*  SCC window: carry_byte*buf[0] is scalar below; the pmaddubs loop
      covers pairs (buf[i],buf[i+1]) for i in [0..body_end-1].  In fold
      mode scalar seeds use folded values to match the SIMD loop.  */
  u8 b0_user = fold ? FASTENT_FN(fold_byte_inline)(buf[0]) : buf[0];

  if (!st->have_first) { st->first_byte = b0_user; st->have_first = 1; }
  if (st->have_carry)
    st->cross_product += (i64) st->carry_byte * (i64) b0_user;
  st->have_carry = 1;

  const __m256i sign_xor   = _mm256_set1_epi8((char) 0x80);
  const __m256i zero       = _mm256_setzero_si256();
  __m256i scc_acc64        = _mm256_setzero_si256(); /*  4 i64 lanes  */
  __m256i lhs_sad          = _mm256_setzero_si256();

  FASTENT_HIST_DECL;

  /*  MC Pi state hoisted into locals for register residency.  */
  i32 mc_pos     = st->mc_pos;
  u8  m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8  m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];
  u64 mc_count   = st->mc_count;
  u64 mc_inside  = st->mc_inside;

  const __m256i ones16_sum = _mm256_set1_epi16(1);
  /*  Prefetch ~8 strides ahead into L2 for streaming workloads.  */
#define PREFETCH_DIST 512
  for (i = 0; i < body_end; i += 64) {
    PREFETCH_R(buf + i + PREFETCH_DIST);
    /*  SCC: two 32-byte chunks, widen-mul-madd (no saturation);
        folded in-register first when fold is set.  */
    __m256i va0 = _mm256_loadu_si256((const __m256i *) (buf + i +  0));
    __m256i vb0 = _mm256_loadu_si256((const __m256i *) (buf + i +  1));
    __m256i va1 = _mm256_loadu_si256((const __m256i *) (buf + i + 32));
    __m256i vb1 = _mm256_loadu_si256((const __m256i *) (buf + i + 33));
    if (fold) {
      va0 = FASTENT_FN(fold_vec_inline)(va0);
      vb0 = FASTENT_FN(fold_vec_inline)(vb0);
      va1 = FASTENT_FN(fold_vec_inline)(va1);
      vb1 = FASTENT_FN(fold_vec_inline)(vb1);
    }
    __m256i vbs0 = _mm256_xor_si256(vb0, sign_xor);
    __m256i vbs1 = _mm256_xor_si256(vb1, sign_xor);

    /*  Widen each 32-byte vector into two i16 lanes (16 i16 each).  */
    __m256i va0_lo = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(va0));
    __m256i va0_hi = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(va0, 1));
    __m256i va1_lo = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(va1));
    __m256i va1_hi = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(va1, 1));
    __m256i vb0_lo = _mm256_cvtepi8_epi16(_mm256_castsi256_si128(vbs0));
    __m256i vb0_hi = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(vbs0, 1));
    __m256i vb1_lo = _mm256_cvtepi8_epi16(_mm256_castsi256_si128(vbs1));
    __m256i vb1_hi = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(vbs1, 1));

    /*  Signed mul: a (u16 in [0,255]) * b (i16 in [-128,127]) fits i16.  */
    __m256i prod0_lo = _mm256_mullo_epi16(va0_lo, vb0_lo);
    __m256i prod0_hi = _mm256_mullo_epi16(va0_hi, vb0_hi);
    __m256i prod1_lo = _mm256_mullo_epi16(va1_lo, vb1_lo);
    __m256i prod1_hi = _mm256_mullo_epi16(va1_hi, vb1_hi);

    /*  madd_epi16 widens i16 -> i32 (no saturation) and sums pairs.  */
    __m256i s0_lo = _mm256_madd_epi16(prod0_lo, ones16_sum);
    __m256i s0_hi = _mm256_madd_epi16(prod0_hi, ones16_sum);
    __m256i s1_lo = _mm256_madd_epi16(prod1_lo, ones16_sum);
    __m256i s1_hi = _mm256_madd_epi16(prod1_hi, ones16_sum);

    /*  Each i32 lane is a sum of 2 byte-products in [-65280..64770];
        widen to i64 before cross-iter accumulation since an i32 acc
        overflows on consistent-sign inputs.  */
    __m256i sum32 = _mm256_add_epi32(_mm256_add_epi32(s0_lo, s0_hi),
                                     _mm256_add_epi32(s1_lo, s1_hi));
    __m256i sum64_lo =
      _mm256_cvtepi32_epi64(_mm256_castsi256_si128(sum32));
    __m256i sum64_hi =
      _mm256_cvtepi32_epi64(_mm256_extracti128_si256(sum32, 1));
    scc_acc64 = _mm256_add_epi64(scc_acc64,
                  _mm256_add_epi64(sum64_lo, sum64_hi));
    /*  LHS byte sum via PSADBW for sign correction.  */
    __m256i sad0 = _mm256_sad_epu8(va0, zero);
    __m256i sad1 = _mm256_sad_epu8(va1, zero);
    lhs_sad = _mm256_add_epi64(lhs_sad, _mm256_add_epi64(sad0, sad1));

    /*  Histogram: 64 inc-mem across FASTENT_BANKS banks, from buf or
        (fold mode) the laundered L1 stage. See launder note above.  */
    ALIGN(32) u8 stage[64 + 16];
    FASTENT_STAGE_PTR p;
    if (fold) {
      _mm256_store_si256((__m256i *) (stage +  0), va0);
      _mm256_store_si256((__m256i *) (stage + 32), va1);
      FASTENT_STAGE_PTR sp = stage;
      FASTENT_LAUNDER(sp);
      p = sp;
    } else {
      p = buf + i;
    }
    HIST_N(p,  0); HIST_N(p,  8); HIST_N(p, 16); HIST_N(p, 24);
    HIST_N(p, 32); HIST_N(p, 40); HIST_N(p, 48); HIST_N(p, 56);

    /*  MC Pi: drain ring, then bulk hexads. Two accumulators (mi_a,
        mi_b) break the sbb serial dependency.  */
    u64 mi_a = 0, mi_b = 0;
    int drain_fired = 0;
#define MC_HIT(d, acc) acc += ((d) <= FASTENT_INCIRC)
#define MC_DRAIN() do { \
      u32 _x = ((u32) m0 << 16) | ((u32) m1 << 8) | (u32) m2; \
      u32 _y = ((u32) m3 << 16) | ((u32) m4 << 8) | (u32) m5; \
      u64 _d = (u64) _x * (u64) _x + (u64) _y * (u64) _y; \
      MC_HIT(_d, mi_a); \
      drain_fired = 1; \
    } while (0)

    u32 p_idx;
    switch (mc_pos) {
      case 0: p_idx = 0; break;
      case 1: m1 = p[0]; m2 = p[1]; m3 = p[2]; m4 = p[3]; m5 = p[4];
              MC_DRAIN(); p_idx = 5; break;
      case 2: m2 = p[0]; m3 = p[1]; m4 = p[2]; m5 = p[3];
              MC_DRAIN(); p_idx = 4; break;
      case 3: m3 = p[0]; m4 = p[1]; m5 = p[2];
              MC_DRAIN(); p_idx = 3; break;
      case 4: m4 = p[0]; m5 = p[1];
              MC_DRAIN(); p_idx = 2; break;
      default: m5 = p[0];
              MC_DRAIN(); p_idx = 1; break;
    }

    u32 n_hexads = (64u - p_idx) / 6u;
    /*  SIMD bulk: 2 hexads (12 bytes) per iter via PSHUFB + VPMULUDQ;
        cmpgt_epi64 is SSE4.2, always present with AVX2.  */
    const u8 * q = p + p_idx;
    const __m128i mc_shuf = _mm_setr_epi8(2, 1, 0, -1, 5, 4, 3, -1,
                                          8, 7, 6, -1, 11, 10, 9, -1);
    const __m128i mc_lim  = _mm_set1_epi64x((i64) (FASTENT_INCIRC + 1ULL));
    u32 k = 0;
    for (; k + 2 <= n_hexads; k += 2) {
      __m128i v   = _mm_loadu_si128((const __m128i *) (q + k * 6u));
      __m128i xy  = _mm_shuffle_epi8(v, mc_shuf);          /*  [x0,y0,x1,y1]  */
      __m128i xs  = _mm_mul_epu32(xy, xy);        /*  x^2 (u64)  */
      __m128i yshr = _mm_srli_epi64(xy, 32);
      __m128i ys  = _mm_mul_epu32(yshr, yshr);    /*  y^2 (u64)  */
      __m128i d   = _mm_add_epi64(xs, ys);
      __m128i mask = _mm_cmpgt_epi64(mc_lim, d);
      int bits = _mm_movemask_pd(_mm_castsi128_pd(mask));
      mi_a += (u64) FASTENT_POPCOUNT32(bits);
    }
    /*  Scalar tail (0 or 1 hexad).  */
    for (; k < n_hexads; k++) {
      u32 o = k * 6u;
      u32 x = ((u32) q[o + 0] << 16) | ((u32) q[o + 1] << 8) | q[o + 2];
      u32 y = ((u32) q[o + 3] << 16) | ((u32) q[o + 4] << 8) | q[o + 5];
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
      mi_b += (d <= FASTENT_INCIRC);
    }
    mc_count  += n_hexads + (drain_fired ? 1u : 0u);
    mc_inside += mi_a + mi_b;
#undef MC_DRAIN
#undef MC_HIT

    /*  Stash trailing < 6 bytes.  */
    u32 stash_at    = p_idx + n_hexads * 6u;
    u32 stash_count = 64u - stash_at;
    mc_pos = (i32) stash_count;
    if (stash_count >= 1) m0 = p[stash_at + 0];
    if (stash_count >= 2) m1 = p[stash_at + 1];
    if (stash_count >= 3) m2 = p[stash_at + 2];
    if (stash_count >= 4) m3 = p[stash_at + 3];
    if (stash_count >= 5) m4 = p[stash_at + 4];
  }

  /*  Horizontal reduce scc_acc64 (4 i64 lanes) into cross_product.  */
  __m128i s64_lo = _mm256_castsi256_si128(scc_acc64);
  __m128i s64_hi = _mm256_extracti128_si256(scc_acc64, 1);
  __m128i s64    = _mm_add_epi64(s64_lo, s64_hi);
  i64 scc_sum    = (i64) _mm_cvtsi128_si64(s64)
                 + (i64) _mm_cvtsi128_si64(_mm_unpackhi_epi64(s64, s64));

  /*  Horizontal reduce lhs_sad (4 u64 lanes) into byte-sum correction.  */
  __m128i lo64 = _mm256_castsi256_si128(lhs_sad);
  __m128i hi64 = _mm256_extracti128_si256(lhs_sad, 1);
  __m128i sb   = _mm_add_epi64(lo64, hi64);
  u64 lhs_sum  = (u64) _mm_cvtsi128_si64(sb)
               + (u64) _mm_cvtsi128_si64(_mm_unpackhi_epi64(sb, sb));

  /*  scc_sum = sum a_i*(b_i-128); add 128*sum_a to get sum a_i*b_i.  */
  st->cross_product += scc_sum + (i64) (128ULL * lhs_sum);

  /*  Epilogue: buf[body_end]'s SCC pair was counted by the last
      shifted load, but it still needs histogram + carry + MC.  */
  {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
    st->total_bytes += body_end;
    st->bank[(u32) (st->total_bytes) & (FASTENT_BANKS - 1)][b]++;
    st->total_bytes++;
    st->carry_byte = b;
    st->last_byte  = b;

    /*  MC: push b into the live ring, maybe fire one hexad.  */
    switch (mc_pos) {
      case 0: m0 = b; break;
      case 1: m1 = b; break;
      case 2: m2 = b; break;
      case 3: m3 = b; break;
      case 4: m4 = b; break;
      default: m5 = b; break;
    }
    mc_pos++;
    if (mc_pos == 6) {
      u32 x = ((u32) m0 << 16) | ((u32) m1 << 8) | (u32) m2;
      u32 y = ((u32) m3 << 16) | ((u32) m4 << 8) | (u32) m5;
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
      mc_count++;
      mc_inside += (d <= FASTENT_INCIRC);
      mc_pos = 0;
    }
  }

  /*  Save MC state back.  */
  st->mc_pos = mc_pos;
  st->mc_buf[0] = m0; st->mc_buf[1] = m1; st->mc_buf[2] = m2;
  st->mc_buf[3] = m3; st->mc_buf[4] = m4; st->mc_buf[5] = m5;
  st->mc_count  = mc_count;
  st->mc_inside = mc_inside;

  return body_end + 1;
}

#elif defined(FASTENT_VARIANT_AVX512)

/*  AVX-512 SIMD body.  */

static INLINE sz
FASTENT_FN(simd_body_impl)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len,
    int fold) {
  sz i;
  /*  Stride 128 (two 64-byte vectors). SCC needs byte +1 readable, so
      the body stops at len - 64 (64-byte read-ahead margin).  */
  if (len < 129) return 0;

  const sz body_max = len - 64;
  sz iters = body_max / 128;
  if (iters == 0) return 0;
  const sz body_end = iters * 128;

  u8 b0_user = fold ? FASTENT_FN(fold_byte_inline)(buf[0]) : buf[0];
  if (!st->have_first) { st->first_byte = b0_user; st->have_first = 1; }
  if (st->have_carry)
    st->cross_product += (i64) st->carry_byte * (i64) b0_user;
  st->have_carry = 1;

  const __m512i sign_xor = _mm512_set1_epi8((char) 0x80);
  const __m512i zero512  = _mm512_setzero_si512();
  __m512i scc_acc64      = _mm512_setzero_si512(); /*  8 i64 lanes  */
  __m512i lhs_sad        = _mm512_setzero_si512();

  FASTENT_HIST_DECL;

  i32 mc_pos     = st->mc_pos;
  u8  m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8  m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];
  u64 mc_count   = st->mc_count;
  u64 mc_inside  = st->mc_inside;

  const __m512i ones16_sum = _mm512_set1_epi16(1);
  (void) ones16_sum;  /*  unused in the BITALG/VNNI sub-variant  */

#define PREFETCH_DIST 512
  for (i = 0; i < body_end; i += 128) {
    PREFETCH_R(buf + i + PREFETCH_DIST + 0);
    PREFETCH_R(buf + i + PREFETCH_DIST + 64);

    __m512i va0 = _mm512_loadu_si512((const void *) (buf + i +  0));
    __m512i vb0 = _mm512_loadu_si512((const void *) (buf + i +  1));
    __m512i va1 = _mm512_loadu_si512((const void *) (buf + i + 64));
    __m512i vb1 = _mm512_loadu_si512((const void *) (buf + i + 65));
    if (fold) {
      va0 = FASTENT_FN(fold_vec_inline)(va0);
      vb0 = FASTENT_FN(fold_vec_inline)(vb0);
      va1 = FASTENT_FN(fold_vec_inline)(va1);
      vb1 = FASTENT_FN(fold_vec_inline)(vb1);
    }
    __m512i vbs0 = _mm512_xor_si512(vb0, sign_xor);
    __m512i vbs1 = _mm512_xor_si512(vb1, sign_xor);

#ifdef FASTENT_AVX512_HAVE_VNNI
    /*  VPDPBUSD fuses (u8*i8) multiply, 4-way sum and i32 accumulate,
        replacing the widen/mullo/madd chain. Each i32 lane is a sum
        of 4 byte-products in [-32640, 32385]; widen to i64 per iter.  */
    __m512i sum32 = _mm512_setzero_si512();
    sum32 = _mm512_dpbusd_epi32(sum32, va0, vbs0);
    sum32 = _mm512_dpbusd_epi32(sum32, va1, vbs1);
    __m512i sum64_lo =
      _mm512_cvtepi32_epi64(_mm512_castsi512_si256(sum32));
    __m512i sum64_hi =
      _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(sum32, 1));
    scc_acc64 = _mm512_add_epi64(scc_acc64,
                  _mm512_add_epi64(sum64_lo, sum64_hi));
#else
    /*  Widen each 64-byte vector into two i16 lanes (32 i16 each).  */
    __m512i va0_lo = _mm512_cvtepu8_epi16(_mm512_castsi512_si256(va0));
    __m512i va0_hi = _mm512_cvtepu8_epi16(_mm512_extracti64x4_epi64(va0, 1));
    __m512i va1_lo = _mm512_cvtepu8_epi16(_mm512_castsi512_si256(va1));
    __m512i va1_hi = _mm512_cvtepu8_epi16(_mm512_extracti64x4_epi64(va1, 1));
    __m512i vb0_lo = _mm512_cvtepi8_epi16(_mm512_castsi512_si256(vbs0));
    __m512i vb0_hi = _mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(vbs0, 1));
    __m512i vb1_lo = _mm512_cvtepi8_epi16(_mm512_castsi512_si256(vbs1));
    __m512i vb1_hi = _mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(vbs1, 1));

    __m512i prod0_lo = _mm512_mullo_epi16(va0_lo, vb0_lo);
    __m512i prod0_hi = _mm512_mullo_epi16(va0_hi, vb0_hi);
    __m512i prod1_lo = _mm512_mullo_epi16(va1_lo, vb1_lo);
    __m512i prod1_hi = _mm512_mullo_epi16(va1_hi, vb1_hi);

    __m512i s0_lo = _mm512_madd_epi16(prod0_lo, ones16_sum);
    __m512i s0_hi = _mm512_madd_epi16(prod0_hi, ones16_sum);
    __m512i s1_lo = _mm512_madd_epi16(prod1_lo, ones16_sum);
    __m512i s1_hi = _mm512_madd_epi16(prod1_hi, ones16_sum);

    /*  Each i32 lane is a sum of 2 byte-products in [-65280..64770];
        widen to i64 before cross-iter accumulation.  */
    __m512i sum32 = _mm512_add_epi32(_mm512_add_epi32(s0_lo, s0_hi),
                                     _mm512_add_epi32(s1_lo, s1_hi));
    __m512i sum64_lo =
      _mm512_cvtepi32_epi64(_mm512_castsi512_si256(sum32));
    __m512i sum64_hi =
      _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(sum32, 1));
    scc_acc64 = _mm512_add_epi64(scc_acc64,
                  _mm512_add_epi64(sum64_lo, sum64_hi));
#endif
    /*  LHS byte sum via PSADBW for the sign correction.  */
    __m512i sad0 = _mm512_sad_epu8(va0, zero512);
    __m512i sad1 = _mm512_sad_epu8(va1, zero512);
    lhs_sad = _mm512_add_epi64(lhs_sad, _mm512_add_epi64(sad0, sad1));

    /*  Histogram + MC stage: laundered L1 stage in fold mode, else
        direct buf reads (see launder note above).  */
    ALIGN(64) u8 stage[128 + 16];
    FASTENT_STAGE_PTR p;
    if (fold) {
      _mm512_store_si512((void *) (stage +  0), va0);
      _mm512_store_si512((void *) (stage + 64), va1);
      FASTENT_STAGE_PTR sp = stage;
      FASTENT_LAUNDER(sp);
      p = sp;
    } else {
      p = buf + i;
    }

    /*  128 inc-mem across FASTENT_BANKS banks.  */
    HIST_N(p,   0); HIST_N(p,   8); HIST_N(p,  16); HIST_N(p,  24);
    HIST_N(p,  32); HIST_N(p,  40); HIST_N(p,  48); HIST_N(p,  56);
    HIST_N(p,  64); HIST_N(p,  72); HIST_N(p,  80); HIST_N(p,  88);
    HIST_N(p,  96); HIST_N(p, 104); HIST_N(p, 112); HIST_N(p, 120);

    /*  MC Pi: AVX2 drain + bulk + stash, sized for the 128-byte
        stride (up to 21 hexads/iter).  */
    u64 mi_a = 0, mi_b = 0;
    int drain_fired = 0;
#define MC_HIT(d, acc) acc += ((d) <= FASTENT_INCIRC)
#define MC_DRAIN() do { \
      u32 _x = ((u32) m0 << 16) | ((u32) m1 << 8) | (u32) m2; \
      u32 _y = ((u32) m3 << 16) | ((u32) m4 << 8) | (u32) m5; \
      u64 _d = (u64) _x * (u64) _x + (u64) _y * (u64) _y; \
      MC_HIT(_d, mi_a); \
      drain_fired = 1; \
    } while (0)

    u32 p_idx;
    switch (mc_pos) {
      case 0: p_idx = 0; break;
      case 1: m1 = p[0]; m2 = p[1]; m3 = p[2]; m4 = p[3]; m5 = p[4];
              MC_DRAIN(); p_idx = 5; break;
      case 2: m2 = p[0]; m3 = p[1]; m4 = p[2]; m5 = p[3];
              MC_DRAIN(); p_idx = 4; break;
      case 3: m3 = p[0]; m4 = p[1]; m5 = p[2];
              MC_DRAIN(); p_idx = 3; break;
      case 4: m4 = p[0]; m5 = p[1];
              MC_DRAIN(); p_idx = 2; break;
      default: m5 = p[0];
              MC_DRAIN(); p_idx = 1; break;
    }

    u32 n_hexads = (128u - p_idx) / 6u;
    const u8 * q = p + p_idx;
    const __m128i mc_shuf = _mm_setr_epi8(2, 1, 0, -1, 5, 4, 3, -1,
                                          8, 7, 6, -1, 11, 10, 9, -1);
    const __m128i mc_lim  = _mm_set1_epi64x((i64) (FASTENT_INCIRC + 1ULL));
    u32 k = 0;
    /*  256-bit bulk: 4 hexads per iter (=24 bytes of source).  */
    const __m256i mc_shuf256 = _mm256_broadcastsi128_si256(mc_shuf);
    const __m256i mc_lim256  = _mm256_set1_epi64x((i64) (FASTENT_INCIRC + 1ULL));
    for (; k + 4 <= n_hexads; k += 4) {
      __m128i v0_xmm = _mm_loadu_si128((const __m128i *) (q + k * 6u));
      __m128i v1_xmm = _mm_loadu_si128((const __m128i *) (q + k * 6u + 12u));
      __m256i v   = _mm256_inserti128_si256(_mm256_castsi128_si256(v0_xmm),
                                            v1_xmm, 1);
      __m256i xy  = _mm256_shuffle_epi8(v, mc_shuf256);
      __m256i xs  = _mm256_mul_epu32(xy, xy);
      __m256i yshr = _mm256_srli_epi64(xy, 32);
      __m256i ys  = _mm256_mul_epu32(yshr, yshr);
      __m256i d   = _mm256_add_epi64(xs, ys);
      __m256i mask = _mm256_cmpgt_epi64(mc_lim256, d);
      int bits = _mm256_movemask_pd(_mm256_castsi256_pd(mask));
      mi_a += (u64) FASTENT_POPCOUNT32(bits);
    }
    /*  128-bit residual: 2 hexads per iter.  */
    for (; k + 2 <= n_hexads; k += 2) {
      __m128i v   = _mm_loadu_si128((const __m128i *) (q + k * 6u));
      __m128i xy  = _mm_shuffle_epi8(v, mc_shuf);
      __m128i xs  = _mm_mul_epu32(xy, xy);
      __m128i yshr = _mm_srli_epi64(xy, 32);
      __m128i ys  = _mm_mul_epu32(yshr, yshr);
      __m128i d   = _mm_add_epi64(xs, ys);
      __m128i mask = _mm_cmpgt_epi64(mc_lim, d);
      int bits = _mm_movemask_pd(_mm_castsi128_pd(mask));
      mi_b += (u64) FASTENT_POPCOUNT32(bits);
    }
    /*  Scalar tail (0 or 1 hexad).  */
    for (; k < n_hexads; k++) {
      u32 o = k * 6u;
      u32 x = ((u32) q[o + 0] << 16) | ((u32) q[o + 1] << 8) | q[o + 2];
      u32 y = ((u32) q[o + 3] << 16) | ((u32) q[o + 4] << 8) | q[o + 5];
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
      mi_b += (d <= FASTENT_INCIRC);
    }
    mc_count  += n_hexads + (drain_fired ? 1u : 0u);
    mc_inside += mi_a + mi_b;
#undef MC_DRAIN
#undef MC_HIT

    u32 stash_at    = p_idx + n_hexads * 6u;
    u32 stash_count = 128u - stash_at;
    mc_pos = (i32) stash_count;
    if (stash_count >= 1) m0 = p[stash_at + 0];
    if (stash_count >= 2) m1 = p[stash_at + 1];
    if (stash_count >= 3) m2 = p[stash_at + 2];
    if (stash_count >= 4) m3 = p[stash_at + 3];
    if (stash_count >= 5) m4 = p[stash_at + 4];
  }

  /*  Horizontal reduce scc_acc64 (8 i64 lanes) -> scalar.  */
  i64 scc_sum = (i64) _mm512_reduce_add_epi64(scc_acc64);
  u64 lhs_sum = (u64) _mm512_reduce_add_epi64(lhs_sad);
  st->cross_product += scc_sum + (i64) (128ULL * lhs_sum);

  /*  Epilogue: process buf[body_end] for histogram + carry + MC.  */
  {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
    st->total_bytes += body_end;
    st->bank[(u32) (st->total_bytes) & (FASTENT_BANKS - 1)][b]++;
    st->total_bytes++;
    st->carry_byte = b;
    st->last_byte  = b;

    switch (mc_pos) {
      case 0: m0 = b; break;
      case 1: m1 = b; break;
      case 2: m2 = b; break;
      case 3: m3 = b; break;
      case 4: m4 = b; break;
      default: m5 = b; break;
    }
    mc_pos++;
    if (mc_pos == 6) {
      u32 x = ((u32) m0 << 16) | ((u32) m1 << 8) | (u32) m2;
      u32 y = ((u32) m3 << 16) | ((u32) m4 << 8) | (u32) m5;
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
      mc_count++;
      mc_inside += (d <= FASTENT_INCIRC);
      mc_pos = 0;
    }
  }

  st->mc_pos = mc_pos;
  st->mc_buf[0] = m0; st->mc_buf[1] = m1; st->mc_buf[2] = m2;
  st->mc_buf[3] = m3; st->mc_buf[4] = m4; st->mc_buf[5] = m5;
  st->mc_count  = mc_count;
  st->mc_inside = mc_inside;

  return body_end + 1;
}

#elif defined(FASTENT_VARIANT_SSE41) || defined(FASTENT_VARIANT_SSSE3)

static INLINE sz
FASTENT_FN(simd_body_impl)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len,
    int fold) {
  sz i;
  i32 k;
  /*  SSE stride = 32 bytes (two 16-byte vectors). Need 16-byte read-ahead.  */
  if (len < 33) return 0;
  const sz body_max = len - 16;
  sz iters = body_max / 32;
  if (iters == 0) return 0;
  const sz body_end = iters * 32;

  u8 b0_user = fold ? FASTENT_FN(fold_byte_inline)(buf[0]) : buf[0];
  if (!st->have_first) { st->first_byte = b0_user; st->have_first = 1; }
  if (st->have_carry)
    st->cross_product += (i64) st->carry_byte * (i64) b0_user;
  st->have_carry = 1;

  const __m128i sign_xor   = _mm_set1_epi8((char) 0x80);
  const __m128i ones16_sum = _mm_set1_epi16(1);
  const __m128i zero       = _mm_setzero_si128();
  __m128i scc_acc64        = _mm_setzero_si128();  /*  2 i64 lanes  */
  __m128i lhs_sad          = _mm_setzero_si128();

  FASTENT_HIST_DECL;

  /*  MC Pi state hoisted into locals.  */
  i32 mc_pos     = st->mc_pos;
  u8  m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8  m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];
  u64 mc_count   = st->mc_count;
  u64 mc_inside  = st->mc_inside;

  for (i = 0; i < body_end; i += 32) {
    __m128i va0 = _mm_loadu_si128((const __m128i *) (buf + i +  0));
    __m128i vb0 = _mm_loadu_si128((const __m128i *) (buf + i +  1));
    __m128i va1 = _mm_loadu_si128((const __m128i *) (buf + i + 16));
    __m128i vb1 = _mm_loadu_si128((const __m128i *) (buf + i + 17));
    if (fold) {
      va0 = FASTENT_FN(fold_vec_inline)(va0);
      vb0 = FASTENT_FN(fold_vec_inline)(vb0);
      va1 = FASTENT_FN(fold_vec_inline)(va1);
      vb1 = FASTENT_FN(fold_vec_inline)(vb1);
    }
    __m128i vbs0 = _mm_xor_si128(vb0, sign_xor);
    __m128i vbs1 = _mm_xor_si128(vb1, sign_xor);

    /*  Widen to 8 i16 halves (SSSE3-safe): zero-extend va via unpack
        with zero; sign-extend vbs via unpack with cmpgt(0,vbs).  */
    __m128i va0_lo = _mm_unpacklo_epi8(va0, zero);
    __m128i va0_hi = _mm_unpackhi_epi8(va0, zero);
    __m128i va1_lo = _mm_unpacklo_epi8(va1, zero);
    __m128i va1_hi = _mm_unpackhi_epi8(va1, zero);
    __m128i sign0  = _mm_cmpgt_epi8(zero, vbs0);
    __m128i sign1  = _mm_cmpgt_epi8(zero, vbs1);
    __m128i vb0_lo = _mm_unpacklo_epi8(vbs0, sign0);
    __m128i vb0_hi = _mm_unpackhi_epi8(vbs0, sign0);
    __m128i vb1_lo = _mm_unpacklo_epi8(vbs1, sign1);
    __m128i vb1_hi = _mm_unpackhi_epi8(vbs1, sign1);

    __m128i prod0_lo = _mm_mullo_epi16(va0_lo, vb0_lo);
    __m128i prod0_hi = _mm_mullo_epi16(va0_hi, vb0_hi);
    __m128i prod1_lo = _mm_mullo_epi16(va1_lo, vb1_lo);
    __m128i prod1_hi = _mm_mullo_epi16(va1_hi, vb1_hi);

    __m128i s0_lo = _mm_madd_epi16(prod0_lo, ones16_sum);
    __m128i s0_hi = _mm_madd_epi16(prod0_hi, ones16_sum);
    __m128i s1_lo = _mm_madd_epi16(prod1_lo, ones16_sum);
    __m128i s1_hi = _mm_madd_epi16(prod1_hi, ones16_sum);

    /*  Widen i32 -> i64 via sign-shift + unpack (SSSE3-safe).  */
    __m128i sum32 = _mm_add_epi32(_mm_add_epi32(s0_lo, s0_hi),
                                  _mm_add_epi32(s1_lo, s1_hi));
    __m128i sign = _mm_srai_epi32(sum32, 31);  /*  i32 sign bits  */
    __m128i sum64_lo = _mm_unpacklo_epi32(sum32, sign);
    __m128i sum64_hi = _mm_unpackhi_epi32(sum32, sign);
    scc_acc64 = _mm_add_epi64(scc_acc64, _mm_add_epi64(sum64_lo, sum64_hi));
    __m128i sad0 = _mm_sad_epu8(va0, zero);
    __m128i sad1 = _mm_sad_epu8(va1, zero);
    lhs_sad = _mm_add_epi64(lhs_sad, _mm_add_epi64(sad0, sad1));

    /*  Histogram from buf or laundered L1 stage (see note above).  */
    ALIGN(16) u8 stage[32];
    FASTENT_STAGE_PTR p;
    if (fold) {
      _mm_store_si128((__m128i *) (stage +  0), va0);
      _mm_store_si128((__m128i *) (stage + 16), va1);
      FASTENT_STAGE_PTR sp = stage;
      FASTENT_LAUNDER(sp);
      p = sp;
    } else {
      p = buf + i;
    }
    HIST_N(p,  0); HIST_N(p,  8); HIST_N(p, 16); HIST_N(p, 24);

    /*  MC Pi: scalar drain + bulk + stash (32-byte stride).  */
#define MC_HEXAD(x0, x1, x2, y0, y1, y2) do { \
      u32 x = ((u32) (x0) << 16) | ((u32) (x1) << 8) | (u32) (x2); \
      u32 y = ((u32) (y0) << 16) | ((u32) (y1) << 8) | (u32) (y2); \
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y; \
      mc_count++; \
      mc_inside += (d <= FASTENT_INCIRC); \
    } while (0)

    u32 p_idx;
    switch (mc_pos) {
      case 0: p_idx = 0; break;
      case 1: m1=p[0]; m2=p[1]; m3=p[2]; m4=p[3]; m5=p[4];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 5; break;
      case 2: m2=p[0]; m3=p[1]; m4=p[2]; m5=p[3];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 4; break;
      case 3: m3=p[0]; m4=p[1]; m5=p[2];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 3; break;
      case 4: m4=p[0]; m5=p[1];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 2; break;
      default: m5=p[0];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 1; break;
    }
    i32 n_hexads = (i32) ((32u - p_idx) / 6u);
    Fk(n_hexads,
      u32 o = p_idx + (u32) k * 6u;
      MC_HEXAD(p[o+0], p[o+1], p[o+2], p[o+3], p[o+4], p[o+5]));
    u32 stash_at    = p_idx + n_hexads * 6u;
    u32 stash_count = 32u - stash_at;
    mc_pos = (i32) stash_count;
    if (stash_count >= 1) m0 = p[stash_at + 0];
    if (stash_count >= 2) m1 = p[stash_at + 1];
    if (stash_count >= 3) m2 = p[stash_at + 2];
    if (stash_count >= 4) m3 = p[stash_at + 3];
    if (stash_count >= 5) m4 = p[stash_at + 4];
#undef MC_HEXAD
  }

  i64 scc_sum = (i64) _mm_cvtsi128_si64(scc_acc64)
    + (i64) _mm_cvtsi128_si64(_mm_unpackhi_epi64(scc_acc64, scc_acc64));

  u64 lhs_sum = (u64) _mm_cvtsi128_si64(lhs_sad)
              + (u64) _mm_cvtsi128_si64(_mm_unpackhi_epi64(lhs_sad, lhs_sad));

  st->cross_product += scc_sum + (i64) (128ULL * lhs_sum);

  /*  Epilogue: histogram + MC for buf[body_end]; its SCC pair was
      already added by the last shifted load.  */
  u8 last_b;
  {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
    last_b = b;
    st->total_bytes += body_end;
    st->bank[(u32) (st->total_bytes) & (FASTENT_BANKS - 1)][b]++;
    st->total_bytes++;
    switch (mc_pos) {
      case 0: m0 = b; break;
      case 1: m1 = b; break;
      case 2: m2 = b; break;
      case 3: m3 = b; break;
      case 4: m4 = b; break;
      default: m5 = b; break;
    }
    mc_pos++;
    if (mc_pos == 6) {
      u32 x = ((u32) m0 << 16) | ((u32) m1 << 8) | (u32) m2;
      u32 y = ((u32) m3 << 16) | ((u32) m4 << 8) | (u32) m5;
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
      mc_count++;
      mc_inside += (d <= FASTENT_INCIRC);
      mc_pos = 0;
    }
  }

  st->mc_pos = mc_pos;
  st->mc_buf[0] = m0; st->mc_buf[1] = m1; st->mc_buf[2] = m2;
  st->mc_buf[3] = m3; st->mc_buf[4] = m4; st->mc_buf[5] = m5;
  st->mc_count  = mc_count;
  st->mc_inside = mc_inside;

  /*  carry/last take buf[body_end] (folded if -f).  */
  st->carry_byte = last_b;
  st->last_byte  = last_b;

  return body_end + 1;
}

#elif defined(FASTENT_VARIANT_NEON)

/*  AArch64 NEON body, stride 32 mirroring SSE.  */

static INLINE sz
FASTENT_FN(simd_body_impl)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len,
    int fold) {
  sz i;
  i32 k;
  if (len < 33) return 0;
  const sz body_max = len - 16;
  sz iters = body_max / 32;
  if (iters == 0) return 0;
  const sz body_end = iters * 32;

  u8 b0_user = fold ? FASTENT_FN(fold_byte_inline)(buf[0]) : buf[0];
  if (!st->have_first) { st->first_byte = b0_user; st->have_first = 1; }
  if (st->have_carry)
    st->cross_product += (i64) st->carry_byte * (i64) b0_user;
  st->have_carry = 1;

  const uint8x16_t sign_xor = vdupq_n_u8(0x80);
  int64x2_t  scc_acc64 = vdupq_n_s64(0);
  uint64x2_t lhs_sad   = vdupq_n_u64(0);

  FASTENT_HIST_DECL;

  i32 mc_pos     = st->mc_pos;
  u8  m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8  m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];
  u64 mc_count   = st->mc_count;
  u64 mc_inside  = st->mc_inside;

  for (i = 0; i < body_end; i += 32) {
    PREFETCH_R(buf + i + 512);

    uint8x16_t va0 = vld1q_u8(buf + i +  0);
    uint8x16_t vb0 = vld1q_u8(buf + i +  1);
    uint8x16_t va1 = vld1q_u8(buf + i + 16);
    uint8x16_t vb1 = vld1q_u8(buf + i + 17);
    if (fold) {
      va0 = FASTENT_FN(fold_vec_inline)(va0);
      vb0 = FASTENT_FN(fold_vec_inline)(vb0);
      va1 = FASTENT_FN(fold_vec_inline)(va1);
      vb1 = FASTENT_FN(fold_vec_inline)(vb1);
    }
    int8x16_t vbs0 = vreinterpretq_s8_u8(veorq_u8(vb0, sign_xor));
    int8x16_t vbs1 = vreinterpretq_s8_u8(veorq_u8(vb1, sign_xor));

    /*  Widen va u8 -> i16 (255 fits).  */
    int16x8_t va0_lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(va0)));
    int16x8_t va0_hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(va0)));
    int16x8_t va1_lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(va1)));
    int16x8_t va1_hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(va1)));
    /*  Sign-widen vbs s8 -> s16.  */
    int16x8_t vb0_lo = vmovl_s8(vget_low_s8(vbs0));
    int16x8_t vb0_hi = vmovl_s8(vget_high_s8(vbs0));
    int16x8_t vb1_lo = vmovl_s8(vget_low_s8(vbs1));
    int16x8_t vb1_hi = vmovl_s8(vget_high_s8(vbs1));

    /*  i16 * i16 -> i16: |va|<=255, |vb|<=128, |product|<=32640 fits.  */
    int16x8_t prod0_lo = vmulq_s16(va0_lo, vb0_lo);
    int16x8_t prod0_hi = vmulq_s16(va0_hi, vb0_hi);
    int16x8_t prod1_lo = vmulq_s16(va1_lo, vb1_lo);
    int16x8_t prod1_hi = vmulq_s16(va1_hi, vb1_hi);

    /*  Pair-sum i16 lanes into i32 lanes (madd_epi16-with-ones).  */
    int32x4_t s0_lo = vpaddlq_s16(prod0_lo);
    int32x4_t s0_hi = vpaddlq_s16(prod0_hi);
    int32x4_t s1_lo = vpaddlq_s16(prod1_lo);
    int32x4_t s1_hi = vpaddlq_s16(prod1_hi);

    int32x4_t sum32 = vaddq_s32(vaddq_s32(s0_lo, s0_hi),
                                vaddq_s32(s1_lo, s1_hi));
    /*  Widen i32 lanes -> i64 lanes for cross-iter accumulation.  */
    int64x2_t sum64_lo = vmovl_s32(vget_low_s32(sum32));
    int64x2_t sum64_hi = vmovl_s32(vget_high_s32(sum32));
    scc_acc64 = vaddq_s64(scc_acc64, vaddq_s64(sum64_lo, sum64_hi));

    /*  LHS byte sum for the 128*sum_a sign correction.  */
    uint16x8_t sa0_16 = vpaddlq_u8(va0);
    uint16x8_t sa1_16 = vpaddlq_u8(va1);
    uint16x8_t sa_16  = vaddq_u16(sa0_16, sa1_16);
    uint32x4_t sa_32  = vpaddlq_u16(sa_16);
    uint64x2_t sa_64  = vpaddlq_u32(sa_32);
    lhs_sad = vaddq_u64(lhs_sad, sa_64);

    /*  Histogram + MC stage buffer (fold mode) or direct buf reads.  */
    ALIGN(16) u8 stage[32];
    FASTENT_STAGE_PTR p;
    if (fold) {
      vst1q_u8(stage +  0, va0);
      vst1q_u8(stage + 16, va1);
      FASTENT_STAGE_PTR sp = stage;
      FASTENT_LAUNDER(sp);
      p = sp;
    } else {
      p = buf + i;
    }
    HIST_N(p,  0); HIST_N(p,  8); HIST_N(p, 16); HIST_N(p, 24);

    /*  MC Pi: scalar drain + scalar bulk + scalar stash.  */
#define MC_HEXAD(x0, x1, x2, y0, y1, y2) do { \
      u32 x = ((u32) (x0) << 16) | ((u32) (x1) << 8) | (u32) (x2); \
      u32 y = ((u32) (y0) << 16) | ((u32) (y1) << 8) | (u32) (y2); \
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y; \
      mc_count++; \
      mc_inside += (d <= FASTENT_INCIRC); \
    } while (0)

    u32 p_idx;
    switch (mc_pos) {
      case 0: p_idx = 0; break;
      case 1: m1=p[0]; m2=p[1]; m3=p[2]; m4=p[3]; m5=p[4];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 5; break;
      case 2: m2=p[0]; m3=p[1]; m4=p[2]; m5=p[3];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 4; break;
      case 3: m3=p[0]; m4=p[1]; m5=p[2];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 3; break;
      case 4: m4=p[0]; m5=p[1];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 2; break;
      default: m5=p[0];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 1; break;
    }
    i32 n_hexads = (i32) ((32u - p_idx) / 6u);
    Fk(n_hexads,
      u32 o = p_idx + (u32) k * 6u;
      MC_HEXAD(p[o+0], p[o+1], p[o+2], p[o+3], p[o+4], p[o+5]));
    u32 stash_at    = p_idx + (u32) n_hexads * 6u;
    u32 stash_count = 32u - stash_at;
    mc_pos = (i32) stash_count;
    if (stash_count >= 1) m0 = p[stash_at + 0];
    if (stash_count >= 2) m1 = p[stash_at + 1];
    if (stash_count >= 3) m2 = p[stash_at + 2];
    if (stash_count >= 4) m3 = p[stash_at + 3];
    if (stash_count >= 5) m4 = p[stash_at + 4];
#undef MC_HEXAD
  }

  i64 scc_sum = (i64) fastent_neon_addvq_s64(scc_acc64);
  u64 lhs_sum = (u64) fastent_neon_addvq_u64(lhs_sad);
  st->cross_product += scc_sum + (i64) (128ULL * lhs_sum);

  /*  Epilogue: histogram + carry + MC for buf[body_end]; its SCC pair
      was already counted by the last shifted load.  */
  u8 last_b;
  {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
    last_b = b;
    st->total_bytes += body_end;
    st->bank[(u32) (st->total_bytes) & (FASTENT_BANKS - 1)][b]++;
    st->total_bytes++;
    switch (mc_pos) {
      case 0: m0 = b; break;
      case 1: m1 = b; break;
      case 2: m2 = b; break;
      case 3: m3 = b; break;
      case 4: m4 = b; break;
      default: m5 = b; break;
    }
    mc_pos++;
    if (mc_pos == 6) {
      u32 x = ((u32) m0 << 16) | ((u32) m1 << 8) | (u32) m2;
      u32 y = ((u32) m3 << 16) | ((u32) m4 << 8) | (u32) m5;
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
      mc_count++;
      mc_inside += (d <= FASTENT_INCIRC);
      mc_pos = 0;
    }
  }

  st->mc_pos = mc_pos;
  st->mc_buf[0] = m0; st->mc_buf[1] = m1; st->mc_buf[2] = m2;
  st->mc_buf[3] = m3; st->mc_buf[4] = m4; st->mc_buf[5] = m5;
  st->mc_count  = mc_count;
  st->mc_inside = mc_inside;

  st->carry_byte = last_b;
  st->last_byte  = last_b;

  return body_end + 1;
}

#elif defined(FASTENT_VARIANT_WASM128)

/*  WASM SIMD128 byte body, stride 32 as SSE/NEON.  SCC uses
    wasm_i32x4_dot_i16x8 (signed i16 pair-sum to i32); LHS sum via
    extadd-pairwise ladder; histogram + MC Pi scalar 4-banked.  */

static INLINE sz
FASTENT_FN(simd_body_impl)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len,
    int fold) {
  sz i;
  i32 k;
  if (len < 33) return 0;
  const sz body_max = len - 16;
  sz iters = body_max / 32;
  if (iters == 0) return 0;
  const sz body_end = iters * 32;

  u8 b0_user = fold ? FASTENT_FN(fold_byte_inline)(buf[0]) : buf[0];
  if (!st->have_first) { st->first_byte = b0_user; st->have_first = 1; }
  if (st->have_carry)
    st->cross_product += (i64) st->carry_byte * (i64) b0_user;
  st->have_carry = 1;

  const v128_t sign_xor = wasm_i8x16_splat((i8) 0x80);
  v128_t scc_acc64 = wasm_i64x2_splat(0);
  v128_t lhs_sad   = wasm_i64x2_splat(0);

  FASTENT_HIST_DECL;

  i32 mc_pos     = st->mc_pos;
  u8  m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8  m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];
  u64 mc_count   = st->mc_count;
  u64 mc_inside  = st->mc_inside;

  const v128_t ones32 = wasm_i32x4_splat(1);

  for (i = 0; i < body_end; i += 32) {
    PREFETCH_R(buf + i + 512);

    v128_t va0 = wasm_v128_load(buf + i +  0);
    v128_t vb0 = wasm_v128_load(buf + i +  1);
    v128_t va1 = wasm_v128_load(buf + i + 16);
    v128_t vb1 = wasm_v128_load(buf + i + 17);
    if (fold) {
      va0 = FASTENT_FN(fold_vec_inline)(va0);
      vb0 = FASTENT_FN(fold_vec_inline)(vb0);
      va1 = FASTENT_FN(fold_vec_inline)(va1);
      vb1 = FASTENT_FN(fold_vec_inline)(vb1);
    }
    v128_t vbs0 = wasm_v128_xor(vb0, sign_xor);
    v128_t vbs1 = wasm_v128_xor(vb1, sign_xor);

    /*  Widen va u8 -> i16 (zero); vbs i8 -> i16 (sign).  */
    v128_t va0_lo = wasm_i16x8_extend_low_u8x16(va0);
    v128_t va0_hi = wasm_i16x8_extend_high_u8x16(va0);
    v128_t va1_lo = wasm_i16x8_extend_low_u8x16(va1);
    v128_t va1_hi = wasm_i16x8_extend_high_u8x16(va1);
    v128_t vb0_lo = wasm_i16x8_extend_low_i8x16(vbs0);
    v128_t vb0_hi = wasm_i16x8_extend_high_i8x16(vbs0);
    v128_t vb1_lo = wasm_i16x8_extend_low_i8x16(vbs1);
    v128_t vb1_hi = wasm_i16x8_extend_high_i8x16(vbs1);

    /*  dot_i16x8: signed i16 -> i32 pair-summed (PMADDWD).  */
    v128_t s0_lo = wasm_i32x4_dot_i16x8(va0_lo, vb0_lo);
    v128_t s0_hi = wasm_i32x4_dot_i16x8(va0_hi, vb0_hi);
    v128_t s1_lo = wasm_i32x4_dot_i16x8(va1_lo, vb1_lo);
    v128_t s1_hi = wasm_i32x4_dot_i16x8(va1_hi, vb1_hi);

    /*  Widen i32 -> i64 before cross-iter accumulation (i32 acc
        overflows on consistent-sign inputs).  */
    v128_t sum32 = wasm_i32x4_add(wasm_i32x4_add(s0_lo, s0_hi),
                                  wasm_i32x4_add(s1_lo, s1_hi));
    v128_t sum64_lo = wasm_i64x2_extend_low_i32x4(sum32);
    v128_t sum64_hi = wasm_i64x2_extend_high_i32x4(sum32);
    scc_acc64 = wasm_i64x2_add(scc_acc64,
                  wasm_i64x2_add(sum64_lo, sum64_hi));

    /*  LHS byte sum for the 128*sum_a sign correction.  */
    v128_t sa0_16 = wasm_i16x8_extadd_pairwise_u8x16(va0);
    v128_t sa1_16 = wasm_i16x8_extadd_pairwise_u8x16(va1);
    v128_t sa_16  = wasm_i16x8_add(sa0_16, sa1_16);
    v128_t sa_32  = wasm_i32x4_extadd_pairwise_u16x8(sa_16);
    v128_t sa_64_lo = wasm_u64x2_extmul_low_u32x4(sa_32, ones32);
    v128_t sa_64_hi = wasm_u64x2_extmul_high_u32x4(sa_32, ones32);
    lhs_sad = wasm_i64x2_add(lhs_sad,
                wasm_i64x2_add(sa_64_lo, sa_64_hi));

    /*  Histogram + MC stage buffer (fold mode) or direct buf reads.  */
    ALIGN(16) u8 stage[32];
    FASTENT_STAGE_PTR p;
    if (fold) {
      wasm_v128_store(stage +  0, va0);
      wasm_v128_store(stage + 16, va1);
      FASTENT_STAGE_PTR sp = stage;
      FASTENT_LAUNDER(sp);
      p = sp;
    } else {
      p = buf + i;
    }
    HIST_N(p,  0); HIST_N(p,  8); HIST_N(p, 16); HIST_N(p, 24);

    /*  MC Pi: all-scalar (NEON-style).  */
#define MC_HEXAD(x0, x1, x2, y0, y1, y2) do { \
      u32 x = ((u32) (x0) << 16) | ((u32) (x1) << 8) | (u32) (x2); \
      u32 y = ((u32) (y0) << 16) | ((u32) (y1) << 8) | (u32) (y2); \
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y; \
      mc_count++; \
      mc_inside += (d <= FASTENT_INCIRC); \
    } while (0)

    u32 p_idx;
    switch (mc_pos) {
      case 0: p_idx = 0; break;
      case 1: m1=p[0]; m2=p[1]; m3=p[2]; m4=p[3]; m5=p[4];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 5; break;
      case 2: m2=p[0]; m3=p[1]; m4=p[2]; m5=p[3];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 4; break;
      case 3: m3=p[0]; m4=p[1]; m5=p[2];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 3; break;
      case 4: m4=p[0]; m5=p[1];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 2; break;
      default: m5=p[0];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 1; break;
    }
    i32 n_hexads = (i32) ((32u - p_idx) / 6u);
    Fk(n_hexads,
      u32 o = p_idx + (u32) k * 6u;
      MC_HEXAD(p[o+0], p[o+1], p[o+2], p[o+3], p[o+4], p[o+5]));
    u32 stash_at    = p_idx + (u32) n_hexads * 6u;
    u32 stash_count = 32u - stash_at;
    mc_pos = (i32) stash_count;
    if (stash_count >= 1) m0 = p[stash_at + 0];
    if (stash_count >= 2) m1 = p[stash_at + 1];
    if (stash_count >= 3) m2 = p[stash_at + 2];
    if (stash_count >= 4) m3 = p[stash_at + 3];
    if (stash_count >= 5) m4 = p[stash_at + 4];
#undef MC_HEXAD
  }

  i64 scc_sum = (i64) wasm_i64x2_extract_lane(scc_acc64, 0)
              + (i64) wasm_i64x2_extract_lane(scc_acc64, 1);
  u64 lhs_sum = (u64) wasm_i64x2_extract_lane(lhs_sad, 0)
              + (u64) wasm_i64x2_extract_lane(lhs_sad, 1);
  st->cross_product += scc_sum + (i64) (128ULL * lhs_sum);

  /*  Epilogue: histogram + carry + MC for buf[body_end]; its SCC pair
      was already counted by the last shifted load.  */
  u8 last_b;
  {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
    last_b = b;
    st->total_bytes += body_end;
    st->bank[(u32) (st->total_bytes) & (FASTENT_BANKS - 1)][b]++;
    st->total_bytes++;
    switch (mc_pos) {
      case 0: m0 = b; break;
      case 1: m1 = b; break;
      case 2: m2 = b; break;
      case 3: m3 = b; break;
      case 4: m4 = b; break;
      default: m5 = b; break;
    }
    mc_pos++;
    if (mc_pos == 6) {
      u32 x = ((u32) m0 << 16) | ((u32) m1 << 8) | (u32) m2;
      u32 y = ((u32) m3 << 16) | ((u32) m4 << 8) | (u32) m5;
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
      mc_count++;
      mc_inside += (d <= FASTENT_INCIRC);
      mc_pos = 0;
    }
  }

  st->mc_pos = mc_pos;
  st->mc_buf[0] = m0; st->mc_buf[1] = m1; st->mc_buf[2] = m2;
  st->mc_buf[3] = m3; st->mc_buf[4] = m4; st->mc_buf[5] = m5;
  st->mc_count  = mc_count;
  st->mc_inside = mc_inside;

  st->carry_byte = last_b;
  st->last_byte  = last_b;

  return body_end + 1;
}

#endif

/*  SIMD trampolines: pin the compile-time `fold` constant of
    simd_body_impl for the two analyse entry points.  */
#ifdef FASTENT_HAVE_SIMD
static inline sz FASTENT_FN(simd_body)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len) {
  return FASTENT_FN(simd_body_impl)(st, buf, len, 0);
}
static inline sz FASTENT_FN(simd_body_fold)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len) {
  return FASTENT_FN(simd_body_impl)(st, buf, len, 1);
}
#endif

/*  Public entry point.  */

static INLINE void
FASTENT_FN(analyze_impl)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len,
    int fold) {
  if (len == 0) return;

#ifdef FASTENT_HAVE_SIMD
  sz body = fold ? FASTENT_FN(simd_body_fold)(st, buf, len)
                 : FASTENT_FN(simd_body)(st, buf, len);
  if (body > 0) {
    sz start_bank = st->total_bytes;
    FASTENT_FN(scalar_body_impl)(st, buf + body, len - body, start_bank, fold);
  } else {
    sz start_bank = st->total_bytes;
    FASTENT_FN(scalar_body_impl)(st, buf, len, start_bank, fold);
  }
#else
  sz start_bank = st->total_bytes;
  FASTENT_FN(scalar_body_impl)(st, buf, len, start_bank, fold);
#endif
  /*  MC Pi is folded into the histogram/SCC pass; no separate walk.  */
}

void FASTENT_FN(analyze)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len) {
  FASTENT_FN(analyze_impl)(st, buf, len, 0);
#if defined(FASTENT_VARIANT_AVX2) || defined(FASTENT_VARIANT_AVX512)
  _mm256_zeroupper();
#endif
}

void FASTENT_FN(analyze_fold)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len) {
  FASTENT_FN(analyze_impl)(st, buf, len, 1);
}

/*  Case fold in place: ASCII A-Z and Latin-1 0xC0-0xDE (except 0xD7)
    lowered. Range tests use saturating sub (SSSE3-grade ops only).  */

static inline int FASTENT_FN(fold_is_upper_scalar)(u32 c) {
  return ((u32) (c - 'A')  < 26u) ||
         ((u32) (c - 0xC0u) < 31u && c != 0xD7u);
}

void FASTENT_FN(fold)(u8 * buf, sz len) {
  sz i = 0;
#ifdef FASTENT_HAVE_SIMD
  if (len >= FASTENT_SIMD_VLEN) {
    const FASTENT_SIMD_VEC zero    = V_SETZERO();
    const FASTENT_SIMD_VEC v_amin  = V_SET1_EPI8('A');      /*  0x41  */
    const FASTENT_SIMD_VEC v_zmax  = V_SET1_EPI8('Z');      /*  0x5A  */
    const FASTENT_SIMD_VEC v_c0min = V_SET1_EPI8(0xC0);
    const FASTENT_SIMD_VEC v_demax = V_SET1_EPI8(0xDE);
    const FASTENT_SIMD_VEC v_d7    = V_SET1_EPI8(0xD7);
    const FASTENT_SIMD_VEC v_0x20  = V_SET1_EPI8(0x20);

    const sz simd_end = len - (len % FASTENT_SIMD_VLEN);
    for (; i < simd_end; i += FASTENT_SIMD_VLEN) {
      FASTENT_SIMD_VEC c = V_LOAD(buf + i);

      /*  ASCII c in ['A','Z']: subs_epu8(K,c)==0 iff c>=K, so
          s_ge_a==0 iff c>='A', s_le_z==0 iff c<='Z'; both 0 = in range.  */
      FASTENT_SIMD_VEC s_ge_a  = V_SUBS_EPU8(v_amin, c);
      FASTENT_SIMD_VEC s_le_z  = V_SUBS_EPU8(c, v_zmax);
      FASTENT_SIMD_VEC m_ascii = V_CMPEQ_EPI8(V_OR(s_ge_a, s_le_z), zero);

      /*  Latin-1: c in [0xC0, 0xDE] and c != 0xD7.  */
      FASTENT_SIMD_VEC s_ge_c0 = V_SUBS_EPU8(v_c0min, c);
      FASTENT_SIMD_VEC s_le_de = V_SUBS_EPU8(c, v_demax);
      FASTENT_SIMD_VEC m_lat   = V_CMPEQ_EPI8(V_OR(s_ge_c0, s_le_de), zero);
      FASTENT_SIMD_VEC m_d7    = V_CMPEQ_EPI8(c, v_d7);
      m_lat = V_ANDNOT(m_d7, m_lat);

      FASTENT_SIMD_VEC mask  = V_OR(m_ascii, m_lat);
      FASTENT_SIMD_VEC delta = V_AND(mask, v_0x20);
      V_STORE(buf + i, V_ADD_EPI8(c, delta));
    }
  }
#endif
  for (; i < len; i++) {
    u32 c = buf[i];
    if (FASTENT_FN(fold_is_upper_scalar)(c)) buf[i] = (u8) (c + 0x20u);
  }
}

/*  Bit-mode analyser, MSB-first per byte.  */

#ifdef FASTENT_HAVE_SIMD

static INLINE sz
FASTENT_FN(bits_simd_body_impl)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len,
    int fold) {
  sz i;
  /*  Cross-byte needs buf[i+VLEN] readable, so the body stops at
      len - VLEN.  */
  if (len < (sz) FASTENT_SIMD_VLEN + 1) return 0;
  const sz body_max = len - FASTENT_SIMD_VLEN;
  sz iters = body_max / FASTENT_SIMD_VLEN;
  if (iters == 0) return 0;
  const sz body_end = iters * FASTENT_SIMD_VLEN;

  u8 b0_user = fold ? FASTENT_FN(fold_byte_inline)(buf[0]) : buf[0];
  if (!st->have_first) {
    st->first_byte = (u8) ((b0_user >> 7) & 1u);
    st->have_first = 1;
  }
  if (st->have_carry) {
    u32 prev_lsb = (u32) (st->carry_byte & 1u);
    u32 curr_msb = (u32) ((b0_user >> 7) & 1u);
    st->cross_product += (i64) (prev_lsb & curr_msb);
  }
  st->have_carry = 1;

  /*  Nibble-popcount LUT, replicated per 128-bit PSHUFB lane.
      AVX-512+BITALG uses VPOPCNTB, NEON vcntq_u8, WASM popcnt.  */
#if defined(FASTENT_VARIANT_AVX2)
  const FASTENT_SIMD_VEC popcnt_lut = _mm256_setr_epi8(
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
#elif defined(FASTENT_VARIANT_AVX512)
#if defined(FASTENT_AVX512_HAVE_BITALG)
    /*  Unused: VPOPCNTB replaces the LUT lookups.  */
#else
    /*  AVX-512 F+BW: nibble LUT broadcast into all four PSHUFB lanes
        via vbroadcasti32x4 (_mm512_setr_epi8 does not exist).  */
    const FASTENT_SIMD_VEC popcnt_lut = _mm512_broadcast_i32x4(
      _mm_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4));
#endif
#elif defined(FASTENT_VARIANT_NEON)
  /*  Unused on NEON: vcntq_u8 replaces the LUT lookups.  */
#elif defined(FASTENT_VARIANT_WASM128)
  /*  Unused on WASM: wasm_i8x16_popcnt replaces the LUT lookups.  */
#else
  const FASTENT_SIMD_VEC popcnt_lut = _mm_setr_epi8(
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
#endif
#if !((defined(FASTENT_VARIANT_AVX512) && defined(FASTENT_AVX512_HAVE_BITALG)) \
      || defined(FASTENT_VARIANT_NEON) \
      || defined(FASTENT_VARIANT_WASM128))
  const FASTENT_SIMD_VEC nibble_mask = V_SET1_EPI8(0x0F);
#endif
  const FASTENT_SIMD_VEC mask_7f     = V_SET1_EPI8(0x7F);
  const FASTENT_SIMD_VEC mask_01     = V_SET1_EPI8(0x01);
  const FASTENT_SIMD_VEC zero        = V_SETZERO();

  FASTENT_SIMD_VEC acc_ones   = V_SETZERO();
  FASTENT_SIMD_VEC acc_within = V_SETZERO();
  FASTENT_SIMD_VEC acc_cross  = V_SETZERO();

  /*  MC Pi state hoisted into locals.  */
  i32 mc_pos     = st->mc_pos;
  u8  m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8  m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];
  u64 mc_count   = st->mc_count;
  u64 mc_inside  = st->mc_inside;

  for (i = 0; i < body_end; i += FASTENT_SIMD_VLEN) {
    PREFETCH_R(buf + i + 512);

    FASTENT_SIMD_VEC va = V_LOAD(buf + i);
    FASTENT_SIMD_VEC vb = V_LOAD(buf + i + 1);
    if (fold) {
      va = FASTENT_FN(fold_vec_inline)(va);
      vb = FASTENT_FN(fold_vec_inline)(vb);
    }

#if defined(FASTENT_VARIANT_AVX512) && defined(FASTENT_AVX512_HAVE_BITALG)
    /*  Byte-wise popcount via VPOPCNTB, PSADBW reduce to qword.  */
    FASTENT_SIMD_VEC pc_va = _mm512_popcnt_epi8(va);
    acc_ones = V_ADD_EPI64(acc_ones, V_SAD_EPU8(pc_va, zero));

    /*  Within-byte adjacent-1 pairs.  */
    FASTENT_SIMD_VEC va_shr1 = V_AND(V_SRLI_EPI16(va, 1), mask_7f);
    FASTENT_SIMD_VEC pairs   = V_AND(va, va_shr1);
    FASTENT_SIMD_VEC pc_pairs = _mm512_popcnt_epi8(pairs);
    acc_within = V_ADD_EPI64(acc_within, V_SAD_EPU8(pc_pairs, zero));
#elif defined(FASTENT_VARIANT_NEON)
    /*  Byte-wise popcount via vcntq_u8; vpaddl ladder in V_SAD_EPU8.  */
    FASTENT_SIMD_VEC pc_va = vcntq_u8(va);
    acc_ones = V_ADD_EPI64(acc_ones, V_SAD_EPU8(pc_va, zero));

    FASTENT_SIMD_VEC va_shr1 = V_AND(V_SRLI_EPI16(va, 1), mask_7f);
    FASTENT_SIMD_VEC pairs   = V_AND(va, va_shr1);
    FASTENT_SIMD_VEC pc_pairs = vcntq_u8(pairs);
    acc_within = V_ADD_EPI64(acc_within, V_SAD_EPU8(pc_pairs, zero));
#elif defined(FASTENT_VARIANT_WASM128)
    /*  Byte-wise popcount via wasm_i8x16_popcnt; ladder in V_SAD_EPU8.  */
    FASTENT_SIMD_VEC pc_va = wasm_i8x16_popcnt(va);
    acc_ones = V_ADD_EPI64(acc_ones, V_SAD_EPU8(pc_va, zero));

    FASTENT_SIMD_VEC va_shr1 = V_AND(V_SRLI_EPI16(va, 1), mask_7f);
    FASTENT_SIMD_VEC pairs   = V_AND(va, va_shr1);
    FASTENT_SIMD_VEC pc_pairs = wasm_i8x16_popcnt(pairs);
    acc_within = V_ADD_EPI64(acc_within, V_SAD_EPU8(pc_pairs, zero));
#else
    /*  popcount(va) byte-wise via PSHUFB-LUT.  */
    FASTENT_SIMD_VEC lo = V_AND(va, nibble_mask);
    FASTENT_SIMD_VEC hi = V_AND(V_SRLI_EPI16(va, 4), nibble_mask);
    FASTENT_SIMD_VEC pc_va = V_ADD_EPI8(V_SHUFFLE_EPI8(popcnt_lut, lo),
                                         V_SHUFFLE_EPI8(popcnt_lut, hi));
    acc_ones = V_ADD_EPI64(acc_ones, V_SAD_EPU8(pc_va, zero));

    /*  popcount(va & (va>>1)); SRLI16 + AND 0x7F = byte-wise >>1.  */
    FASTENT_SIMD_VEC va_shr1 = V_AND(V_SRLI_EPI16(va, 1), mask_7f);
    FASTENT_SIMD_VEC pairs   = V_AND(va, va_shr1);
    FASTENT_SIMD_VEC plo = V_AND(pairs, nibble_mask);
    FASTENT_SIMD_VEC phi = V_AND(V_SRLI_EPI16(pairs, 4), nibble_mask);
    FASTENT_SIMD_VEC pc_pairs = V_ADD_EPI8(V_SHUFFLE_EPI8(popcnt_lut, plo),
                                            V_SHUFFLE_EPI8(popcnt_lut, phi));
    acc_within = V_ADD_EPI64(acc_within, V_SAD_EPU8(pc_pairs, zero));
#endif

    /*  Cross-byte (va.LSB & vb.MSB) per lane; AND 1 after the epi16
        shift drops the adjacent byte's contamination.  */
    FASTENT_SIMD_VEC va_lsb = V_AND(va, mask_01);
    FASTENT_SIMD_VEC vb_msb = V_AND(V_SRLI_EPI16(vb, 7), mask_01);
    FASTENT_SIMD_VEC cross  = V_AND(va_lsb, vb_msb);
    acc_cross = V_ADD_EPI64(acc_cross, V_SAD_EPU8(cross, zero));

    /*  MC Pi: scalar drain + SIMD bulk + scalar tail + stash; fold
        mode uses the laundered L1 stage (see launder note above).  */
    /*  VLEN-aligned: V_STORE may lower to an aligned move that #GPs on a
        sub-VLEN address.  */
    ALIGN(FASTENT_SIMD_VLEN) u8 bits_stage[FASTENT_SIMD_VLEN + 16];
    FASTENT_STAGE_PTR p;
    if (fold) {
      V_STORE(bits_stage, va);
      FASTENT_STAGE_PTR sp = bits_stage;
      FASTENT_LAUNDER(sp);
      p = sp;
    } else {
      p = buf + i;
    }
#define MC_HEXAD(x0, x1, x2, y0, y1, y2) do { \
      u32 x = ((u32) (x0) << 16) | ((u32) (x1) << 8) | (u32) (x2); \
      u32 y = ((u32) (y0) << 16) | ((u32) (y1) << 8) | (u32) (y2); \
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y; \
      mc_count++; \
      mc_inside += (d <= FASTENT_INCIRC); \
    } while (0)
    u32 p_idx;
    switch (mc_pos) {
      case 0: p_idx = 0; break;
      case 1: m1=p[0]; m2=p[1]; m3=p[2]; m4=p[3]; m5=p[4];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 5; break;
      case 2: m2=p[0]; m3=p[1]; m4=p[2]; m5=p[3];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 4; break;
      case 3: m3=p[0]; m4=p[1]; m5=p[2];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 3; break;
      case 4: m4=p[0]; m5=p[1];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 2; break;
      default: m5=p[0];
              MC_HEXAD(m0,m1,m2,m3,m4,m5); p_idx = 1; break;
    }
    u32 n_hexads = ((u32) FASTENT_SIMD_VLEN - p_idx) / 6u;
    /*  SIMD bulk (byte-mode pattern); mi_simd counts hits, mc_count
        gets all n_hexads added below.  */
    u64 mi_simd = 0;
    const u8 * q = p + p_idx;
#if !defined(FASTENT_VARIANT_NEON) && !defined(FASTENT_VARIANT_WASM128)
    const __m128i mc_shuf = _mm_setr_epi8(2, 1, 0, -1, 5, 4, 3, -1,
                                          8, 7, 6, -1, 11, 10, 9, -1);
    const __m128i mc_lim  = _mm_set1_epi64x((i64) (FASTENT_INCIRC + 1ULL));
#endif
    u32 k = 0;
#if defined(FASTENT_VARIANT_AVX2) || defined(FASTENT_VARIANT_AVX512)
    /*  256-bit: 4 hexads (24 bytes) per iter.  */
    const __m256i mc_shuf256 = _mm256_broadcastsi128_si256(mc_shuf);
    const __m256i mc_lim256  = _mm256_set1_epi64x((i64) (FASTENT_INCIRC + 1ULL));
    for (; k + 4 <= n_hexads; k += 4) {
      __m128i v0_xmm = _mm_loadu_si128((const __m128i *) (q + k * 6u));
      __m128i v1_xmm = _mm_loadu_si128((const __m128i *) (q + k * 6u + 12u));
      __m256i v   = _mm256_inserti128_si256(_mm256_castsi128_si256(v0_xmm),
                                            v1_xmm, 1);
      __m256i xy  = _mm256_shuffle_epi8(v, mc_shuf256);
      __m256i xs  = _mm256_mul_epu32(xy, xy);
      __m256i yshr = _mm256_srli_epi64(xy, 32);
      __m256i ys  = _mm256_mul_epu32(yshr, yshr);
      __m256i d   = _mm256_add_epi64(xs, ys);
      __m256i mask = _mm256_cmpgt_epi64(mc_lim256, d);
      int bits = _mm256_movemask_pd(_mm256_castsi256_pd(mask));
      mi_simd += (u64) FASTENT_POPCOUNT32(bits);
    }
#endif
    /*  128-bit residual: 2 hexads/iter. Needs SSE4.2 cmpgt_epi64
        (present with AVX2/SSE4.1); SSSE3-only falls to scalar.  */
#if defined(FASTENT_VARIANT_AVX2) || defined(FASTENT_VARIANT_SSE41) \
 || defined(FASTENT_VARIANT_AVX512)
    for (; k + 2 <= n_hexads; k += 2) {
      __m128i v   = _mm_loadu_si128((const __m128i *) (q + k * 6u));
      __m128i xy  = _mm_shuffle_epi8(v, mc_shuf);
      __m128i xs  = _mm_mul_epu32(xy, xy);
      __m128i yshr = _mm_srli_epi64(xy, 32);
      __m128i ys  = _mm_mul_epu32(yshr, yshr);
      __m128i d   = _mm_add_epi64(xs, ys);
      __m128i mask = _mm_cmpgt_epi64(mc_lim, d);
      int bits = _mm_movemask_pd(_mm_castsi128_pd(mask));
      mi_simd += (u64) FASTENT_POPCOUNT32(bits);
    }
#elif !defined(FASTENT_VARIANT_NEON) && !defined(FASTENT_VARIANT_WASM128)
    (void) mc_shuf; (void) mc_lim;
#endif
    /*  Scalar tail (0/1 hexad on AVX2+SSE41; full on SSSE3).  */
    for (; k < n_hexads; k++) {
      u32 o = k * 6u;
      u32 x = ((u32) q[o + 0] << 16) | ((u32) q[o + 1] << 8) | q[o + 2];
      u32 y = ((u32) q[o + 3] << 16) | ((u32) q[o + 4] << 8) | q[o + 5];
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
      mi_simd += (d <= FASTENT_INCIRC);
    }
    mc_count  += n_hexads;
    mc_inside += mi_simd;
    u32 stash_at    = p_idx + n_hexads * 6u;
    u32 stash_count = (u32) FASTENT_SIMD_VLEN - stash_at;
    mc_pos = (i32) stash_count;
    if (stash_count >= 1) m0 = p[stash_at + 0];
    if (stash_count >= 2) m1 = p[stash_at + 1];
    if (stash_count >= 3) m2 = p[stash_at + 2];
    if (stash_count >= 4) m3 = p[stash_at + 3];
    if (stash_count >= 5) m4 = p[stash_at + 4];
#undef MC_HEXAD
  }

  /*  Reduce acc_ones / acc_within / acc_cross (i64 lanes).  */
#if defined(FASTENT_VARIANT_AVX512)
  u64 sum_ones   = (u64) _mm512_reduce_add_epi64(acc_ones);
  u64 sum_within = (u64) _mm512_reduce_add_epi64(acc_within);
  u64 sum_cross  = (u64) _mm512_reduce_add_epi64(acc_cross);
#elif defined(FASTENT_VARIANT_AVX2)
  __m128i lo_ones = _mm256_castsi256_si128(acc_ones);
  __m128i hi_ones = _mm256_extracti128_si256(acc_ones, 1);
  __m128i s_ones  = _mm_add_epi64(lo_ones, hi_ones);
  u64 sum_ones = (u64) _mm_cvtsi128_si64(s_ones)
              + (u64) _mm_cvtsi128_si64(_mm_unpackhi_epi64(s_ones, s_ones));
  __m128i lo_w = _mm256_castsi256_si128(acc_within);
  __m128i hi_w = _mm256_extracti128_si256(acc_within, 1);
  __m128i s_w  = _mm_add_epi64(lo_w, hi_w);
  u64 sum_within = (u64) _mm_cvtsi128_si64(s_w)
              + (u64) _mm_cvtsi128_si64(_mm_unpackhi_epi64(s_w, s_w));
  __m128i lo_c = _mm256_castsi256_si128(acc_cross);
  __m128i hi_c = _mm256_extracti128_si256(acc_cross, 1);
  __m128i s_c  = _mm_add_epi64(lo_c, hi_c);
  u64 sum_cross = (u64) _mm_cvtsi128_si64(s_c)
              + (u64) _mm_cvtsi128_si64(_mm_unpackhi_epi64(s_c, s_c));
#elif defined(FASTENT_VARIANT_NEON)
  u64 sum_ones   = fastent_neon_addvq_u64(vreinterpretq_u64_u8(acc_ones));
  u64 sum_within = fastent_neon_addvq_u64(vreinterpretq_u64_u8(acc_within));
  u64 sum_cross  = fastent_neon_addvq_u64(vreinterpretq_u64_u8(acc_cross));
#elif defined(FASTENT_VARIANT_WASM128)
  u64 sum_ones   = (u64) wasm_i64x2_extract_lane(acc_ones, 0)
                 + (u64) wasm_i64x2_extract_lane(acc_ones, 1);
  u64 sum_within = (u64) wasm_i64x2_extract_lane(acc_within, 0)
                 + (u64) wasm_i64x2_extract_lane(acc_within, 1);
  u64 sum_cross  = (u64) wasm_i64x2_extract_lane(acc_cross, 0)
                 + (u64) wasm_i64x2_extract_lane(acc_cross, 1);
#else
  u64 sum_ones = (u64) _mm_cvtsi128_si64(acc_ones)
    + (u64) _mm_cvtsi128_si64(_mm_unpackhi_epi64(acc_ones, acc_ones));
  u64 sum_within = (u64) _mm_cvtsi128_si64(acc_within)
    + (u64) _mm_cvtsi128_si64(_mm_unpackhi_epi64(acc_within, acc_within));
  u64 sum_cross = (u64) _mm_cvtsi128_si64(acc_cross)
    + (u64) _mm_cvtsi128_si64(_mm_unpackhi_epi64(acc_cross, acc_cross));
#endif

  st->bit_hist[1]   += sum_ones;
  st->bit_hist[0]   += (u64) body_end * 8u - sum_ones;
  st->cross_product += (i64) sum_within + (i64) sum_cross;
  st->total_bytes   += (u64) body_end * 8u;

  /*  Epilogue: buf[body_end]'s cross-byte pair was counted by the
      last shifted load; still needs popcount + MC, done scalar.  */
  {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
    u32 ones_b = (u32) FASTENT_POPCOUNT32(b);
    st->bit_hist[1] += ones_b;
    st->bit_hist[0] += 8u - ones_b;
    u32 within_b = (u32) FASTENT_POPCOUNT32(b & (b >> 1));
    st->cross_product += (i64) within_b;
    st->carry_byte = (u8) (b & 1u);
    st->last_byte  = (u8) (b & 1u);
    st->total_bytes += 8;

    switch (mc_pos) {
      case 0: m0 = b; break;
      case 1: m1 = b; break;
      case 2: m2 = b; break;
      case 3: m3 = b; break;
      case 4: m4 = b; break;
      default: m5 = b; break;
    }
    mc_pos++;
    if (mc_pos == 6) {
      u32 x = ((u32) m0 << 16) | ((u32) m1 << 8) | (u32) m2;
      u32 y = ((u32) m3 << 16) | ((u32) m4 << 8) | (u32) m5;
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
      mc_count++;
      mc_inside += (d <= FASTENT_INCIRC);
      mc_pos = 0;
    }
  }

  st->mc_pos = mc_pos;
  st->mc_buf[0] = m0; st->mc_buf[1] = m1; st->mc_buf[2] = m2;
  st->mc_buf[3] = m3; st->mc_buf[4] = m4; st->mc_buf[5] = m5;
  st->mc_count  = mc_count;
  st->mc_inside = mc_inside;

  return body_end + 1;
}

/*  Bit-mode SIMD trampolines.  */
static inline sz FASTENT_FN(bits_simd_body)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len) {
  return FASTENT_FN(bits_simd_body_impl)(st, buf, len, 0);
}
static inline sz FASTENT_FN(bits_simd_body_fold)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len) {
  return FASTENT_FN(bits_simd_body_impl)(st, buf, len, 1);
}

#endif  /*  FASTENT_HAVE_SIMD  */

/*  Scalar bit-mode walker (scalar entry and SIMD tail). Templated on
    `fold` for the fused -f -b path.  */
static INLINE void
FASTENT_FN(bits_scalar_body_impl)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len,
    int fold) {
  sz i;
  for (i = 0; i < len; i++) {
    const u8 byte = fold ? FASTENT_FN(fold_byte_inline)(buf[i]) : buf[i];
    const u32 ones_in_byte = (u32) FASTENT_POPCOUNT32(byte);
    st->bit_hist[1] += ones_in_byte;
    st->bit_hist[0] += 8u - ones_in_byte;
    const u32 within = (u32) FASTENT_POPCOUNT32(byte & (byte >> 1));
    st->cross_product += (i64) within;
    if (st->have_carry) {
      const u32 prev_lsb = (u32) (st->carry_byte & 1u);
      const u32 curr_msb = (u32) ((byte >> 7) & 1u);
      st->cross_product += (i64) (prev_lsb & curr_msb);
    } else {
      st->first_byte = (u8) ((byte >> 7) & 1u);
      st->have_first = 1;
    }
    st->carry_byte = (u8) (byte & 1u);
    st->last_byte  = (u8) (byte & 1u);
    st->have_carry = 1;
    st->total_bytes += 8;

    st->mc_buf[st->mc_pos++] = byte;
    if (st->mc_pos >= 6) {
      const u32 x = ((u32) st->mc_buf[0] << 16) | ((u32) st->mc_buf[1] << 8)
                  |  (u32) st->mc_buf[2];
      const u32 y = ((u32) st->mc_buf[3] << 16) | ((u32) st->mc_buf[4] << 8)
                  |  (u32) st->mc_buf[5];
      const u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
      st->mc_count++;
      st->mc_inside += (d <= FASTENT_INCIRC);
      st->mc_pos = 0;
    }
  }
}

static inline void FASTENT_FN(bits_scalar_body)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len) {
  FASTENT_FN(bits_scalar_body_impl)(st, buf, len, 0);
}

static INLINE void
FASTENT_FN(analyze_bits_impl)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len,
    int fold) {
  if (len == 0) return;
#ifdef FASTENT_HAVE_SIMD
  sz body = fold ? FASTENT_FN(bits_simd_body_fold)(st, buf, len)
                 : FASTENT_FN(bits_simd_body)(st, buf, len);
  FASTENT_FN(bits_scalar_body_impl)(st, buf + body, len - body, fold);
#else
  FASTENT_FN(bits_scalar_body_impl)(st, buf, len, fold);
#endif
}

void FASTENT_FN(analyze_bits)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len) {
  FASTENT_FN(analyze_bits_impl)(st, buf, len, 0);
}

void FASTENT_FN(analyze_bits_fold)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len) {
  FASTENT_FN(analyze_bits_impl)(st, buf, len, 1);
}

/*  Byte-mode order-1 digram + longest-run kernel (-ee level-2).  */

/*  Inlined copy of fastent_lr_run (analyze.c).  Identical logic; the
    hot SIMD path must not pay a non-inlined call per run boundary
    (~99.6% boundary density on random input).  */
static INLINE void
FASTENT_FN(lr_run_i)(fastent_chunk_state * st, u32 q, u64 n) {
  if (!st->lr_have) {
    st->lr_have = 1;  st->lr_sym = (u8) q;  st->lr_cur = n;
    st->lr_head_sym = (u8) q;  st->lr_head_len = n;  st->lr_head_open = 1;
  } else if (q == st->lr_sym) {
    st->lr_cur += n;
    if (st->lr_head_open) st->lr_head_len += n;
  } else {
    if (st->lr_cur > st->lr_max) st->lr_max = st->lr_cur;
    st->lr_sym = (u8) q;  st->lr_cur = n;  st->lr_head_open = 0;
  }
}

static INLINE void
FASTENT_FN(digram_scalar_run)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len) {
  u32 * RESTRICT t = st->dg_u32;
  u32 prev;
  sz i = 0;
  if (st->dg_have) { prev = st->dg_prev; } else {
    prev = buf[0];
    fastent_lr_one(st, prev);
    i = 1;
    st->dg_have = 1;
  }
  for (; i + 2 <= len; i += 2) {
    u32 c0 = buf[i], c1 = buf[i + 1];
    t[0u * FASTENT_BG_TABLE + ((prev << 8) | c0)]++;  fastent_lr_one(st, c0);
    t[1u * FASTENT_BG_TABLE + ((c0   << 8) | c1)]++;  fastent_lr_one(st, c1);
    prev = c1;
  }
  for (; i < len; i++) {
    u32 c = buf[i];
    t[(prev << 8) | c]++;  fastent_lr_one(st, c);
    prev = c;
  }
  st->dg_prev = (u8) prev;
}

#ifdef FASTENT_HAVE_SIMD
#if !defined(FASTENT_VARIANT_AVX512)
/*  W-bit equality mask: bit j set iff buf[base+j] == buf[base+j-1].
    base >= 1 and base+W <= len so both loads are in bounds.  AVX-512
    produces the mask natively (k register) and is handled inline.  */
static INLINE u64
FASTENT_FN(eq_mask)(const u8 * RESTRICT buf, sz base) {
  FASTENT_SIMD_VEC v  = V_LOAD(buf + base);
  FASTENT_SIMD_VEC vp = V_LOAD(buf + base - 1);
  FASTENT_SIMD_VEC eq = V_CMPEQ_EPI8(v, vp);
#if defined(FASTENT_VARIANT_NEON)
  /*  No movemask on NEON: narrow each 16-bit lane to 4 bits via
      shrn so a 128-bit compare result becomes a 64-bit value with
      4 bits per byte; bit (4*j) is the per-byte equal flag.  */
  uint8x8_t nn = vshrn_n_u16(vreinterpretq_u16_u8(eq), 4);
  u64 packed = vget_lane_u64(vreinterpret_u64_u8(nn), 0);
  u64 m = 0;
  Fi(16, if ((packed >> (u32) (i * 4)) & 0xFu) m |= (u64) 1u << i);
  return m;
#elif defined(FASTENT_VARIANT_WASM128)
  return (u64) (u16) wasm_i8x16_bitmask(eq);
#else
  return (u64) V_MOVEMASK_EPI8(eq);
#endif
}
#endif
#endif


/*  Scatter digram pairs of buf[i0..len-1] into st->dg_u32, feed the run
    machine (boundary buf[i]!=buf[i-1]).  */
void FASTENT_FN(digram_bytes)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz len) {
#ifndef FASTENT_HAVE_SIMD
  FASTENT_FN(digram_scalar_run)(st, buf, len);
#else
  i32 i;
  if (len == 0) return;
  u32 * RESTRICT t = st->dg_u32;

  sz i0;
  if (st->dg_have) { i0 = 0; } else {
    fastent_lr_one(st, buf[0]);   /*  bootstrap: buf[0], no left pair  */
    st->dg_prev = buf[0];
    st->dg_have = 1;
    i0 = 1;
  }
  if (i0 >= len) { st->dg_prev = buf[len - 1]; return; }

  /*  Run scan state: the run currently open starts at runstart with
      symbol runsym; closed runs are flushed to fastent_lr_run.  */
  sz  runstart = i0;
  u32 runsym   = buf[i0];

  /*  Scalar prologue: the SIMD window needs buf[base-1], so starts at index
      >= 1.  */
  sz k = i0;
  if (k == 0) {
    u32 c = buf[0];
    t[((u32) (0u & 1u)) * FASTENT_BG_TABLE
      + (((u32) st->dg_prev << 8) | c)]++;
    k = 1;
  }

  const sz NB_BLK = 64;
  while (k >= 1 && k + NB_BLK <= len) {
    u64 bnd;
#if defined(FASTENT_VARIANT_AVX512)
    {
      __m512i v  = _mm512_loadu_si512((const void *)(buf + k));
      __m512i vp = _mm512_loadu_si512((const void *)(buf + k - 1));
      bnd = ~(u64) _mm512_cmpeq_epi8_mask(v, vp);
    }
#elif FASTENT_SIMD_VLEN == 64
    bnd = ~FASTENT_FN(eq_mask)(buf, k);
#elif FASTENT_SIMD_VLEN == 32
    {
      u64 m0 = FASTENT_FN(eq_mask)(buf, k) & 0xFFFFFFFFull;
      u64 m1 = FASTENT_FN(eq_mask)(buf, k + 32) & 0xFFFFFFFFull;
      bnd = ~(m0 | (m1 << 32));
    }
#else  /*  VLEN == 16  */
    {
      u64 m0 = FASTENT_FN(eq_mask)(buf, k)      & 0xFFFFull;
      u64 m1 = FASTENT_FN(eq_mask)(buf, k + 16) & 0xFFFFull;
      u64 m2 = FASTENT_FN(eq_mask)(buf, k + 32) & 0xFFFFull;
      u64 m3 = FASTENT_FN(eq_mask)(buf, k + 48) & 0xFFFFull;
      bnd = ~(m0 | (m1 << 16) | (m2 << 32) | (m3 << 48));
    }
#endif

    /*  Digram keys for buf[k..k+63]: vectorised index production, scalar
        inc-mem scatter (stays scalar per the Zen4-scatter rationale).  */
    {
      ALIGN(64) u16 keys[64];
      FASTENT_STAGE_PTR sp = buf + k;
      FASTENT_LAUNDER(sp);
      Fi(64, keys[i] = (u16) (((u32) sp[(sz) i - 1] << 8) | sp[i]));
      {
        u32 * RESTRICT t0 = t;
        u32 * RESTRICT t1 = t + FASTENT_BG_TABLE;
        const u32 par = (u32) (k & 1u);
        Fi(64,
          if (i + 8 < 64) PREFETCH(&t0[keys[i + 8]]);
          if (((u32) i ^ par) & 1u) t1[keys[i]]++;
          else                      t0[keys[i]]++);
      }
    }

    /*  Boundary bit j (abs k..k+63): buf[k+j]!=buf[k+j-1].  */
    if (bnd == ~(u64) 0) {
      FASTENT_FN(lr_run_i)(st, runsym, (u64) (k - runstart));
      FASTENT_FN(lr_run_i)(st, buf[k], 1u);
      st->lr_sym = buf[k + 62];   /*  collapse singletons 2..63  */
      runstart = k + 63;
      runsym   = buf[k + 63];
    } else {
      u64 b = bnd;
      while (b) {
        const sz bpos = k + FASTENT_CTZ64(b);
        FASTENT_FN(lr_run_i)(st, runsym, (u64) (bpos - runstart));
        runstart = bpos;
        runsym   = buf[bpos];
        b &= b - 1;
      }
    }
    k += NB_BLK;
  }

  /*  Scalar tail: remaining pairs + remaining boundaries.  */
  for (; k < len; k++) {
    u32 c = buf[k];
    t[(u32) (k & 1u) * FASTENT_BG_TABLE + (((u32) buf[k - 1] << 8) | c)]++;
    if (buf[k] != buf[k - 1]) {
      FASTENT_FN(lr_run_i)(st, runsym, (u64) (k - runstart));
      runstart = k;
      runsym   = buf[k];
    }
  }

  /*  Close the final open run [runstart .. len-1].  */
  FASTENT_FN(lr_run_i)(st, runsym, (u64) (len - runstart));
  st->dg_prev = buf[len - 1];
#endif
}

/*  Bit -ee level-2 fused block kernel, one per ISA.  */

/*  +8 padding words: the AVX-512 B1/B2 loop loads succ = W[w+1] one
    vector wide, so reading W[NW..NW+7] must stay in bounds and read
    zero (matches the scalar (w+1<NW)?W[w+1]:0 boundary).  */
#define FASTENT_DG_BITS_WORDS_T ((65536u / 8u) + 8u)

/*  Longest run of consecutive 0-bits of t strictly between its lowest (lo)
    and highest (hi) set bit, +1 = the largest adjacent set-bit distance
    inside this word.  */
static INLINE u64
FASTENT_FN(maxgap_in_word)(u64 t, u32 lo, u32 hi) {
  u64 lomask, himask, z, y;
  u32 k;
  if (hi <= lo) return 0;
  lomask = ~(((u64) 1 << (lo + 1)) - 1ull);
  himask = ((u64) 1 << hi) - 1ull;
  z = (~t) & lomask & himask;
  y = z;  k = 0;
  while (y) { y &= y << 1; k++; }
  return (u64) k + 1ull;
}

/*  Signed nibble LUTs for the +-1 cusum walk (4 bits, LSB-first): net = end
    value, mn = min prefix, mx = max prefix.  */
#define FASTENT_DG_NIB_NET { -4,-2,-2, 0,-2, 0, 0, 2,-2, 0, 0, 2, 0, 2, 2, 4 }
#define FASTENT_DG_NIB_MN  { -4,-2,-2, 0,-2, 0,-1, 0,-3,-1,-1, 0,-2, 0,-1, 0 }
#define FASTENT_DG_NIB_MX  {  0, 1, 0, 2, 0, 1, 1, 3, 0, 1, 0, 2, 0, 2, 2, 4 }

#if defined(FASTENT_VARIANT_AVX512)
/*  Rotate every 64-bit lane left by nb bytes within the lane (the 8
    bytes wrap), so a 1/2/4-byte rotate + min/max tree reduces all 8
    lane bytes to the lane's horizontal min/max in 3 steps.  */
static INLINE __m512i
FASTENT_FN(rotl_lane8)(__m512i v, int nb) {
  const int s = nb * 8;
  return _mm512_or_si512(_mm512_slli_epi64(v, s),
                         _mm512_srli_epi64(v, 64 - s));
}
/*  Exclusive prefix sum over the 8 i64 lanes (lane 0 = first word):
    Hillis-Steele inclusive then subtract self.  */
static INLINE __m512i
FASTENT_FN(expref_i64)(__m512i v) {
  __m512i t = v;
  t = _mm512_add_epi64(t, _mm512_maskz_permutexvar_epi64(0xFE,
        _mm512_set_epi64(6,5,4,3,2,1,0,0), t));
  t = _mm512_add_epi64(t, _mm512_maskz_permutexvar_epi64(0xFC,
        _mm512_set_epi64(5,4,3,2,1,0,0,0), t));
  t = _mm512_add_epi64(t, _mm512_maskz_permutexvar_epi64(0xF0,
        _mm512_set_epi64(3,2,1,0,0,0,0,0), t));
  return _mm512_sub_epi64(t, v);
}
#endif

#if defined(FASTENT_VARIANT_AVX512) || defined(FASTENT_VARIANT_AVX2) \
 || defined(FASTENT_VARIANT_SSE41)  || defined(FASTENT_VARIANT_SSSE3)
#define FASTENT_DG_B4_SIMD 1
/*  SIMD B4 over the leading full words; returns count consumed (lane-width
    multiple), caller's scalar loop does the rest + the partial.  */
static INLINE sz
FASTENT_FN(cusum_full_words)(
    const u64 * W, sz nfull, i64 * po, i64 * pgmn, i64 * pgmx) {
  static const i8 nnet[16] = FASTENT_DG_NIB_NET;
  static const i8 nmn[16]  = FASTENT_DG_NIB_MN;
  static const i8 nmx[16]  = FASTENT_DG_NIB_MX;
#if defined(FASTENT_VARIANT_AVX512)
  enum { LW = 8 };
  const __m512i Lnet = _mm512_broadcast_i32x4(
      _mm_loadu_si128((const void *) nnet));
  const __m512i Lmn  = _mm512_broadcast_i32x4(
      _mm_loadu_si128((const void *) nmn));
  const __m512i Lmx  = _mm512_broadcast_i32x4(
      _mm_loadu_si128((const void *) nmx));
  const __m512i m0f  = _mm512_set1_epi8(0x0F);
  sz w;  i64 o = *po, g1 = *pgmn, g2 = *pgmx;
  __m512i vmin = _mm512_set1_epi64(g1);
  __m512i vmax = _mm512_set1_epi64(g2);
  for (w = 0; w + LW <= nfull; w += LW) {
    __m512i a   = _mm512_loadu_si512((const void *)(W + w));
    __m512i lo  = _mm512_and_si512(a, m0f);
    __m512i hi  = _mm512_and_si512(_mm512_srli_epi16(a, 4), m0f);
    __m512i nl  = _mm512_shuffle_epi8(Lnet, lo);
    __m512i bnet = _mm512_add_epi8(nl, _mm512_shuffle_epi8(Lnet, hi));
    __m512i bmin = _mm512_min_epi8(_mm512_shuffle_epi8(Lmn, lo),
                     _mm512_add_epi8(nl, _mm512_shuffle_epi8(Lmn, hi)));
    __m512i bmax = _mm512_max_epi8(_mm512_shuffle_epi8(Lmx, lo),
                     _mm512_add_epi8(nl, _mm512_shuffle_epi8(Lmx, hi)));
    /*  In-lane byte prefix sum via 64-bit shifts, kept in i8 (8-byte
        cumulative net + extreme stays in [-72,72]).  */
    {
      __m512i incl = bnet;
      incl = _mm512_add_epi8(incl, _mm512_slli_epi64(incl, 8));
      incl = _mm512_add_epi8(incl, _mm512_slli_epi64(incl, 16));
      incl = _mm512_add_epi8(incl, _mm512_slli_epi64(incl, 32));
      {
        __m512i excl = _mm512_sub_epi8(incl, bnet);
        __m512i mn   = _mm512_add_epi8(excl, bmin);
        __m512i mx   = _mm512_add_epi8(excl, bmax);
        __m512i wnet, opref;
        mn = _mm512_min_epi8(mn, FASTENT_FN(rotl_lane8)(mn, 1));
        mn = _mm512_min_epi8(mn, FASTENT_FN(rotl_lane8)(mn, 2));
        mn = _mm512_min_epi8(mn, FASTENT_FN(rotl_lane8)(mn, 4));
        mx = _mm512_max_epi8(mx, FASTENT_FN(rotl_lane8)(mx, 1));
        mx = _mm512_max_epi8(mx, FASTENT_FN(rotl_lane8)(mx, 2));
        mx = _mm512_max_epi8(mx, FASTENT_FN(rotl_lane8)(mx, 4));
        /*  Lane min/max is replicated in every byte of the lane; the
            low byte truncate (VPMOVQB) then sign-extend gives one i64
            per word.  Lane byte 7 (arith >> 56) is the word net.  */
        wnet  = _mm512_srai_epi64(incl, 56);
        opref = _mm512_add_epi64(_mm512_set1_epi64(o),
                                 FASTENT_FN(expref_i64)(wnet));
        vmin  = _mm512_min_epi64(vmin, _mm512_add_epi64(opref,
                  _mm512_cvtepi8_epi64(_mm512_cvtepi64_epi8(mn))));
        vmax  = _mm512_max_epi64(vmax, _mm512_add_epi64(opref,
                  _mm512_cvtepi8_epi64(_mm512_cvtepi64_epi8(mx))));
        o += _mm512_reduce_add_epi64(wnet);
      }
    }
  }
  g1 = _mm512_reduce_min_epi64(vmin);
  g2 = _mm512_reduce_max_epi64(vmax);
  *po = o;  *pgmn = g1;  *pgmx = g2;
  return w;
#else
  /*  AVX2 (4 words/lane) / SSE (2 words/lane) share the i64-lane
      formulation; vector width LW from the V_ macros.  */
  enum { LW = FASTENT_SIMD_VLEN / 8 };
  sz w;  i64 o = *po, g1 = *pgmn, g2 = *pgmx;
  (void) nnet;  (void) nmn;  (void) nmx;
  for (w = 0; w + LW <= nfull; w += LW) {
    int k;
    for (k = 0; k < LW; k++) {
      const u64 a = W[w + k];
      int j;  i64 r = 0;
      for (j = 0; j < 8; j++) {
        const u32 v = (u32) ((a >> (j * 8)) & 0xFFu);
        const i32 lo = v & 15u, hi = (v >> 4) & 15u;
        const i32 cn = nnet[lo] + nnet[hi];
        i32 cmn = nmn[lo];
        i32 cmx = nmx[lo];
        if (nnet[lo] + nmn[hi] < cmn) cmn = nnet[lo] + nmn[hi];
        if (nnet[lo] + nmx[hi] > cmx) cmx = nnet[lo] + nmx[hi];
        if (o + r + cmn < g1) g1 = o + r + cmn;
        if (o + r + cmx > g2) g2 = o + r + cmx;
        r += cn;
      }
      o += r;
    }
  }
  *po = o;  *pgmn = g1;  *pgmx = g2;
  return w;
#endif
}
#endif

#if defined(FASTENT_VARIANT_AVX512) && defined(FASTENT_AVX512_HAVE_BITALG)
#define FASTENT_DG_B3_SIMD 1
/*  In-lane prefix-OR: bit i := OR of bits 0..i within each 64-bit lane
    (monotone: 0 below the lowest set bit, 1 from it up).  */
static INLINE __m512i FASTENT_FN(pfx_or64)(__m512i v) {
  v = _mm512_or_si512(v, _mm512_slli_epi64(v, 1));
  v = _mm512_or_si512(v, _mm512_slli_epi64(v, 2));
  v = _mm512_or_si512(v, _mm512_slli_epi64(v, 4));
  v = _mm512_or_si512(v, _mm512_slli_epi64(v, 8));
  v = _mm512_or_si512(v, _mm512_slli_epi64(v, 16));
  v = _mm512_or_si512(v, _mm512_slli_epi64(v, 32));
  return v;
}
/*  In-lane suffix-OR: bit i := OR of bits i..63 (monotone the other
    way: 1 up to the highest set bit, 0 above).  */
static INLINE __m512i FASTENT_FN(sfx_or64)(__m512i v) {
  v = _mm512_or_si512(v, _mm512_srli_epi64(v, 1));
  v = _mm512_or_si512(v, _mm512_srli_epi64(v, 2));
  v = _mm512_or_si512(v, _mm512_srli_epi64(v, 4));
  v = _mm512_or_si512(v, _mm512_srli_epi64(v, 8));
  v = _mm512_or_si512(v, _mm512_srli_epi64(v, 16));
  v = _mm512_or_si512(v, _mm512_srli_epi64(v, 32));
  return v;
}
/*  Per-64-bit-lane popcount (BITALG VPOPCNTB + SAD), as the B1/B2
    block does; result is the lane bit count in each i64 lane.  */
static INLINE __m512i FASTENT_FN(popcnt64v)(__m512i v) {
  return _mm512_sad_epu8(_mm512_popcnt_epi8(v), _mm512_setzero_si512());
}

/*  Nibble (4-bit, LSB-first) longest-run-of-1s state: pre = leading 1-run
    from bit 0, suf = trailing 1-run ending at bit 3, mx = longest internal
    1-run, full = all four bits set.  */
#define FASTENT_DG_LR_PRE { 0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4 }
#define FASTENT_DG_LR_SUF { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,4 }
#define FASTENT_DG_LR_MX  { 0,1,1,2,1,1,2,3,1,1,1,2,2,2,3,4 }
#define FASTENT_DG_LR_FUL { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 }

typedef struct {
  __m512i pre, suf, mx, full, wid;
} FASTENT_FN(lrstate);

/*  Compose state a (lower bits) then b (higher bits): the join run
    spans a's suffix + b's prefix; pre/suf extend through a fully-set
    operand.  All fields are i8 lanes (max value 64, fits).  */
static INLINE FASTENT_FN(lrstate)
FASTENT_FN(lr_cmb)(FASTENT_FN(lrstate) a, FASTENT_FN(lrstate) b) {
  FASTENT_FN(lrstate) r;
  __m512i join = _mm512_add_epi8(a.suf, b.pre);
  __mmask64 af = _mm512_cmpneq_epi8_mask(a.full, _mm512_setzero_si512());
  __mmask64 bf = _mm512_cmpneq_epi8_mask(b.full, _mm512_setzero_si512());
  r.mx   = _mm512_max_epi8(_mm512_max_epi8(a.mx, b.mx), join);
  r.pre  = _mm512_mask_blend_epi8(af, a.pre,
             _mm512_add_epi8(a.wid, b.pre));
  r.suf  = _mm512_mask_blend_epi8(bf, b.suf,
             _mm512_add_epi8(b.wid, a.suf));
  r.full = _mm512_and_si512(a.full, b.full);
  r.wid  = _mm512_add_epi8(a.wid, b.wid);
  return r;
}

/*  Longest run of 1-bits per 64-bit lane, loop-free: nibble PSHUFB state LUTs
    -> per-byte state, 3-step in-lane ordered tournament (stride 1/2/4 byte
    shift + compose) folds 8 bytes into lane byte 0.  */
static INLINE __m512i
FASTENT_FN(longest_run1)(__m512i zv) {
  static const i8 lpre[16] = FASTENT_DG_LR_PRE;
  static const i8 lsuf[16] = FASTENT_DG_LR_SUF;
  static const i8 lmx[16]  = FASTENT_DG_LR_MX;
  static const i8 lful[16] = FASTENT_DG_LR_FUL;
  const __m512i Lpre = _mm512_broadcast_i32x4(
      _mm_loadu_si128((const void *) lpre));
  const __m512i Lsuf = _mm512_broadcast_i32x4(
      _mm_loadu_si128((const void *) lsuf));
  const __m512i Lmx  = _mm512_broadcast_i32x4(
      _mm_loadu_si128((const void *) lmx));
  const __m512i Lful = _mm512_broadcast_i32x4(
      _mm_loadu_si128((const void *) lful));
  const __m512i m0f  = _mm512_set1_epi8(0x0F);
  __m512i lo = _mm512_and_si512(zv, m0f);
  __m512i hi = _mm512_and_si512(_mm512_srli_epi16(zv, 4), m0f);
  FASTENT_FN(lrstate) a, b, s;
  int sh;
  a.pre  = _mm512_shuffle_epi8(Lpre, lo);
  a.suf  = _mm512_shuffle_epi8(Lsuf, lo);
  a.mx   = _mm512_shuffle_epi8(Lmx,  lo);
  a.full = _mm512_shuffle_epi8(Lful, lo);
  a.wid  = _mm512_set1_epi8(4);
  b.pre  = _mm512_shuffle_epi8(Lpre, hi);
  b.suf  = _mm512_shuffle_epi8(Lsuf, hi);
  b.mx   = _mm512_shuffle_epi8(Lmx,  hi);
  b.full = _mm512_shuffle_epi8(Lful, hi);
  b.wid  = _mm512_set1_epi8(4);
  s = FASTENT_FN(lr_cmb)(a, b);                /*  per-byte, wid 8  */
  for (sh = 1; sh < 8; sh <<= 1) {
    FASTENT_FN(lrstate) hib;
    hib.pre  = _mm512_srli_epi64(s.pre,  sh * 8);
    hib.suf  = _mm512_srli_epi64(s.suf,  sh * 8);
    hib.mx   = _mm512_srli_epi64(s.mx,   sh * 8);
    hib.full = _mm512_srli_epi64(s.full, sh * 8);
    hib.wid  = _mm512_srli_epi64(s.wid,  sh * 8);
    s = FASTENT_FN(lr_cmb)(s, hib);
  }
  /*  Lane byte 0 holds mx (a small non-negative count); zero-extend
      that byte to the whole i64 lane.  */
  return _mm512_and_si512(s.mx, _mm512_set1_epi64(0xFF));
}
/*  SIMD B3 over leading "normal" words [0,ntw).  */
static INLINE sz
FASTENT_FN(maxgap_full_words)(
    const u64 * W, sz ntw, u64 * pgap, i64 * pfirst, i64 * plast,
    int * phave) {
  i64 first = *pfirst, last = *plast;
  u64 gap = *pgap;
  int have = *phave;
  sz w;
  for (w = 0; w + 8 <= ntw; w += 8) {
    __m512i a  = _mm512_loadu_si512((const void *)(W + w));
    __m512i nx = _mm512_loadu_si512((const void *)(W + w + 1));
    __m512i t  = _mm512_xor_si512(a,
        _mm512_or_si512(_mm512_srli_epi64(a, 1),
                        _mm512_slli_epi64(nx, 63)));
    __m512i P  = FASTENT_FN(pfx_or64)(t);
    __m512i S  = FASTENT_FN(sfx_or64)(t);
    /*  0 at i is "strictly between" iff a set bit exists below it
        (P>>1) and above it (S<<1).  */
    __m512i z  = _mm512_andnot_si512(t,
        _mm512_and_si512(_mm512_srli_epi64(P, 1),
                         _mm512_slli_epi64(S, 1)));
    __m512i notP = _mm512_andnot_si512(P, _mm512_set1_epi64(-1));
    __m512i lo = FASTENT_FN(popcnt64v)(notP);
    __m512i hi = _mm512_sub_epi64(FASTENT_FN(popcnt64v)(S),
                                  _mm512_set1_epi64(1));
    {
      __m512i kc = FASTENT_FN(longest_run1)(z);
      __mmask8 hasz = _mm512_test_epi64_mask(z, z);
      __m512i wg = _mm512_maskz_add_epi64(hasz, kc,
                     _mm512_set1_epi64(1));
      i64 TWv[8], LO[8], HI[8], WG[8];
      int k;
      _mm512_storeu_si512((void *) TWv, t);
      _mm512_storeu_si512((void *) LO,  lo);
      _mm512_storeu_si512((void *) HI,  hi);
      _mm512_storeu_si512((void *) WG,  wg);
      for (k = 0; k < 8; k++) {
        if (!TWv[k]) continue;
        {
          const i64 fp = (i64) (w + k) * 64 + LO[k];
          const i64 lp = (i64) (w + k) * 64 + HI[k];
          if (have) {
            const u64 g = (u64) (fp - last);
            if (g > gap) gap = g;
          } else { first = fp;  have = 1; }
          if ((u64) WG[k] > gap) gap = (u64) WG[k];
          last = lp;
        }
      }
    }
  }
  *pgap = gap;  *pfirst = first;  *plast = last;  *phave = have;
  return w;
}
#endif

/*  Fold a block's run sequence into the longest-run machine without
    enumerating internal runs; bit-identical to the per-run fastent_lr_run
    sequence (the block's runs strictly alternate, so completed runs only
    raise lr_max and the tail reopens).  */
static INLINE void
FASTENT_FN(lr_runs_summary)(
    fastent_chunk_state * st, u32 head_sym, u64 head_len, u64 internal_max,
    u32 tail_sym, u64 tail_len, int multi) {
  if (!multi) { FASTENT_FN(lr_run_i)(st, head_sym, head_len); return; }
  FASTENT_FN(lr_run_i)(st, head_sym, head_len);
  if (st->lr_cur > st->lr_max) st->lr_max = st->lr_cur;
  if (internal_max > st->lr_max) st->lr_max = internal_max;
  st->lr_head_open = 0;
  st->lr_sym = (u8) tail_sym;
  st->lr_cur = tail_len;
}

void FASTENT_FN(digram_bits_blk)(
    fastent_chunk_state * st, const u8 * RESTRICT buf, sz cl,
    const i32 * cs_mn, const i32 * cs_mx, const i32 * cs_net) {
  u64 W[FASTENT_DG_BITS_WORDS_T];
  const sz M  = cl * 8;
  const sz NW = (M + 63) / 64;
  const sz cw = cl / 8;
  sz w, i;

  /*  B0: pack via RB64 (per-byte bit reverse of an explicit-LE 8-byte
      load) over whole words; the <8-byte tail is packed byte-wise.  */
  for (w = 0; w < cw; w++) W[w] = fastent_rb64_ld_(buf + w * 8);
  if (NW > cw) {
    u64 last = 0;
    for (i = cw * 8; i < cl; i++)
      last |= (u64) fastent_bitrev8_(buf[i]) << ((u32) (i & 7) * 8u);
    W[cw] = last;
  }
  for (i = NW; i < NW + 8; i++) W[i] = 0ull;   /*  succ-load pad  */

  const u32 b0    = (u32) (W[0] & 1u);
  const sz  lbpos = M - 1;
  const u32 lbit  = (u32) ((W[lbpos >> 6] >> (lbpos & 63)) & 1u);
  const sz  wl    = lbpos >> 6;
  const u32 bl    = (u32) (lbpos & 63);
  const u64 lmask = bl ? ((1ull << bl) - 1ull) : 0ull;

  u64 n11 = 0, ntr = 0, n10 = 0;

#if defined(FASTENT_VARIANT_AVX512) && defined(FASTENT_AVX512_HAVE_BITALG)
  /*  B1/B2 via VPOPCNTB (_mm512_popcnt_epi8) + SAD horizontal sum:
      8 words/instr for W&succ, W^succ, W&~succ; succ[w] =
      (W[w]>>1)|(W[w+1]<<63).  BITALG has byte popcount, not _epi64.  */
  {
    sz wv = NW & ~(sz) 7;
    __m512i z     = _mm512_setzero_si512();
    __m512i acc11 = z, acctr = z, acc10 = z;
    for (w = 0; w < wv; w += 8) {
      __m512i a  = _mm512_loadu_si512((const void *)(W + w));
      __m512i nx = _mm512_loadu_si512((const void *)(W + w + 1));
      __m512i sa = _mm512_or_si512(_mm512_srli_epi64(a, 1),
                                   _mm512_slli_epi64(nx, 63));
      acc11 = _mm512_add_epi64(acc11, _mm512_sad_epu8(
                _mm512_popcnt_epi8(_mm512_and_si512(a, sa)), z));
      acctr = _mm512_add_epi64(acctr, _mm512_sad_epu8(
                _mm512_popcnt_epi8(_mm512_xor_si512(a, sa)), z));
      acc10 = _mm512_add_epi64(acc10, _mm512_sad_epu8(
                _mm512_popcnt_epi8(_mm512_andnot_si512(sa, a)), z));
    }
    n11 += (u64) _mm512_reduce_add_epi64(acc11);
    ntr += (u64) _mm512_reduce_add_epi64(acctr);
    n10 += (u64) _mm512_reduce_add_epi64(acc10);
    for (w = wv; w < NW; w++) {
      const u64 a  = W[w];
      const u64 nx = (w + 1 < NW) ? W[w + 1] : 0ull;
      const u64 sa = (a >> 1) | (nx << 63);
      n11 += FASTENT_POPCOUNT64(a & sa);
      ntr += FASTENT_POPCOUNT64(a ^ sa);
      n10 += FASTENT_POPCOUNT64(a & ~sa);
    }
  }
#else
  /*  Portable B1/B2: compiler vectorises this with the TU -m flags.  */
  for (w = 0; w < NW; w++) {
    const u64 a  = W[w];
    const u64 nx = (w + 1 < NW) ? W[w + 1] : 0ull;
    const u64 sa = (a >> 1) | (nx << 63);
    n11 += FASTENT_POPCOUNT64(a & sa);
    ntr += FASTENT_POPCOUNT64(a ^ sa);
    n10 += FASTENT_POPCOUNT64(a & ~sa);
  }
#endif

  /*  B3 longest run + B4 cusum, closed-form on the loaded words (== old
      per-transition loop via lr_runs_summary).  */
  i64 o = 0, gmn = ((i64) 1 << 60), gmx = -((i64) 1 << 60);
  i64 first_p = -1, last_p = -1;
  u64 gapmax  = 0;
  int have_tr = 0;
  sz b4w = 0, b3w = 0;
#ifdef FASTENT_DG_B4_SIMD
  {
    const sz nfull = (cl % 8u == 0) ? NW : (NW ? NW - 1 : 0);
    b4w = FASTENT_FN(cusum_full_words)(W, nfull, &o, &gmn, &gmx);
  }
#endif
#ifdef FASTENT_DG_B3_SIMD
  /*  [0,NW-1): the last word wl=NW-1 needs the lmask fold, handled by
      the scalar tail; earlier words use the full succ W[w+1].  */
  if (NW > 1)
    b3w = FASTENT_FN(maxgap_full_words)(W, NW - 1, &gapmax,
                                        &first_p, &last_p, &have_tr);
#endif
  /*  SIMD B3/B4 consumed [0,b3w)/[0,b4w); the scalar loop only needs
      the words neither covered (plus the lmask-special last word, which
      is >= both since the SIMD passes stop before it).  */
  for (w = (b3w < b4w ? b3w : b4w); w < NW; w++) {
    const u64 a  = W[w];
    const u64 nx = (w + 1 < NW) ? W[w + 1] : 0ull;
    const u64 sa = (a >> 1) | (nx << 63);
    u64 tw = a ^ sa;
    if (w == wl) tw &= lmask;
    if (w >= b3w && tw) {
      const u32 lo = FASTENT_CTZ64(tw);
      const u32 hi = 63u - FASTENT_CLZ64(tw);
      const i64 fp = (i64) w * 64 + (i64) lo;
      const i64 lp = (i64) w * 64 + (i64) hi;
      if (have_tr) {
        const u64 g = (u64) (fp - last_p);
        if (g > gapmax) gapmax = g;
      } else {
        first_p = fp;  have_tr = 1;
      }
      {
        const u64 gw = FASTENT_FN(maxgap_in_word)(tw, lo, hi);
        if (gw > gapmax) gapmax = gw;
      }
      last_p = lp;
    }
    /*  B4 scalar tail: words the SIMD pass did not consume.  Full
        words jump o by the closed-form net 2*POP-64 (= sum of byte
        nets); the single trailing partial chains cs_net per byte.  */
    if (w >= b4w) {
      const int full = (w + 1 != NW) || (cl - w * 8 == 8);
      if (full) {
        i64 r = 0;
        u32 j;
        for (j = 0; j < 8; j++) {
          const u32 v = (u32) ((a >> (j * 8)) & 0xFFu);
          const i64 base = o + r;
          if (base + cs_mn[v] < gmn) gmn = base + cs_mn[v];
          if (base + cs_mx[v] > gmx) gmx = base + cs_mx[v];
          r += cs_net[v];
        }
        o += 2 * (i64) FASTENT_POPCOUNT64(a) - 64;
      } else {
        const u32 jb = (u32) (cl - w * 8);
        u32 j;
        for (j = 0; j < jb; j++) {
          const u32 v = (u32) ((a >> (j * 8)) & 0xFFu);
          if (o + cs_mn[v] < gmn) gmn = o + cs_mn[v];
          if (o + cs_mx[v] > gmx) gmx = o + cs_mx[v];
          o += cs_net[v];
        }
      }
    }
  }

  ntr -= lbit;  n10 -= lbit;
  const u64 n01 = ntr - n10;
  const u64 n00 = (u64) (M - 1) - ntr - n11;

  if (st->dg_have) st->bit_bigram[st->dg_prev & 1u][b0]++;
  st->bit_bigram[0][0] += n00;  st->bit_bigram[0][1] += n01;
  st->bit_bigram[1][0] += n10;  st->bit_bigram[1][1] += n11;
  st->dg_prev = (u8) lbit;  st->dg_have = 1;

  if (!st->rn_have) { st->rn_have = 1;  st->rn_count = 1 + ntr; }
  else {
    if (b0 != st->rn_last) st->rn_count++;
    st->rn_count += ntr;
  }
  st->rn_last = (u8) lbit;

  {
    const i64 cs0 = st->cs_sum;
    if (cs0 + gmn < st->cs_min) st->cs_min = cs0 + gmn;
    if (cs0 + gmx > st->cs_max) st->cs_max = cs0 + gmx;
    st->cs_sum = cs0 + o;
  }

  if (!have_tr) {
    FASTENT_FN(lr_runs_summary)(st, b0, (u64) (lbpos + 1), 0, b0,
                                (u64) (lbpos + 1), 0);
  } else {
    const u64 head_len = (u64) (first_p + 1);
    const u64 tail_len = (u64) ((i64) lbpos - last_p);
    const u32 tail_sym = b0 ^ (u32) (ntr & 1u);
    FASTENT_FN(lr_runs_summary)(st, b0, head_len, gapmax,
                                tail_sym, tail_len, 1);
  }
}
