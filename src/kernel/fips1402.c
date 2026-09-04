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
#include "fips1402.h"

#include "port-cpu.h"
#include "port-thread.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define FIPS_BLOCK_BYTES FASTENT_FIPS_BLOCK_BYTES

/*  Variant table.  */

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
#ifdef HAVE_AVX512_VPOPCNTDQ
static int favail_avx512vp_(void) {
  return CPU_HAS(avx512f) && CPU_HAS(avx512bw)
      && CPU_HAS(avx512vpopcntdq);
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
#ifdef HAVE_AVX512_VPOPCNTDQ
  { FASTENT_VAR_AVX512_VPOPCNTDQ, "avx512+vpopcntdq", favail_avx512vp_,
    fastent_fips_run_blocks_avx512_vpopcntdq },
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
  i32 i;
  for (i = 1; i < (i32) FIPS_VARIANTS_N; i++) {
    if (fips_variants_[i].available()) best = &fips_variants_[i];
  }
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
  u64 hi = (u64) (k + 1) * c->nblocks / (u64) c->T;
  c->run(c->buf + lo * FIPS_BLOCK_BYTES, hi - lo, &c->shards[k]);
}
#endif

void fastent_fips140_run(
    const u8 * buf, sz len, int threads, fastent_fips_report * out) {
  i32 k;
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
      (fastent_fips_report *) calloc((sz) T, sizeof (*c.shards));
    if (c.shards) {
      fastent_parallel_for((sz) T, fips_worker_, &c);
      Fk(T,
        out->blocks       += c.shards[k].blocks;
        out->monobit_fail += c.shards[k].monobit_fail;
        out->poker_fail   += c.shards[k].poker_fail;
        out->runs_fail    += c.shards[k].runs_fail;
        out->longrun_fail += c.shards[k].longrun_fail;
        out->blocks_pass  += c.shards[k].blocks_pass);
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

/*  Streaming FIPS driver, bounded memory (sub-2500 carry only).  Same
    per-ISA batched runner as fastent_fips140_run, so verdicts/leftover
    are byte-identical to slurp.  Serial on -j.  */

void fastent_fips140_stream_init(
    fastent_fips_stream * s, fastent_fips_report * out) {
  memset(out, 0, sizeof *out);
  s->out = out;
  s->run = fastent_pick_fips_variant(NULL);
  s->fill = 0;
}

/*  Feed an any-size chunk: complete any carried partial block, dispatch
    full blocks directly from the caller buffer, then retain only the
    final sub-block residue.  */
void fastent_fips140_stream_push(
    fastent_fips_stream * s, const u8 * buf, sz len) {
  if (s->fill) {
    sz need = FIPS_BLOCK_BYTES - s->fill;
    if (need > len) need = len;
    memcpy(s->carry + s->fill, buf, need);
    s->fill += need;  buf += need;  len -= need;
    if (s->fill == FIPS_BLOCK_BYTES) {
      s->run(s->carry, 1u, s->out);
      s->fill = 0;
    }
  }

  if (len >= FIPS_BLOCK_BYTES) {
    u64 nb = (u64) len / FIPS_BLOCK_BYTES;
    sz done = (sz) nb * FIPS_BLOCK_BYTES;
    s->run(buf, nb, s->out);
    buf += done;  len -= done;
  }

  if (len) {
    memcpy(s->carry, buf, len);
    s->fill = len;
  }
}

/*  The stream pusher dispatches every full 2500-byte block as soon as
    it is complete.  The remaining carry is the final leftover.  */
int fastent_fips140_stream_finish(fastent_fips_stream * s) {
  s->out->leftover = (u64) s->fill;
  s->fill = 0;
  return 0;
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
