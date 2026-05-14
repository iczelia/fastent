/*  fastent: templated inner-loop body. Included from analyze-scalar.c,
    analyze-ssse3.c, analyze-sse41.c, analyze-avx2.c.

    Each TU defines one of:
      FASTENT_VARIANT_SCALAR
      FASTENT_VARIANT_SSSE3
      FASTENT_VARIANT_SSE41
      FASTENT_VARIANT_AVX2

    and (optionally) FASTENT_HAVE_SIMD if the SIMD body should be used.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).

    Algorithm notes.

    For an input byte stream b[0..N-1] we compute, in a single pass:

      hist[v]     = sum_i [b[i] == v]            (256 bins, banked 4-way)
      cross_prod  = sum_{i=0..N-2} b[i]*b[i+1]   (SCC partial; wrap added
                                                  at finalize: + b[N-1]*b[0])
      mc_inside   = number of hexads (b[6k..6k+5]) whose two 24-bit
                    components squared-and-summed are <= INCIRC.

    SCC SIMD path: PMADDUBSW saturates so we widen to 16-bit and use
    signed-mul.  Pre-XOR b with 0x80 to map b -> b - 128 in i8 range;
    mullo_epi16 gives the canonical signed product a*(b-128); madd_epi16
    sums adjacent pairs into i32 (no saturation).  The 128*sum_a
    correction is computed on the fly via PSADBW and added inside the
    same loop, so st->cross_product is always the canonical sum after
    every analyze() call.

    MC Pi is interleaved into the SIMD body via a scalar drain + SIMD
    bulk + scalar tail dance per stride; the persistent 6-byte ring
    handles cross-call boundaries.  */

#include "analyze.h"

#ifdef FASTENT_HAVE_SIMD
  #include <immintrin.h>
#endif

#if defined(FASTENT_VARIANT_AVX512)
  #define FASTENT_VAR_SUFFIX _avx512
  #define FASTENT_SIMD_VEC   __m512i
  #define FASTENT_SIMD_VLEN  64
  #define V_SET1_EPI8(x)       _mm512_set1_epi8((char)(x))
  #define V_SETZERO()          _mm512_setzero_si512()
  #define V_LOAD(p)            _mm512_loadu_si512((const void *)(p))
  #define V_STORE(p, v)        _mm512_storeu_si512((void *)(p), (v))
  #define V_AND(a, b)          _mm512_and_si512((a), (b))
  #define V_OR(a, b)           _mm512_or_si512((a), (b))
  #define V_ANDNOT(a, b)       _mm512_andnot_si512((a), (b))
  #define V_ADD_EPI8(a, b)     _mm512_add_epi8((a), (b))
  #define V_ADD_EPI64(a, b)    _mm512_add_epi64((a), (b))
  #define V_SUBS_EPU8(a, b)    _mm512_subs_epu8((a), (b))
  /*  CMPEQ on AVX-512 returns a mask register, not a vector; we
      synthesize a 0/-1 vector by masking a -1 splat so the fold helper
      can stay variant-agnostic.  */
  #define V_CMPEQ_EPI8(a, b)   _mm512_maskz_set1_epi8( \
                                  _mm512_cmpeq_epi8_mask((a), (b)), -1)
  #define V_SRLI_EPI16(a, n)   _mm512_srli_epi16((a), (n))
  #define V_SHUFFLE_EPI8(t, i) _mm512_shuffle_epi8((t), (i))
  #define V_SAD_EPU8(a, b)     _mm512_sad_epu8((a), (b))
#elif defined(FASTENT_VARIANT_AVX2)
  #define FASTENT_VAR_SUFFIX _avx2
  #define FASTENT_SIMD_VEC   __m256i
  #define FASTENT_SIMD_VLEN  32
  #define V_SET1_EPI8(x)       _mm256_set1_epi8((char)(x))
  #define V_SETZERO()          _mm256_setzero_si256()
  #define V_LOAD(p)            _mm256_loadu_si256((const __m256i *)(p))
  #define V_STORE(p, v)        _mm256_storeu_si256((__m256i *)(p), (v))
  #define V_AND(a, b)          _mm256_and_si256((a), (b))
  #define V_OR(a, b)           _mm256_or_si256((a), (b))
  #define V_ANDNOT(a, b)       _mm256_andnot_si256((a), (b))
  #define V_ADD_EPI8(a, b)     _mm256_add_epi8((a), (b))
  #define V_ADD_EPI64(a, b)    _mm256_add_epi64((a), (b))
  #define V_SUBS_EPU8(a, b)    _mm256_subs_epu8((a), (b))
  #define V_CMPEQ_EPI8(a, b)   _mm256_cmpeq_epi8((a), (b))
  #define V_SRLI_EPI16(a, n)   _mm256_srli_epi16((a), (n))
  #define V_SHUFFLE_EPI8(t, i) _mm256_shuffle_epi8((t), (i))
  #define V_SAD_EPU8(a, b)     _mm256_sad_epu8((a), (b))
#elif defined(FASTENT_VARIANT_SSE41)
  #define FASTENT_VAR_SUFFIX _sse41
  #define FASTENT_SIMD_VEC   __m128i
  #define FASTENT_SIMD_VLEN  16
  #define V_SET1_EPI8(x)       _mm_set1_epi8((char)(x))
  #define V_SETZERO()          _mm_setzero_si128()
  #define V_LOAD(p)            _mm_loadu_si128((const __m128i *)(p))
  #define V_STORE(p, v)        _mm_storeu_si128((__m128i *)(p), (v))
  #define V_AND(a, b)          _mm_and_si128((a), (b))
  #define V_OR(a, b)           _mm_or_si128((a), (b))
  #define V_ANDNOT(a, b)       _mm_andnot_si128((a), (b))
  #define V_ADD_EPI8(a, b)     _mm_add_epi8((a), (b))
  #define V_ADD_EPI64(a, b)    _mm_add_epi64((a), (b))
  #define V_SUBS_EPU8(a, b)    _mm_subs_epu8((a), (b))
  #define V_CMPEQ_EPI8(a, b)   _mm_cmpeq_epi8((a), (b))
  #define V_SRLI_EPI16(a, n)   _mm_srli_epi16((a), (n))
  #define V_SHUFFLE_EPI8(t, i) _mm_shuffle_epi8((t), (i))
  #define V_SAD_EPU8(a, b)     _mm_sad_epu8((a), (b))
#elif defined(FASTENT_VARIANT_SSSE3)
  #define FASTENT_VAR_SUFFIX _ssse3
  #define FASTENT_SIMD_VEC   __m128i
  #define FASTENT_SIMD_VLEN  16
  #define V_SET1_EPI8(x)       _mm_set1_epi8((char)(x))
  #define V_SETZERO()          _mm_setzero_si128()
  #define V_LOAD(p)            _mm_loadu_si128((const __m128i *)(p))
  #define V_STORE(p, v)        _mm_storeu_si128((__m128i *)(p), (v))
  #define V_AND(a, b)          _mm_and_si128((a), (b))
  #define V_OR(a, b)           _mm_or_si128((a), (b))
  #define V_ANDNOT(a, b)       _mm_andnot_si128((a), (b))
  #define V_ADD_EPI8(a, b)     _mm_add_epi8((a), (b))
  #define V_ADD_EPI64(a, b)    _mm_add_epi64((a), (b))
  #define V_SUBS_EPU8(a, b)    _mm_subs_epu8((a), (b))
  #define V_CMPEQ_EPI8(a, b)   _mm_cmpeq_epi8((a), (b))
  #define V_SRLI_EPI16(a, n)   _mm_srli_epi16((a), (n))
  #define V_SHUFFLE_EPI8(t, i) _mm_shuffle_epi8((t), (i))
  #define V_SAD_EPU8(a, b)     _mm_sad_epu8((a), (b))
#else
  #define FASTENT_VAR_SUFFIX _scalar
#endif

#define FASTENT_CAT2(a, b) a##b
#define FASTENT_CAT(a, b)  FASTENT_CAT2(a, b)
#define FASTENT_FN(name)   FASTENT_CAT(name, FASTENT_VAR_SUFFIX)

/*  In-register case fold helpers. These produce a byte (or vector of
    bytes) that has ASCII A-Z and Latin-1 0xC0-0xDE (excluding 0xD7)
    mapped to lower-case; every other byte passes through unchanged.
    Both helpers are private to this TU and used by the fused
    fold + analyse path so we never have to materialise a staging copy
    of the input buffer.  See also the standalone fold() body further
    down, which calls fold_vec_inline directly.  */

static inline int FASTENT_FN(fold_is_upper_inline)(unsigned c) {
  return ((unsigned)(c - 'A')   < 26u) ||
         ((unsigned)(c - 0xC0u) < 31u && c != 0xD7u);
}

static inline u8 FASTENT_FN(fold_byte_inline)(u8 b) {
  unsigned c = b;
  if (FASTENT_FN(fold_is_upper_inline)(c)) return (u8)(c + 0x20u);
  return b;
}

#ifdef FASTENT_HAVE_SIMD
static __attribute__((always_inline)) inline FASTENT_SIMD_VEC
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

/*  Scalar single-byte update: histogram + SCC + MC Pi + first/last
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

/*  Scalar histogram + SCC body. Used as fallback and on chunk edges.
    Templated on `fold`: when set, each byte is folded in-register
    before being fed to consume_byte. `fold` is a compile-time constant
    at every call site so the branch dead-eliminates.  */

static __attribute__((always_inline)) inline sz
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

/*  SIMD body: histogram (4 banks rotating by within-iter position mod 4)
    + SCC pmaddubs sign-corrected accumulator. Each variant has a
    slightly different version below.  */

#if defined(FASTENT_VARIANT_AVX2)

static __attribute__((always_inline)) inline sz
FASTENT_FN(simd_body_impl)(fastent_chunk_state * st,
                           const u8 * FASTENT_RESTRICT buf,
                           sz len, int fold) {
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

  /*  In fused-fold mode all the per-byte scalar inputs (first_byte
      seed, SCC carry, epilogue byte) must observe folded values so
      they agree with what the SIMD loop sees.  */
  u8 b0_user = fold ? FASTENT_FN(fold_byte_inline)(buf[0]) : buf[0];

  /*  Initialise first_byte / carry from scalar entry into byte 0.  */
  if (!st->have_first) { st->first_byte = b0_user; st->have_first = 1; }
  /*  Cross product carry * buf[0] (only if we had a previous chunk):  */
  if (st->have_carry)
    st->cross_product += (i64) st->carry_byte * (i64) b0_user;
  st->have_carry = 1;

  const __m256i sign_xor   = _mm256_set1_epi8((char) 0x80);
  const __m256i zero       = _mm256_setzero_si256();
  __m256i scc_acc64        = _mm256_setzero_si256(); /*  4 i64 lanes  */
  __m256i lhs_sad          = _mm256_setzero_si256();

  /*  Histogram: 4 banks. We process 16 bytes per quarter-iter (4 quarters
      per stride = 64 bytes). Within a quarter, byte 0..3 hit banks 0..3.  */
  u32 * FASTENT_RESTRICT b0 = st->bank[0];
  u32 * FASTENT_RESTRICT b1 = st->bank[1];
  u32 * FASTENT_RESTRICT b2 = st->bank[2];
  u32 * FASTENT_RESTRICT b3 = st->bank[3];

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
    /*  SCC: two 32-byte chunks, widen-mul-madd path (no saturation).
        In fused-fold mode every loaded vector is folded in-register
        before any downstream op consumes it.  */
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

    /*  Histogram: 64 byte increments across 4 banks via direct movzbl
        loads (or, in fused-fold mode, movzbl loads from an L1-resident
        stack stage of the folded va0/va1).  The asm launder on the
        stage pointer is load-bearing: without it GCC turns the byte
        reads into a vpextrb chain off the just-stored vector, which
        serialises the histogram and costs ~2x.  */
    u8 stage[64] __attribute__((aligned(32)));
    const u8 * p;
    if (fold) {
      _mm256_store_si256((__m256i *) (stage +  0), va0);
      _mm256_store_si256((__m256i *) (stage + 32), va1);
      const u8 * sp = stage;
      __asm__("" : "+r"(sp) :: "memory");
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
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
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

#elif defined(FASTENT_VARIANT_AVX512)

/*  AVX-512 SIMD body.

    Stride = 128 bytes (two 64-byte vectors).  The SCC pmaddubs-like
    path is the same as AVX2, just widened to 512-bit lanes via
    _mm512_madd_epi16 over u16 x i16 products.  The byte histogram
    stays scalar 4-banked: every AVX-512 CD scatter-based variant we
    benchmarked (Intel Opt. Manual section 15.16.1 vpermd-fold, Cordes
    replicated 16x sub-histograms, naive popcount-scatter) lost to the
    scalar inc-mem chain by 1.5-3x on Zen 4 because VPSCATTERDD is
    ~16 c reciprocal throughput on this microarchitecture.  */

static __attribute__((always_inline)) inline sz
FASTENT_FN(simd_body_impl)(fastent_chunk_state * st,
                           const u8 * FASTENT_RESTRICT buf,
                           sz len, int fold) {
  /*  AVX-512 stride = 128 bytes (two 64-byte vectors). Need byte +1
      readable for the SCC shifted load on the last iter, so the body
      stops at len - 64 (leave 64-byte read-ahead margin).  */
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

  #define FASTENT_PREFETCH_DIST 512
  for (sz i = 0; i < body_end; i += 128) {
    __builtin_prefetch(buf + i + FASTENT_PREFETCH_DIST + 0,  0, 1);
    __builtin_prefetch(buf + i + FASTENT_PREFETCH_DIST + 64, 0, 1);

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

    /*  Accumulate per-pair i32 sums as i64 lanes. Each i32 lane is the
        sum of two byte-products in [-65280..64770]; we widen to i64
        before accumulating across iters to avoid overflow on
        consistent-sign inputs.  */
    __m512i sum32 = _mm512_add_epi32(_mm512_add_epi32(s0_lo, s0_hi),
                                     _mm512_add_epi32(s1_lo, s1_hi));
    __m512i sum64_lo = _mm512_cvtepi32_epi64(_mm512_castsi512_si256(sum32));
    __m512i sum64_hi = _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(sum32, 1));
    scc_acc64 = _mm512_add_epi64(scc_acc64,
                  _mm512_add_epi64(sum64_lo, sum64_hi));
    /*  LHS byte sum via PSADBW for the sign correction.  */
    __m512i sad0 = _mm512_sad_epu8(va0, zero512);
    __m512i sad1 = _mm512_sad_epu8(va1, zero512);
    lhs_sad = _mm512_add_epi64(lhs_sad, _mm512_add_epi64(sad0, sad1));

    /*  Histogram & MC Pi stage buffer.  In fused-fold mode we go via
        an L1-resident stack stage (with the same asm pointer laundering
        as the AVX2 path); else we read directly from buf.  */
    u8 stage[128] __attribute__((aligned(64)));
    const u8 * p;
    if (fold) {
      _mm512_store_si512((void *) (stage +  0), va0);
      _mm512_store_si512((void *) (stage + 64), va1);
      const u8 * sp = stage;
      __asm__("" : "+r"(sp) :: "memory");
      p = sp;
    } else {
      p = buf + i;
    }

    /*  Scalar 4-banked inc-mem chain: 128 byte increments per iter
        distributed across 4 bank arrays; mirrors the AVX2 path's bank
        discipline so cross-variant tail merges line up.  */
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

    /*  MC Pi: same drain + bulk + stash dance as the AVX2 body, but
        sized for a 128-byte stride (so up to 21 hexads per iter).  */
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

    unsigned n_hexads = (128u - p_idx) / 6u;
    const u8 * q = p + p_idx;
    const __m128i mc_shuf = _mm_setr_epi8(2, 1, 0, -1, 5, 4, 3, -1,
                                          8, 7, 6, -1, 11, 10, 9, -1);
    const __m128i mc_lim  = _mm_set1_epi64x((i64)(FASTENT_INCIRC + 1ULL));
    unsigned k = 0;
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
      mi_a += (u64) __builtin_popcount(bits);
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
      mi_b += (u64) __builtin_popcount(bits);
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

    unsigned stash_at    = p_idx + n_hexads * 6u;
    unsigned stash_count = 128u - stash_at;
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
    st->bank[(unsigned)(st->total_bytes) & (FASTENT_BANKS - 1)][b]++;
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

static __attribute__((always_inline)) inline sz
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

    /*  Same stage-buffer + asm-launder pattern as the AVX2 body; the
        asm is what forces movzbl-from-L1 instead of vpextrb.  */
    u8 stage[32] __attribute__((aligned(16)));
    const u8 * p;
    if (fold) {
      _mm_store_si128((__m128i *) (stage +  0), va0);
      _mm_store_si128((__m128i *) (stage + 16), va1);
      const u8 * sp = stage;
      __asm__("" : "+r"(sp) :: "memory");
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
    int n_hexads = (int)((32u - p_idx) / 6u);
    Fk(n_hexads,
       unsigned o = p_idx + (unsigned) k * 6u;
       MC_HEXAD(p[o+0], p[o+1], p[o+2], p[o+3], p[o+4], p[o+5]))
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
  u8 last_b;
  {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
    last_b = b;
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

  /*  carry/last get the value from buf[body_end] (folded if -f).  */
  st->carry_byte = last_b;
  st->last_byte  = last_b;

  return body_end + 1;
}

#endif

/*  SIMD trampolines: the simd_body_impl above is templated on the
    compile-time `fold` constant; these inline trampolines pin it for
    the two analyse entry points.  Emitted for both AVX2 and SSE
    variants (whichever owns simd_body_impl in this TU).  */
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

static __attribute__((always_inline)) inline void
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
  /*  MC Pi has been folded into the histogram/SCC pass and into
      consume_byte; no separate walk required.  */
}

void FASTENT_FN(analyze)(fastent_chunk_state * st, const u8 * FASTENT_RESTRICT buf, sz len) {
  FASTENT_FN(analyze_impl)(st, buf, len, 0);
}

void FASTENT_FN(analyze_fold)(fastent_chunk_state * st, const u8 * FASTENT_RESTRICT buf, sz len) {
  FASTENT_FN(analyze_impl)(st, buf, len, 1);
}

/*  Case fold: ASCII A-Z and Latin-1 0xC0-0xDE (excluding 0xD7) folded
    to lower-case in place. Range tests use saturating unsigned subtract
    so we stay on SSSE3-grade integer ops only.  */

static inline int FASTENT_FN(fold_is_upper_scalar)(unsigned c) {
  return ((unsigned)(c - 'A')  < 26u) ||
         ((unsigned)(c - 0xC0u) < 31u && c != 0xD7u);
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

      /*  ASCII: c in ['A', 'Z'].
          subs_epu8(K, c) saturates to 0 iff c >= K (because then
          K - c <= 0).  So s_ge_a == 0 iff c >= 'A', s_le_z == 0 iff
          c <= 'Z'.  Both zero <=> in range.  */
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
    unsigned c = buf[i];
    if (FASTENT_FN(fold_is_upper_scalar)(c)) buf[i] = (u8)(c + 0x20u);
  }
}

/*  Bit-mode analyser.

    For an input byte stream b[0..N-1] the bit stream interleaves bits
    MSB-first (b[i] bit 7, bit 6, ..., bit 0, b[i+1] bit 7, ...). We
    compute, in one pass over the byte buffer:

      bit_hist[1]  += sum_i popcount(b[i])
      bit_hist[0]  += 8*N - bit_hist[1]
      cross_product +=  sum_i popcount(b[i] & (b[i] >> 1))   (within-byte)
                      + sum_{i<N-1} (b[i] & 1) & (b[i+1] >> 7)   (cross-byte)
      MC Pi is byte-driven (one trial per 6 input bytes, same as byte
      mode).

    The SIMD body vectorises the three counters via PSHUFB-LUT popcount
    + PSADBW reduction. Per VLEN bytes we issue ~3 popcounts and 3 SADs.
    Carry / first / last are stored as bit values (0 or 1) so that the
    existing run_mmap_mt merge (carry * first) is exactly the cross-slab
    bit-pair contribution; no separate bit-mode merge is needed.  */

#ifdef FASTENT_HAVE_SIMD

static __attribute__((always_inline)) inline sz
FASTENT_FN(bits_simd_body_impl)(fastent_chunk_state * st,
                                const u8 * FASTENT_RESTRICT buf,
                                sz len, int fold) {
  /*  We need buf[i+VLEN] readable in the last iter for the +1 shifted
      load (used by the cross-byte computation), so the body stops at
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
    unsigned prev_lsb = (unsigned)(st->carry_byte & 1u);
    unsigned curr_msb = (unsigned)((b0_user >> 7) & 1u);
    st->cross_product += (i64)(prev_lsb & curr_msb);
  }
  st->have_carry = 1;

  /*  PSHUFB-LUT for nibble popcount. AVX2 PSHUFB is per-lane so we
      duplicate the 16-entry table into both lanes.  On AVX-512 we have
      VPOPCNTB (BITALG + VPOPCNTDQ) directly, so the LUT is unused.  */
#if defined(FASTENT_VARIANT_AVX2)
  const FASTENT_SIMD_VEC popcnt_lut = _mm256_setr_epi8(
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
#elif defined(FASTENT_VARIANT_AVX512)
  /*  Unused on AVX-512: VPOPCNTB replaces the LUT lookups.  */
#else
  const FASTENT_SIMD_VEC popcnt_lut = _mm_setr_epi8(
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
#endif
#if !defined(FASTENT_VARIANT_AVX512)
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
    __builtin_prefetch(buf + i + 512, 0, 1);

    FASTENT_SIMD_VEC va = V_LOAD(buf + i);
    FASTENT_SIMD_VEC vb = V_LOAD(buf + i + 1);
    if (fold) {
      va = FASTENT_FN(fold_vec_inline)(va);
      vb = FASTENT_FN(fold_vec_inline)(vb);
    }

#if defined(FASTENT_VARIANT_AVX512)
    /*  Direct byte-wise popcount via VPOPCNTB; horizontal reduce to
        qword via PSADBW.  */
    FASTENT_SIMD_VEC pc_va = _mm512_popcnt_epi8(va);
    acc_ones = V_ADD_EPI64(acc_ones, V_SAD_EPU8(pc_va, zero));

    /*  Within-byte adjacent-1 pairs.  */
    FASTENT_SIMD_VEC va_shr1 = V_AND(V_SRLI_EPI16(va, 1), mask_7f);
    FASTENT_SIMD_VEC pairs   = V_AND(va, va_shr1);
    FASTENT_SIMD_VEC pc_pairs = _mm512_popcnt_epi8(pairs);
    acc_within = V_ADD_EPI64(acc_within, V_SAD_EPU8(pc_pairs, zero));
#else
    /*  popcount(va) byte-wise via PSHUFB-LUT.  */
    FASTENT_SIMD_VEC lo = V_AND(va, nibble_mask);
    FASTENT_SIMD_VEC hi = V_AND(V_SRLI_EPI16(va, 4), nibble_mask);
    FASTENT_SIMD_VEC pc_va = V_ADD_EPI8(V_SHUFFLE_EPI8(popcnt_lut, lo),
                                         V_SHUFFLE_EPI8(popcnt_lut, hi));
    acc_ones = V_ADD_EPI64(acc_ones, V_SAD_EPU8(pc_va, zero));

    /*  Within-byte adjacent-1 pairs: popcount(va & (va >> 1)).
        SRLI_EPI16 + AND 0x7F gives byte-wise >> 1.  */
    FASTENT_SIMD_VEC va_shr1 = V_AND(V_SRLI_EPI16(va, 1), mask_7f);
    FASTENT_SIMD_VEC pairs   = V_AND(va, va_shr1);
    FASTENT_SIMD_VEC plo = V_AND(pairs, nibble_mask);
    FASTENT_SIMD_VEC phi = V_AND(V_SRLI_EPI16(pairs, 4), nibble_mask);
    FASTENT_SIMD_VEC pc_pairs = V_ADD_EPI8(V_SHUFFLE_EPI8(popcnt_lut, plo),
                                            V_SHUFFLE_EPI8(popcnt_lut, phi));
    acc_within = V_ADD_EPI64(acc_within, V_SAD_EPU8(pc_pairs, zero));
#endif

    /*  Cross-byte pair: (va.LSB & vb.MSB) per lane.
        vb.MSB = (vb >> 7) & 1 (the SRLI epi16 contaminates bit 1+ from
        the adjacent byte but the AND 1 keeps only bit 0).  */
    FASTENT_SIMD_VEC va_lsb = V_AND(va, mask_01);
    FASTENT_SIMD_VEC vb_msb = V_AND(V_SRLI_EPI16(vb, 7), mask_01);
    FASTENT_SIMD_VEC cross  = V_AND(va_lsb, vb_msb);
    acc_cross = V_ADD_EPI64(acc_cross, V_SAD_EPU8(cross, zero));

    /*  MC Pi: scalar drain + SIMD bulk hexads + scalar tail + stash.
        Same stage-buffer + asm-launder pattern as the byte-mode body
        when fold is set (forces movzbl-from-L1, not vpextrb).  */
    u8 bits_stage[FASTENT_SIMD_VLEN] __attribute__((aligned(32)));
    const u8 * p;
    if (fold) {
      V_STORE(bits_stage, va);
      const u8 * sp = bits_stage;
      __asm__("" : "+r"(sp) :: "memory");
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
    unsigned n_hexads = ((unsigned) FASTENT_SIMD_VLEN - p_idx) / 6u;
    /*  SIMD bulk: same intrinsic pattern as the byte-mode bodies.
        Accounts for in-circle hits via mi_simd; mc_count gets all
        n_hexads added below.  */
    u64 mi_simd = 0;
    const u8 * q = p + p_idx;
    const __m128i mc_shuf = _mm_setr_epi8(2, 1, 0, -1, 5, 4, 3, -1,
                                          8, 7, 6, -1, 11, 10, 9, -1);
    const __m128i mc_lim  = _mm_set1_epi64x((i64)(FASTENT_INCIRC + 1ULL));
    unsigned k = 0;
#if defined(FASTENT_VARIANT_AVX2) || defined(FASTENT_VARIANT_AVX512)
    /*  4 hexads per iter via 256-bit. We need 24 bytes of source readable
        per iter; n_hexads <= 5 (AVX2) or <= 10 (AVX-512) so we may see
        2-3 such iters per outer iter on the wider stride.  */
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
      mi_simd += (u64) __builtin_popcount(bits);
    }
#endif
    /*  128-bit residual: 2 hexads per iter. On AVX-2 path requires
        SSE4.2 cmpgt_epi64 (always present with AVX2). On SSE4.1 path
        same. On SSSE3-only we skip the SIMD and fall through to scalar
        because cmpgt_epi64 is SSE4.2.  */
#if defined(FASTENT_VARIANT_AVX2) || defined(FASTENT_VARIANT_SSE41) || defined(FASTENT_VARIANT_AVX512)
    for (; k + 2 <= n_hexads; k += 2) {
      __m128i v   = _mm_loadu_si128((const __m128i *) (q + k * 6u));
      __m128i xy  = _mm_shuffle_epi8(v, mc_shuf);
      __m128i xs  = _mm_mul_epu32(xy, xy);
      __m128i yshr = _mm_srli_epi64(xy, 32);
      __m128i ys  = _mm_mul_epu32(yshr, yshr);
      __m128i d   = _mm_add_epi64(xs, ys);
      __m128i mask = _mm_cmpgt_epi64(mc_lim, d);
      int bits = _mm_movemask_pd(_mm_castsi128_pd(mask));
      mi_simd += (u64) __builtin_popcount(bits);
    }
#else
    (void) mc_shuf; (void) mc_lim;
#endif
    /*  Scalar tail (0/1 hexad on AVX2+SSE41; full on SSSE3).  */
    for (; k < n_hexads; k++) {
      unsigned o = k * 6u;
      u32 x = ((u32) q[o + 0] << 16) | ((u32) q[o + 1] << 8) | q[o + 2];
      u32 y = ((u32) q[o + 3] << 16) | ((u32) q[o + 4] << 8) | q[o + 5];
      u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
      mi_simd += (d <= FASTENT_INCIRC);
    }
    mc_count  += n_hexads;
    mc_inside += mi_simd;
    unsigned stash_at    = p_idx + n_hexads * 6u;
    unsigned stash_count = (unsigned) FASTENT_SIMD_VLEN - stash_at;
    mc_pos = (int) stash_count;
    if (stash_count >= 1) m0 = p[stash_at + 0];
    if (stash_count >= 2) m1 = p[stash_at + 1];
    if (stash_count >= 3) m2 = p[stash_at + 2];
    if (stash_count >= 4) m3 = p[stash_at + 3];
    if (stash_count >= 5) m4 = p[stash_at + 4];
    #undef MC_HEXAD
  }

  /*  Horizontal reduce acc_ones / acc_within / acc_cross (i64 lanes).  */
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

  /*  Epilogue: byte buf[body_end] hasn't yet been histogrammed or fed
      to MC Pi (its cross-byte pair with buf[body_end-1] WAS counted via
      the last iter's shifted load). Process it scalarly.  */
  {
    u8 b = fold ? FASTENT_FN(fold_byte_inline)(buf[body_end])
                : buf[body_end];
    unsigned ones_b = (unsigned) __builtin_popcount(b);
    st->bit_hist[1] += ones_b;
    st->bit_hist[0] += 8u - ones_b;
    unsigned within_b = (unsigned) __builtin_popcount(b & (b >> 1));
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

/*  Scalar bit-mode walker: one byte at a time. Used by the scalar
    variant entry and as the tail of every SIMD body.  Templated on
    `fold` so the fused -f -b path can drive a single-pass scalar
    walker too.  */
static __attribute__((always_inline)) inline void
FASTENT_FN(bits_scalar_body_impl)(fastent_chunk_state * st,
                                  const u8 * FASTENT_RESTRICT buf,
                                  sz len, int fold) {
  for (sz i = 0; i < len; i++) {
    const u8 byte = fold ? FASTENT_FN(fold_byte_inline)(buf[i]) : buf[i];
    const unsigned ones_in_byte = (unsigned) __builtin_popcount(byte);
    st->bit_hist[1] += ones_in_byte;
    st->bit_hist[0] += 8u - ones_in_byte;
    const unsigned within = (unsigned) __builtin_popcount(byte & (byte >> 1));
    st->cross_product += (i64) within;
    if (st->have_carry) {
      const unsigned prev_lsb = (unsigned)(st->carry_byte & 1u);
      const unsigned curr_msb = (unsigned)((byte >> 7) & 1u);
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

static __attribute__((always_inline)) inline void
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
