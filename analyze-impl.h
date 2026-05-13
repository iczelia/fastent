/*  fastent  --  templated inner-loop body. Included from analyze-scalar.c,
    analyze-ssse3.c, analyze-sse41.c, analyze-avx2.c.

    Each TU defines one of:
      FASTENT_VARIANT_SCALAR
      FASTENT_VARIANT_SSSE3
      FASTENT_VARIANT_SSE41
      FASTENT_VARIANT_AVX2

    and (optionally) FASTENT_HAVE_SIMD if the SIMD body should be used.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).

    ----------------------------------------------------------------------
    Algorithm notes.

    For an input byte stream b[0..N-1] we compute, in a single pass:

      hist[v]     = sum_i [b[i] == v]            (256 bins, banked 4-way)
      cross_prod  = sum_{i=0..N-2} b[i]*b[i+1]   (SCC partial; wrap added
                                                  at finalize: + b[N-1]*b[0])
      mc_inside   = number of hexads (b[6k..6k+5]) whose two 24-bit
                    components squared-and-summed are <= INCIRC.

    SCC SIMD path: we want sum a_i * b_i with a, b unsigned bytes.
    PMADDUBSW is unusable directly because its 16-bit pair-sum saturates
    when products are large. Instead we widen to 16-bit and use signed-mul.

    Pre-XOR b with 0x80 so b' = b - 128 in [-128..127] (signed i8).
    Expand a as u8 -> u16, b' as i8 -> i16. mullo_epi16 of u16 x i16
    treated as i16 gives the canonical signed product a*(b-128) which
    fits in [-32640..32385], so no 16-bit overflow. madd_epi16 then sums
    adjacent pairs into i32 (no saturation). The 128*sum_a correction is
    computed on the fly via PSADBW horizontal byte-sums of the LHS
    operand, then added back inside the same loop. So state->cross_product
    is always the canonical un-corrected sum after each analyze() call.

    MC Pi runs in a second pass over the same buffer (cache-resident for
    chunks <= L2). This decouples the awkward 6-byte stride from the
    SIMD-aligned histogram/SCC body and is empirically faster than
    interleaving.  */

#include "analyze.h"

#ifdef FASTENT_HAVE_SIMD
  #include <immintrin.h>
#endif

#if defined(FASTENT_VARIANT_AVX2)
  #define FASTENT_VAR_SUFFIX _avx2
  #define FASTENT_SIMD_VEC   __m256i
  #define FASTENT_SIMD_VLEN  32
#elif defined(FASTENT_VARIANT_SSE41)
  #define FASTENT_VAR_SUFFIX _sse41
  #define FASTENT_SIMD_VEC   __m128i
  #define FASTENT_SIMD_VLEN  16
#elif defined(FASTENT_VARIANT_SSSE3)
  #define FASTENT_VAR_SUFFIX _ssse3
  #define FASTENT_SIMD_VEC   __m128i
  #define FASTENT_SIMD_VLEN  16
#else
  #define FASTENT_VAR_SUFFIX _scalar
#endif

#define FASTENT_CAT2(a, b) a##b
#define FASTENT_CAT(a, b)  FASTENT_CAT2(a, b)
#define FASTENT_FN(name)   FASTENT_CAT(name, FASTENT_VAR_SUFFIX)

/*  ----------------------------------------------------------------------
    Scalar single-byte update: histogram + SCC + MC Pi + first/last
    tracking. Used by head/tail of all variants and entire body of scalar
    variant. MC is folded in so we don't need a second pass over the
    buffer.  */

static inline void FASTENT_FN(consume_byte)(fastent_chunk_state * st, u8 b,
                                        unsigned bank_idx) {
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

/*  ----------------------------------------------------------------------
    Monte Carlo Pi pass. Walks the buffer once, draining the persistent
    6-byte ring left over from any previous call and consuming the new
    bytes; every 6 buffered bytes fires one hexad.  */

static inline void FASTENT_FN(monte_carlo_pass)(fastent_chunk_state * st,
                                            const u8 * FASTENT_RESTRICT buf,
                                            sz len) {
  int mp = st->mc_pos;
  u8 m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8 m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];

  /*  Drain prefix bytes into the ring until we have a full hexad
      OR exhaust the buffer.  */
  sz i = 0;
  while (mp < 6 && i < len) {
    switch (mp) {
      case 0: m0 = buf[i]; break;
      case 1: m1 = buf[i]; break;
      case 2: m2 = buf[i]; break;
      case 3: m3 = buf[i]; break;
      case 4: m4 = buf[i]; break;
      case 5: m5 = buf[i]; break;
    }
    mp++;
    i++;
    if (mp == 6) {
      u32 x = ((u32) m0 << 16) | ((u32) m1 << 8) | (u32) m2;
      u32 y = ((u32) m3 << 16) | ((u32) m4 << 8) | (u32) m5;
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
      st->mc_count++;
      st->mc_inside += (d <= FASTENT_INCIRC);
      mp = 0;
    }
  }

  /*  Bulk: as long as >= 6 bytes remain, fire hexads directly from buf.
      Unroll by 4 for ILP -- modern OoO will keep all 4 squarings in
      flight at once.  */
  while (i + 24 <= len) {
    u32 x0 = ((u32) buf[i +  0] << 16) | ((u32) buf[i +  1] << 8) | buf[i +  2];
    u32 y0 = ((u32) buf[i +  3] << 16) | ((u32) buf[i +  4] << 8) | buf[i +  5];
    u32 x1 = ((u32) buf[i +  6] << 16) | ((u32) buf[i +  7] << 8) | buf[i +  8];
    u32 y1 = ((u32) buf[i +  9] << 16) | ((u32) buf[i + 10] << 8) | buf[i + 11];
    u32 x2 = ((u32) buf[i + 12] << 16) | ((u32) buf[i + 13] << 8) | buf[i + 14];
    u32 y2 = ((u32) buf[i + 15] << 16) | ((u32) buf[i + 16] << 8) | buf[i + 17];
    u32 x3 = ((u32) buf[i + 18] << 16) | ((u32) buf[i + 19] << 8) | buf[i + 20];
    u32 y3 = ((u32) buf[i + 21] << 16) | ((u32) buf[i + 22] << 8) | buf[i + 23];
    u64 d0 = (u64) x0 * (u64) x0 + (u64) y0 * (u64) y0;
    u64 d1 = (u64) x1 * (u64) x1 + (u64) y1 * (u64) y1;
    u64 d2 = (u64) x2 * (u64) x2 + (u64) y2 * (u64) y2;
    u64 d3 = (u64) x3 * (u64) x3 + (u64) y3 * (u64) y3;
    st->mc_inside += (d0 <= FASTENT_INCIRC);
    st->mc_inside += (d1 <= FASTENT_INCIRC);
    st->mc_inside += (d2 <= FASTENT_INCIRC);
    st->mc_inside += (d3 <= FASTENT_INCIRC);
    st->mc_count  += 4;
    i += 24;
  }
  while (i + 6 <= len) {
    u32 x = ((u32) buf[i + 0] << 16) | ((u32) buf[i + 1] << 8) | buf[i + 2];
    u32 y = ((u32) buf[i + 3] << 16) | ((u32) buf[i + 4] << 8) | buf[i + 5];
    u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
    st->mc_count++;
    st->mc_inside += (d <= FASTENT_INCIRC);
    i += 6;
  }

  /*  Stash trailing < 6 bytes into the ring.  */
  while (i < len) {
    switch (mp) {
      case 0: m0 = buf[i]; break;
      case 1: m1 = buf[i]; break;
      case 2: m2 = buf[i]; break;
      case 3: m3 = buf[i]; break;
      case 4: m4 = buf[i]; break;
      case 5: m5 = buf[i]; break;
    }
    mp++;
    i++;
  }
  st->mc_pos = mp;
  st->mc_buf[0] = m0; st->mc_buf[1] = m1; st->mc_buf[2] = m2;
  st->mc_buf[3] = m3; st->mc_buf[4] = m4; st->mc_buf[5] = m5;
}

/*  ----------------------------------------------------------------------
    Scalar histogram + SCC body. Used as fallback and on chunk edges.  */

static inline sz FASTENT_FN(scalar_body)(fastent_chunk_state * st,
                                     const u8 * FASTENT_RESTRICT buf,
                                     sz len, sz start_bank) {
  sz i;
  for (i = 0; i < len; i++) FASTENT_FN(consume_byte)(st, buf[i], start_bank + i);
  return i;
}

/*  ----------------------------------------------------------------------
    SIMD body: histogram (4 banks rotating by within-iter position mod 4)
    + SCC pmaddubs sign-corrected accumulator. Each variant has a
    slightly different version below.  */

#if defined(FASTENT_VARIANT_AVX2)

static inline sz FASTENT_FN(simd_body)(fastent_chunk_state * st,
                                   const u8 * FASTENT_RESTRICT buf,
                                   sz len) {
  /*  AVX2 stride = 64 bytes (two 32-byte vectors). We need byte +1
      readable for the SCC pmaddubs shifted load, so the body stops
      at len - 32 (leave 32-byte read-ahead margin).  */
  if (len < 65) return 0;

  const sz body_max = len - 32;
  sz iters = body_max / 64;
  if (iters == 0) return 0;
  const sz body_end = iters * 64;

  /*  Set up the SCC carry: the very first SCC product in this body's
      window is carry_byte (from the previous byte) * buf[0]. We handle
      this scalar before the SIMD loop, then bytes [0..body_end-1] get
      histogrammed inside the SIMD loop and bytes [1..body_end] get their
      cross-products via pmaddubs over (a=buf[i..i+31], b=buf[i+1..i+32]).
      That covers byte-pairs (buf[i], buf[i+1]) for i in [0..body_end-1],
      which yields products buf[0]*buf[1], buf[1]*buf[2], ...,
      buf[body_end-1]*buf[body_end].  */

  /*  Initialise first_byte / carry from scalar entry into byte 0.  */
  if (!st->have_first) { st->first_byte = buf[0]; st->have_first = 1; }
  /*  Cross product carry * buf[0] (only if we had a previous chunk):  */
  if (st->have_carry) {
    st->cross_product += (i64) st->carry_byte * (i64) buf[0];
  }
  st->have_carry = 1;

  const __m256i sign_xor   = _mm256_set1_epi8((char) 0x80);
  const __m256i zero       = _mm256_setzero_si256();
  __m256i scc_acc64        = _mm256_setzero_si256(); /*  4 i64 lanes  */
  __m256i lhs_sad          = _mm256_setzero_si256();

  /*  Histogram: 4 banks. We process 16 bytes per quarter-iter (4 quarters
      per stride = 64 bytes). Within a quarter, byte 0..3 hit banks 0..3.  */
  u32 * b0 = st->bank[0];
  u32 * b1 = st->bank[1];
  u32 * b2 = st->bank[2];
  u32 * b3 = st->bank[3];

  /*  MC Pi interleaved into the inner loop. Hoist state into locals so
      the compiler can keep them in registers.  */
  int mc_pos     = st->mc_pos;
  u8  m0 = st->mc_buf[0], m1 = st->mc_buf[1], m2 = st->mc_buf[2];
  u8  m3 = st->mc_buf[3], m4 = st->mc_buf[4], m5 = st->mc_buf[5];
  u64 mc_count   = st->mc_count;
  u64 mc_inside  = st->mc_inside;

  const __m256i ones16_sum = _mm256_set1_epi16(1);
  /*  Software prefetch: pulls cache lines ~8 strides ahead into L2.
      Effective on streaming workloads where the HW prefetcher under-
      pipelines the memory controller.  */
  #define FASTENT_PREFETCH_DIST 512
  for (sz i = 0; i < body_end; i += 64) {
    __builtin_prefetch(buf + i + FASTENT_PREFETCH_DIST, 0, 1);
    /*  SCC: two 32-byte chunks, widen-mul-madd path (no saturation).  */
    __m256i va0 = _mm256_loadu_si256((const __m256i *) (buf + i +  0));
    __m256i vb0 = _mm256_loadu_si256((const __m256i *) (buf + i +  1));
    __m256i va1 = _mm256_loadu_si256((const __m256i *) (buf + i + 32));
    __m256i vb1 = _mm256_loadu_si256((const __m256i *) (buf + i + 33));
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

    /*  i32 lane sums fit no problem (each is sum of 2 byte-products in
        [-65280..64770]). Per iter we add four such i32 vectors here.
        However, accumulating across many iters in an i32 vector
        overflows for input with consistent-sign products (e.g.
        alternating.bin). Convert to i64 lanes for accumulation.  */
    __m256i sum32 = _mm256_add_epi32(_mm256_add_epi32(s0_lo, s0_hi),
                                     _mm256_add_epi32(s1_lo, s1_hi));
    __m256i sum64_lo = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(sum32));
    __m256i sum64_hi = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(sum32, 1));
    scc_acc64 = _mm256_add_epi64(scc_acc64,
                  _mm256_add_epi64(sum64_lo, sum64_hi));
    /*  LHS byte sum via PSADBW for sign correction.  */
    __m256i sad0 = _mm256_sad_epu8(va0, zero);
    __m256i sad1 = _mm256_sad_epu8(va1, zero);
    lhs_sad = _mm256_add_epi64(lhs_sad, _mm256_add_epi64(sad0, sad1));

    /*  Histogram: 64 byte increments across 4 banks. Direct movzbl
        reads from the input buffer; compiler emits a tight load/inc
        chain that pipelines through the L1d cache.  */
    const u8 * p = buf + i;
    #define HIST4(o) \
      b0[p[(o) + 0]]++; b1[p[(o) + 1]]++; \
      b2[p[(o) + 2]]++; b3[p[(o) + 3]]++
    HIST4( 0); HIST4( 4); HIST4( 8); HIST4(12);
    HIST4(16); HIST4(20); HIST4(24); HIST4(28);
    HIST4(32); HIST4(36); HIST4(40); HIST4(44);
    HIST4(48); HIST4(52); HIST4(56); HIST4(60);
    #undef HIST4

    /*  MC Pi. Drain ring if needed, then fire bulk hexads. The bulk
        loop uses two interleaved accumulators (mi_a, mi_b) to break
        the sbb serial dependency on a single accumulator.  */
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

    unsigned p_idx;
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

    unsigned n_hexads = (64u - p_idx) / 6u;
    /*  SIMD bulk MC Pi: process pairs of hexads (2 hexads = 12 bytes)
        via PSHUFB + VPMULUDQ. SSE4.2 cmpgt_epi64 is fine here because
        the AVX2 variant always has it.  */
    const u8 * q = p + p_idx;
    const __m128i mc_shuf = _mm_setr_epi8(2, 1, 0, -1, 5, 4, 3, -1,
                                          8, 7, 6, -1, 11, 10, 9, -1);
    const __m128i mc_lim  = _mm_set1_epi64x((i64)(FASTENT_INCIRC + 1ULL));
    unsigned k = 0;
    for (; k + 2 <= n_hexads; k += 2) {
      __m128i v   = _mm_loadu_si128((const __m128i *) (q + k * 6u));
      __m128i xy  = _mm_shuffle_epi8(v, mc_shuf);          /*  [x0,y0,x1,y1]  */
      __m128i xs  = _mm_mul_epu32(xy, xy);                  /*  x0^2, x1^2 (u64)  */
      __m128i yshr = _mm_srli_epi64(xy, 32);
      __m128i ys  = _mm_mul_epu32(yshr, yshr);              /*  y0^2, y1^2     */
      __m128i d   = _mm_add_epi64(xs, ys);
      __m128i mask = _mm_cmpgt_epi64(mc_lim, d);
      int bits = _mm_movemask_pd(_mm_castsi128_pd(mask));
      mi_a += (u64) __builtin_popcount(bits);
    }
    /*  Scalar tail (0 or 1 hexad).  */
    for (; k < n_hexads; k++) {
      unsigned o = k * 6u;
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
    unsigned stash_at    = p_idx + n_hexads * 6u;
    unsigned stash_count = 64u - stash_at;
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

  /*  scc_sum is sum a_i*(b_i-128). Add 128*sum_a_i to recover sum a_i*b_i.  */
  st->cross_product += scc_sum + (i64)(128ULL * lhs_sum);

  /*  Epilogue: process buf[body_end] for histogram + carry + MC. This
      byte was read by the SCC shifted load (so its product with
      buf[body_end-1] is already in cross_product) but not yet
      histogrammed or fed to MC Pi.  */
  {
    u8 b = buf[body_end];
    st->total_bytes += body_end;
    st->bank[(unsigned)(st->total_bytes) & (FASTENT_BANKS - 1)][b]++;
    st->total_bytes++;
    st->carry_byte = b;
    st->last_byte  = b;

    /*  MC: push b into the live ring (still in registers) and possibly
        fire one hexad, then save state.  */
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

#elif defined(FASTENT_VARIANT_SSE41) || defined(FASTENT_VARIANT_SSSE3)

static inline sz FASTENT_FN(simd_body)(fastent_chunk_state * st,
                                   const u8 * FASTENT_RESTRICT buf,
                                   sz len) {
  /*  SSE stride = 32 bytes (two 16-byte vectors). Need 16-byte read-ahead.  */
  if (len < 33) return 0;
  const sz body_max = len - 16;
  sz iters = body_max / 32;
  if (iters == 0) return 0;
  const sz body_end = iters * 32;

  if (!st->have_first) { st->first_byte = buf[0]; st->have_first = 1; }
  if (st->have_carry) {
    st->cross_product += (i64) st->carry_byte * (i64) buf[0];
  }
  st->have_carry = 1;

  const __m128i sign_xor   = _mm_set1_epi8((char) 0x80);
  const __m128i ones16_sum = _mm_set1_epi16(1);
  const __m128i zero       = _mm_setzero_si128();
  __m128i scc_acc64        = _mm_setzero_si128();  /*  2 i64 lanes  */
  __m128i lhs_sad          = _mm_setzero_si128();

  u32 * b0 = st->bank[0];
  u32 * b1 = st->bank[1];
  u32 * b2 = st->bank[2];
  u32 * b3 = st->bank[3];

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
    __m128i vbs0 = _mm_xor_si128(vb0, sign_xor);
    __m128i vbs1 = _mm_xor_si128(vb1, sign_xor);

    /*  Widen each 16-byte vector to 8 i16 (low / high halves). SSSE3-
        compatible: zero-extend va via unpack with zero; sign-extend vbs
        via unpack with cmpgt(0, vbs) which produces a 0/-1 sign mask.  */
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

    /*  Widen 4 i32 lanes to 4 i64 lanes via sign-shift + unpack
        (SSSE3-compatible). Then accumulate as i64.  */
    __m128i sum32 = _mm_add_epi32(_mm_add_epi32(s0_lo, s0_hi),
                                  _mm_add_epi32(s1_lo, s1_hi));
    __m128i sign = _mm_srai_epi32(sum32, 31);  /*  i32 sign bits  */
    __m128i sum64_lo = _mm_unpacklo_epi32(sum32, sign);
    __m128i sum64_hi = _mm_unpackhi_epi32(sum32, sign);
    scc_acc64 = _mm_add_epi64(scc_acc64, _mm_add_epi64(sum64_lo, sum64_hi));
    __m128i sad0 = _mm_sad_epu8(va0, zero);
    __m128i sad1 = _mm_sad_epu8(va1, zero);
    lhs_sad = _mm_add_epi64(lhs_sad, _mm_add_epi64(sad0, sad1));

    /*  Direct byte reads -> movzx + inc, no vpextrb chain.  */
    const u8 * p = buf + i;
    #define HIST4(o) \
      b0[p[(o) + 0]]++; b1[p[(o) + 1]]++; \
      b2[p[(o) + 2]]++; b3[p[(o) + 3]]++
    HIST4( 0); HIST4( 4); HIST4( 8); HIST4(12);
    HIST4(16); HIST4(20); HIST4(24); HIST4(28);
    #undef HIST4

    /*  MC Pi: same generic drain + bulk + stash pattern as AVX2,
        scaled for a 32-byte stride (4-5 hexads per iter).  */
    #define MC_HEXAD(x0, x1, x2, y0, y1, y2) do { \
      u32 x = ((u32)(x0) << 16) | ((u32)(x1) << 8) | (u32)(x2); \
      u32 y = ((u32)(y0) << 16) | ((u32)(y1) << 8) | (u32)(y2); \
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y; \
      mc_count++; \
      mc_inside += (d <= FASTENT_INCIRC); \
    } while (0)

    unsigned p_idx;
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
    unsigned n_hexads   = (32u - p_idx) / 6u;
    for (unsigned k = 0; k < n_hexads; k++) {
      unsigned o = p_idx + k * 6u;
      MC_HEXAD(p[o+0], p[o+1], p[o+2], p[o+3], p[o+4], p[o+5]);
    }
    unsigned stash_at    = p_idx + n_hexads * 6u;
    unsigned stash_count = 32u - stash_at;
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

  /*  Epilogue: histogram + MC for buf[body_end] (SCC product already
      added by the shifted load in the last pmaddubs iter).  */
  {
    u8 b = buf[body_end];
    st->total_bytes += body_end;
    st->bank[(unsigned)(st->total_bytes) & (FASTENT_BANKS - 1)][b]++;
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

  /*  carry/last get the value from buf[body_end].  */
  st->carry_byte = buf[body_end];
  st->last_byte  = buf[body_end];

  return body_end + 1;
}

#endif

/*  ----------------------------------------------------------------------
    Public entry point.  */

void FASTENT_FN(analyze)(fastent_chunk_state * st, const u8 * FASTENT_RESTRICT buf, sz len) {
  if (len == 0) return;

#ifdef FASTENT_HAVE_SIMD
  sz body = FASTENT_FN(simd_body)(st, buf, len);
  if (body > 0) {
    sz start_bank = st->total_bytes;
    FASTENT_FN(scalar_body)(st, buf + body, len - body, start_bank);
  } else {
    sz start_bank = st->total_bytes;
    FASTENT_FN(scalar_body)(st, buf, len, start_bank);
  }
#else
  sz start_bank = st->total_bytes;
  FASTENT_FN(scalar_body)(st, buf, len, start_bank);
#endif
  /*  MC Pi has been folded into the histogram/SCC pass and into
      consume_byte; no separate walk required.  */
}
