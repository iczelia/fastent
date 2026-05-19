/*  fastent: SSE4 batched Berlekamp-Massey (2 windows per pass).

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "bm.h"

#include <immintrin.h>
#include <string.h>

/*  Two 512-bit windows run in the 2 u64 lanes of one __m128i; the
    eight polynomial words are eight vectors.  The Massey two-register
    form carries bs_ = x^(N-mm) B advanced by a uniform 1-bit poly *x
    per step, so the discrepancy update is the bare c ^= bs_ with no
    per-lane variable shift, keeping the lanes independent and the
    per-window L bit-identical to the scalar reference.  */

#define WB FASTENT_BM_W64   /*  8 polynomial words  */

static inline void bm_polx_(__m128i v[WB]) {
  for (u32 w = WB; w-- > 1; )
    v[w] = _mm_or_si128(_mm_srli_epi64(v[w], 1),
                        _mm_slli_epi64(v[w - 1], 63));
  v[0] = _mm_srli_epi64(v[0], 1);
}

/*  Per-lane parity of a u64 vector, broadcast to a full-lane mask
    (all-ones where parity is 1).  Fold 64->1 by xor-halving, then
    expand the low bit.  */
static inline __m128i bm_parmask_(__m128i a) {
  a = _mm_xor_si128(a, _mm_srli_epi64(a, 32));
  a = _mm_xor_si128(a, _mm_srli_epi64(a, 16));
  a = _mm_xor_si128(a, _mm_srli_epi64(a, 8));
  a = _mm_xor_si128(a, _mm_srli_epi64(a, 4));
  a = _mm_xor_si128(a, _mm_srli_epi64(a, 2));
  a = _mm_xor_si128(a, _mm_srli_epi64(a, 1));
  a = _mm_and_si128(a, _mm_set1_epi64x(1));
  return _mm_sub_epi64(_mm_setzero_si128(), a);
}

/*  Pack one 64-byte window into 8 u64, MSB-first (byte i high to low,
    bit 7 of byte 0 is poly coefficient 0).  */
static inline void bm_pack_(const u8 * src, u64 s[WB]) {
  Fi(WB, s[i] = 0)
  for (u32 i = 0; i < FASTENT_BM_WB; i++)
    s[i >> 3] |= (u64) src[i] << (56u - 8u * (i & 7u));
}

/*  Process exactly 2 full M-bit windows; write their L into Lout.  */
static void bm_batch2_(const u8 * src, u32 * Lout) {
  u64 sp[2][WB];
  Fi(2, bm_pack_(src + (u32) i * FASTENT_BM_WB, sp[i]))

  __m128i cv[WB], bsv[WB], rv[WB], tv[WB];
  for (u32 w = 0; w < WB; w++) {
    cv[w] = _mm_setzero_si128();
    bsv[w] = _mm_setzero_si128();
    rv[w] = _mm_setzero_si128();
  }
  __m128i one63 = _mm_set1_epi64x((i64) ((u64) 1 << 63));
  cv[0] = one63;
  bsv[0] = one63;
  bm_polx_(bsv);                 /*  bs_ at start of N=0 is x^1 * 1  */

  i64 L[2] = { 0, 0 };

  for (u32 N = 0; N < FASTENT_BM_M; N++) {
    bm_polx_(rv);
    u32 wi = N >> 6, sh = 63u - (N & 63u);
    u64 b0 = (sp[0][wi] >> sh) & 1ull, b1 = (sp[1][wi] >> sh) & 1ull;
    rv[0] = _mm_or_si128(rv[0], _mm_set_epi64x(
        (i64) (b1 << 63), (i64) (b0 << 63)));

    __m128i acc = _mm_setzero_si128();
    for (u32 w = 0; w < WB; w++)
      acc = _mm_xor_si128(acc, _mm_and_si128(cv[w], rv[w]));
    __m128i dmask = bm_parmask_(acc);

    for (u32 w = 0; w < WB; w++) tv[w] = cv[w];
    for (u32 w = 0; w < WB; w++)
      cv[w] = _mm_xor_si128(cv[w], _mm_and_si128(bsv[w], dmask));

    /*  Length change where d set and 2L <= N.  2L<=N is N-2L>=0;
        with L,N small the i64 lane compare is exact.  */
    __m128i Lv = _mm_loadu_si128((const __m128i *) L);
    __m128i twoL = _mm_add_epi64(Lv, Lv);
    __m128i Nv = _mm_set1_epi64x((i64) N);
    /*  2L <= N  <=>  NOT (2L > N).  */
    __m128i gt = _mm_cmpgt_epi64(twoL, Nv);
    __m128i chg = _mm_andnot_si128(gt, dmask);
    for (u32 w = 0; w < WB; w++)
      bsv[w] = _mm_blendv_epi8(bsv[w], tv[w], chg);
    /*  L <- N + 1 - L where chg.  */
    __m128i newL = _mm_sub_epi64(_mm_set1_epi64x((i64) N + 1), Lv);
    Lv = _mm_blendv_epi8(Lv, newL, chg);
    _mm_storeu_si128((__m128i *) L, Lv);

    bm_polx_(bsv);
  }
  Fi(2, Lout[i] = (u32) L[i])
}

/*  Score nfull full windows; SSE in groups of 2.  Returns the count
    actually scored (the largest multiple of 2 <= nfull); the caller
    handles the 0..1 tail with the scalar reference.  */
sz fastent_bm_windows_sse(const u8 * src, sz nfull, u32 * Lout) {
  sz g = nfull & ~(sz) 1;
  for (sz i = 0; i < g; i += 2)
    bm_batch2_(src + i * FASTENT_BM_WB, Lout + i);
  return g;
}
