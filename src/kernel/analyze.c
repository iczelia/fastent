/*  fastent: variant dispatcher, reduction, and bit-mode analyser.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "analyze.h"  /*  Pulls common.h with feature macros.  */
#include "chisq.h"
#include "fastent-math.h"
#include "port-cpu.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void fastent_chunk_state_init(fastent_chunk_state * st) {
  memset(st, 0, sizeof(*st));
}

/*  Order-1 bigram shadow planes (zeroed).  NULL return = OOM; caller
    reports and exits.  Freed via fastent_bigram_free.  */
u64 * fastent_bigram_alloc(void) {
  return (u64 *) calloc((sz) FASTENT_BG_CELLS, sizeof(u64));
}

void fastent_bigram_free(u64 * bg) {
  free(bg);
}

/*  Collapse the FASTENT_BG_NB round-robin planes for one 16-bit
    digram key into its total count.  */
static u64 bg_cell_(const u64 * bg, i32 key) {
  u64 c = 0;
  Fi(FASTENT_BG_NB, c += bg[i * FASTENT_BG_TABLE + key])
  return c;
}

void fastent_finalize(fastent_chunk_state * FASTENT_RESTRICT st, int binary,
                      fastent_result * FASTENT_RESTRICT out) {
  memset(out, 0, sizeof(*out));

  if (st->have_first && st->have_carry)
    st->cross_product += (i64) st->last_byte * (i64) st->first_byte;

  if (binary) {
    out->hist[0] = st->bit_hist[0];  out->hist[1] = st->bit_hist[1];
  } else {
    Fi(256,
       out->hist[i] = (u64) st->bank[0][i] + st->bank[1][i]
                    + st->bank[2][i] + st->bank[3][i])
  }
  out->total_samples = st->total_bytes;

  const i32 bins = binary ? 2 : 256;
  const f64 totalc = (f64) out->total_samples;

  f64 sum_x  = 0.0;
  f64 sum_x2 = 0.0;
  Fi(bins,
     sum_x  += (f64) i * (f64) out->hist[i];
     sum_x2 += (f64) i * (f64) i * (f64) out->hist[i])

  const f64 scct1 = (f64) st->cross_product;
  const f64 scct2_sq = sum_x * sum_x;
  const f64 denom = totalc * sum_x2 - scct2_sq;
  out->scc = (denom == 0.0) ? -100000.0 : (totalc * scct1 - scct2_sq) / denom;

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
     })
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
    const f64 var = sum_x2 / totalc - out->mean * out->mean;
    out->variance = var > 0.0 ? var : 0.0;
    out->stddev   = sqrt(out->variance);
  } else {
    out->min_entropy = out->collision_entropy = NAN;
    out->redundancy  = out->variance = out->stddev = NAN;
  }

  out->ic = (out->total_samples >= 2)
            ? (sum_c2 - totalc) / (totalc * (totalc - 1.0)) : NAN;

  if (binary || out->total_samples == 0) {
    out->poker_chisq = out->poker_p = NAN;
  } else {
    u64 nib[16];
    memset(nib, 0, sizeof(nib));
    Fi(256,
       const u64 c = out->hist[i];
       nib[i & 15] += c;
       nib[(i >> 4) & 15] += c)
    const f64 ek = totalc / 8.0;  /*  2*N nibbles / 16 bins  */
    f64 pk = 0.0;
    Fi(16, const f64 d = (f64) nib[i] - ek; pk += (d * d) / ek)
    out->poker_chisq = pk;
    out->poker_p     = fastent_chisq_tail_df(pk, 15);
  }

  /*  Per-bit-position bias.  ones[k] = sum of hist[v] over v with bit
      k set, from the byte histogram.  Catches structured binary (dead
      high bits, ASCII bit 7) that order-0 entropy/chi-square miss.
      Byte mode only; bit mode has no byte histogram.  */
  if (binary || out->total_samples == 0) {
    Fi(8, out->bit_freq[i] = NAN)
    out->bit_bias_max   = NAN;
    out->bit_bias_worst = -1;
  } else {
    u64 ones[8];
    memset(ones, 0, sizeof(ones));
    Fi(256,
       const u64 c = out->hist[i];
       Fj(8, if (i & (1 << j)) ones[j] += c))
    f64 worst = -1.0;
    i32 wk = 0;
    Fi(8,
       const f64 f = (f64) ones[i] / totalc;
       out->bit_freq[i] = f;
       const f64 d = f < 0.5 ? 0.5 - f : f - 0.5;
       if (d > worst) { worst = d; wk = i; })
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
       if (c) { R[i >> 8] += c; S[i & 0xFF] += c; M += (f64) c; })
    if (M >= 1.0) {
      f64 hj = 0.0, hp = 0.0, hc = 0.0;
      Fi(FASTENT_BG_TABLE,
         const u64 c = bg_cell_(bg, i);
         if (c) hj += fastent_entropy_term((f64) c / M))
      Fi(256,
         if (R[i]) hp += fastent_entropy_term((f64) R[i] / M);
         if (S[i]) hc += fastent_entropy_term((f64) S[i] / M))
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

  /*  Runs / longest run / cusum from the fused -ee pass.  st->lr_have
      is set iff that pass ran with >= 1 symbol, doubling as the -ee
      gate.  Byte runs-vs-median = 1 + count of adjacent byte pairs
      whose median class differs; that pair multiset is exactly the
      digram table, so no rescan and it works for streams too.  */
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
    if (out->total_samples == 1) {
      out->runs = 1.0;
    } else {
      const u64 * bg = st->bigram;
      u64 acc = 0, chg = 0;
      const u64 half = (out->total_samples + 1) / 2;
      i32 m = 0;
      Fi(256, acc += out->hist[i];  if (acc >= half) { m = i; break; })
      Fi(FASTENT_BG_TABLE,
         const u64 c = bg_cell_(bg, i);
         if (c && (((i >> 8) >= m) != ((i & 0xFF) >= m))) chg += c)
      out->runs = (f64) (1 + chg);
    }
  }
}

/*  Variant table.  Order matters: dispatcher picks the LAST entry
    whose `available` returns true, so place narrower (preferred)
    variants after their wider supersets.  AVX-512 has two tiers:
    base (F+BW, PSHUFB-LUT popcount) and bitalg (+VPOPCNTB).  */

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
  return CPU_HAS(avx512f) && CPU_HAS(avx512bw) && CPU_HAS(avx512bitalg);
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
    fold_##SUFFIX }

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
  ENTRY(SVE2_,            "sve2",          avail_sve2_,    sve2),
#endif
#ifdef HAVE_WASM128
  ENTRY(WASM128_,         "wasm-simd128",  avail_wasm128_, wasm128),
#endif
};

#undef ENTRY

#define VARIANTS_N (sizeof variants_ / sizeof variants_[0])

static const variant_entry * pick_(void) {
  const variant_entry * best = &variants_[0];
  Fi0((int) VARIANTS_N, 1,
      if (variants_[i].available()) best = &variants_[i])
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

const char * fastent_variant_name(fastent_variant v) {
  Fi((int) VARIANTS_N,
     if (variants_[i].variant == v) return variants_[i].name)
  return "scalar";
}
