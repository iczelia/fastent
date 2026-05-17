/*  fastent: AArch64 SVE2 FIPS 140-2 variant.  Self-contained because
    fips1402-impl.h assumes a compile-time fixed vector stride; SVE2
    has a runtime vector length.  Monobit uses svcnt + svaddv; poker
    and the one-pass run-length spectrum reuse the portable scalar
    formulation (identical integer summaries to every other variant).

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "fips1402.h"

#if defined(__ARM_BIG_ENDIAN) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#error "fastent SVE2 FIPS variant is not supported on big-endian ARM"
#endif

#include <arm_sve.h>
#include <string.h>

#define FIPS_BLOCK_BYTES 2500u
#define FIPS_BLOCK_BITS  20000u
#define FIPS_LONGRUN     34u
#define FIPS_NW          ((FIPS_BLOCK_BITS + 63u) / 64u)   /*  313 words  */

/*  MSB-first big-endian word load (little-endian host: byte-swap).  */
static inline u64 fips_load_be_sve2(const u8 * p) {
  u64 v;
  memcpy(&v, p, 8);
  return FASTENT_BSWAP64(v);
}

static inline void fips_pack_sve2(const u8 * b, u64 W[FIPS_NW]) {
  i32 w;
  for (w = 0; w < (i32) FIPS_NW - 1; w++)
    W[w] = fips_load_be_sve2(b + (sz) w * 8);
  {
    u64 t = 0;
    Fi(4, t |= (u64) b[2496 + i] << (56 - i * 8))
    W[FIPS_NW - 1] = t;
  }
}

/*  One-pass run-length spectrum (same definition as the scalar TU).  */
static void fips_runs_sve2(const u64 W[FIPS_NW],
                           u32 cnt1[7], u32 cnt0[7], int * lr) {
  i32 w;
  u32 run_len = 0, cur_bit = 0;
  int have = 0;
  Fi(7, cnt1[i] = 0; cnt0[i] = 0)
  *lr = 0;
  for (w = 0; w < (i32) FIPS_NW; w++) {
    u64 v = W[w];
    u32 nbits = (w == (i32) FIPS_NW - 1) ? 32u : 64u;
    u32 pos;
    for (pos = 0; pos < nbits; pos++) {
      u32 bit = (u32) ((v >> (63 - pos)) & 1u);
      if (!have) { cur_bit = bit; run_len = 1; have = 1; }
      else if (bit == cur_bit) { run_len++; }
      else {
        u32 idx = run_len < 6u ? run_len : 6u;
        if (cur_bit) cnt1[idx]++; else cnt0[idx]++;
        if (run_len >= FIPS_LONGRUN) *lr = 1;
        cur_bit = bit; run_len = 1;
      }
    }
  }
  if (have) {
    u32 idx = run_len < 6u ? run_len : 6u;
    if (cur_bit) cnt1[idx]++; else cnt0[idx]++;
    if (run_len >= FIPS_LONGRUN) *lr = 1;
  }
}

/*  Monobit via SVE2 byte popcount: svcnt_u8 then svaddv over
    svwhilelt-predicated 2500-byte loops.  */
static u64 fips_monobit_sve2(const u8 * b) {
  u64 ones = 0;
  const u64 vl = svcntb();
  u64 i = 0;
  for (; i + vl <= FIPS_BLOCK_BYTES; i += vl) {
    svbool_t pg = svptrue_b8();
    svuint8_t v = svld1_u8(pg, b + i);
    ones += (u64) svaddv_u8(pg, svcnt_u8_x(pg, v));
  }
  for (; i < FIPS_BLOCK_BYTES; i++) ones += (u64) FASTENT_POPCOUNT32(b[i]);
  return ones;
}

static void fips_block_sve2(const u8 * b, fastent_fips_report * r) {
  u64 W[FIPS_NW];

  u64 ones = fips_monobit_sve2(b);
  const int mono_ok = (ones > 9725ull && ones < 10275ull);

  u32 f[16];
  memset(f, 0, sizeof f);
  Fi((int) FIPS_BLOCK_BYTES, f[b[i] >> 4]++;  f[b[i] & 0x0Fu]++)
  u64 ssq = 0;
  Fi(16, ssq += (u64) f[i] * (u64) f[i])
  const f64 X = (16.0 / 5000.0) * (f64) ssq - 5000.0;
  const int poker_ok = (X > 2.16 && X < 46.17);

  fips_pack_sve2(b, W);
  u32 cnt1[7], cnt0[7];
  int lr;
  fips_runs_sve2(W, cnt1, cnt0, &lr);

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

void fastent_fips_run_blocks_sve2(const u8 * buf, u64 nblocks,
                                  fastent_fips_report * r) {
  u64 i;
  for (i = 0; i < nblocks; i++)
    fips_block_sve2(buf + i * FIPS_BLOCK_BYTES, r);
}
