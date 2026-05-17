/*  fastent: templated FIPS 140-2 block body.  Included from
    fips1402-scalar.c, fips1402-ssse3.c, fips1402-sse41.c,
    fips1402-avx2.c, fips1402-avx512.c, fips1402-avx512-bitalg.c,
    fips1402-neon.c, fips1402-wasm128simd.c.

    Each TU defines one of:
      FASTENT_VARIANT_SCALAR
      FASTENT_VARIANT_SSSE3
      FASTENT_VARIANT_SSE41
      FASTENT_VARIANT_AVX2
      FASTENT_VARIANT_AVX512  (+ optionally FASTENT_AVX512_HAVE_BITALG)
      FASTENT_VARIANT_NEON
      FASTENT_VARIANT_WASM128

    and (optionally) FASTENT_HAVE_SIMD if the SIMD body should run.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).

    Four tests over each independent 20000-bit (2500-byte) block:
    monobit, poker, runs and long run.  Bits are MSB first within
    each byte; poker's 4-bit groups are the high then the low nibble
    of each byte.  Constants are the FIPS 140-2 values (long-run
    threshold 34).  Blocks are independent, so the result is a pure
    integer reduction with no boundary stitch: bit-identical across
    thread count, IO mode, and ISA.

    Per-block summaries (all integer, sum-mergeable):
      ones      total set bits (monobit)
      f[16]     nibble histogram (poker)
      cnt1[k]   number of 1-runs of length min(k,6), k = 1..6
      cnt0[k]   number of 0-runs of length min(k,6), k = 1..6
      lr        1 iff some run reaches FIPS_LONGRUN (34)

    The runs path packs the byte stream into MSB-first big-endian
    words (bit i of the stream = bit 7-(i&7) of byte i>>3).  On a
    little-endian host an 8-byte load byte-swapped with
    __builtin_bswap64 lays byte 8w into the most significant 8 bits
    of word w, so "next bit in stream order" is a plain left shift
    with carry-in from the next word's MSB; no per-byte bit reversal
    is needed.  The run-length spectrum is then read in ONE pass from
    the transition bitmap T = V ^ (V shifted one bit in stream
    order), with the block end forced to a boundary, producing both
    polarities at once.  */

#include "common.h"
#include "fips1402.h"

#if defined(FASTENT_VARIANT_NEON)
  #include "analyze-vec-neon.h"
#elif defined(FASTENT_VARIANT_WASM128)
  #include "analyze-vec-wasm.h"
#elif defined(FASTENT_VARIANT_AVX512) || defined(FASTENT_VARIANT_AVX2) \
   || defined(FASTENT_VARIANT_SSE41)  || defined(FASTENT_VARIANT_SSSE3)
  #include "analyze-vec-x86.h"
  /*  The shared x86 vec header only knows the base and BITALG AVX-512
      tiers, so it stamps _avx512 here.  The FIPS-only VPOPCNTDQ tier
      needs a distinct symbol suffix so its TU does not collide with
      the base AVX-512 FIPS body in the link.  */
  #if defined(FASTENT_AVX512_HAVE_VPOPCNTDQ)
    #undef  FASTENT_VAR_SUFFIX
    #define FASTENT_VAR_SUFFIX _avx512_vpopcntdq
  #endif
#else
  #define FASTENT_VAR_SUFFIX _scalar
#endif

#define FASTENT_CAT2(a, b) a##b
#define FASTENT_CAT(a, b)  FASTENT_CAT2(a, b)
#define FASTENT_FN(name)   FASTENT_CAT(name, FASTENT_VAR_SUFFIX)

#include <string.h>

#define FIPS_BLOCK_BYTES 2500u
#define FIPS_BLOCK_BITS  20000u
#define FIPS_LONGRUN     34u
#define FIPS_NW          ((FIPS_BLOCK_BITS + 63u) / 64u)   /*  313 words  */
/*  Valid bits in the final word: 20000 - 64*(FIPS_NW-1) = 32.  In
    the MSB-first big-endian word layout those 32 stream bits occupy
    the top 32 hardware bits, so the valid-bit mask is the high half.  */
#define FIPS_LAST_MASK_BE 0xFFFFFFFF00000000ull

/*  MSB-first big-endian word load.  On little-endian a byte-swap of
    the raw 8 bytes places byte (8w) into bits 63..56 of word w, so
    bit i of the stream = bit 63-(i&63) of word i>>6.  On big-endian
    the natural load already has that layout.  */
static FASTENT_ALWAYS_INLINE u64 FASTENT_FN(fips_load_be)(const u8 * p) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  u64 v;
  memcpy(&v, p, 8);
  return v;
#else
  u64 v;
  memcpy(&v, p, 8);
  return FASTENT_BSWAP64(v);
#endif
}

/*  Pack one 2500-byte block into FIPS_NW MSB-first words.  The last
    word holds 32 valid bits; trailing bits are zero.  2500 = 312*8 +
    4, so 312 full 8-byte words plus a 4-byte tail.  */
static FASTENT_ALWAYS_INLINE void
FASTENT_FN(fips_pack)(const u8 * b, u64 W[FIPS_NW]) {
  i32 w;
  for (w = 0; w < (i32) FIPS_NW - 1; w++)
    W[w] = FASTENT_FN(fips_load_be)(b + (sz) w * 8);
  /*  Tail: 4 bytes -> high 32 bits of the final word, low 32 zero.  */
  {
    u64 t = 0;
    Fi(4, t |= (u64) b[2496 + i] << (56 - i * 8))
    W[FIPS_NW - 1] = t;
  }
}

/*  Shift the packed bit stream n positions later (1<=n<=32): the
    stream successor at distance n.  In an MSB-first big-endian word
    that is a left shift with carry from the next word's top n bits;
    past the last word is zero.  */
static FASTENT_ALWAYS_INLINE void
FASTENT_FN(fips_succn)(const u64 src[FIPS_NW], u64 dst[FIPS_NW], u32 n) {
  i32 w;
  for (w = 0; w < (i32) FIPS_NW; w++) {
    u64 carry = (w + 1 < (i32) FIPS_NW) ? (src[w + 1] >> (64u - n))
                                        : 0ull;
    dst[w] = (src[w] << n) | carry;
  }
}

/*  Bounded-window run-length spectrum per polarity P: S=P&~pred(P),
    A_k=P&succ(A_{k-1}), ge[k]=popcount(S&A_k) runs>=k; bucket=
    ge[k]-ge[k+1], long-run=34-equal window via log-doubling AND
    (1,2,4,8,16,+2).  Out-of-block zero = reference recurrence.  */

static void FASTENT_FN(fips_runs_side)(const u64 P[FIPS_NW],
                                       u32 ge[7], int * lr34) {
  u64 S[FIPS_NW], A[FIPS_NW], nx[FIPS_NW];
  i32 w;
  u32 k;
  for (w = 0; w < (i32) FIPS_NW; w++) {
    const u64 pred = (P[w] >> 1)
                   | (w > 0 ? (P[w - 1] << 63) : 0ull);
    S[w] = P[w] & ~pred;
    A[w] = P[w];
  }
  Fi(7, ge[i] = 0)
  {
    u64 g = 0;
    for (w = 0; w < (i32) FIPS_NW; w++)
      g += FASTENT_POPCOUNT64(S[w] & A[w]);
    ge[1] = (u32) g;
  }
  for (k = 2; k <= 6; k++) {
    u64 g = 0;
    FASTENT_FN(fips_succn)(A, nx, 1u);
    for (w = 0; w < (i32) FIPS_NW; w++) {
      A[w] = P[w] & nx[w];
      g   += FASTENT_POPCOUNT64(S[w] & A[w]);
    }
    ge[k] = (u32) g;
  }

  /*  34-equal-window existence by log-doubling.  D2 = 2-window,
      D4 = 4, D8 = 8, D16 = 16, D32 = 32; 34 = 32-window AND the
      2-window shifted 32 positions later.  */
  {
    u64 D[FIPS_NW], E[FIPS_NW], D2[FIPS_NW];
    u32 win;
    int hit = 0;
    for (w = 0; w < (i32) FIPS_NW; w++) D[w] = P[w];
    FASTENT_FN(fips_succn)(D, E, 1u);
    for (w = 0; w < (i32) FIPS_NW; w++) D[w] &= E[w];   /*  2  */
    for (w = 0; w < (i32) FIPS_NW; w++) D2[w] = D[w];
    for (win = 2u; win < 32u; win <<= 1) {
      FASTENT_FN(fips_succn)(D, E, win);
      for (w = 0; w < (i32) FIPS_NW; w++) D[w] &= E[w]; /*  4,8,16,32  */
    }
    /*  D is now the 32-equal-window mask.  AND with the 2-window
        shifted 32 later to require 34 consecutive equal bits.  */
    FASTENT_FN(fips_succn)(D2, E, 32u);
    for (w = 0; w < (i32) FIPS_NW; w++)
      if (D[w] & E[w]) { hit = 1; break; }
    *lr34 = hit;
  }
}


/*  Run-length spectrum for both polarities.  cnt1/cnt0 hold exact-
    length buckets 1..5 and bucket 6 = length >= 6; *lr = 1 iff some
    run (either polarity) reaches 34.  */
static void FASTENT_FN(fips_runs)(const u64 W[FIPS_NW],
                                  u32 cnt1[7], u32 cnt0[7], int * lr) {
  u64 C[FIPS_NW];
  u32 ge1[7], ge0[7];
  int lr1, lr0;
  i32 w;
  for (w = 0; w < (i32) FIPS_NW; w++) C[w] = ~W[w];
  C[FIPS_NW - 1] &= FIPS_LAST_MASK_BE;     /*  drop dead tail bits  */
  FASTENT_FN(fips_runs_side)(W, ge1, &lr1);
  FASTENT_FN(fips_runs_side)(C, ge0, &lr0);
  Fi0(7, 1,
      cnt1[i] = (i < 6) ? ge1[i] - ge1[i + 1] : ge1[6];
      cnt0[i] = (i < 6) ? ge0[i] - ge0[i + 1] : ge0[6])
  cnt1[0] = cnt0[0] = 0;
  *lr = (lr1 || lr0);
}

#ifdef FASTENT_HAVE_SIMD
/*  Horizontal sum of the epi64 lanes of a SIMD vector.  Each SAD
    reduction already produced one u64 per 64-bit lane, so a scalar
    fold over the stored lanes finishes the reduction portably for
    every ISA (called once per block, off the hot path).  */
static FASTENT_ALWAYS_INLINE u64
FASTENT_FN(fips_hsum_epi64)(FASTENT_SIMD_VEC v) {
  u64 lanes[FASTENT_SIMD_VLEN / 8];
  u64 s = 0;
  i32 i;
  V_STORE(lanes, v);
  for (i = 0; i < (i32)(FASTENT_SIMD_VLEN / 8); i++) s += lanes[i];
  return s;
}

/*  Byte-wise popcount of a vector, reduced (PSADBW / pairwise ladder)
    to per-64-bit-lane partial sums.  BITALG uses VPOPCNTB, NEON
    vcntq_u8, WASM wasm_i8x16_popcnt; the rest use the Mula PSHUFB
    nibble-LUT (arXiv:1611.07612).  */
static FASTENT_ALWAYS_INLINE FASTENT_SIMD_VEC
FASTENT_FN(fips_popcnt_bytes)(FASTENT_SIMD_VEC v) {
#if defined(FASTENT_VARIANT_AVX512) && defined(FASTENT_AVX512_HAVE_BITALG)
  return V_SAD_EPU8(_mm512_popcnt_epi8(v), V_SETZERO());
#elif defined(FASTENT_VARIANT_NEON)
  return V_SAD_EPU8(vcntq_u8(v), V_SETZERO());
#elif defined(FASTENT_VARIANT_WASM128)
  return V_SAD_EPU8(wasm_i8x16_popcnt(v), V_SETZERO());
#else
  #if defined(FASTENT_VARIANT_AVX2)
    const FASTENT_SIMD_VEC lut = _mm256_setr_epi8(
      0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,
      0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4);
  #elif defined(FASTENT_VARIANT_AVX512)
    const FASTENT_SIMD_VEC lut = _mm512_broadcast_i32x4(
      _mm_setr_epi8(0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4));
  #else
    const FASTENT_SIMD_VEC lut = _mm_setr_epi8(
      0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4);
  #endif
  const FASTENT_SIMD_VEC nmask = V_SET1_EPI8(0x0F);
  FASTENT_SIMD_VEC lo = V_AND(v, nmask);
  FASTENT_SIMD_VEC hi = V_AND(V_SRLI_EPI16(v, 4), nmask);
  FASTENT_SIMD_VEC pc = V_ADD_EPI8(V_SHUFFLE_EPI8(lut, lo),
                                   V_SHUFFLE_EPI8(lut, hi));
  return V_SAD_EPU8(pc, V_SETZERO());
#endif
}

/*  Monobit: total set bits over the 2500 raw bytes, vector bulk +
    scalar tail.  AVX-512 VPOPCNTDQ uses _mm512_popcnt_epi64 instead
    of the PSHUFB-LUT + PSADBW ladder; integer result identical.  */
static FASTENT_ALWAYS_INLINE u64
FASTENT_FN(fips_monobit)(const u8 * b) {
#if defined(FASTENT_VARIANT_AVX512) \
 && defined(FASTENT_AVX512_HAVE_VPOPCNTDQ)
  __m512i acc = _mm512_setzero_si512();
  i32 i = 0;
  for (; i + 64 <= (i32) FIPS_BLOCK_BYTES; i += 64)
    acc = _mm512_add_epi64(
            acc,
            _mm512_popcnt_epi64(
              _mm512_loadu_si512((const void *) (b + i))));
  u64 ones = (u64) _mm512_reduce_add_epi64(acc);
  for (; i < (i32) FIPS_BLOCK_BYTES; i++)
    ones += FASTENT_POPCOUNT32(b[i]);
  return ones;
#else
  FASTENT_SIMD_VEC acc = V_SETZERO();
  i32 i = 0;
  for (; i + FASTENT_SIMD_VLEN <= (i32) FIPS_BLOCK_BYTES;
         i += FASTENT_SIMD_VLEN)
    acc = V_ADD_EPI64(acc,
                      FASTENT_FN(fips_popcnt_bytes)(V_LOAD(b + i)));
  u64 ones = FASTENT_FN(fips_hsum_epi64)(acc);
  for (; i < (i32) FIPS_BLOCK_BYTES; i++)
    ones += FASTENT_POPCOUNT32(b[i]);
  return ones;
#endif
}

/*  Poker nibble histogram via per-value mask compare: count(t) =
    popcount(lo==t)+popcount(hi==t) over all bytes; SAD turns each
    0/-1 compare mask into a per-lane count, negated back.  16
    values, both nibble planes, exact integer.  */
static FASTENT_ALWAYS_INLINE void
FASTENT_FN(fips_poker)(const u8 * b, u32 f[16]) {
  const FASTENT_SIMD_VEC nmask = V_SET1_EPI8(0x0F);
  FASTENT_SIMD_VEC acc[16];
  i32 t, i = 0;
  Fi(16, acc[i] = V_SETZERO())
  for (; i + FASTENT_SIMD_VLEN <= (i32) FIPS_BLOCK_BYTES;
         i += FASTENT_SIMD_VLEN) {
    FASTENT_SIMD_VEC v  = V_LOAD(b + i);
    FASTENT_SIMD_VEC lo = V_AND(v, nmask);
    FASTENT_SIMD_VEC hi = V_AND(V_SRLI_EPI16(v, 4), nmask);
    for (t = 0; t < 16; t++) {
      FASTENT_SIMD_VEC tv = V_SET1_EPI8((char) t);
      /*  cmpeq -> 0xFF where equal; SAD vs zero sums 0xFF=255 per
          match into each 64-bit lane, divided out after the loop.  */
      acc[t] = V_ADD_EPI64(acc[t],
                 V_ADD_EPI64(V_SAD_EPU8(V_CMPEQ_EPI8(lo, tv),
                                        V_SETZERO()),
                             V_SAD_EPU8(V_CMPEQ_EPI8(hi, tv),
                                        V_SETZERO())));
    }
  }
  for (t = 0; t < 16; t++)
    f[t] = (u32) (FASTENT_FN(fips_hsum_epi64)(acc[t]) / 255u);
  /*  Scalar tail bytes.  */
  for (; i < (i32) FIPS_BLOCK_BYTES; i++) {
    f[b[i] >> 4]++;  f[b[i] & 0x0Fu]++;
  }
}
#endif  /*  FASTENT_HAVE_SIMD  */

/*  Test one 2500-byte block and fold its verdict into *r.  Monobit
    and poker read the raw bytes (order-invariant); runs read the
    packed words.  */
static FASTENT_ALWAYS_INLINE void
FASTENT_FN(fips_block)(const u8 * b, fastent_fips_report * r) {
  u64 W[FIPS_NW];

  /*  Monobit: total set bits over the raw bytes.  */
#ifdef FASTENT_HAVE_SIMD
  u64 ones = FASTENT_FN(fips_monobit)(b);
#else
  u64 ones = 0;
  {
    i32 w;
    for (w = 0; w + 8 <= (i32) FIPS_BLOCK_BYTES; w += 8) {
      u64 x;
      memcpy(&x, b + w, 8);
      ones += FASTENT_POPCOUNT64(x);
    }
    /*  2500 = 312*8 + 4; popcount the 4-byte tail.  */
    {
      u32 t;
      memcpy(&t, b + 2496, 4);
      ones += FASTENT_POPCOUNT32(t);
    }
  }
#endif
  const int mono_ok = (ones > 9725ull && ones < 10275ull);

  /*  Poker: 5000 4-bit groups, high then low nibble of each byte.  */
  u32 f[16];
#ifdef FASTENT_HAVE_SIMD
  FASTENT_FN(fips_poker)(b, f);
#else
  memset(f, 0, sizeof f);
  Fi((int) FIPS_BLOCK_BYTES, f[b[i] >> 4]++;  f[b[i] & 0x0Fu]++)
#endif
  u64 ssq = 0;
  Fi(16, ssq += (u64) f[i] * (u64) f[i])
  const f64 X = (16.0 / 5000.0) * (f64) ssq - 5000.0;
  const int poker_ok = (X > 2.16 && X < 46.17);

  /*  Runs / long run: one pass over the packed words producing both
      polarities.  Exact-length bucket k counts runs of length k for
      k = 1..5; bucket 6 counts runs of length >= 6.  */
  FASTENT_FN(fips_pack)(b, W);
  u32 cnt1[7], cnt0[7];
  int lr;
  FASTENT_FN(fips_runs)(W, cnt1, cnt0, &lr);

  static const u32 lo_[7] = { 0, 2315, 1114, 527, 240, 103, 103 };
  static const u32 hi_[7] = { 0, 2685, 1386, 723, 384, 209, 209 };
  int runs_ok = 1;
  Fi0(7, 1,
      if (cnt1[i] < lo_[i] || cnt1[i] > hi_[i]) runs_ok = 0;
      if (cnt0[i] < lo_[i] || cnt0[i] > hi_[i]) runs_ok = 0)
  const int long_ok = !lr;

  r->blocks++;
  if (!mono_ok)  r->monobit_fail++;
  if (!poker_ok) r->poker_fail++;
  if (!runs_ok)  r->runs_fail++;
  if (!long_ok)  r->longrun_fail++;
  if (mono_ok && poker_ok && runs_ok && long_ok) r->blocks_pass++;
}

/*  Batched runner: process nblocks consecutive blocks.  */
void FASTENT_FN(fastent_fips_run_blocks)(const u8 * buf, u64 nblocks,
                                         fastent_fips_report * r) {
  u64 i;
  for (i = 0; i < nblocks; i++)
    FASTENT_FN(fips_block)(buf + i * FIPS_BLOCK_BYTES, r);
}
