/*  fastent: FIPS 140-2 RNG power-up self-tests (4.9.1) dispatcher.

    Four tests over each independent 20000-bit (2500-byte) block:
    monobit, poker, runs and long run.  Bits are taken MSB first
    within each byte; the poker test's 4-bit groups are the high
    then the low nibble of each byte.  Constants are the FIPS 140-2
    values (the long-run threshold is 34; the rng-tools rngtest
    utility historically uses 26).  Blocks are independent, so the
    work parallelises with no boundary stitch and the result is a
    pure integer reduction: byte-identical across thread count, IO
    mode, and ISA.

    The per-block kernel lives in fips1402-impl.h, instantiated once
    per ISA TU (fips1402-scalar.c .. fips1402-wasm128simd.c, plus the
    self-contained fips1402-sve2.c).  This file picks the best
    available variant once and drives the (block-granular) threaded
    split, mirroring analyze.c's variant dispatcher.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "fips1402.h"

#include "port-cpu.h"
#include "port-thread.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define FIPS_BLOCK_BYTES 2500u

/*  Variant table.  Order matters: the dispatcher picks the LAST
    entry whose `available` returns true, so place narrower
    (preferred) variants after their wider supersets, exactly like
    analyze.c.  AVX-512 has two tiers: base (F+BW, PSHUFB-LUT
    popcount) and bitalg (+VPOPCNTB).  */

#define CPU_HAS(name)      (fastent_cpu_get()->name)

typedef struct {
  fastent_variant      variant;
  const char *         name;
  int                (*available)(void);
  fastent_fips_run_fn  run;
} fips_variant_entry;

static int favail_scalar_(void)  { return 1; }
#ifdef HAVE_SSSE3
static int favail_ssse3_(void)   { return CPU_HAS(ssse3); }
#endif
#ifdef HAVE_SSE41
static int favail_sse41_(void)   { return CPU_HAS(sse42); }
#endif
#ifdef HAVE_AVX2
static int favail_avx2_(void)    { return CPU_HAS(avx2); }
#endif
#ifdef HAVE_AVX512
static int favail_avx512_(void) {
  return CPU_HAS(avx512f) && CPU_HAS(avx512bw);
}
#endif
#ifdef HAVE_AVX512_BITALG
static int favail_avx512b_(void) {
  return CPU_HAS(avx512f) && CPU_HAS(avx512bw) && CPU_HAS(avx512bitalg);
}
#endif
#ifdef HAVE_NEON
static int favail_neon_(void)    { return CPU_HAS(neon); }
#endif
#ifdef HAVE_SVE2
static int favail_sve2_(void)    { return CPU_HAS(sve2); }
#endif
#ifdef HAVE_WASM128
static int favail_wasm128_(void) { return CPU_HAS(wasm128); }
#endif

static const fips_variant_entry fips_variants_[] = {
  { FASTENT_VAR_SCALAR, "scalar", favail_scalar_,
    fastent_fips_run_blocks_scalar },
#ifdef HAVE_SSSE3
  { FASTENT_VAR_SSSE3_, "ssse3", favail_ssse3_,
    fastent_fips_run_blocks_ssse3 },
#endif
#ifdef HAVE_SSE41
  { FASTENT_VAR_SSE41_, "sse4.1", favail_sse41_,
    fastent_fips_run_blocks_sse41 },
#endif
#ifdef HAVE_AVX2
  { FASTENT_VAR_AVX2_, "avx2", favail_avx2_,
    fastent_fips_run_blocks_avx2 },
#endif
#ifdef HAVE_AVX512
  { FASTENT_VAR_AVX512_, "avx512", favail_avx512_,
    fastent_fips_run_blocks_avx512 },
#endif
#ifdef HAVE_AVX512_BITALG
  { FASTENT_VAR_AVX512_BITALG, "avx512+bitalg", favail_avx512b_,
    fastent_fips_run_blocks_avx512_bitalg },
#endif
#ifdef HAVE_NEON
  { FASTENT_VAR_NEON_, "neon", favail_neon_,
    fastent_fips_run_blocks_neon },
#endif
#ifdef HAVE_SVE2
  { FASTENT_VAR_SVE2_, "sve2", favail_sve2_,
    fastent_fips_run_blocks_sve2 },
#endif
#ifdef HAVE_WASM128
  { FASTENT_VAR_WASM128_, "wasm-simd128", favail_wasm128_,
    fastent_fips_run_blocks_wasm128 },
#endif
};

#define FIPS_VARIANTS_N \
  (sizeof fips_variants_ / sizeof fips_variants_[0])

static const fips_variant_entry * fips_pick_(void) {
  const fips_variant_entry * best = &fips_variants_[0];
  Fi0((int) FIPS_VARIANTS_N, 1,
      if (fips_variants_[i].available()) best = &fips_variants_[i])
  return best;
}

fastent_fips_run_fn fastent_pick_fips_variant(fastent_variant * which) {
  const fips_variant_entry * e = fips_pick_();
  if (which) *which = e->variant;
  return e->run;
}

#ifdef FASTENT_HAVE_THREADS
typedef struct {
  const u8 *            buf;
  u64                   nblocks;
  i32                   T;
  fastent_fips_report * shards;
  fastent_fips_run_fn   run;
} fips_ctx;

static void fips_worker_(sz k, void * vctx) {
  fips_ctx * c = (fips_ctx *) vctx;
  u64 lo = (u64) k * c->nblocks / (u64) c->T;
  u64 hi = (u64)(k + 1) * c->nblocks / (u64) c->T;
  c->run(c->buf + lo * FIPS_BLOCK_BYTES, hi - lo, &c->shards[k]);
}
#endif

void fastent_fips140_run(const u8 * buf, sz len, int threads,
                         fastent_fips_report * out) {
  memset(out, 0, sizeof *out);
  u64 nblocks = (u64) len / FIPS_BLOCK_BYTES;
  out->leftover = (u64) len - nblocks * FIPS_BLOCK_BYTES;
  if (nblocks == 0) return;

  fastent_fips_run_fn run = fastent_pick_fips_variant(NULL);

#ifdef FASTENT_HAVE_THREADS
  if (threads > 1) {
    i32 T = threads;
    if ((u64) T > nblocks) T = (i32) nblocks;
    fastent_set_num_threads(T);
    fips_ctx c;
    c.buf = buf;  c.nblocks = nblocks;  c.T = T;  c.run = run;
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
  run(buf, nblocks, out);
}

int fastent_fips140_print(const fastent_fips_report * r, FILE * fp) {
  if (r->blocks == 0) {
    fprintf(fp, "FIPS 140-2 RNG self-tests: insufficient data "
                "(need >= %u bytes, %" PRIu64 " leftover)\n",
            FIPS_BLOCK_BYTES, (u64) r->leftover);
    return 0;
  }
  const int pass = (r->blocks_pass == r->blocks);
  const u64 n = (u64) r->blocks;
  fprintf(fp, "FIPS 140-2 RNG self-tests (20000-bit blocks)\n");
  fprintf(fp, "  blocks tested  : %" PRIu64 "\n", n);
  fprintf(fp, "  leftover bytes : %" PRIu64 "\n",
          (u64) r->leftover);
  fprintf(fp, "  monobit        : %s  (%" PRIu64 " failed)\n",
          r->monobit_fail ? "FAIL" : "PASS",
          (u64) r->monobit_fail);
  fprintf(fp, "  poker          : %s  (%" PRIu64 " failed)\n",
          r->poker_fail ? "FAIL" : "PASS",
          (u64) r->poker_fail);
  fprintf(fp, "  runs           : %s  (%" PRIu64 " failed)\n",
          r->runs_fail ? "FAIL" : "PASS",
          (u64) r->runs_fail);
  fprintf(fp, "  long run       : %s  (%" PRIu64 " failed)\n",
          r->longrun_fail ? "FAIL" : "PASS",
          (u64) r->longrun_fail);
  fprintf(fp, "  overall        : %s  (%" PRIu64 "/%" PRIu64
              " blocks passed)\n",
          pass ? "PASS" : "FAIL",
          (u64) r->blocks_pass, n);
  return pass;
}
