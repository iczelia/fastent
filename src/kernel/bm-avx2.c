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

#include <immintrin.h>
#include <string.h>

/*  Four 512-bit windows run in the 4 u64 lanes of one __m256i; the eight
    polynomial words are eight vectors.  */

#define WB FASTENT_BM_W64   /*  8 polynomial words  */

/*  poly *x on an 8-vector MSB-first polynomial: word w gets
    (w>>1) | (w-1 << 63), word 0 just >>1.  */
static inline void bm_polx_(__m256i v[WB]) {
  u32 w;
  for (w = WB; w-- > 1; )
    v[w] = _mm256_or_si256(_mm256_srli_epi64(v[w], 1),
                           _mm256_slli_epi64(v[w - 1], 63));
  v[0] = _mm256_srli_epi64(v[0], 1);
}

/*  Per-lane parity of a u64 vector, broadcast to a full-lane mask
    (all-ones where parity is 1).  Fold 64->1 by xor-halving, then
    expand the low bit.  */
static inline __m256i bm_parmask_(__m256i a) {
  a = _mm256_xor_si256(a, _mm256_srli_epi64(a, 32));
  a = _mm256_xor_si256(a, _mm256_srli_epi64(a, 16));
  a = _mm256_xor_si256(a, _mm256_srli_epi64(a, 8));
  a = _mm256_xor_si256(a, _mm256_srli_epi64(a, 4));
  a = _mm256_xor_si256(a, _mm256_srli_epi64(a, 2));
  a = _mm256_xor_si256(a, _mm256_srli_epi64(a, 1));
  a = _mm256_and_si256(a, _mm256_set1_epi64x(1));
  return _mm256_sub_epi64(_mm256_setzero_si256(), a);
}

/*  Pack one 64-byte window into 8 u64, MSB-first (byte i high to low,
    bit 7 of byte 0 is poly coefficient 0).  */
static inline void bm_pack_(const u8 * src, u64 s[WB]) {
  i32 i;
  Fi(WB, s[i] = 0);
  for (i = 0; i < FASTENT_BM_WB; i++)
    s[i >> 3] |= (u64) src[i] << (56u - 8u * (i & 7u));
}

/*  Process exactly 4 full M-bit windows; write their L into Lout.  */
static void bm_batch4_(const u8 * src, u32 * Lout) {
  u64 sp[4][WB];
  i32 i;
  u32 N, w;
  Fi(4, bm_pack_(src + (u32) i * FASTENT_BM_WB, sp[i]));

  __m256i cv[WB], bsv[WB], rv[WB], tv[WB];
  for (w = 0; w < WB; w++) {
    cv[w] = _mm256_setzero_si256();
    bsv[w] = _mm256_setzero_si256();
    rv[w] = _mm256_setzero_si256();
  }
  __m256i one63 = _mm256_set1_epi64x((i64) ((u64) 1 << 63));
  cv[0] = one63;
  bsv[0] = one63;
  bm_polx_(bsv);                 /*  bs_ at start of N=0 is x^1 * 1  */

  i64 L[4] = { 0, 0, 0, 0 };

  for (N = 0; N < FASTENT_BM_M; N++) {
    bm_polx_(rv);
    u32 wi = N >> 6, sh = 63u - (N & 63u);
    u64 b0 = (sp[0][wi] >> sh) & 1ull, b1 = (sp[1][wi] >> sh) & 1ull;
    u64 b2 = (sp[2][wi] >> sh) & 1ull, b3 = (sp[3][wi] >> sh) & 1ull;
    rv[0] = _mm256_or_si256(rv[0], _mm256_set_epi64x(
        (i64) (b3 << 63), (i64) (b2 << 63),
        (i64) (b1 << 63), (i64) (b0 << 63)));

    __m256i acc = _mm256_setzero_si256();
    for (w = 0; w < WB; w++)
      acc = _mm256_xor_si256(acc, _mm256_and_si256(cv[w], rv[w]));
    __m256i dmask = bm_parmask_(acc);

    for (w = 0; w < WB; w++) tv[w] = cv[w];
    for (w = 0; w < WB; w++)
      cv[w] = _mm256_xor_si256(cv[w],
                               _mm256_and_si256(bsv[w], dmask));

    /*  Length change where d set and 2L <= N.  2L<=N is N-2L>=0;
        with L,N small the i64 lane compare is exact.  */
    __m256i Lv = _mm256_loadu_si256((const __m256i *) L);
    __m256i twoL = _mm256_add_epi64(Lv, Lv);
    __m256i Nv = _mm256_set1_epi64x((i64) N);
    /*  2L <= N  <=>  NOT (2L > N).  */
    __m256i gt = _mm256_cmpgt_epi64(twoL, Nv);
    __m256i chg = _mm256_andnot_si256(gt, dmask);
    for (w = 0; w < WB; w++)
      bsv[w] = _mm256_blendv_epi8(bsv[w], tv[w], chg);
    /*  L <- N + 1 - L where chg.  */
    __m256i newL = _mm256_sub_epi64(
        _mm256_set1_epi64x((i64) N + 1), Lv);
    Lv = _mm256_blendv_epi8(Lv, newL, chg);
    _mm256_storeu_si256((__m256i *) L, Lv);

    bm_polx_(bsv);
  }
  Fi(4, Lout[i] = (u32) L[i]);
}

/*  Score nfull full windows; AVX2 in groups of 4.  Returns the count
    actually scored (the largest multiple of 4 <= nfull); the caller
    handles the 0..3 tail with the scalar reference.  */
sz fastent_bm_windows_avx2(const u8 * src, sz nfull, u32 * Lout) {
  sz g = nfull & ~(sz) 3;
  sz i;
  for (i = 0; i < g; i += 4)
    bm_batch4_(src + i * FASTENT_BM_WB, Lout + i);
  return g;
}
