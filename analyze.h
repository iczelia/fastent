/*  fastent  --  analysis state and per-variant entry points.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_ANALYZE_H
#define FASTENT_ANALYZE_H

#include "common.h"

#define FASTENT_BANKS  4
#define FASTENT_SIMD_STRIDE_AVX2 96 /*  16 hexads exactly  */
#define FASTENT_SIMD_STRIDE_SSE  48 /*   8 hexads exactly  */

/*  incirc = (256^3 - 1)^2 = 281474943156225  (fits in 49 bits).  */
#define FASTENT_INCIRC ((u64) 281474943156225ULL)

/*  Per-thread chunk state. Accumulates histogram, SCC cross-product,
    Monte Carlo hits, and the first/last/carry bytes needed to stitch
    chunk boundaries at finalize.  */
typedef struct {
  /*  Banked histogram: bank[k][v] counts bytes of value v at stream
      positions p with (p mod FASTENT_BANKS) == k.  Banks merged at finalize.  */
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

  /*  Bookkeeping for the SIMD body's "position mod 4" bank rotation
      across multiple analyze() calls on the same state.  */
  u64 stream_pos;
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
  u64 hist[256];     /*  For -c output. In bit mode, hist[0]/hist[1] are bit counts.  */
} fastent_result;

/*  Dispatch flags returned by fastent_pick_variant().  */
typedef enum {
  FASTENT_VAR_SCALAR = 0,
  FASTENT_VAR_SSSE3_  = 1,
  FASTENT_VAR_SSE41_  = 2,
  FASTENT_VAR_AVX2_   = 3
} fastent_variant;

typedef void (* fastent_analyze_fn)(fastent_chunk_state *, const u8 *, sz);

void fastent_chunk_state_init(fastent_chunk_state * st);
void fastent_chunk_state_merge(fastent_chunk_state * dst, const fastent_chunk_state * src,
                           u8 stitch_first_byte, int has_next);
void fastent_finalize(fastent_chunk_state * st, int binary, fastent_result * out);

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

fastent_analyze_fn fastent_pick_variant(fastent_variant * which);
const char *   fastent_variant_name(fastent_variant v);

/*  Bit-mode analyser. Always scalar; bit mode is rare and not perf-critical.
    Updates a bit-mode equivalent state structure.  */
void analyze_bits_scalar(fastent_chunk_state * st, const u8 * buf, sz len);

#endif
