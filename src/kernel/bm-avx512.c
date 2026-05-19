/*  fastent: AVX-512 batched Berlekamp-Massey (8 windows per pass).

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "bm.h"

#include <immintrin.h>
#include <string.h>

/*  Eight 512-bit windows run in the 8 u64 lanes of one __m512i; the
    eight polynomial words are eight vectors.  The Massey two-register
    form carries bs_ = x^(N-mm) B advanced by a uniform 1-bit poly *x
    per step, so the discrepancy update is the bare c ^= bs_ with no
    per-lane variable shift, keeping the lanes independent and the
    per-window L bit-identical to the scalar reference.  */

#define WB FASTENT_BM_W64   /*  8 polynomial words  */

static inline void bm_polx_(__m512i v[WB]) {
  for (u32 w = WB; w-- > 1; )
    v[w] = _mm512_or_si512(_mm512_srli_epi64(v[w], 1),
                           _mm512_slli_epi64(v[w - 1], 63));
  v[0] = _mm512_srli_epi64(v[0], 1);
}

/*  Per-lane parity of a u64 vector as an 8-bit mask (bit k set where
    lane k has odd popcount).  */
static inline __mmask8 bm_parmask_(__m512i a) {
  a = _mm512_xor_si512(a, _mm512_srli_epi64(a, 32));
  a = _mm512_xor_si512(a, _mm512_srli_epi64(a, 16));
  a = _mm512_xor_si512(a, _mm512_srli_epi64(a, 8));
  a = _mm512_xor_si512(a, _mm512_srli_epi64(a, 4));
  a = _mm512_xor_si512(a, _mm512_srli_epi64(a, 2));
  a = _mm512_xor_si512(a, _mm512_srli_epi64(a, 1));
  return _mm512_test_epi64_mask(a, _mm512_set1_epi64(1));
}

/*  Pack one 64-byte window into 8 u64, MSB-first (byte i high to low,
    bit 7 of byte 0 is poly coefficient 0).  */
static inline void bm_pack_(const u8 * src, u64 s[WB]) {
  Fi(WB, s[i] = 0)
  for (u32 i = 0; i < FASTENT_BM_WB; i++)
    s[i >> 3] |= (u64) src[i] << (56u - 8u * (i & 7u));
}

/*  Process exactly 8 full M-bit windows; write their L into Lout.  */
static void bm_batch8_(const u8 * src, u32 * Lout) {
  u64 sp[8][WB];
  Fi(8, bm_pack_(src + (u32) i * FASTENT_BM_WB, sp[i]))

  __m512i cv[WB], bsv[WB], rv[WB], tv[WB];
  for (u32 w = 0; w < WB; w++) {
    cv[w] = _mm512_setzero_si512();
    bsv[w] = _mm512_setzero_si512();
    rv[w] = _mm512_setzero_si512();
  }
  __m512i one63 = _mm512_set1_epi64((i64) ((u64) 1 << 63));
  cv[0] = one63;
  bsv[0] = one63;
  bm_polx_(bsv);                 /*  bs_ at start of N=0 is x^1 * 1  */

  i64 L[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

  for (u32 N = 0; N < FASTENT_BM_M; N++) {
    bm_polx_(rv);
    u32 wi = N >> 6, sh = 63u - (N & 63u);
    u64 nb[8];
    Fi(8, nb[i] = ((sp[i][wi] >> sh) & 1ull) << 63)
    rv[0] = _mm512_or_si512(rv[0], _mm512_set_epi64(
        (i64) nb[7], (i64) nb[6], (i64) nb[5], (i64) nb[4],
        (i64) nb[3], (i64) nb[2], (i64) nb[1], (i64) nb[0]));

    __m512i acc = _mm512_setzero_si512();
    for (u32 w = 0; w < WB; w++)
      acc = _mm512_xor_si512(acc, _mm512_and_si512(cv[w], rv[w]));
    __mmask8 dm = bm_parmask_(acc);

    for (u32 w = 0; w < WB; w++) tv[w] = cv[w];
    for (u32 w = 0; w < WB; w++)
      cv[w] = _mm512_mask_xor_epi64(cv[w], dm, cv[w], bsv[w]);

    /*  Length change where d set and 2L <= N.  2L<=N is N-2L>=0;
        with L,N small the i64 lane compare is exact.  */
    __m512i Lv = _mm512_loadu_si512((const void *) L);
    __m512i twoL = _mm512_add_epi64(Lv, Lv);
    __m512i Nv = _mm512_set1_epi64((i64) N);
    /*  2L <= N: a true le mask, intersected with the discrepancy.  */
    __mmask8 le = _mm512_cmple_epi64_mask(twoL, Nv);
    __mmask8 chg = dm & le;
    for (u32 w = 0; w < WB; w++)
      bsv[w] = _mm512_mask_blend_epi64(chg, bsv[w], tv[w]);
    /*  L <- N + 1 - L where chg.  */
    __m512i newL = _mm512_sub_epi64(_mm512_set1_epi64((i64) N + 1), Lv);
    Lv = _mm512_mask_blend_epi64(chg, Lv, newL);
    _mm512_storeu_si512((void *) L, Lv);

    bm_polx_(bsv);
  }
  Fi(8, Lout[i] = (u32) L[i])
}

/*  Score nfull full windows; AVX-512 in groups of 8.  Returns the
    count actually scored (the largest multiple of 8 <= nfull); the
    caller handles the 0..7 tail with the scalar reference.  */
sz fastent_bm_windows_avx512(const u8 * src, sz nfull, u32 * Lout) {
  sz g = nfull & ~(sz) 7;
  for (sz i = 0; i < g; i += 8)
    bm_batch8_(src + i * FASTENT_BM_WB, Lout + i);
  return g;
}
