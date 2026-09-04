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

#include "analyze.h"  /*  Pulls common.h with feature macros.  */
#include "chisq.h"
#include "fastent-math.h"
#include "port-cpu.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void fastent_chunk_state_init(fastent_chunk_state * st) {
  memset(st, 0, sizeof (*st));
}

/*  Order-1 bigram shadow planes (zeroed).  NULL return = OOM; caller
    reports and exits.  Freed via fastent_bigram_free.  */
u64 * fastent_bigram_alloc(void) {
  return (u64 *) calloc((sz) FASTENT_BG_CELLS, sizeof (u64));
}

void fastent_bigram_free(u64 * bg) {
  free(bg);
}

/*  u32 digram chunk shadow (zeroed).  NULL = OOM.  */
u32 * fastent_dg_u32_alloc(void) {
  return (u32 *) calloc((sz) FASTENT_BG_CELLS, sizeof (u32));
}

void fastent_dg_u32_free(u32 * s) {
  free(s);
}

/*  Stream the u32 chunk shadow into the u64 master and zero it.  */
void fastent_dg_drain(fastent_chunk_state * st) {
  u32 * RESTRICT s = st->dg_u32;
  u64 * RESTRICT d = st->bigram;
  i32 i;
  if (!s || !d) return;
  Fi((int) FASTENT_BG_CELLS, d[i] += s[i];  s[i] = 0);
  st->dg_chunk_bytes = 0;
}

/*  Widen-add the u32 order-0 banks into the u64 master and zero them.  */
void fastent_hist_flush_(fastent_chunk_state * st) {
  i32 i, j;
  Fi(FASTENT_BANKS,
    u32 * RESTRICT s = st->bank[i];
    u64 * RESTRICT d = st->hist_master;
    Fj(256, d[j] += s[j];  s[j] = 0););
  st->hist_chunk_bytes = 0;
}

/*  Longest identical-symbol run, one symbol.  Shared by the bit-mode
    scan and the scalar byte digram reference.  */
void fastent_lr_one(fastent_chunk_state * st, u32 s) {
  if (!st->lr_have) {
    st->lr_have = 1;  st->lr_sym = (u8) s;  st->lr_cur = 1;
    st->lr_head_sym = (u8) s;  st->lr_head_len = 1;  st->lr_head_open = 1;
  } else if (s == st->lr_sym) {
    st->lr_cur++;
    if (st->lr_head_open) st->lr_head_len++;
  } else {
    if (st->lr_cur > st->lr_max) st->lr_max = st->lr_cur;
    st->lr_sym = (u8) s;  st->lr_cur = 1;
    st->lr_head_open = 0;        /*  leading run closed; head frozen  */
  }
}

/*  Feed one whole run (symbol q, length n) into the longest-run machine;
    bit-for-bit equivalent to n fastent_lr_one(q) calls, so the head /
    open-run / lr_max bookkeeping the mmap and SPMC merges rely on is
    preserved exactly.  */
void fastent_lr_run(fastent_chunk_state * st, u32 q, u64 n) {
  if (!st->lr_have) {
    st->lr_have = 1;  st->lr_sym = (u8) q;  st->lr_cur = n;
    st->lr_head_sym = (u8) q;  st->lr_head_len = n;  st->lr_head_open = 1;
  } else if (q == st->lr_sym) {
    st->lr_cur += n;
    if (st->lr_head_open) st->lr_head_len += n;
  } else {
    if (st->lr_cur > st->lr_max) st->lr_max = st->lr_cur;
    st->lr_sym = (u8) q;  st->lr_cur = n;  st->lr_head_open = 0;
  }
}

/*  var = sum_x2/n minus mean^2.  */
static f64 variance_(f64 sum_x2_over_n, f64 mean) {
  volatile f64 sq = mean * mean;
  return sum_x2_over_n - sq;
}

/*  a*b minus c with the product rounded before the subtract: same
    aarch64-FMA-fusion 1-ULP split as variance_, here on the SCC
    numerator and denominator.  Pins the x86 two-rounding result.  */
static f64 mul_sub_(f64 a, f64 b, f64 c) {
  volatile f64 p = a * b;
  return p - c;
}

/*  Collapse the FASTENT_BG_NB round-robin planes for one 16-bit
    digram key into its total count.  */
static u64 bg_cell_(const u64 * bg, i32 key) {
  u64 c = 0;
  i32 i;
  Fi(FASTENT_BG_NB, c += bg[i * FASTENT_BG_TABLE + key]);
  return c;
}

void fastent_finalize(
    fastent_chunk_state * RESTRICT st, int binary,
    fastent_result * RESTRICT out) {
  i32 i, j;
  memset(out, 0, sizeof (*out));

  /*  Flush any pending u32 digram chunk into the u64 master before
      the order-1 reduction reads st->bigram.  No-op when the chunk
      shadow is absent (bit mode, no -ee, or the post-merge out).  */
  fastent_dg_drain(st);

  /*  Drain the pending u32 order-0 banks into the u64 master before
      the reduction reads it; no-op in bit mode (banks stay zero).  */
  fastent_hist_flush_(st);

  if (st->have_first && st->have_carry)
    st->cross_product += (i64) st->last_byte * (i64) st->first_byte;

  if (binary) { out->hist[0] = st->bit_hist[0];  out->hist[1] = st->bit_hist[1]; } else {
    Fi(256, out->hist[i] = st->hist_master[i]);
  }
  out->total_samples = st->total_bytes;

  const i32 bins = binary ? 2 : 256;
  const f64 totalc = (f64) out->total_samples;

  /*  Exact integer moments (i <= 255, hist u64); f64 accumulation
      would lose the low bits to catastrophic cancellation in the
      variance subtraction at large N.  */
  u64 isum_x = 0, isum_x2 = 0;
  Fi(bins,
    isum_x  += (u64) i * out->hist[i];
    isum_x2 += (u64) (i * i) * out->hist[i]);
  const f64 sum_x  = (f64) isum_x;
  const f64 sum_x2 = (f64) isum_x2;

  const f64 scct1 = (f64) st->cross_product;
  const f64 scct2_sq = sum_x * sum_x;
  const f64 denom = mul_sub_(totalc, sum_x2, scct2_sq);
  out->scc = (denom == 0.0) ? FASTENT_SCC_UNDEF
                            : mul_sub_(totalc, scct1, scct2_sq) / denom;

  /*  Use +NaN (formats as "nan" everywhere); 0.0/0.0 can print
      "-nan" on glibc x86 but "nan" on musl aarch64.  */
  out->mean = (out->total_samples > 0) ? (sum_x / totalc) : NAN;

  const f64 cexp = totalc / (f64) bins;
  f64 chisq = 0.0, entropy = 0.0, sum_c2 = 0.0;
  u64 max_count = 0, min_count = (u64) -1;
  int mode_value = -1, rarest_value = -1;
  u32 distinct = 0;
  Fi(bins,
    const u64 c = out->hist[i];
    const f64 a = (f64) c - cexp;
    chisq += (a * a) / cexp;
    const f64 p = (f64) c / totalc;
    entropy += fastent_entropy_term(p);
    sum_c2 += (f64) c * (f64) c;
    if (c) {
      distinct++;
      if (c > max_count) { max_count = c; mode_value   = i; }
      if (c < min_count) { min_count = c; rarest_value = i; }
    });
  out->chi_square = chisq;  out->entropy = entropy;

  out->chi_probability = fastent_chisq_tail(chisq, binary);

  out->monte_pi = 4.0 * ((f64) st->mc_inside / (f64) st->mc_count);

  /*  H_inf = log2(N / max_count), H_2 = log2(N^2 / sum_c2): log2 of
      an exact ratio >= 1, via the libm-free fastent_log2 family.  */
  const f64 hmax = binary ? 1.0 : 8.0;
  out->distinct      = distinct;
  out->mode_value    = mode_value;
  out->mode_count    = max_count;
  out->rarest_value  = rarest_value;
  out->rarest_count  = (rarest_value < 0) ? 0 : min_count;

  if (out->total_samples > 0) {
    out->min_entropy       = fastent_log2_ratio(out->total_samples,
                                                max_count);
    out->collision_entropy =
        fastent_log2_ge1(totalc * totalc / sum_c2);
    out->redundancy        = 1.0 - entropy / hmax;
    const f64 var = variance_(sum_x2 / totalc, out->mean);
    out->variance = var > 0.0 ? var : 0.0;
    out->stddev   = sqrt(out->variance);
  } else {
    out->min_entropy = out->collision_entropy = NAN;
    out->redundancy  = out->variance = out->stddev = NAN;
  }

  out->ic = (out->total_samples >= 2)
            ? (sum_c2 - totalc) / (totalc * (totalc - 1.0)) : NAN;

  if (binary || out->total_samples == 0) { out->poker_chisq = out->poker_p = NAN; } else {
    u64 nib[16];
    memset(nib, 0, sizeof (nib));
    Fi(256,
      const u64 c = out->hist[i];
      nib[i & 15] += c;
      nib[(i >> 4) & 15] += c);
    const f64 ek = totalc / 8.0;  /*  2*N nibbles / 16 bins  */
    f64 pk = 0.0;
    Fi(16, const f64 d = (f64) nib[i] - ek; pk += (d * d) / ek);
    out->poker_chisq = pk;
    out->poker_p     = fastent_chisq_tail_df(pk, 15);
  }

  /*  Per-bit-position bias.  */
  if (binary || out->total_samples == 0) {
    Fi(8, out->bit_freq[i] = NAN);
    out->bit_bias_max   = NAN;
    out->bit_bias_worst = -1;
  } else {
    u64 ones[8];
    memset(ones, 0, sizeof (ones));
    Fi(256,
      const u64 c = out->hist[i];
      Fj(8, if (i & (1 << j)) ones[j] += c););
    f64 worst = -1.0;
    i32 wk = 0;
    Fi(8,
      const f64 f = (f64) ones[i] / totalc;
      out->bit_freq[i] = f;
      const f64 d = f < 0.5 ? 0.5 - f : f - 0.5;
      if (d > worst) { worst = d; wk = i; });
    out->bit_bias_max   = worst;
    out->bit_bias_worst = wk;
  }

  /*  Order-1 H(cur|prev) and I(prev;cur).  Byte mode (-ee) sums the
      NB 64K digram tables (key = prev<<8 | cur); bit mode the 2x2
      bit_bigram.  Logs via fastent_entropy_term (bit-identical).  */
  out->conditional_entropy = out->mutual_information = NAN;
  if (!binary && st->bigram && out->total_samples >= 2) {
    const u64 * bg = st->bigram;
    u64 R[256], S[256];
    memset(R, 0, sizeof R);  memset(S, 0, sizeof S);
    f64 M = 0.0;
    Fi(FASTENT_BG_TABLE,
      const u64 c = bg_cell_(bg, i);
      if (c) { R[i >> 8] += c; S[i & 0xFF] += c; M += (f64) c; });
    if (M >= 1.0) {
      f64 hj = 0.0, hp = 0.0, hc = 0.0;
      Fi(FASTENT_BG_TABLE,
        const u64 c = bg_cell_(bg, i);
        if (c) hj += fastent_entropy_term((f64) c / M));
      Fi(256,
        if (R[i]) hp += fastent_entropy_term((f64) R[i] / M);
        if (S[i]) hc += fastent_entropy_term((f64) S[i] / M));
      out->conditional_entropy = hj - hp;          /*  H(cur|prev)  */
      out->mutual_information  = hp + hc - hj;      /*  I(prev;cur)  */
    }
  } else if (binary) {
    const u64 c00 = st->bit_bigram[0][0], c01 = st->bit_bigram[0][1];
    const u64 c10 = st->bit_bigram[1][0], c11 = st->bit_bigram[1][1];
    const f64 M = (f64) c00 + (f64) c01 + (f64) c10 + (f64) c11;
    if (M >= 1.0) {          /*  all-zero => -ee bit mode did not run  */
      const u64 R0 = c00 + c01, R1 = c10 + c11;   /*  prev marginal  */
      const u64 S0 = c00 + c10, S1 = c01 + c11;   /*  cur  marginal  */
      f64 hj = 0.0, hp = 0.0, hc = 0.0;
      if (c00) hj += fastent_entropy_term((f64) c00 / M);
      if (c01) hj += fastent_entropy_term((f64) c01 / M);
      if (c10) hj += fastent_entropy_term((f64) c10 / M);
      if (c11) hj += fastent_entropy_term((f64) c11 / M);
      if (R0)  hp += fastent_entropy_term((f64) R0 / M);
      if (R1)  hp += fastent_entropy_term((f64) R1 / M);
      if (S0)  hc += fastent_entropy_term((f64) S0 / M);
      if (S1)  hc += fastent_entropy_term((f64) S1 / M);
      out->conditional_entropy = hj - hp;
      out->mutual_information  = hp + hc - hj;
    }
  }

  /*  Runs / longest run / cusum from the fused -ee pass; st->lr_have gates
      -ee.  */
  out->runs = out->longest_run = out->cusum_max = NAN;
  if (st->lr_have) {
    const u64 lr = st->lr_cur > st->lr_max ? st->lr_cur : st->lr_max;
    out->longest_run = (f64) lr;
    if (binary) {
      out->runs = (f64) st->rn_count;
      const i64 lo = st->cs_min < 0 ? -st->cs_min : st->cs_min;
      const i64 hi = st->cs_max < 0 ? -st->cs_max : st->cs_max;
      out->cusum_max = (f64) (lo > hi ? lo : hi);
    }
  }
  if (!binary && st->bigram && out->total_samples >= 1) {
    if (out->total_samples == 1) { out->runs = 1.0; } else {
      const u64 * bg = st->bigram;
      u64 acc = 0, chg = 0;
      const u64 half = (out->total_samples + 1) / 2;
      i32 m = 0;
      Fi(256, acc += out->hist[i];  if (acc >= half) { m = i; break; });
      Fi(FASTENT_BG_TABLE,
        const u64 c = bg_cell_(bg, i);
        if (c && (((i >> 8) >= m) != ((i & 0xFF) >= m))) chg += c);
      out->runs = (f64) (1 + chg);
    }
  }

  /*  LZ77F (-e) sentinels: fastent_lz_finalize overwrites these
      when extended >= 1, else NaN (as conditional_entropy below).  */
  out->lz_cr_excess = out->lz_lit_h = out->lz_lit_kl = NAN;
  out->lz_match_cov = out->lz_off_conc = out->lz_mlen_excess = NAN;
  out->lz_lit_chi = out->lz_lit_chi_p = out->lz_deviation = NAN;
  out->lz_nmatch = 0;
  out->lz_megamatch = 0;
  out->lz = NULL;

  /*  Linear-complexity (-eee) sentinels: fastent_bm_finalize
      overwrites these when extended >= 3, else NaN.  */
  out->bm_deviation = out->bm_mean_lc = out->bm_mu = NAN;
  out->bm_chi = out->bm_chi_p = NAN;
  out->bm_windows = 0;
  out->bm_degenerate = 0;
  Fi(64, out->bm_lhist[i] = 0);

  /*  Maurer (-eee) sentinels: fastent_maurer_finalize overwrites
      these when extended >= 3, else NaN.  */
  out->maurer_fn = out->maurer_expected = out->maurer_dev = NAN;
  out->maurer_k = 0;
  out->maurer_degenerate = 0;
  Fi(64, out->maurer_lhist[i] = 0);

  /*  Binary matrix-rank (-eee) sentinels: fastent_mrank_finalize
      overwrites these when extended >= 3, else NaN / 0.  */
  out->mrank_dev = out->mrank_chi = out->mrank_chi_p = NAN;
  out->mrank_matrices = 0;
  out->mrank_r32 = out->mrank_r31 = out->mrank_rlo = 0;
  out->mrank_underpowered = 0;

  /*  Permutation entropy (-e) sentinels: fastent_perment_finalize
      overwrites these when extended >= 1, else NaN / 0.  */
  out->perment_h_norm = out->perment_deviation = NAN;
  out->perment_chi = out->perment_chi_p = NAN;
  out->perment_windows = 0;
  Fi(24, out->perment_hist[i] = 0);
}

/*  Variant table.  */

#define CPU_HAS(name)      (fastent_cpu_get()->name)

typedef struct {
  fastent_variant     variant;
  const char *        name;
  int               (*available)(void);
  fastent_analyze_fn  byte;
  fastent_analyze_fn  bits;
  fastent_analyze_fn  fold_byte;
  fastent_analyze_fn  fold_bits;
  fastent_fold_fn     fold;
  fastent_digram_byte_fn digram_byte;
  fastent_digram_bits_fn digram_bits;
} variant_entry;

static int avail_scalar_(void)  { return 1; }
#ifdef HAVE_SSSE3
static int avail_ssse3_(void)   { return CPU_HAS(ssse3); }
#endif
#ifdef HAVE_SSE41
static int avail_sse41_(void)   { return CPU_HAS(sse42); }
#endif
#ifdef HAVE_AVX2
static int avail_avx2_(void)    { return CPU_HAS(avx2); }
#endif
#ifdef HAVE_AVX512
static int avail_avx512_(void) {
  return CPU_HAS(avx512f) && CPU_HAS(avx512bw);
}
#endif
#ifdef HAVE_AVX512_BITALG
static int avail_avx512b_(void) {
  return CPU_HAS(avx512f) && CPU_HAS(avx512bw)
      && CPU_HAS(avx512bitalg) && CPU_HAS(avx512vnni);
}
#endif
#ifdef HAVE_NEON
static int avail_neon_(void)    { return CPU_HAS(neon); }
#endif
#ifdef HAVE_SVE2
static int avail_sve2_(void)    { return CPU_HAS(sve2); }
#endif
#ifdef HAVE_WASM128
static int avail_wasm128_(void) { return CPU_HAS(wasm128); }
#endif

#define ENTRY(VARIANT, NAME, AVAIL, SUFFIX)                           \
  { FASTENT_VAR_##VARIANT, NAME, AVAIL,                               \
    analyze_##SUFFIX,           analyze_bits_##SUFFIX,                \
    analyze_fold_##SUFFIX,      analyze_bits_fold_##SUFFIX,           \
    fold_##SUFFIX,             digram_bytes_##SUFFIX,                 \
    digram_bits_blk_##SUFFIX }

/*  analyze-sve2.c is self-contained (no analyze-impl.h), so it emits no
    digram_bits_blk_sve2.  */
#define ENTRY_DGSCALAR(VARIANT, NAME, AVAIL, SUFFIX)                  \
  { FASTENT_VAR_##VARIANT, NAME, AVAIL,                               \
    analyze_##SUFFIX,           analyze_bits_##SUFFIX,                \
    analyze_fold_##SUFFIX,      analyze_bits_fold_##SUFFIX,           \
    fold_##SUFFIX,             digram_bytes_##SUFFIX,                 \
    digram_bits_blk_scalar }

static const variant_entry variants_[] = {
  ENTRY(SCALAR,           "scalar",        avail_scalar_,  scalar),
#ifdef HAVE_SSSE3
  ENTRY(SSSE3_,           "ssse3",         avail_ssse3_,   ssse3),
#endif
#ifdef HAVE_SSE41
  ENTRY(SSE41_,           "sse4.1",        avail_sse41_,   sse41),
#endif
#ifdef HAVE_AVX2
  ENTRY(AVX2_,            "avx2",          avail_avx2_,    avx2),
#endif
#ifdef HAVE_AVX512
  ENTRY(AVX512_,          "avx512",        avail_avx512_,  avx512),
#endif
#ifdef HAVE_AVX512_BITALG
  ENTRY(AVX512_BITALG,    "avx512+bitalg", avail_avx512b_, avx512_bitalg),
#endif
#ifdef HAVE_NEON
  ENTRY(NEON_,            "neon",          avail_neon_,    neon),
#endif
#ifdef HAVE_SVE2
  ENTRY_DGSCALAR(SVE2_,   "sve2",          avail_sve2_,    sve2),
#endif
#ifdef HAVE_WASM128
  ENTRY(WASM128_,         "wasm-simd128",  avail_wasm128_, wasm128),
#endif
};

#undef ENTRY
#undef ENTRY_DGSCALAR

#define VARIANTS_N (sizeof variants_ / sizeof variants_[0])

static const variant_entry * pick_(void) {
  const variant_entry * best = &variants_[0];
  i32 i;
  for (i = 1; i < (i32) VARIANTS_N; i++) { if (variants_[i].available()) best = &variants_[i]; }
  return best;
}

fastent_analyze_fn fastent_pick_variant(fastent_variant * which) {
  const variant_entry * e = pick_();
  if (which) *which = e->variant;
  return e->byte;
}

fastent_analyze_fn fastent_pick_bits_variant(fastent_variant * which) {
  const variant_entry * e = pick_();
  if (which) *which = e->variant;
  return e->bits;
}

fastent_analyze_fn fastent_pick_fold_byte_variant(fastent_variant * which) {
  const variant_entry * e = pick_();
  if (which) *which = e->variant;
  return e->fold_byte;
}

fastent_analyze_fn fastent_pick_fold_bits_variant(fastent_variant * which) {
  const variant_entry * e = pick_();
  if (which) *which = e->variant;
  return e->fold_bits;
}

fastent_fold_fn fastent_pick_fold_variant(fastent_variant * which) {
  const variant_entry * e = pick_();
  if (which) *which = e->variant;
  return e->fold;
}

fastent_digram_byte_fn
fastent_pick_digram_byte_variant(fastent_variant * which) {
  const variant_entry * e = pick_();
  if (which) *which = e->variant;
  return e->digram_byte;
}

fastent_digram_bits_fn
fastent_pick_digram_bits_variant(fastent_variant * which) {
  const variant_entry * e = pick_();
  if (which) *which = e->variant;
  return e->digram_bits;
}

const char * fastent_variant_name(fastent_variant v) {
  i32 i;
  Fi((int) VARIANTS_N,
    if (variants_[i].variant == v) return variants_[i].name);
  return "scalar";
}
