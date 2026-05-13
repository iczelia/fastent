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

/*  Merge `src` into `dst`. Used for joining per-thread states.
    `stitch_first_byte` is the first byte of `src`'s region.
    `has_next` indicates if there's another chunk after `src`.  */
void fastent_chunk_state_merge(fastent_chunk_state * dst, const fastent_chunk_state * src,
                           u8 stitch_first_byte, int has_next) {
  /*  Sum banks.  */
  Fi(FASTENT_BANKS, Fj(256, dst->bank[i][j] += src->bank[i][j]))

  /*  Cross-chunk SCC product. dst's last byte (== dst->carry_byte) times
      src's first byte (== stitch_first_byte). Only valid if dst has any
      previously processed bytes.  */
  if (dst->have_carry && src->have_first) {
    dst->cross_product += (i64) dst->carry_byte * (i64) stitch_first_byte;
  }
  dst->cross_product += src->cross_product;

  dst->total_bytes += src->total_bytes;

  if (!dst->have_first && src->have_first) {
    dst->first_byte = src->first_byte;
    dst->have_first = 1;
  }
  if (src->total_bytes > 0) {
    dst->last_byte = src->last_byte;
    dst->carry_byte = src->last_byte;
    dst->have_carry = 1;
  }

  /*  MC Pi: walk src's pre-existing ring drain + bulk bytes that fell
      within src's processed range. Easiest is to take the union of the
      mc_inside / mc_count counters and re-run the boundary fixup via
      the dst's existing ring.

      Since each per-thread state runs its own analyze() pass, its
      mc_buf[0..mc_pos-1] holds bytes that were trailing within its
      slab (didn't complete a hexad). To stitch:
        1) Concatenate dst's trailing mc_buf with src's leading
           "bytes that would have completed a hexad were it not for
           the slab boundary" -- but those leading bytes ALREADY went
           into src's ring. Specifically, if src's ring had a
           non-empty leading fill at start (which it didn't because
           it started empty), those bytes went into src's mc_pos.
        2) The simpler model: when we merge, the only bytes we still
           need to resolve are dst's trailing ring + the bytes that
           were used by src as if its ring started empty.

      To keep the merge simple, we require that callers feed states
      one slab at a time in order, and that we use a stitching helper
      that takes the boundary byte sequence explicitly. See
      fastent_thread_stitch_mc() (callers in fastent.c).

      Here we just accumulate the inside/count counters and copy
      src's trailing ring into dst.  */
  (void) has_next;
  dst->mc_count  += src->mc_count;
  dst->mc_inside += src->mc_inside;
  if (src->mc_pos > 0) {
    memcpy(dst->mc_buf, src->mc_buf, sizeof(dst->mc_buf));
    dst->mc_pos = src->mc_pos;
  }
}

/*  ----------------------------------------------------------------------
    Final reduction: turn fastent_chunk_state into fastent_result.

    For byte mode: histogram bins are 0..255.
    For bit mode: state was populated by analyze_bits_scalar; bank[0][0]
    is the count of 0-bits, bank[0][1] the count of 1-bits.  */

void fastent_finalize(fastent_chunk_state * st, int binary, fastent_result * out) {
  memset(out, 0, sizeof(*out));

  /*  Add the wrap-around SCC term.  */
  if (st->have_first && st->have_carry) {
    st->cross_product += (i64) st->last_byte * (i64) st->first_byte;
  }

  /*  Merge banks into final histogram.  */
  if (binary) {
    out->hist[0] = st->bit_hist[0];
    out->hist[1] = st->bit_hist[1];
    out->total_samples = st->total_bytes;  /*  bits  */
  } else {
    Fi(256,
       out->hist[i] = (u64) st->bank[0][i] + st->bank[1][i]
                    + st->bank[2][i] + st->bank[3][i])
    out->total_samples = st->total_bytes;  /*  bytes  */
  }

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
  if (denom == 0.0) {
    out->scc = -100000.0;
  } else {
    out->scc = (totalc * scct1 - scct2_sq) / denom;
  }

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
  out->chi_square = chisq;
  out->entropy = entropy;

  /*  Chi-square tail probability.  fabs() clears the sign of any NaN
      result so a tail probability of "nan" still prints positively.  */
  out->chi_probability = fabs(pochisq(chisq, binary ? 1 : 255));

  /*  Monte Carlo Pi.  */
  out->monte_pi = 4.0 * ((f64) st->mc_inside / (f64) st->mc_count);
}

/*  ----------------------------------------------------------------------
    Runtime variant pick.

    Compile-time HAVE_AVX2/HAVE_SSE41/HAVE_SSSE3 means the variant TU was
    built; we still confirm at runtime via __builtin_cpu_supports so a
    binary built on a wider host can run on a narrower one.  */

fastent_analyze_fn fastent_pick_variant(fastent_variant * which) {
  fastent_variant v = FASTENT_VAR_SCALAR;
  fastent_analyze_fn fn = analyze_scalar;

#if defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();

  #ifdef HAVE_SSSE3
    if (__builtin_cpu_supports("ssse3")) {
      v = FASTENT_VAR_SSSE3_;
      fn = analyze_ssse3;
    }
  #endif
  #ifdef HAVE_SSE41
    if (__builtin_cpu_supports("sse4.1")) {
      v = FASTENT_VAR_SSE41_;
      fn = analyze_sse41;
    }
  #endif
  #ifdef HAVE_AVX2
    if (__builtin_cpu_supports("avx2")) {
      v = FASTENT_VAR_AVX2_;
      fn = analyze_avx2;
    }
  #endif
#endif

  if (which) *which = v;
  return fn;
}

const char * fastent_variant_name(fastent_variant v) {
  switch (v) {
    case FASTENT_VAR_AVX2_:   return "avx2";
    case FASTENT_VAR_SSE41_:  return "sse4.1";
    case FASTENT_VAR_SSSE3_:  return "ssse3";
    case FASTENT_VAR_SCALAR: return "scalar";
  }
  return "scalar";
}

/*  ----------------------------------------------------------------------
    Bit-mode analyser (scalar). Each input byte produces 8 bit samples,
    MSB-first. The histogram has 2 bins (0 and 1). The SCC cross-product
    counts adjacent (1, 1) pairs in the bit stream. MC Pi is still
    byte-driven (one trial per 6 input bytes, just like byte mode).  */

void analyze_bits_scalar(fastent_chunk_state * st, const u8 * buf, sz len) {
  if (len == 0) return;

  /*  For SCC in bit mode, we need:
        - count of (1,1) adjacent bit pairs across the whole bit stream
        - first bit (= MSB of first byte)
        - last bit  (= LSB of last byte)
      Cross product = sum b[i]*b[i+1] in the bit stream (no wrap yet).
      Within-byte pair count for byte b: popcount(b & (b >> 1)).
      Cross-byte pair: (prev_LSB & curr_MSB).
      Histogram updates: hist[1] += popcount(byte), hist[0] += 8-popcount.  */

  for (sz i = 0; i < len; i++) {
    const u8 byte = buf[i];

    /*  Bit histogram in u64 (bank[] is u32; would overflow > 500 MiB).  */
    const unsigned ones_in_byte = (unsigned) __builtin_popcount(byte);
    st->bit_hist[1] += ones_in_byte;
    st->bit_hist[0] += 8u - ones_in_byte;

    /*  Within-byte (1,1) adjacent pairs.  */
    const unsigned within = (unsigned) __builtin_popcount(byte & (byte >> 1));
    st->cross_product += (i64) within;

    /*  Cross-byte pair.  */
    if (st->have_carry) {
      const unsigned prev_lsb  = (unsigned)(st->carry_byte & 1u);
      const unsigned curr_msb  = (unsigned)((byte >> 7) & 1u);
      st->cross_product += (i64)(prev_lsb & curr_msb);
    } else {
      /*  First byte ever: record first bit (MSB).  */
      st->first_byte = (u8)((byte >> 7) & 1u);  /*  store the bit value  */
      st->have_first = 1;
    }
    st->carry_byte = byte;
    st->last_byte  = (u8)(byte & 1u);  /*  last bit value  */
    st->have_carry = 1;
    st->total_bytes += 8;

    /*  Monte Carlo Pi (byte-driven, same as byte mode).  */
    st->mc_buf[st->mc_pos++] = byte;
    if (st->mc_pos >= 6) {
      const u32 x = ((u32) st->mc_buf[0] << 16) | ((u32) st->mc_buf[1] << 8)
                  |  (u32) st->mc_buf[2];
      const u32 y = ((u32) st->mc_buf[3] << 16) | ((u32) st->mc_buf[4] << 8)
                  |  (u32) st->mc_buf[5];
      const u64 d = (u64) x * (u64) x + (u64) y * (u64) y;
      st->mc_count++;
      st->mc_inside += (d <= FASTENT_INCIRC);
      st->mc_pos = 0;
    }
  }
}
