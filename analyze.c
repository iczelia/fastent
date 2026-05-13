/*  fastent  --  variant dispatcher, reduction, and bit-mode analyser.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "analyze.h"  /*  Pulls common.h with feature macros.  */
#include "chisq.h"

#include <math.h>
#include <string.h>

#define FASTENT_LOG2OF10 3.32192809488736234787

static inline f64 fastent_log2(f64 x) {
  return FASTENT_LOG2OF10 * log10(x);
}

/*  ----------------------------------------------------------------------
    State init / merge.  */

void fastent_chunk_state_init(fastent_chunk_state * st) {
  memset(st, 0, sizeof(*st));
}

/*  ----------------------------------------------------------------------
    Final reduction: turn fastent_chunk_state into fastent_result.

    For byte mode: histogram bins are 0..255.
    For bit mode: state was populated by analyze_bits_scalar; bank[0][0]
    is the count of 0-bits, bank[0][1] the count of 1-bits.  */

void fastent_finalize(fastent_chunk_state * st, int binary, fastent_result * out) {
  memset(out, 0, sizeof(*out));

  /*  Add the wrap-around SCC term.  */
  if (st->have_first && st->have_carry)
    st->cross_product += (i64) st->last_byte * (i64) st->first_byte;

  /*  Merge banks into final histogram.  */
  if (binary) {
    out->hist[0] = st->bit_hist[0];  out->hist[1] = st->bit_hist[1];
  } else {
    Fi(256,
       out->hist[i] = (u64) st->bank[0][i] + st->bank[1][i]
                    + st->bank[2][i] + st->bank[3][i])
  }
  out->total_samples = st->total_bytes;  /*  bits in -b mode, else bytes  */

  const int bins = binary ? 2 : 256;
  const f64 totalc = (f64) out->total_samples;

  /*  sum_v v * hist[v]  and  sum_v v^2 * hist[v]  in double.  */
  f64 sum_x  = 0.0;
  f64 sum_x2 = 0.0;
  Fi(bins,
     sum_x  += (f64) i * (f64) out->hist[i];
     sum_x2 += (f64) i * (f64) i * (f64) out->hist[i])

  /*  SCC.  */
  const f64 scct1 = (f64) st->cross_product;
  const f64 scct2_sq = sum_x * sum_x;
  const f64 denom = totalc * sum_x2 - scct2_sq;
  out->scc = (denom == 0.0) ? -100000.0 : (totalc * scct1 - scct2_sq) / denom;

  /*  Mean. Unguarded division to match original behaviour on empty
      input (yields -nan, formatted as "-nan" in default mode).  */
  out->mean = sum_x / totalc;

  /*  Chi-square + entropy.  */
  const f64 cexp = totalc / (f64) bins;
  f64 chisq = 0.0, entropy = 0.0;
  Fi(bins,
     const f64 a = (f64) out->hist[i] - cexp;
     chisq += (a * a) / cexp;
     const f64 p = (f64) out->hist[i] / totalc;
     if (p > 0.0) entropy += p * fastent_log2(1.0 / p))
  out->chi_square = chisq;  out->entropy = entropy;

  out->chi_probability = fastent_chisq_tail(chisq, binary);

  /*  Monte Carlo Pi.  */
  out->monte_pi = 4.0 * ((f64) st->mc_inside / (f64) st->mc_count);
}

/*  ----------------------------------------------------------------------
    Runtime variant pick.

    Compile-time HAVE_AVX2/HAVE_SSE41/HAVE_SSSE3 means the variant TU was
    built; we still confirm at runtime via __builtin_cpu_supports so a
    binary built on a wider host can run on a narrower one.  */

/*  Runtime feature probe shared across the pickers.  Returns 1 iff the
    host has every AVX-512 extension referenced by the AVX-512 SIMD
    body: F (foundation), BW (byte/word ops), CD (VPCONFLICTD),
    VPOPCNTDQ + BITALG (VPOPCNTB).  All four ship together on Zen 4
    and Ice Lake-SP+, so a single combined check is fine.  */
#if defined(HAVE_AVX512) && (defined(__GNUC__) || defined(__clang__))
static int fastent_have_avx512_runtime(void) {
  return __builtin_cpu_supports("avx512f")
      && __builtin_cpu_supports("avx512bw")
      && __builtin_cpu_supports("avx512cd")
      && __builtin_cpu_supports("avx512vpopcntdq")
      && __builtin_cpu_supports("avx512bitalg");
}
#endif

fastent_analyze_fn fastent_pick_variant(fastent_variant * which) {
  fastent_variant v = FASTENT_VAR_SCALAR;
  fastent_analyze_fn fn = analyze_scalar;

#if defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  #ifdef HAVE_SSSE3
    if (__builtin_cpu_supports("ssse3"))  { v = FASTENT_VAR_SSSE3_;  fn = analyze_ssse3; }
  #endif
  #ifdef HAVE_SSE41
    if (__builtin_cpu_supports("sse4.1")) { v = FASTENT_VAR_SSE41_;  fn = analyze_sse41; }
  #endif
  #ifdef HAVE_AVX2
    if (__builtin_cpu_supports("avx2"))   { v = FASTENT_VAR_AVX2_;   fn = analyze_avx2; }
  #endif
  #ifdef HAVE_AVX512
    if (fastent_have_avx512_runtime())    { v = FASTENT_VAR_AVX512_; fn = analyze_avx512; }
  #endif
#endif

  if (which) *which = v;
  return fn;
}

const char * fastent_variant_name(fastent_variant v) {
  switch (v) {
    case FASTENT_VAR_AVX512_: return "avx512";
    case FASTENT_VAR_AVX2_:   return "avx2";
    case FASTENT_VAR_SSE41_:  return "sse4.1";
    case FASTENT_VAR_SSSE3_:  return "ssse3";
    case FASTENT_VAR_SCALAR: return "scalar";
  }
  return "scalar";
}

/*  Bit-mode picker.  Mirrors fastent_pick_variant exactly.  The actual
    analyze_bits_<variant> bodies live in analyze-impl.h and are emitted
    alongside the byte-mode bodies in each per-ISA TU.  */
fastent_analyze_fn fastent_pick_bits_variant(fastent_variant * which) {
  fastent_variant v = FASTENT_VAR_SCALAR;
  fastent_analyze_fn fn = analyze_bits_scalar;

#if defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  #ifdef HAVE_SSSE3
    if (__builtin_cpu_supports("ssse3"))  { v = FASTENT_VAR_SSSE3_;  fn = analyze_bits_ssse3; }
  #endif
  #ifdef HAVE_SSE41
    if (__builtin_cpu_supports("sse4.1")) { v = FASTENT_VAR_SSE41_;  fn = analyze_bits_sse41; }
  #endif
  #ifdef HAVE_AVX2
    if (__builtin_cpu_supports("avx2"))   { v = FASTENT_VAR_AVX2_;   fn = analyze_bits_avx2; }
  #endif
  #ifdef HAVE_AVX512
    if (fastent_have_avx512_runtime())    { v = FASTENT_VAR_AVX512_; fn = analyze_bits_avx512; }
  #endif
#endif

  if (which) *which = v;
  return fn;
}

/*  Fused fold + byte-mode analyse picker.  Mirrors fastent_pick_variant.  */
fastent_analyze_fn fastent_pick_fold_byte_variant(fastent_variant * which) {
  fastent_variant v = FASTENT_VAR_SCALAR;
  fastent_analyze_fn fn = analyze_fold_scalar;

#if defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  #ifdef HAVE_SSSE3
    if (__builtin_cpu_supports("ssse3"))  { v = FASTENT_VAR_SSSE3_;  fn = analyze_fold_ssse3; }
  #endif
  #ifdef HAVE_SSE41
    if (__builtin_cpu_supports("sse4.1")) { v = FASTENT_VAR_SSE41_;  fn = analyze_fold_sse41; }
  #endif
  #ifdef HAVE_AVX2
    if (__builtin_cpu_supports("avx2"))   { v = FASTENT_VAR_AVX2_;   fn = analyze_fold_avx2; }
  #endif
  #ifdef HAVE_AVX512
    if (fastent_have_avx512_runtime())    { v = FASTENT_VAR_AVX512_; fn = analyze_fold_avx512; }
  #endif
#endif

  if (which) *which = v;
  return fn;
}

/*  Fused fold + bit-mode analyse picker.  */
fastent_analyze_fn fastent_pick_fold_bits_variant(fastent_variant * which) {
  fastent_variant v = FASTENT_VAR_SCALAR;
  fastent_analyze_fn fn = analyze_bits_fold_scalar;

#if defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  #ifdef HAVE_SSSE3
    if (__builtin_cpu_supports("ssse3"))  { v = FASTENT_VAR_SSSE3_;  fn = analyze_bits_fold_ssse3; }
  #endif
  #ifdef HAVE_SSE41
    if (__builtin_cpu_supports("sse4.1")) { v = FASTENT_VAR_SSE41_;  fn = analyze_bits_fold_sse41; }
  #endif
  #ifdef HAVE_AVX2
    if (__builtin_cpu_supports("avx2"))   { v = FASTENT_VAR_AVX2_;   fn = analyze_bits_fold_avx2; }
  #endif
  #ifdef HAVE_AVX512
    if (fastent_have_avx512_runtime())    { v = FASTENT_VAR_AVX512_; fn = analyze_bits_fold_avx512; }
  #endif
#endif

  if (which) *which = v;
  return fn;
}

/*  Case-fold picker.  */
fastent_fold_fn fastent_pick_fold_variant(fastent_variant * which) {
  fastent_variant v = FASTENT_VAR_SCALAR;
  fastent_fold_fn fn = fold_scalar;

#if defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  #ifdef HAVE_SSSE3
    if (__builtin_cpu_supports("ssse3"))  { v = FASTENT_VAR_SSSE3_;  fn = fold_ssse3; }
  #endif
  #ifdef HAVE_SSE41
    if (__builtin_cpu_supports("sse4.1")) { v = FASTENT_VAR_SSE41_;  fn = fold_sse41; }
  #endif
  #ifdef HAVE_AVX2
    if (__builtin_cpu_supports("avx2"))   { v = FASTENT_VAR_AVX2_;   fn = fold_avx2; }
  #endif
  #ifdef HAVE_AVX512
    if (fastent_have_avx512_runtime())    { v = FASTENT_VAR_AVX512_; fn = fold_avx512; }
  #endif
#endif

  if (which) *which = v;
  return fn;
}
