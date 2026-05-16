/*  fastent: analysis state and per-variant entry points.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_ANALYZE_H
#define FASTENT_ANALYZE_H

#include "common.h"

#define FASTENT_BANKS  4
#define FASTENT_SIMD_STRIDE_AVX2 96 /*  16 hexads exactly  */
#define FASTENT_SIMD_STRIDE_SSE  48 /*   8 hexads exactly  */

/*  incirc = (256^3 - 1)^2 = 281474943156225  (fits in 49 bits).  */
#define FASTENT_INCIRC ((u64) 281474943156225ULL)

/*  Order-1 bigram accumulator (opt-in, -ee).  Counted as an order-0
    histogram of the 16-bit digram key ((prev<<8)|cur) over FASTENT_BG_NB
    round-robin shadow tables, so consecutive pairs land in different
    tables and the store-to-load-forward dependency does not serialise
    (the htscodecs hist1_4 technique); tables summed at finalize.  u64
    so a cell can reach N-1 (all-same-byte input) at any size.  */
#define FASTENT_BG_NB    4
#define FASTENT_BG_TABLE 65536
#define FASTENT_BG_CELLS (FASTENT_BG_NB * FASTENT_BG_TABLE)

/*  Per-thread chunk state. Accumulates histogram, SCC cross-product,
    Monte Carlo hits, and the first/last/carry bytes needed to stitch
    chunk boundaries at finalize.  */
typedef struct {
  /*  Banked histogram: bank[k][v] counts value v at stream positions
      p with (p mod FASTENT_BANKS) == k.  Merged at finalize.  */
  u32 bank[FASTENT_BANKS][256];

  /*  Sum of x[i] * x[i+1] over all bytes processed by this state so far,
      MINUS the wrap-around term (which is added globally at finalize).
      The SIMD body applies its sign correction internally before
      adding to this field, so it always holds the canonical unsigned
      cross-product sum.  */
  i64 cross_product;

  u64 total_bytes;
  u64 mc_count;
  u64 mc_inside;

  /*  Bit-mode hist (separate u64s because bank[] is u32 and would
      overflow for files > 512 MiB in bit mode).  */
  u64 bit_hist[2];

  /*  Cross-chunk state:  */
  u8  carry_byte;       /*  Most recent byte for SCC product carry  */
  u8  first_byte;       /*  Very first byte of the stream (this state)  */
  u8  last_byte;        /*  Most recent byte processed  */
  u8  have_carry;
  u8  have_first;

  /*  Monte Carlo 6-byte ring:  */
  u8  mc_buf[6];
  int mc_pos;

  /*  Order-1 digram: FASTENT_BG_CELLS u64 (NB flat 65536 tables),
      NULL unless -ee byte mode is active; allocated/freed by the
      runner.  bit_bigram[prev_bit][cur_bit] is the tiny bit-mode
      counterpart, always present.  dg_prev/dg_have carry the previous
      byte across fastent_digram_count calls (independent of the SCC
      carry, which the SIMD analyser owns).  */
  u64 * bigram;
  u64   bit_bigram[2][2];
  u8    dg_prev;
  u8    dg_have;

  /*  -ee level-2 sequential extras (runs / longest run / cusum),
      filled by fastent_runs_count; zeroed at init.  Symbols are bits
      in bit mode, byte values in byte mode.  Stream accumulates these
      across chunks in one state; mmap does a single serial scan of
      the whole buffer (no cross-slab stitch in this first cut).  */
  u64 lr_max;        /*  longest completed identical-symbol run  */
  u64 lr_cur;        /*  open run length                         */
  u8  lr_sym;        /*  open run symbol                          */
  u8  lr_have;
  u64 rn_count;      /*  bit-runs (bit mode)                      */
  u8  rn_last;       /*  most recent bit                          */
  u8  rn_have;
  i64 cs_sum;        /*  live +-1 walk position                   */
  i64 cs_min;        /*  min of the walk (<= 0)                   */
  i64 cs_max;        /*  max of the walk (>= 0)                   */
} fastent_chunk_state;

/*  Final reduced results.  */
typedef struct {
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
  int mode_value;           /*  most frequent symbol (-1 if none)  */
  u64 mode_count;
  int rarest_value;         /*  least frequent observed (-1 none)  */
  u64 rarest_count;
  f64 bit_freq[8];          /*  P(bit k=1), k=0 LSB..7 MSB; byte mode  */
  f64 bit_bias_max;         /*  max_k |bit_freq[k]-0.5|; NaN in bits  */
  int bit_bias_worst;       /*  argmax k of that bias; -1 in bit mode  */
  f64 conditional_entropy;  /*  H(cur|prev); NaN if no bigram / bits  */
  f64 mutual_information;   /*  I(prev;cur); NaN likewise  */
  f64 runs;                 /*  bit: 0/1 runs; byte: below/>=median
                                runs; NaN if not computed  */
  f64 longest_run;          /*  longest identical-symbol run (bits or
                                bytes); NaN if no samples  */
  f64 cusum_max;            /*  max |S|, +-1 bit walk; bit mode only  */

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
  FASTENT_VAR_SVE2_         = 8
} fastent_variant;

typedef void (* fastent_analyze_fn)(fastent_chunk_state *, const u8 *, sz);

void fastent_chunk_state_init(fastent_chunk_state * st);
/*  FASTENT_BG_CELLS u64, zeroed; NULL on OOM.  */
u64 * fastent_bigram_alloc(void);
void  fastent_bigram_free(u64 * bg);
void fastent_finalize(fastent_chunk_state * FASTENT_RESTRICT st, int binary,
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
void analyze_fold_avx512_bitalg(fastent_chunk_state * st,
                                const u8 * buf, sz len);
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

/*  Order-1 digram counter (opt-in, -ee).  Runs as its own tight pass
    over the same bytes the fast SIMD analyser just consumed, so the
    order-0 stats keep full SIMD throughput.  Byte mode fills the
    NB-table 64K histogram (st->bigram); bit mode fills bit_bigram.
    binary selects the mode; dg_prev/dg_have carry across calls.  */
void fastent_digram_count(fastent_chunk_state * st, const u8 * buf,
                          sz len, int binary);

/*  -ee sequential extras, same per-chunk pass model as the digram
    counter: longest identical-symbol run (both modes), 0/1 runs and
    the +-1 cusum walk (bit mode).  Byte runs-vs-median needs the
    median, a finalize quantity, so it is a separate post-finalize
    rescan; fastent_byte_runs_median counts below/>=median runs over
    a resident buffer (mmap only) and returns the run count.  */
void fastent_runs_count(fastent_chunk_state * st, const u8 * buf,
                        sz len, int binary);
u64 fastent_byte_runs_median(const u8 * buf, sz len, int median);

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
void analyze_bits_avx512_bitalg(fastent_chunk_state * st,
                                const u8 * buf, sz len);
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
void analyze_bits_fold_scalar(fastent_chunk_state * st, const u8 * buf, sz len);
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
void analyze_bits_fold_avx512(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_AVX512_BITALG
void analyze_bits_fold_avx512_bitalg(fastent_chunk_state * st,
                                     const u8 * buf, sz len);
#endif
#ifdef HAVE_NEON
void analyze_bits_fold_neon(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_SVE2
void analyze_bits_fold_sve2(fastent_chunk_state * st, const u8 * buf, sz len);
#endif
#ifdef HAVE_WASM128
void analyze_bits_fold_wasm128(fastent_chunk_state * st,
                               const u8 * buf, sz len);
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
