/*  fastent: FIPS 140-2 RNG power-up self-tests (4.9.1).

    Four tests over each independent 20000-bit (2500-byte) block:
    monobit, poker, runs and long run.  Bits are taken MSB first
    within each byte; the poker test's 4-bit groups are the high
    then the low nibble of each byte.  Constants are the FIPS 140-2
    values (the long-run threshold is 34; the rng-tools rngtest
    utility historically uses 26).  Blocks are independent, so the
    work parallelises with no boundary stitch.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "fips1402.h"

#include "port-thread.h"

#include <stdlib.h>
#include <string.h>

#define FIPS_BLOCK_BYTES 2500u
#define FIPS_BLOCK_BITS  20000u
#define FIPS_LONGRUN     34u
#define FIPS_NW          ((FIPS_BLOCK_BITS + 63u) / 64u)   /*  313 words  */
/*  Valid bits in the final word: 20000 - 64*(FIPS_NW-1) = 32.  */
#define FIPS_LAST_MASK   0x00000000FFFFFFFFull


/*  One run polarity.  ge[k] = number of maximal runs of length >= k;
    *lr34 = 1 iff some run reaches FIPS_LONGRUN.  A_k[p] holds "bits
    p..p+k-1 are all set" via the recurrence A_k = V & succ(A_{k-1});
    out-of-block positions are zero, so the block end terminates a
    run.  S marks run starts, so popcount(S & A_k) counts runs >= k.
    Fully branchless and word-parallel.  */
static void fips_runs_side_(const u64 * V, u32 ge[7], int * lr34) {
  u64 A[FIPS_NW], S[FIPS_NW], nx[FIPS_NW];
  int w;
  u32 k;
  for (w = 0; w < (int) FIPS_NW; w++) {
    const u64 pred = (V[w] << 1) | (w > 0 ? V[w - 1] >> 63 : 0ull);
    S[w] = V[w] & ~pred;
    A[w] = V[w];
  }
  Fi0(7, 1, ge[i] = 0)
  *lr34 = 0;
  for (k = 1; ; k++) {
    u64 orA = 0;
    if (k >= 2) {
      for (w = 0; w < (int) FIPS_NW; w++)
        nx[w] = (A[w] >> 1)
              | (w + 1 < (int) FIPS_NW ? A[w + 1] << 63 : 0ull);
      for (w = 0; w < (int) FIPS_NW; w++) A[w] = V[w] & nx[w];
    }
    if (k <= 6) {
      u64 g = 0;
      for (w = 0; w < (int) FIPS_NW; w++) {
        orA |= A[w];  g += FASTENT_POPCOUNT64(S[w] & A[w]);
      }
      ge[k] = (u32) g;
    } else {
      for (w = 0; w < (int) FIPS_NW; w++) orA |= A[w];
    }
    if (k == FIPS_LONGRUN) { *lr34 = (orA != 0);  break; }
    if (orA == 0) break;        /*  no run >= k; longer ge stay 0  */
  }
}

/*  Test one 2500-byte block and fold the verdict into r.  */
static void fips_block_(const u8 * b, fastent_fips_report * r) {
  u64 W[FIPS_NW], C[FIPS_NW];
  int w;

  /*  Pack the MSB-first bit stream into words; trailing bits stay
      zero.  C is the complement, masked to the valid 20000 bits so a
      0-run cannot run off the block end.  */
  memset(W, 0, sizeof W);
  Fi((int) FIPS_BLOCK_BYTES,
     W[i >> 3] |= (u64) fastent_bitrev8_(b[i]) << ((u32)(i & 7) * 8u))
  for (w = 0; w < (int) FIPS_NW; w++) C[w] = ~W[w];
  C[FIPS_NW - 1] &= FIPS_LAST_MASK;

  /*  Monobit: total set bits (popcount is bit-position invariant).  */
  u64 ones = 0;
  for (w = 0; w < (int) FIPS_NW; w++) ones += FASTENT_POPCOUNT64(W[w]);
  const int mono_ok = (ones > 9725ull && ones < 10275ull);

  /*  Poker: 5000 4-bit groups, high then low nibble.  */
  u32 f[16];
  memset(f, 0, sizeof f);
  Fi((int) FIPS_BLOCK_BYTES, f[b[i] >> 4]++;  f[b[i] & 0x0Fu]++)
  u64 ssq = 0;
  Fi(16, ssq += (u64) f[i] * (u64) f[i])
  const f64 X = (16.0 / 5000.0) * (f64) ssq - 5000.0;
  const int poker_ok = (X > 2.16 && X < 46.17);

  /*  Runs / long run.  Exact-length bucket L = ge[L]-ge[L+1] for
      L=1..5; bucket 6 = ge[6] (length >= 6).  */
  u32 ge1[7], ge0[7];
  int lr1, lr0;
  fips_runs_side_(W, ge1, &lr1);
  fips_runs_side_(C, ge0, &lr0);

  static const u32 lo_[7] = { 0, 2315, 1114, 527, 240, 103, 103 };
  static const u32 hi_[7] = { 0, 2685, 1386, 723, 384, 209, 209 };
  int runs_ok = 1;
  Fi0(7, 1,
      const u32 c1 = (i < 6) ? ge1[i] - ge1[i + 1] : ge1[6];
      const u32 c0 = (i < 6) ? ge0[i] - ge0[i + 1] : ge0[6];
      if (c1 < lo_[i] || c1 > hi_[i]) runs_ok = 0;
      if (c0 < lo_[i] || c0 > hi_[i]) runs_ok = 0)
  const int long_ok = !(lr1 || lr0);

  r->blocks++;
  if (!mono_ok)  r->monobit_fail++;
  if (!poker_ok) r->poker_fail++;
  if (!runs_ok)  r->runs_fail++;
  if (!long_ok)  r->longrun_fail++;
  if (mono_ok && poker_ok && runs_ok && long_ok) r->blocks_pass++;
}

#ifdef FASTENT_HAVE_THREADS
typedef struct {
  const u8 *            buf;
  u64                   nblocks;
  int                   T;
  fastent_fips_report * shards;
} fips_ctx;

static void fips_worker_(sz k, void * vctx) {
  fips_ctx * c = (fips_ctx *) vctx;
  u64 lo = (u64) k * c->nblocks / (u64) c->T;
  u64 hi = (u64)(k + 1) * c->nblocks / (u64) c->T;
  fastent_fips_report * r = &c->shards[k];
  u64 i;
  for (i = lo; i < hi; i++)
    fips_block_(c->buf + i * FIPS_BLOCK_BYTES, r);
}
#endif

void fastent_fips140_run(const u8 * buf, sz len, int threads,
                         fastent_fips_report * out) {
  memset(out, 0, sizeof *out);
  u64 nblocks = (u64) len / FIPS_BLOCK_BYTES;
  out->leftover = (u64) len - nblocks * FIPS_BLOCK_BYTES;
  if (nblocks == 0) return;

#ifdef FASTENT_HAVE_THREADS
  if (threads > 1) {
    int T = threads;
    if ((u64) T > nblocks) T = (int) nblocks;
    fastent_set_num_threads(T);
    fips_ctx c;
    c.buf = buf;  c.nblocks = nblocks;  c.T = T;
    c.shards =
      (fastent_fips_report *) calloc((sz) T, sizeof(*c.shards));
    if (c.shards) {
      fastent_parallel_for((sz) T, fips_worker_, &c);
      Fk(T,
         out->blocks       += c.shards[k].blocks;
         out->monobit_fail += c.shards[k].monobit_fail;
         out->poker_fail   += c.shards[k].poker_fail;
         out->runs_fail    += c.shards[k].runs_fail;
         out->longrun_fail += c.shards[k].longrun_fail;
         out->blocks_pass  += c.shards[k].blocks_pass)
      free(c.shards);
      return;
    }
    /*  Allocation failed: fall through to the serial path.  */
  }
#else
  (void) threads;
#endif
  { u64 i;  for (i = 0; i < nblocks; i++)
      fips_block_(buf + i * FIPS_BLOCK_BYTES, out); }
}

int fastent_fips140_print(const fastent_fips_report * r, FILE * fp) {
  if (r->blocks == 0) {
    fprintf(fp, "FIPS 140-2 RNG self-tests: insufficient data "
                "(need >= %u bytes, %llu leftover)\n",
            FIPS_BLOCK_BYTES, (unsigned long long) r->leftover);
    return 0;
  }
  const int pass = (r->blocks_pass == r->blocks);
  const unsigned long long n = (unsigned long long) r->blocks;
  fprintf(fp, "FIPS 140-2 RNG self-tests (20000-bit blocks)\n");
  fprintf(fp, "  blocks tested  : %llu\n", n);
  fprintf(fp, "  leftover bytes : %llu\n",
          (unsigned long long) r->leftover);
  fprintf(fp, "  monobit        : %s  (%llu failed)\n",
          r->monobit_fail ? "FAIL" : "PASS",
          (unsigned long long) r->monobit_fail);
  fprintf(fp, "  poker          : %s  (%llu failed)\n",
          r->poker_fail ? "FAIL" : "PASS",
          (unsigned long long) r->poker_fail);
  fprintf(fp, "  runs           : %s  (%llu failed)\n",
          r->runs_fail ? "FAIL" : "PASS",
          (unsigned long long) r->runs_fail);
  fprintf(fp, "  long run       : %s  (%llu failed)\n",
          r->longrun_fail ? "FAIL" : "PASS",
          (unsigned long long) r->longrun_fail);
  fprintf(fp, "  overall        : %s  (%llu/%llu blocks passed)\n",
          pass ? "PASS" : "FAIL",
          (unsigned long long) r->blocks_pass, n);
  return pass;
}
