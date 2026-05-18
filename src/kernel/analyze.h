/*  fastent: analysis state and per-variant entry points.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_ANALYZE_H
#define FASTENT_ANALYZE_H

#include "common.h"

/*  Order-0 histogram shadow banks: power-of-two, per-ISA overridable
    (the &(FASTENT_BANKS-1) index masks are generic).  8 chosen by
    measurement on Zen 4: breaks more store-to-load-forward chains than
    4, the u32 banks are 8 KiB (L1-resident).  */
#ifndef FASTENT_BANKS
#define FASTENT_BANKS  8
#endif

/*  u32 order-0 drain bound.  Worst case all-same-byte: a bank cell
    gains <= 1 per byte, so after FASTENT_HIST_CHUNK bytes it holds
    < 2^30 < 2^32 (4x margin, any FASTENT_BANKS); 1 GiB <= 4 GB meets
    the once-per-4-GB ask.  Mirrors the proven dg_u32 bound.  */
#define FASTENT_HIST_CHUNK ((u64) (1u << 30))

#define FASTENT_SIMD_STRIDE_AVX2 96 /*  16 hexads exactly  */
#define FASTENT_SIMD_STRIDE_SSE  48 /*   8 hexads exactly  */

/*  incirc = (256^3 - 1)^2 = 281474943156225  (fits in 49 bits).  */
#define FASTENT_INCIRC ((u64) 281474943156225ULL)

/*  Order-1 bigram accumulator (-ee).  Order-0 histogram of key
    ((prev<<8)|cur) over FASTENT_BG_NB round-robin shadows to break
    store-to-load-forward, summed at finalize; 2 planes (1 MiB) fit L2.
    u64 so a cell reaches N-1 (all-same-byte) at any size.  */
#define FASTENT_BG_NB    2
#define FASTENT_BG_TABLE 65536
#define FASTENT_BG_CELLS (FASTENT_BG_NB * FASTENT_BG_TABLE)

/*  Drain the u32 digram shadow into the u64 master every this many
    bytes.  (1u<<31) - 65536 leaves headroom below 2^31 so a single
    u32 cell (< pairs seen < bytes seen) cannot wrap before a drain.  */
#define FASTENT_DG_U32_CHUNK ((u64)((1u << 31) - 65536u))

/*  Scalar case fold: ASCII A-Z and Latin-1 0xC0-0xDE (not 0xD7) to
    lower-case, other bytes unchanged. Same rule as the SIMD fold.  */
static inline u8 fastent_fold_byte(u8 b) {
  u32 c = b;
  if (((u32)(c - 'A') < 26u) ||
      ((u32)(c - 0xC0u) < 31u && c != 0xD7u))
    return (u8)(c + 0x20u);
  return b;
}

/*  Per-thread chunk state. Accumulates histogram, SCC cross-product,
    Monte Carlo hits, and the first/last/carry bytes needed to stitch
    chunk boundaries at finalize.  */
typedef struct {
  /*  Order-0 histogram: bank[k][v] is the hot L1 u32 working counter
      (k = pos mod FASTENT_BANKS, breaks store-to-load-forward).
      hist_master[v] is the authoritative u64; banks widen-drain in
      every FASTENT_HIST_CHUNK bytes + at finalize, so no cell wraps.  */
  u32 bank[FASTENT_BANKS][256];
  u64 hist_master[256];
  u64 hist_chunk_bytes;

  /*  Sum of x[i] * x[i+1] over bytes seen so far, MINUS the wrap term
      (added globally at finalize).  The SIMD body applies its sign
      correction before adding, so this holds the canonical unsigned
      cross-product sum.  */
  i64 cross_product;

  u64 total_bytes;
  u64 mc_count;
  u64 mc_inside;

  /*  Bit-mode hist: own u64s, summed directly (no banking; only two
      cells, drained at finalize is unnecessary).  */
  u64 bit_hist[2];

  /*  Cross-chunk state:  */
  u8  carry_byte;       /*  Most recent byte for SCC product carry  */
  u8  first_byte;       /*  Very first byte of the stream (this state)  */
  u8  last_byte;        /*  Most recent byte processed  */
  u8  have_carry;
  u8  have_first;

  /*  Monte Carlo 6-byte ring:  */
  u8  mc_buf[6];
  i32 mc_pos;

  /*  Order-1 digram: FASTENT_BG_CELLS u64, NULL unless -ee byte mode.
      bit_bigram is the always-present bit-mode counterpart.  dg_prev/
      dg_have carry the previous byte across fastent_digram_count
      calls (independent of the SCC carry).  */
  u64 * bigram;
  u64   bit_bigram[2][2];
  u8    dg_prev;
  u8    dg_have;

  /*  u32 chunk shadow of `bigram` (half the u64 hot set), drained
      every FASTENT_DG_U32_CHUNK bytes and before any merge.  Chunk
      B<=2^31 has <B pairs so no u32 cell wraps; the drain is
      associativity-only, so bigram[] is bit-identical for any -j.  */
  u32 * dg_u32;
  u64   dg_chunk_bytes;

  /*  -ee level-2 sequential extras (runs / longest run / cusum).
      Symbols are bits or byte values.  Streams accumulate across
      chunks; with -j slabs merge via a boundary stitch (run_mmap_mt_),
      lr_head_* recording the leading run so straddlers splice.  */
  u64 lr_max;        /*  longest completed identical-symbol run  */
  u64 lr_cur;        /*  open run length                         */
  u8  lr_sym;        /*  open run symbol                          */
  u8  lr_have;
  u64 lr_head_len;   /*  length of this slab's leading run        */
  u8  lr_head_sym;   /*  symbol of that leading run               */
  u8  lr_head_open;  /*  set while the leading run is still the
                         only run (whole slab is one run)         */
  u64 rn_count;      /*  bit-runs (bit mode)                      */
  u8  rn_last;       /*  most recent bit                          */
  u8  rn_have;
  i64 cs_sum;        /*  live +-1 walk position                   */
  i64 cs_min;        /*  min of the walk (<= 0)                   */
  i64 cs_max;        /*  max of the walk (>= 0)                   */
} fastent_chunk_state;

/*  scc sentinel for a zero SCC denominator (undefined serial
    correlation).  A real coefficient is in [-1,1] so anything past
    the threshold is the sentinel; renderers test FASTENT_SCC_DEFINED.  */
#define FASTENT_SCC_UNDEF      (-100000.0)
#define FASTENT_SCC_DEFINED(s) ((s) > -99999.0)

/*  LZ77F (-eee) raw tables; defined in lzest.h, only a pointer here.  */
struct fastent_lz77f_tables;

/*  Final reduced results.  */
typedef struct fastent_result {
  u64 total_samples;
  f64 entropy;
  f64 chi_square;
  f64 chi_probability;
  f64 mean;
  f64 monte_pi;
  f64 scc;

  /*  Extended stats (always computed; surfaced only under -e).  */
  f64 min_entropy;          /*  H_inf = -log2(max p_i), per sample  */
  f64 collision_entropy;    /*  H_2 = -log2(sum p_i^2), per sample  */
  f64 ic;                   /*  index of coincidence  */
  f64 poker_chisq;          /*  16-bin nibble chi-square; NaN in bits  */
  f64 poker_p;              /*  upper tail, df=15; NaN in bit mode  */
  f64 variance;             /*  of sample value  */
  f64 stddev;
  f64 redundancy;           /*  1 - H/Hmax  */
  u32 distinct;             /*  distinct symbols observed  */
  i32 mode_value;           /*  most frequent symbol (-1 if none)  */
  u64 mode_count;
  i32 rarest_value;         /*  least frequent observed (-1 none)  */
  u64 rarest_count;
  f64 bit_freq[8];          /*  P(bit k=1), k=0 LSB..7 MSB; byte mode  */
  f64 bit_bias_max;         /*  max_k |bit_freq[k]-0.5|; NaN in bits  */
  i32 bit_bias_worst;       /*  argmax k of that bias; -1 in bit mode  */
  f64 conditional_entropy;  /*  H(cur|prev); NaN if no bigram / bits  */
  f64 mutual_information;   /*  I(prev;cur); NaN likewise  */
  f64 runs;                 /*  bit: 0/1 runs; byte: below/>=median
                                runs; NaN if not computed  */
  f64 longest_run;          /*  longest identical-symbol run (bits or
                                bytes); NaN if no samples  */
  f64 cusum_max;            /*  max |S|, +-1 bit walk; bit mode only  */

  /*  LZ77F estimator (-eee).  NaN sentinel unless extended >= 3.  lz
      holds the 3 raw tables, heap-allocated only under -eee and freed
      per result so the recursive row struct stays small.  */
  f64 lz_cr_excess;         /*  S1 max(0,(outsz_rand-B)/n)  */
  f64 lz_lit_h;             /*  S2 H_lit (literal byte entropy, bits)  */
  f64 lz_lit_kl;            /*  S2 8 - H_lit (= KL to uniform)  */
  f64 lz_match_cov;         /*  S3 1 - lit_bytes/n  */
  f64 lz_off_conc;          /*  1 - H(off)/log2(65535); 0 if nmatch<2  */
  f64 lz_mlen_excess;       /*  mean match len - 4; 0 if nmatch<2  */
  f64 lz_lit_chi;           /*  literal chi-square vs uniform, df=255  */
  f64 lz_lit_chi_p;         /*  its upper-tail p (advisory companion)  */
  f64 lz_deviation;         /*  headline z = max(S1,S2/8,S3)/sigma0  */
  u64 lz_nmatch;            /*  total LZ77 matches  */
  i32 lz_megamatch;         /*  1 = single dominant match (mega note)  */
  struct fastent_lz77f_tables * lz;  /*  3 raw tables; NULL unless -eee */

  u64 hist[256];            /*  -c output; bits: hist[0]/hist[1]  */
} fastent_result;

/*  Dispatch flags returned by fastent_pick_variant().  */
typedef enum {
  FASTENT_VAR_SCALAR        = 0,
  FASTENT_VAR_SSSE3_        = 1,
  FASTENT_VAR_SSE41_        = 2,
  FASTENT_VAR_AVX2_         = 3,
  FASTENT_VAR_AVX512_       = 4,
  FASTENT_VAR_AVX512_BITALG = 5,
  FASTENT_VAR_NEON_         = 6,
  FASTENT_VAR_WASM128_      = 7,
  FASTENT_VAR_SVE2_         = 8,
  /*  FIPS-only extra tier (no analyze body): AVX-512 + VPOPCNTDQ
      (wide VPOPCNTQ monobit and run-start popcount).  The FIPS
      dispatcher carries its own name strings; this value only needs
      to be distinct.  */
  FASTENT_VAR_AVX512_VPOPCNTDQ = 9
} fastent_variant;

typedef void (* fastent_analyze_fn)(fastent_chunk_state *, const u8 *, sz);

void fastent_chunk_state_init(fastent_chunk_state * st);
/*  FASTENT_BG_CELLS u64, zeroed; NULL on OOM.  */
u64 * fastent_bigram_alloc(void);
void  fastent_bigram_free(u64 * bg);
/*  FASTENT_BG_CELLS u32 chunk shadow, zeroed; NULL on OOM.  */
u32 * fastent_dg_u32_alloc(void);
void  fastent_dg_u32_free(u32 * s);
/*  Stream the u32 chunk shadow into st->bigram (add + zero) and
    reset st->dg_chunk_bytes.  No-op if either table is absent.  */
void  fastent_dg_drain(fastent_chunk_state * st);
/*  Widen-add the u32 order-0 banks into st->hist_master, zero the
    banks, reset st->hist_chunk_bytes.  Fixed bank/value order so the
    u64 sum is order-independent: flush cadence (any -j, any driver)
    cannot change the result.  */
void  fastent_hist_flush_(fastent_chunk_state * st);
void fastent_finalize(
    fastent_chunk_state * FASTENT_RESTRICT st, int binary,
    fastent_result * FASTENT_RESTRICT out);

/*  Variant entries (always declared; analyze.c picks one at runtime).  */
void analyze_scalar(fastent_chunk_state * st, const u8 * buf, sz len);
#ifdef HAVE_SSSE3
void analyze_ssse3(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_SSE41
void analyze_sse41(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX2
void analyze_avx2(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX512
void analyze_avx512(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX512_BITALG
void analyze_avx512_bitalg(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_NEON
void analyze_neon(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_SVE2
void analyze_sve2(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_WASM128
void analyze_wasm128(fastent_chunk_state * st, const u8 * buf, sz len);
#endif

/*  Fused fold+analyse entries: same SIMD body as analyze_<variant> but
    each loaded vector is case-folded in-register before histogram /
    SCC / MC Pi consume it.  Drops the 32 KiB staging-copy pass used
    by the old fold_then_analyze_slab path.  */
void analyze_fold_scalar(fastent_chunk_state * st, const u8 * buf, sz len);
#ifdef HAVE_SSSE3
void analyze_fold_ssse3(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_SSE41
void analyze_fold_sse41(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX2
void analyze_fold_avx2(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX512
void analyze_fold_avx512(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX512_BITALG
void analyze_fold_avx512_bitalg(
    fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_NEON
void analyze_fold_neon(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_SVE2
void analyze_fold_sve2(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_WASM128
void analyze_fold_wasm128(fastent_chunk_state * st, const u8 * buf, sz len);
#endif

/*  -ee level-2 pass: one scalar scan folding digram histogram, longest
    run, 0/1 runs and the cusum walk.  Byte mode fills st->bigram, bit
    mode bit_bigram; dg_prev carries across calls.  Byte runs-vs-median
    derived in fastent_finalize from digram counts.  fold matches -f.  */
void fastent_digram_count(
    fastent_chunk_state * st, const u8 * buf, sz len, int binary, int fold);

/*  Longest-run state machine, shared by the bit-mode scan and the
    byte digram kernels (scalar reference and SIMD variants).
    fastent_lr_run(st, q, n) == n calls of fastent_lr_one(st, q).  */
void fastent_lr_one(fastent_chunk_state * st, u32 s);
void fastent_lr_run(fastent_chunk_state * st, u32 q, u64 n);

/*  Byte-mode digram + longest-run kernel, one per ISA.  Each threads
    dg_u32 / longest-run / dg_prev state as the scalar reference, so
    the merged result is byte-identical for any -j.  The always-built
    scalar variant is the reference; SIMD must match it bit-for-bit.  */
typedef void (* fastent_digram_byte_fn)(fastent_chunk_state *,
                                        const u8 *, sz);
void digram_bytes_scalar(fastent_chunk_state * st, const u8 * buf, sz len);
#ifdef HAVE_SSSE3
void digram_bytes_ssse3(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_SSE41
void digram_bytes_sse41(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX2
void digram_bytes_avx2(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX512
void digram_bytes_avx512(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX512_BITALG
void digram_bytes_avx512_bitalg(
    fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_NEON
void digram_bytes_neon(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_SVE2
void digram_bytes_sve2(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_WASM128
void digram_bytes_wasm128(fastent_chunk_state * st, const u8 * buf, sz len);
#endif

fastent_digram_byte_fn fastent_pick_digram_byte_variant(fastent_variant * w);

/*  Bit-mode -ee level-2 fused block kernel (B0 pack + B1/B2 digram +
    B3 closed-form longest run + B4 cusum LUT), one per ISA.  Scalar
    variant is the reference; SIMD reproduces its counters bit-for-bit
    for any -j.  cl <= 64 KiB; state threads across calls.  */
typedef void (* fastent_digram_bits_fn)(fastent_chunk_state *,
                                        const u8 *, sz,
                                        const i32 *, const i32 *,
                                        const i32 *);
void digram_bits_blk_scalar(
    fastent_chunk_state * st, const u8 * buf, sz cl, const i32 *,
    const i32 *, const i32 *);
#ifdef HAVE_SSSE3
void digram_bits_blk_ssse3(
    fastent_chunk_state * st, const u8 * buf, sz cl, const i32 *,
    const i32 *, const i32 *);
#endif
#ifdef HAVE_SSE41
void digram_bits_blk_sse41(
    fastent_chunk_state * st, const u8 * buf, sz cl, const i32 *,
    const i32 *, const i32 *);
#endif
#ifdef HAVE_AVX2
void digram_bits_blk_avx2(
    fastent_chunk_state * st, const u8 * buf, sz cl, const i32 *,
    const i32 *, const i32 *);
#endif
#ifdef HAVE_AVX512
void digram_bits_blk_avx512(
    fastent_chunk_state * st, const u8 * buf, sz cl, const i32 *,
    const i32 *, const i32 *);
#endif
#ifdef HAVE_AVX512_BITALG
void digram_bits_blk_avx512_bitalg(
    fastent_chunk_state * st, const u8 * buf, sz cl, const i32 *,
    const i32 *, const i32 *);
#endif
#ifdef HAVE_NEON
void digram_bits_blk_neon(
    fastent_chunk_state * st, const u8 * buf, sz cl, const i32 *,
    const i32 *, const i32 *);
#endif
#ifdef HAVE_WASM128
void digram_bits_blk_wasm128(
    fastent_chunk_state * st, const u8 * buf, sz cl, const i32 *,
    const i32 *, const i32 *);
#endif
fastent_digram_bits_fn fastent_pick_digram_bits_variant(fastent_variant * w);

fastent_analyze_fn fastent_pick_variant(fastent_variant * which);
fastent_analyze_fn fastent_pick_fold_byte_variant(fastent_variant * which);
const char *   fastent_variant_name(fastent_variant v);

/*  Bit-mode analysers. The scalar version is the fallback; SIMD versions
    are emitted from analyze-impl.h alongside the byte-mode SIMD body.  */
void analyze_bits_scalar(fastent_chunk_state * st, const u8 * buf, sz len);
#ifdef HAVE_SSSE3
void analyze_bits_ssse3(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_SSE41
void analyze_bits_sse41(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX2
void analyze_bits_avx2(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX512
void analyze_bits_avx512(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX512_BITALG
void analyze_bits_avx512_bitalg(
    fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_NEON
void analyze_bits_neon(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_SVE2
void analyze_bits_sve2(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_WASM128
void analyze_bits_wasm128(fastent_chunk_state * st, const u8 * buf, sz len);
#endif

/*  Fused fold + bit-mode analysers.  */
void analyze_bits_fold_scalar(
    fastent_chunk_state * st, const u8 * buf, sz len);
#ifdef HAVE_SSSE3
void analyze_bits_fold_ssse3(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_SSE41
void analyze_bits_fold_sse41(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX2
void analyze_bits_fold_avx2(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX512
void analyze_bits_fold_avx512(
    fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX512_BITALG
void analyze_bits_fold_avx512_bitalg(
    fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_NEON
void analyze_bits_fold_neon(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_SVE2
void analyze_bits_fold_sve2(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_WASM128
void analyze_bits_fold_wasm128(
    fastent_chunk_state * st, const u8 * buf, sz len);
#endif

fastent_analyze_fn fastent_pick_bits_variant(fastent_variant * which);
fastent_analyze_fn fastent_pick_fold_bits_variant(fastent_variant * which);

/*  In-place ASCII + Latin-1 upper -> lower case fold. One variant per
    ISA, picked at startup.  The scalar variant is also the canonical
    fallback for non-SIMD builds.  */
typedef void (* fastent_fold_fn)(u8 * buf, sz len);
void fold_scalar(u8 * buf, sz len);
#ifdef HAVE_SSSE3
void fold_ssse3(u8 * buf, sz len);
#endif
#ifdef HAVE_SSE41
void fold_sse41(u8 * buf, sz len);
#endif
#ifdef HAVE_AVX2
void fold_avx2(u8 * buf, sz len);
#endif
#ifdef HAVE_AVX512
void fold_avx512(u8 * buf, sz len);
#endif
#ifdef HAVE_AVX512_BITALG
void fold_avx512_bitalg(u8 * buf, sz len);
#endif
#ifdef HAVE_NEON
void fold_neon(u8 * buf, sz len);
#endif
#ifdef HAVE_SVE2
void fold_sve2(u8 * buf, sz len);
#endif
#ifdef HAVE_WASM128
void fold_wasm128(u8 * buf, sz len);
#endif

fastent_fold_fn fastent_pick_fold_variant(fastent_variant * which);

#endif
