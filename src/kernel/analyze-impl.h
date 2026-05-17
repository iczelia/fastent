/*  fastent: templated inner-loop body. Included from analyze-scalar.c,
    analyze-ssse3.c, analyze-sse41.c, analyze-avx2.c.

    Each TU defines one of:
      FASTENT_VARIANT_SCALAR
      FASTENT_VARIANT_SSSE3
      FASTENT_VARIANT_SSE41
      FASTENT_VARIANT_AVX2

    and (optionally) FASTENT_HAVE_SIMD if the SIMD body should be used.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).

    Single-pass over byte stream b[0..N-1]:

      hist[v]     = sum_i [b[i] == v]            (256 bins, banked 4-way)
      cross_prod  = sum_{i=0..N-2} b[i]*b[i+1]   (SCC partial; wrap added
                                                  at finalize: + b[N-1]*b[0])
      mc_inside   = number of hexads (b[6k..6k+5]) whose two 24-bit
                    components squared-and-summed are <= INCIRC.

    SCC SIMD: PMADDUBSW saturates, so widen to i16 and signed-mul.
    Pre-XOR b with 0x80 maps b -> b-128 into i8 range; mullo_epi16
    gives a*(b-128); madd_epi16 pair-sums to i32 (no saturation). The
    128*sum_a correction is computed via PSADBW in the same loop, so
    st->cross_product is canonical after every analyze() call.

    MC Pi interleaves into the SIMD body as scalar drain + SIMD bulk +
    scalar tail per stride; a persistent 6-byte ring spans calls.  */

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

/*  Stage-buffer pointer launder. SIMD bodies store the folded vector
    to a stack stage then issue scalar byte loads from it for the
    histogram. Without a launder the compiler folds those loads into a
    vpextrb/umov chain off the source vector, serialising the histogram
    on the FP->GP datapath (~2x on x86, ~1.5x on NEON). GCC/Clang use
    the empty-asm launder; elsewhere `volatile` blocks the same fold.  */
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
  return ((u32)(c - 'A')   < 26u) ||
         ((u32)(c - 0xC0u) < 31u && c != 0xD7u);
}

static inline u8 FASTENT_FN(fold_byte_inline)(u8 b) {
  u32 c = b;
  if (FASTENT_FN(fold_is_upper_inline)(c)) return (u8)(c + 0x20u);
  return b;
}

#ifdef FASTENT_HAVE_SIMD
static FASTENT_ALWAYS_INLINE FASTENT_SIMD_VEC
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

/*  Scalar single-byte update: histogram + SCC + MC Pi + first/last.
    Used by head/tail of all variants and the whole scalar body.  */

static inline void FASTENT_FN(consume_byte)(fastent_chunk_state * st, u8 b,
                                        u32 bank_idx) {
  st->bank[bank_idx & (FASTENT_BANKS - 1)][b]++;
  if (st->have_carry) {
    st->cross_product += (i64) st->carry_byte * (i64) b;
  } else {
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

static FASTENT_ALWAYS_INLINE sz
FASTENT_FN(scalar_body_impl)(fastent_chunk_state * st,
                             const u8 * FASTENT_RESTRICT buf,
                             sz len, sz start_bank, int fold) {
  sz i;
  for (i = 0; i < len; i++) {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[i]) : buf[i];
    FASTENT_FN(consume_byte)(st, b, start_bank + i);
  }
  return i;
}

static inline sz FASTENT_FN(scalar_body)(fastent_chunk_state * st,
                                     const u8 * FASTENT_RESTRICT buf,
                                     sz len, sz start_bank) {
  return FASTENT_FN(scalar_body_impl)(st, buf, len, start_bank, 0);
}

/*  SIMD body: 4-bank histogram + sign-corrected SCC accumulator,
    one variant per ISA below.  */

#if defined(FASTENT_VARIANT_AVX2)

static FASTENT_ALWAYS_INLINE sz
FASTENT_FN(simd_body_impl)(fastent_chunk_state * st,
                           const u8 * FASTENT_RESTRICT buf,
                           sz len, int fold) {
  /*  Stride 64 (two 32-byte vectors). SCC needs byte +1 readable, so
      the body stops at len - 32 (32-byte read-ahead margin).  */
  if (len < 65) return 0;

  const sz body_max = len - 32;
  sz iters = body_max / 64;
  if (iters == 0) return 0;
  const sz body_end = iters * 64;

  /*  SCC window: the first product carry_byte*buf[0] is done scalar
      below; the loop's pmaddubs over (buf[i..i+31], buf[i+1..i+32])
      then covers pairs (buf[i],buf[i+1]) for i in [0..body_end-1].
      In fold mode the scalar seeds (first_byte, carry, epilogue) use
      folded values to match the SIMD loop.  */
  u8 b0_user = fold ? FASTENT_FN(fold_byte_inline)(buf[0]) : buf[0];

  if (!st->have_first) { st->first_byte = b0_user; st->have_first = 1; }
  if (st->have_carry)
    st->cross_product += (i64) st->carry_byte * (i64) b0_user;
  st->have_carry = 1;

  const __m256i sign_xor   = _mm256_set1_epi8((char) 0x80);
  const __m256i zero       = _mm256_setzero_si256();
  __m256i scc_acc64        = _mm256_setzero_si256(); /*  4 i64 lanes  */
  __m256i lhs_sad          = _mm256_setzero_si256();

  /*  4 histogram banks; bytes 0..3 within each quad hit banks 0..3.  */
  u32 * FASTENT_RESTRICT b0 = st->bank[0];
  u32 * FASTENT_RESTRICT b1 = st->bank[1];
  u32 * FASTENT_RESTRICT b2 = st->bank[2];
  u32 * FASTENT_RESTRICT b3 = st->bank[3];

  /*  MC Pi state hoisted into locals for register residency.  */
  int mc_pos     = st->mc_pos;
  u8  m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8  m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];
  u64 mc_count   = st->mc_count;
  u64 mc_inside  = st->mc_inside;

  const __m256i ones16_sum = _mm256_set1_epi16(1);
  /*  Prefetch ~8 strides ahead into L2 for streaming workloads.  */
  #define FASTENT_PREFETCH_DIST 512
  for (sz i = 0; i < body_end; i += 64) {
    FASTENT_PREFETCH_R(buf + i + FASTENT_PREFETCH_DIST);
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

    /*  Histogram: 64 movzbl increments across 4 banks, from buf or
        (fold mode) the laundered L1 stage. See launder note above.  */
    FASTENT_ALIGN(32) u8 stage[64];
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
    #define HIST4(o) \
      b0[p[(o) + 0]]++; b1[p[(o) + 1]]++; \
      b2[p[(o) + 2]]++; b3[p[(o) + 3]]++
    HIST4( 0); HIST4( 4); HIST4( 8); HIST4(12);
    HIST4(16); HIST4(20); HIST4(24); HIST4(28);
    HIST4(32); HIST4(36); HIST4(40); HIST4(44);
    HIST4(48); HIST4(52); HIST4(56); HIST4(60);
    #undef HIST4

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
    const __m128i mc_lim  = _mm_set1_epi64x((i64)(FASTENT_INCIRC + 1ULL));
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
    mc_pos = (int) stash_count;
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
  st->cross_product += scc_sum + (i64)(128ULL * lhs_sum);

  /*  Epilogue: buf[body_end]'s SCC pair was counted by the last
      shifted load, but it still needs histogram + carry + MC.  */
  {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
    st->total_bytes += body_end;
    st->bank[(u32)(st->total_bytes) & (FASTENT_BANKS - 1)][b]++;
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

/*  AVX-512 SIMD body. Stride 128 (two 64-byte vectors); SCC path is
    the AVX2 one widened to 512-bit. Histogram stays scalar 4-banked
    because VPSCATTERDD (~16c recip throughput on Zen 4) loses to the
    inc-mem chain by 1.5-3x.  */

static FASTENT_ALWAYS_INLINE sz
FASTENT_FN(simd_body_impl)(fastent_chunk_state * st,
                           const u8 * FASTENT_RESTRICT buf,
                           sz len, int fold) {
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

  u32 * FASTENT_RESTRICT b0 = st->bank[0];
  u32 * FASTENT_RESTRICT b1 = st->bank[1];
  u32 * FASTENT_RESTRICT b2 = st->bank[2];
  u32 * FASTENT_RESTRICT b3 = st->bank[3];

  int mc_pos     = st->mc_pos;
  u8  m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8  m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];
  u64 mc_count   = st->mc_count;
  u64 mc_inside  = st->mc_inside;

  const __m512i ones16_sum = _mm512_set1_epi16(1);
  (void) ones16_sum;  /*  unused in the BITALG/VNNI sub-variant  */

  #define FASTENT_PREFETCH_DIST 512
  for (sz i = 0; i < body_end; i += 128) {
    FASTENT_PREFETCH_R(buf + i + FASTENT_PREFETCH_DIST + 0);
    FASTENT_PREFETCH_R(buf + i + FASTENT_PREFETCH_DIST + 64);

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
    FASTENT_ALIGN(64) u8 stage[128];
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

    /*  128 inc-mem increments across 4 banks; same bank discipline
        as AVX2 so cross-variant tail merges line up.  */
    #define HIST4(o) \
      b0[p[(o) + 0]]++; b1[p[(o) + 1]]++; \
      b2[p[(o) + 2]]++; b3[p[(o) + 3]]++
    HIST4(  0); HIST4(  4); HIST4(  8); HIST4( 12);
    HIST4( 16); HIST4( 20); HIST4( 24); HIST4( 28);
    HIST4( 32); HIST4( 36); HIST4( 40); HIST4( 44);
    HIST4( 48); HIST4( 52); HIST4( 56); HIST4( 60);
    HIST4( 64); HIST4( 68); HIST4( 72); HIST4( 76);
    HIST4( 80); HIST4( 84); HIST4( 88); HIST4( 92);
    HIST4( 96); HIST4(100); HIST4(104); HIST4(108);
    HIST4(112); HIST4(116); HIST4(120); HIST4(124);
    #undef HIST4

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
    const __m128i mc_lim  = _mm_set1_epi64x((i64)(FASTENT_INCIRC + 1ULL));
    u32 k = 0;
    /*  256-bit bulk: 4 hexads per iter (=24 bytes of source).  */
    const __m256i mc_shuf256 = _mm256_broadcastsi128_si256(mc_shuf);
    const __m256i mc_lim256  = _mm256_set1_epi64x((i64)(FASTENT_INCIRC + 1ULL));
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
    mc_pos = (int) stash_count;
    if (stash_count >= 1) m0 = p[stash_at + 0];
    if (stash_count >= 2) m1 = p[stash_at + 1];
    if (stash_count >= 3) m2 = p[stash_at + 2];
    if (stash_count >= 4) m3 = p[stash_at + 3];
    if (stash_count >= 5) m4 = p[stash_at + 4];
  }

  /*  Horizontal reduce scc_acc64 (8 i64 lanes) -> scalar.  */
  i64 scc_sum = (i64) _mm512_reduce_add_epi64(scc_acc64);
  u64 lhs_sum = (u64) _mm512_reduce_add_epi64(lhs_sad);
  st->cross_product += scc_sum + (i64)(128ULL * lhs_sum);

  /*  Epilogue: process buf[body_end] for histogram + carry + MC.  */
  {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
    st->total_bytes += body_end;
    st->bank[(u32)(st->total_bytes) & (FASTENT_BANKS - 1)][b]++;
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

static FASTENT_ALWAYS_INLINE sz
FASTENT_FN(simd_body_impl)(fastent_chunk_state * st,
                           const u8 * FASTENT_RESTRICT buf,
                           sz len, int fold) {
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

  u32 * FASTENT_RESTRICT b0 = st->bank[0];
  u32 * FASTENT_RESTRICT b1 = st->bank[1];
  u32 * FASTENT_RESTRICT b2 = st->bank[2];
  u32 * FASTENT_RESTRICT b3 = st->bank[3];

  /*  MC Pi state hoisted into locals.  */
  int mc_pos     = st->mc_pos;
  u8  m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8  m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];
  u64 mc_count   = st->mc_count;
  u64 mc_inside  = st->mc_inside;

  for (sz i = 0; i < body_end; i += 32) {
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
    FASTENT_ALIGN(16) u8 stage[32];
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
    #define HIST4(o) \
      b0[p[(o) + 0]]++; b1[p[(o) + 1]]++; \
      b2[p[(o) + 2]]++; b3[p[(o) + 3]]++
    HIST4( 0); HIST4( 4); HIST4( 8); HIST4(12);
    HIST4(16); HIST4(20); HIST4(24); HIST4(28);
    #undef HIST4

    /*  MC Pi: scalar drain + bulk + stash (32-byte stride).  */
    #define MC_HEXAD(x0, x1, x2, y0, y1, y2) do { \
      u32 x = ((u32)(x0) << 16) | ((u32)(x1) << 8) | (u32)(x2); \
      u32 y = ((u32)(y0) << 16) | ((u32)(y1) << 8) | (u32)(y2); \
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
    int n_hexads = (int)((32u - p_idx) / 6u);
    Fk(n_hexads,
       u32 o = p_idx + (u32) k * 6u;
       MC_HEXAD(p[o+0], p[o+1], p[o+2], p[o+3], p[o+4], p[o+5]))
    u32 stash_at    = p_idx + n_hexads * 6u;
    u32 stash_count = 32u - stash_at;
    mc_pos = (int) stash_count;
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

  st->cross_product += scc_sum + (i64)(128ULL * lhs_sum);

  /*  Epilogue: histogram + MC for buf[body_end]; its SCC pair was
      already added by the last shifted load.  */
  u8 last_b;
  {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
    last_b = b;
    st->total_bytes += body_end;
    st->bank[(u32)(st->total_bytes) & (FASTENT_BANKS - 1)][b]++;
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

/*  AArch64 NEON SIMD body. Stride 32 (two 16-byte vectors), mirroring
    SSE. NEON lacks PMADDUBSW/PSADBW/movemask; equivalents:
      SCC: widen va u8->i16 (zero), vbs s8->i16 (sign), i16 mul,
        vpaddlq_s16 pair-sum to i32 (= madd_epi16 with ones); i64 acc.
      LHS sum: vpaddlq u8->u16->u32->u64 ladder into lhs_sad.
      MC Pi: all-scalar; ~5 hexads/iter is too few to amortise SIMD.
    Histogram: 4-banked inc-mem as SSE; laundered L1 stage in fold
    mode (see launder note above).  */

static FASTENT_ALWAYS_INLINE sz
FASTENT_FN(simd_body_impl)(fastent_chunk_state * st,
                           const u8 * FASTENT_RESTRICT buf,
                           sz len, int fold) {
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

  u32 * FASTENT_RESTRICT b0 = st->bank[0];
  u32 * FASTENT_RESTRICT b1 = st->bank[1];
  u32 * FASTENT_RESTRICT b2 = st->bank[2];
  u32 * FASTENT_RESTRICT b3 = st->bank[3];

  int mc_pos     = st->mc_pos;
  u8  m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8  m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];
  u64 mc_count   = st->mc_count;
  u64 mc_inside  = st->mc_inside;

  for (sz i = 0; i < body_end; i += 32) {
    FASTENT_PREFETCH_R(buf + i + 512);

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
    FASTENT_ALIGN(16) u8 stage[32];
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
    #define HIST4(o) \
      b0[p[(o) + 0]]++; b1[p[(o) + 1]]++; \
      b2[p[(o) + 2]]++; b3[p[(o) + 3]]++
    HIST4( 0); HIST4( 4); HIST4( 8); HIST4(12);
    HIST4(16); HIST4(20); HIST4(24); HIST4(28);
    #undef HIST4

    /*  MC Pi: scalar drain + scalar bulk + scalar stash.  */
    #define MC_HEXAD(x0, x1, x2, y0, y1, y2) do { \
      u32 x = ((u32)(x0) << 16) | ((u32)(x1) << 8) | (u32)(x2); \
      u32 y = ((u32)(y0) << 16) | ((u32)(y1) << 8) | (u32)(y2); \
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
    int n_hexads = (int)((32u - p_idx) / 6u);
    Fk(n_hexads,
       u32 o = p_idx + (u32) k * 6u;
       MC_HEXAD(p[o+0], p[o+1], p[o+2], p[o+3], p[o+4], p[o+5]))
    u32 stash_at    = p_idx + (u32) n_hexads * 6u;
    u32 stash_count = 32u - stash_at;
    mc_pos = (int) stash_count;
    if (stash_count >= 1) m0 = p[stash_at + 0];
    if (stash_count >= 2) m1 = p[stash_at + 1];
    if (stash_count >= 3) m2 = p[stash_at + 2];
    if (stash_count >= 4) m3 = p[stash_at + 3];
    if (stash_count >= 5) m4 = p[stash_at + 4];
    #undef MC_HEXAD
  }

  i64 scc_sum = (i64) fastent_neon_addvq_s64(scc_acc64);
  u64 lhs_sum = (u64) fastent_neon_addvq_u64(lhs_sad);
  st->cross_product += scc_sum + (i64)(128ULL * lhs_sum);

  /*  Epilogue: histogram + carry + MC for buf[body_end]; its SCC pair
      was already counted by the last shifted load.  */
  u8 last_b;
  {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
    last_b = b;
    st->total_bytes += body_end;
    st->bank[(u32)(st->total_bytes) & (FASTENT_BANKS - 1)][b]++;
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

/*  WebAssembly SIMD128 byte-mode body. Stride 32 (two 16-byte
    vectors), as SSE/NEON. SCC uses wasm_i32x4_dot_i16x8 (signed i16
    pair-sum to i32, fusing NEON's mul + vpaddlq). LHS sum via the
    extadd-pairwise ladder. Histogram + MC Pi scalar 4-banked, no
    SIMD bulk (~5 hexads/iter).  */

static FASTENT_ALWAYS_INLINE sz
FASTENT_FN(simd_body_impl)(fastent_chunk_state * st,
                           const u8 * FASTENT_RESTRICT buf,
                           sz len, int fold) {
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

  u32 * FASTENT_RESTRICT b0 = st->bank[0];
  u32 * FASTENT_RESTRICT b1 = st->bank[1];
  u32 * FASTENT_RESTRICT b2 = st->bank[2];
  u32 * FASTENT_RESTRICT b3 = st->bank[3];

  int mc_pos     = st->mc_pos;
  u8  m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8  m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];
  u64 mc_count   = st->mc_count;
  u64 mc_inside  = st->mc_inside;

  const v128_t ones32 = wasm_i32x4_splat(1);

  for (sz i = 0; i < body_end; i += 32) {
    FASTENT_PREFETCH_R(buf + i + 512);

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
    FASTENT_ALIGN(16) u8 stage[32];
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
    #define HIST4(o) \
      b0[p[(o) + 0]]++; b1[p[(o) + 1]]++; \
      b2[p[(o) + 2]]++; b3[p[(o) + 3]]++
    HIST4( 0); HIST4( 4); HIST4( 8); HIST4(12);
    HIST4(16); HIST4(20); HIST4(24); HIST4(28);
    #undef HIST4

    /*  MC Pi: all-scalar (NEON-style).  */
    #define MC_HEXAD(x0, x1, x2, y0, y1, y2) do { \
      u32 x = ((u32)(x0) << 16) | ((u32)(x1) << 8) | (u32)(x2); \
      u32 y = ((u32)(y0) << 16) | ((u32)(y1) << 8) | (u32)(y2); \
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
    int n_hexads = (int)((32u - p_idx) / 6u);
    Fk(n_hexads,
       u32 o = p_idx + (u32) k * 6u;
       MC_HEXAD(p[o+0], p[o+1], p[o+2], p[o+3], p[o+4], p[o+5]))
    u32 stash_at    = p_idx + (u32) n_hexads * 6u;
    u32 stash_count = 32u - stash_at;
    mc_pos = (int) stash_count;
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
  st->cross_product += scc_sum + (i64)(128ULL * lhs_sum);

  /*  Epilogue: histogram + carry + MC for buf[body_end]; its SCC pair
      was already counted by the last shifted load.  */
  u8 last_b;
  {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
    last_b = b;
    st->total_bytes += body_end;
    st->bank[(u32)(st->total_bytes) & (FASTENT_BANKS - 1)][b]++;
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
static inline sz FASTENT_FN(simd_body)(fastent_chunk_state * st,
    const u8 * FASTENT_RESTRICT buf, sz len) {
  return FASTENT_FN(simd_body_impl)(st, buf, len, 0);
}
static inline sz FASTENT_FN(simd_body_fold)(fastent_chunk_state * st,
    const u8 * FASTENT_RESTRICT buf, sz len) {
  return FASTENT_FN(simd_body_impl)(st, buf, len, 1);
}
#endif

/*  Public entry point.  */

static FASTENT_ALWAYS_INLINE void
FASTENT_FN(analyze_impl)(fastent_chunk_state * st,
                         const u8 * FASTENT_RESTRICT buf, sz len, int fold) {
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

void FASTENT_FN(analyze)(fastent_chunk_state * st,
                         const u8 * FASTENT_RESTRICT buf, sz len) {
  FASTENT_FN(analyze_impl)(st, buf, len, 0);
#if defined(FASTENT_VARIANT_AVX2) || defined(FASTENT_VARIANT_AVX512)
  _mm256_zeroupper();
#endif
}

void FASTENT_FN(analyze_fold)(fastent_chunk_state * st,
                              const u8 * FASTENT_RESTRICT buf, sz len) {
  FASTENT_FN(analyze_impl)(st, buf, len, 1);
}

/*  Case fold in place: ASCII A-Z and Latin-1 0xC0-0xDE (except 0xD7)
    lowered. Range tests use saturating sub (SSSE3-grade ops only).  */

static inline int FASTENT_FN(fold_is_upper_scalar)(u32 c) {
  return ((u32)(c - 'A')  < 26u) ||
         ((u32)(c - 0xC0u) < 31u && c != 0xD7u);
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
    if (FASTENT_FN(fold_is_upper_scalar)(c)) buf[i] = (u8)(c + 0x20u);
  }
}

/*  Bit-mode analyser. Bits run MSB-first within each byte. One pass:

      bit_hist[1]  += sum_i popcount(b[i])
      bit_hist[0]  += 8*N - bit_hist[1]
      cross_product +=  sum_i popcount(b[i] & (b[i] >> 1))   (within-byte)
                      + sum_{i<N-1} (b[i] & 1) & (b[i+1] >> 7)   (cross-byte)
      MC Pi is byte-driven (one trial per 6 bytes, as byte mode).

    SIMD vectorises the counters via PSHUFB-LUT popcount + PSADBW.
    carry/first/last are bit values (0/1) so the run_mmap_mt
    carry*first merge already gives the cross-slab bit-pair; no
    separate bit-mode merge needed.  */

#ifdef FASTENT_HAVE_SIMD

static FASTENT_ALWAYS_INLINE sz
FASTENT_FN(bits_simd_body_impl)(fastent_chunk_state * st,
                                const u8 * FASTENT_RESTRICT buf,
                                sz len, int fold) {
  /*  Cross-byte needs buf[i+VLEN] readable, so the body stops at
      len - VLEN.  */
  if (len < (sz) FASTENT_SIMD_VLEN + 1) return 0;
  const sz body_max = len - FASTENT_SIMD_VLEN;
  sz iters = body_max / FASTENT_SIMD_VLEN;
  if (iters == 0) return 0;
  const sz body_end = iters * FASTENT_SIMD_VLEN;

  u8 b0_user = fold ? FASTENT_FN(fold_byte_inline)(buf[0]) : buf[0];
  if (!st->have_first) {
    st->first_byte = (u8)((b0_user >> 7) & 1u);
    st->have_first = 1;
  }
  if (st->have_carry) {
    u32 prev_lsb = (u32)(st->carry_byte & 1u);
    u32 curr_msb = (u32)((b0_user >> 7) & 1u);
    st->cross_product += (i64)(prev_lsb & curr_msb);
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
  int mc_pos     = st->mc_pos;
  u8  m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8  m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];
  u64 mc_count   = st->mc_count;
  u64 mc_inside  = st->mc_inside;

  for (sz i = 0; i < body_end; i += FASTENT_SIMD_VLEN) {
    FASTENT_PREFETCH_R(buf + i + 512);

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
    /*  +16: the Monte Carlo SIMD loop loads a full 16-byte vector
        per 6-byte hexad, so the last loadu reads a few bytes past
        the VLEN of staged data.  Only VLEN bytes are written and
        only n_hexads*6 are ever consumed; the pad just keeps the
        over-read in-bounds (mirrors the over-sized byte-mode
        stage[] buffers).  */
    FASTENT_ALIGN(32) u8 bits_stage[FASTENT_SIMD_VLEN + 16];
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
      u32 x = ((u32)(x0) << 16) | ((u32)(x1) << 8) | (u32)(x2); \
      u32 y = ((u32)(y0) << 16) | ((u32)(y1) << 8) | (u32)(y2); \
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
    const __m128i mc_lim  = _mm_set1_epi64x((i64)(FASTENT_INCIRC + 1ULL));
#endif
    u32 k = 0;
#if defined(FASTENT_VARIANT_AVX2) || defined(FASTENT_VARIANT_AVX512)
    /*  256-bit: 4 hexads (24 bytes) per iter.  */
    const __m256i mc_shuf256 = _mm256_broadcastsi128_si256(mc_shuf);
    const __m256i mc_lim256  = _mm256_set1_epi64x((i64)(FASTENT_INCIRC + 1ULL));
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
    mc_pos = (int) stash_count;
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
    st->carry_byte = (u8)(b & 1u);
    st->last_byte  = (u8)(b & 1u);
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
static inline sz FASTENT_FN(bits_simd_body)(fastent_chunk_state * st,
    const u8 * FASTENT_RESTRICT buf, sz len) {
  return FASTENT_FN(bits_simd_body_impl)(st, buf, len, 0);
}
static inline sz FASTENT_FN(bits_simd_body_fold)(fastent_chunk_state * st,
    const u8 * FASTENT_RESTRICT buf, sz len) {
  return FASTENT_FN(bits_simd_body_impl)(st, buf, len, 1);
}

#endif  /*  FASTENT_HAVE_SIMD  */

/*  Scalar bit-mode walker (scalar entry and SIMD tail). Templated on
    `fold` for the fused -f -b path.  */
static FASTENT_ALWAYS_INLINE void
FASTENT_FN(bits_scalar_body_impl)(fastent_chunk_state * st,
                                  const u8 * FASTENT_RESTRICT buf,
                                  sz len, int fold) {
  for (sz i = 0; i < len; i++) {
    const u8 byte = fold ? FASTENT_FN(fold_byte_inline)(buf[i]) : buf[i];
    const u32 ones_in_byte = (u32) FASTENT_POPCOUNT32(byte);
    st->bit_hist[1] += ones_in_byte;
    st->bit_hist[0] += 8u - ones_in_byte;
    const u32 within = (u32) FASTENT_POPCOUNT32(byte & (byte >> 1));
    st->cross_product += (i64) within;
    if (st->have_carry) {
      const u32 prev_lsb = (u32)(st->carry_byte & 1u);
      const u32 curr_msb = (u32)((byte >> 7) & 1u);
      st->cross_product += (i64)(prev_lsb & curr_msb);
    } else {
      st->first_byte = (u8)((byte >> 7) & 1u);
      st->have_first = 1;
    }
    st->carry_byte = (u8)(byte & 1u);
    st->last_byte  = (u8)(byte & 1u);
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

static inline void FASTENT_FN(bits_scalar_body)(fastent_chunk_state * st,
                                                const u8 * FASTENT_RESTRICT buf,
                                                sz len) {
  FASTENT_FN(bits_scalar_body_impl)(st, buf, len, 0);
}

static FASTENT_ALWAYS_INLINE void
FASTENT_FN(analyze_bits_impl)(fastent_chunk_state * st,
    const u8 * FASTENT_RESTRICT buf, sz len, int fold) {
  if (len == 0) return;
#ifdef FASTENT_HAVE_SIMD
  sz body = fold ? FASTENT_FN(bits_simd_body_fold)(st, buf, len)
                 : FASTENT_FN(bits_simd_body)(st, buf, len);
  FASTENT_FN(bits_scalar_body_impl)(st, buf + body, len - body, fold);
#else
  FASTENT_FN(bits_scalar_body_impl)(st, buf, len, fold);
#endif
}

void FASTENT_FN(analyze_bits)(fastent_chunk_state * st,
                              const u8 * FASTENT_RESTRICT buf, sz len) {
  FASTENT_FN(analyze_bits_impl)(st, buf, len, 0);
}

void FASTENT_FN(analyze_bits_fold)(fastent_chunk_state * st,
                                   const u8 * FASTENT_RESTRICT buf, sz len) {
  FASTENT_FN(analyze_bits_impl)(st, buf, len, 1);
}
