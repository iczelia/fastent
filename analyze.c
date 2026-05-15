/*  fastent: variant dispatcher, reduction, and bit-mode analyser.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "analyze.h"  /*  Pulls common.h with feature macros.  */
#include "chisq.h"
#include "fastent-math.h"
#include "port-cpu.h"

#include <math.h>
#include <string.h>

void fastent_chunk_state_init(fastent_chunk_state * st) {
  memset(st, 0, sizeof(*st));
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

  const int bins = binary ? 2 : 256;
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

  /*  +NaN from <math.h> formats consistently as "nan" across
      libc/ISA; 0.0/0.0 can print "-nan" on glibc x86 and "nan" on
      musl aarch64.  */
  out->mean = (out->total_samples > 0) ? (sum_x / totalc) : NAN;

  const f64 cexp = totalc / (f64) bins;
  f64 chisq = 0.0, entropy = 0.0;
  Fi(bins,
     const f64 a = (f64) out->hist[i] - cexp;
     chisq += (a * a) / cexp;
     const f64 p = (f64) out->hist[i] / totalc;
     entropy += fastent_entropy_term(p))
  out->chi_square = chisq;  out->entropy = entropy;

  out->chi_probability = fastent_chisq_tail(chisq, binary);

  out->monte_pi = 4.0 * ((f64) st->mc_inside / (f64) st->mc_count);
}

/*  Variant table.  Order matters: dispatcher picks the LAST entry whose
    `available` predicate returns true, so place narrower (preferred)
    variants after their wider supersets.  AVX-512 has two tiers; base
    (F+BW, PSHUFB-LUT popcount) and bitalg (+VPOPCNTB).  */

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
static int avail_avx512_(void)  { return CPU_HAS(avx512f) && CPU_HAS(avx512bw); }
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
  size_t i;
  for (i = 1; i < VARIANTS_N; i++)
    if (variants_[i].available()) best = &variants_[i];
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
  size_t i;
  for (i = 0; i < VARIANTS_N; i++)
    if (variants_[i].variant == v) return variants_[i].name;
  return "scalar";
}
