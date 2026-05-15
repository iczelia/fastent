/*  fastent: variant dispatcher, reduction, and bit-mode analyser.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "analyze.h"  /*  Pulls common.h with feature macros.  */
#include "chisq.h"
#include "fastent-math.h"

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

/*  HAVE_* gates select which variant TUs were built; the runtime
    probes below confirm the CPU actually supports them.  Hand-rolled
    cpuid because clang's compiler-rt on Windows lacks the libgcc
    __cpu_model that __builtin_cpu_supports needs.  */

#if defined(__i386__) || defined(__x86_64__)

/*  Toggling EFLAGS bit 21 flips only on Pentium-class and newer;
    pre-Pentium 386/486 leave it stuck and we bail to scalar.  */
#if defined(__x86_64__)
static inline int fastent_x86_has_cpuid(void) { return 1; }
#else
static int fastent_x86_has_cpuid(void) {
  unsigned int x, y;
  __asm__ volatile (
    "pushfl\n\t"
    "pushfl\n\t"
    "popl %0\n\t"
    "movl %0, %1\n\t"
    "xorl $0x200000, %1\n\t"
    "pushl %1\n\t"
    "popfl\n\t"
    "pushfl\n\t"
    "popl %1\n\t"
    "popfl\n\t"
    : "=&r"(x), "=&r"(y));
  return ((x ^ y) & 0x200000u) != 0;
}
#endif

/*  On i386 -fPIC, EBX is the PIC base register so we xchg through a
    scratch instead of clobbering it.  */
static inline void fastent_cpuid(unsigned int leaf, unsigned int subleaf,
                                 unsigned int * a, unsigned int * b,
                                 unsigned int * c, unsigned int * d) {
#if defined(__i386__) && defined(__PIC__)
  __asm__ volatile (
    "xchgl %%ebx, %1\n\t"
    "cpuid\n\t"
    "xchgl %%ebx, %1"
    : "=a"(*a), "=r"(*b), "=c"(*c), "=d"(*d)
    : "0"(leaf), "2"(subleaf));
#else
  __asm__ volatile ("cpuid"
    : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
    : "0"(leaf), "2"(subleaf));
#endif
}

/*  Caller must gate on OSXSAVE (CPUID.1.ECX[27]) before invoking.
    Encoded as raw bytes so we don't need -mxsave.  */
static inline u64 fastent_xgetbv0(void) {
  unsigned int lo, hi;
  __asm__ volatile (".byte 0x0f, 0x01, 0xd0"
    : "=a"(lo), "=d"(hi) : "c"(0));
  return ((u64) hi << 32) | (u64) lo;
}

typedef struct {
  unsigned ssse3 : 1;
  unsigned sse41 : 1;
  unsigned sse42 : 1;
  unsigned avx : 1;
  unsigned avx2 : 1;
  unsigned avx512f : 1;
  unsigned avx512bw : 1;
  unsigned avx512bitalg : 1;
} fastent_x86_features;

static fastent_x86_features fastent_x86_probe(void) {
  fastent_x86_features f;
  unsigned int a = 0, b = 0, c = 0, d = 0;
  unsigned int max_leaf = 0;
  int osxsave = 0, avx_os_ok = 0, avx512_os_ok = 0;

  f.ssse3 = f.sse41 = f.sse42 = 0;
  f.avx = f.avx2 = 0;
  f.avx512f = f.avx512bw = f.avx512bitalg = 0;

  if (!fastent_x86_has_cpuid()) return f;
  fastent_cpuid(0, 0, &a, &b, &c, &d);
  max_leaf = a;
  if (max_leaf < 1) return f;

  fastent_cpuid(1, 0, &a, &b, &c, &d);
  f.ssse3 = !!(c & (1u <<  9));
  f.sse41 = !!(c & (1u << 19));
  f.sse42 = !!(c & (1u << 20));
  osxsave = !!(c & (1u << 27));
  /*  AVX needs both CPUID.1.ECX[28] and OS-enabled XCR0 state, else
      the CPU #UDs on AVX instructions.  */
  if (osxsave && (c & (1u << 28))) {
    u64 xcr0 = fastent_xgetbv0();
    if ((xcr0 & 0x6u) == 0x6u) {
      f.avx = 1;
      avx_os_ok = 1;
      /*  Bits 5,6,7 cover opmask, ZMM_Hi256, Hi16_ZMM for AVX-512.  */
      if ((xcr0 & 0xE0u) == 0xE0u) avx512_os_ok = 1;
    }
  }

  if (max_leaf < 7) return f;
  fastent_cpuid(7, 0, &a, &b, &c, &d);
  if (avx_os_ok && (b & (1u <<  5))) f.avx2 = 1;
  if (avx512_os_ok) {
    if (b & (1u << 16)) f.avx512f      = 1;
    if (b & (1u << 30)) f.avx512bw     = 1;
    if (c & (1u << 12)) f.avx512bitalg = 1;
  }
  return f;
}

/*  Cached on first picker call.  Probe is idempotent, so the
    pre-thread-spawn first-call race is harmless.  */
static fastent_x86_features fastent_x86_features_cache;
static int fastent_x86_features_done = 0;

static const fastent_x86_features * fastent_x86_features_get(void) {
  if (!fastent_x86_features_done) {
    fastent_x86_features_cache = fastent_x86_probe();
    fastent_x86_features_done = 1;
  }
  return &fastent_x86_features_cache;
}

#define FASTENT_X86_HAS(name) (fastent_x86_features_get()->name)

/*  Two tiers: base = F+BW (PSHUFB-LUT popcount); bitalg = base +
    BITALG (VPOPCNTB).  Dispatcher prefers bitalg.  */
#ifdef HAVE_AVX512
static int fastent_have_avx512_runtime(void) {
  const fastent_x86_features * f = fastent_x86_features_get();
  return f->avx512f && f->avx512bw;
}
#endif
#ifdef HAVE_AVX512_BITALG
static int fastent_have_avx512_bitalg_runtime(void) {
  const fastent_x86_features * f = fastent_x86_features_get();
  return f->avx512f && f->avx512bw && f->avx512bitalg;
}
#endif

#else

#define FASTENT_X86_HAS(name) 0
#ifdef HAVE_AVX512
static int fastent_have_avx512_runtime(void) { return 0; }
#endif
#ifdef HAVE_AVX512_BITALG
static int fastent_have_avx512_bitalg_runtime(void) { return 0; }
#endif

#endif

/*  WASM SIMD128 is module-load-gated; if the build linked -msimd128
    and the module instantiated, the feature is present.  */
#ifdef HAVE_WASM128
static inline int fastent_have_wasm128_runtime(void) { return 1; }
#endif

#ifdef HAVE_NEON
  #if defined(__aarch64__)
    /*  Mandatory on AArch64 per the procedure call standard.  */
    static inline int fastent_have_neon_runtime(void) { return 1; }
  #elif defined(__arm__) && defined(HAVE_SYS_AUXV_H) && defined(HAVE_GETAUXVAL)
    #include <sys/auxv.h>
    #ifndef HWCAP_NEON
      #define HWCAP_NEON (1u << 12)
    #endif
    static int fastent_have_neon_runtime(void) {
      return (getauxval(AT_HWCAP) & HWCAP_NEON) != 0;
    }
  #else
    static inline int fastent_have_neon_runtime(void) { return 1; }
  #endif
#endif

#ifdef HAVE_SVE2
  #if defined(__linux__) && defined(HAVE_SYS_AUXV_H) && defined(HAVE_GETAUXVAL)
    #include <sys/auxv.h>
    #ifndef HWCAP2_SVE2
      #define HWCAP2_SVE2 (1u << 1)
    #endif
    static int fastent_have_sve2_runtime(void) {
      return (getauxval(AT_HWCAP2) & HWCAP2_SVE2) != 0;
    }
  #elif defined(__FreeBSD__)
    #include <sys/auxv.h>
    #ifndef HWCAP2_SVE2
      #define HWCAP2_SVE2 (1u << 1)
    #endif
    static int fastent_have_sve2_runtime(void) {
      unsigned long hwcap2 = 0;
      if (elf_aux_info(AT_HWCAP2, &hwcap2, sizeof(hwcap2)) != 0) return 0;
      return (hwcap2 & HWCAP2_SVE2) != 0;
    }
  #elif defined(__APPLE__)
    #include <sys/sysctl.h>
    #include <stddef.h>
    static int fastent_have_sve2_runtime(void) {
      int    v   = 0;
      size_t len = sizeof(v);
      if (sysctlbyname("hw.optional.arm.FEAT_SVE2", &v, &len, NULL, 0) != 0)
        return 0;
      return v != 0;
    }
  #else
    static inline int fastent_have_sve2_runtime(void) { return 0; }
  #endif
#endif

fastent_analyze_fn fastent_pick_variant(fastent_variant * which) {
  fastent_variant v = FASTENT_VAR_SCALAR;
  fastent_analyze_fn fn = analyze_scalar;

  #ifdef HAVE_SSSE3
    if (FASTENT_X86_HAS(ssse3))         { v = FASTENT_VAR_SSSE3_;  fn = analyze_ssse3; }
  #endif
  #ifdef HAVE_SSE41
    if (FASTENT_X86_HAS(sse42))         { v = FASTENT_VAR_SSE41_;  fn = analyze_sse41; }
  #endif
  #ifdef HAVE_AVX2
    if (FASTENT_X86_HAS(avx2))          { v = FASTENT_VAR_AVX2_;   fn = analyze_avx2; }
  #endif
  #ifdef HAVE_AVX512
    if (fastent_have_avx512_runtime())         { v = FASTENT_VAR_AVX512_;       fn = analyze_avx512; }
  #endif
  #ifdef HAVE_AVX512_BITALG
    if (fastent_have_avx512_bitalg_runtime())  { v = FASTENT_VAR_AVX512_BITALG; fn = analyze_avx512_bitalg; }
  #endif
  #ifdef HAVE_NEON
    if (fastent_have_neon_runtime())           { v = FASTENT_VAR_NEON_;         fn = analyze_neon; }
  #endif
  #ifdef HAVE_SVE2
    if (fastent_have_sve2_runtime())           { v = FASTENT_VAR_SVE2_;         fn = analyze_sve2; }
  #endif
  #ifdef HAVE_WASM128
    if (fastent_have_wasm128_runtime())        { v = FASTENT_VAR_WASM128_;      fn = analyze_wasm128; }
  #endif

  if (which) *which = v;
  return fn;
}

const char * fastent_variant_name(fastent_variant v) {
  switch (v) {
    case FASTENT_VAR_AVX512_BITALG: return "avx512+bitalg";
    case FASTENT_VAR_AVX512_:       return "avx512";
    case FASTENT_VAR_AVX2_:         return "avx2";
    case FASTENT_VAR_SSE41_:        return "sse4.1";
    case FASTENT_VAR_SSSE3_:        return "ssse3";
    case FASTENT_VAR_SVE2_:         return "sve2";
    case FASTENT_VAR_NEON_:         return "neon";
    case FASTENT_VAR_WASM128_:      return "wasm-simd128";
    case FASTENT_VAR_SCALAR:        return "scalar";
  }
  return "scalar";
}

fastent_analyze_fn fastent_pick_bits_variant(fastent_variant * which) {
  fastent_variant v = FASTENT_VAR_SCALAR;
  fastent_analyze_fn fn = analyze_bits_scalar;

  #ifdef HAVE_SSSE3
    if (FASTENT_X86_HAS(ssse3))         { v = FASTENT_VAR_SSSE3_;  fn = analyze_bits_ssse3; }
  #endif
  #ifdef HAVE_SSE41
    if (FASTENT_X86_HAS(sse42))         { v = FASTENT_VAR_SSE41_;  fn = analyze_bits_sse41; }
  #endif
  #ifdef HAVE_AVX2
    if (FASTENT_X86_HAS(avx2))          { v = FASTENT_VAR_AVX2_;   fn = analyze_bits_avx2; }
  #endif
  #ifdef HAVE_AVX512
    if (fastent_have_avx512_runtime())         { v = FASTENT_VAR_AVX512_;       fn = analyze_bits_avx512; }
  #endif
  #ifdef HAVE_AVX512_BITALG
    if (fastent_have_avx512_bitalg_runtime())  { v = FASTENT_VAR_AVX512_BITALG; fn = analyze_bits_avx512_bitalg; }
  #endif
  #ifdef HAVE_NEON
    if (fastent_have_neon_runtime())           { v = FASTENT_VAR_NEON_;         fn = analyze_bits_neon; }
  #endif
  #ifdef HAVE_SVE2
    if (fastent_have_sve2_runtime())           { v = FASTENT_VAR_SVE2_;         fn = analyze_bits_sve2; }
  #endif
  #ifdef HAVE_WASM128
    if (fastent_have_wasm128_runtime())        { v = FASTENT_VAR_WASM128_;      fn = analyze_bits_wasm128; }
  #endif

  if (which) *which = v;
  return fn;
}

fastent_analyze_fn fastent_pick_fold_byte_variant(fastent_variant * which) {
  fastent_variant v = FASTENT_VAR_SCALAR;
  fastent_analyze_fn fn = analyze_fold_scalar;

  #ifdef HAVE_SSSE3
    if (FASTENT_X86_HAS(ssse3))         { v = FASTENT_VAR_SSSE3_;  fn = analyze_fold_ssse3; }
  #endif
  #ifdef HAVE_SSE41
    if (FASTENT_X86_HAS(sse42))         { v = FASTENT_VAR_SSE41_;  fn = analyze_fold_sse41; }
  #endif
  #ifdef HAVE_AVX2
    if (FASTENT_X86_HAS(avx2))          { v = FASTENT_VAR_AVX2_;   fn = analyze_fold_avx2; }
  #endif
  #ifdef HAVE_AVX512
    if (fastent_have_avx512_runtime())         { v = FASTENT_VAR_AVX512_;       fn = analyze_fold_avx512; }
  #endif
  #ifdef HAVE_AVX512_BITALG
    if (fastent_have_avx512_bitalg_runtime())  { v = FASTENT_VAR_AVX512_BITALG; fn = analyze_fold_avx512_bitalg; }
  #endif
  #ifdef HAVE_NEON
    if (fastent_have_neon_runtime())           { v = FASTENT_VAR_NEON_;         fn = analyze_fold_neon; }
  #endif
  #ifdef HAVE_SVE2
    if (fastent_have_sve2_runtime())           { v = FASTENT_VAR_SVE2_;         fn = analyze_fold_sve2; }
  #endif
  #ifdef HAVE_WASM128
    if (fastent_have_wasm128_runtime())        { v = FASTENT_VAR_WASM128_;      fn = analyze_fold_wasm128; }
  #endif

  if (which) *which = v;
  return fn;
}

fastent_analyze_fn fastent_pick_fold_bits_variant(fastent_variant * which) {
  fastent_variant v = FASTENT_VAR_SCALAR;
  fastent_analyze_fn fn = analyze_bits_fold_scalar;

  #ifdef HAVE_SSSE3
    if (FASTENT_X86_HAS(ssse3))         { v = FASTENT_VAR_SSSE3_;  fn = analyze_bits_fold_ssse3; }
  #endif
  #ifdef HAVE_SSE41
    if (FASTENT_X86_HAS(sse42))         { v = FASTENT_VAR_SSE41_;  fn = analyze_bits_fold_sse41; }
  #endif
  #ifdef HAVE_AVX2
    if (FASTENT_X86_HAS(avx2))          { v = FASTENT_VAR_AVX2_;   fn = analyze_bits_fold_avx2; }
  #endif
  #ifdef HAVE_AVX512
    if (fastent_have_avx512_runtime())         { v = FASTENT_VAR_AVX512_;       fn = analyze_bits_fold_avx512; }
  #endif
  #ifdef HAVE_AVX512_BITALG
    if (fastent_have_avx512_bitalg_runtime())  { v = FASTENT_VAR_AVX512_BITALG; fn = analyze_bits_fold_avx512_bitalg; }
  #endif
  #ifdef HAVE_NEON
    if (fastent_have_neon_runtime())           { v = FASTENT_VAR_NEON_;         fn = analyze_bits_fold_neon; }
  #endif
  #ifdef HAVE_SVE2
    if (fastent_have_sve2_runtime())           { v = FASTENT_VAR_SVE2_;         fn = analyze_bits_fold_sve2; }
  #endif
  #ifdef HAVE_WASM128
    if (fastent_have_wasm128_runtime())        { v = FASTENT_VAR_WASM128_;      fn = analyze_bits_fold_wasm128; }
  #endif

  if (which) *which = v;
  return fn;
}

fastent_fold_fn fastent_pick_fold_variant(fastent_variant * which) {
  fastent_variant v = FASTENT_VAR_SCALAR;
  fastent_fold_fn fn = fold_scalar;

  #ifdef HAVE_SSSE3
    if (FASTENT_X86_HAS(ssse3))         { v = FASTENT_VAR_SSSE3_;  fn = fold_ssse3; }
  #endif
  #ifdef HAVE_SSE41
    if (FASTENT_X86_HAS(sse42))         { v = FASTENT_VAR_SSE41_;  fn = fold_sse41; }
  #endif
  #ifdef HAVE_AVX2
    if (FASTENT_X86_HAS(avx2))          { v = FASTENT_VAR_AVX2_;   fn = fold_avx2; }
  #endif
  #ifdef HAVE_AVX512
    if (fastent_have_avx512_runtime())         { v = FASTENT_VAR_AVX512_;       fn = fold_avx512; }
  #endif
  #ifdef HAVE_AVX512_BITALG
    if (fastent_have_avx512_bitalg_runtime())  { v = FASTENT_VAR_AVX512_BITALG; fn = fold_avx512_bitalg; }
  #endif
  #ifdef HAVE_NEON
    if (fastent_have_neon_runtime())           { v = FASTENT_VAR_NEON_;         fn = fold_neon; }
  #endif
  #ifdef HAVE_SVE2
    if (fastent_have_sve2_runtime())           { v = FASTENT_VAR_SVE2_;         fn = fold_sve2; }
  #endif
  #ifdef HAVE_WASM128
    if (fastent_have_wasm128_runtime())        { v = FASTENT_VAR_WASM128_;      fn = fold_wasm128; }
  #endif

  if (which) *which = v;
  return fn;
}
