/*  fastent: the -ee level-2 extra pass.

    One scalar pass over the same bytes the SIMD analyser already
    consumed (order-0 keeps full SIMD throughput), folding every
    per-symbol level-2 reduction into a single scan:

      digram     : order-0 histogram of the 16-bit key (prev<<8)|cur
                   over FASTENT_BG_NB round-robin shadow tables, so
                   the store-to-load-forward dependency does not
                   serialise; the planes are summed at finalize.  Bit
                   mode uses the 2x2 bit_bigram.
      longest run: longest identical-symbol run (bytes or bits).
      runs       : number of 0/1 runs                (bit mode).
      cusum_max  : extent of the +-1 bit walk         (bit mode).

    Streams call it per chunk; with -j each worker scans its slab and
    run_mmap_mt_ merges the per-slab reductions with a boundary
    stitch, so the result is bit-identical for any thread count.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "analyze.h"

#include <string.h>

#if FASTENT_BG_NB < 2
#error "fastent_digram_count unrolls 2 shadow tables; FASTENT_BG_NB >= 2"
#endif

/*  -f scratch chunk; sized to stay L1/L2-resident across fold+scan.  */
#define FASTENT_DG_FOLD_CHUNK 32768

/*  Cached vectorised fold variant.  Pick is process-stable, so the
    lazy-init race stores the same pointer.  */
static fastent_fold_fn dg_fold_fn_(void) {
  static fastent_fold_fn ff = NULL;
  fastent_fold_fn f = ff;
  if (!f) { f = fastent_pick_fold_variant(NULL);  ff = f; }
  return f;
}

/*  Longest identical-symbol run; symbol is a byte or a bit.  */
static void lr_one_(fastent_chunk_state * st, unsigned s) {
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

static void digram_bytes_(fastent_chunk_state * st,
                          const u8 * FASTENT_RESTRICT buf, sz len) {
  u64 * FASTENT_RESTRICT t = st->bigram;
  unsigned prev;
  sz i = 0;

  if (st->dg_have) {
    prev = st->dg_prev;
  } else {
    /*  First byte of the stream: no left neighbour for a pair, but
        it is still a symbol for the longest-run scan.  */
    prev = buf[0];
    lr_one_(st, prev);
    i = 1;
    st->dg_have = 1;
  }

  for (; i + 2 <= len; i += 2) {
    unsigned c0 = buf[i], c1 = buf[i + 1];
    t[0u * FASTENT_BG_TABLE + ((prev << 8) | c0)]++;  lr_one_(st, c0);
    t[1u * FASTENT_BG_TABLE + ((c0   << 8) | c1)]++;  lr_one_(st, c1);
    prev = c1;
  }
  for (; i < len; i++) {
    unsigned c = buf[i];
    t[(prev << 8) | c]++;  lr_one_(st, c);
    prev = c;
  }
  st->dg_prev = (u8) prev;
}

/*  Bit mode: every adjacent bit pair (MSB->LSB, across byte and chunk
    boundaries via the carried previous bit) feeds the 2x2 bigram,
    longest run, 0/1 run count and the +-1 cusum walk.  dg_prev holds
    the previous bit (0/1) here.  */
static void digram_bits_(fastent_chunk_state * st,
                         const u8 * FASTENT_RESTRICT buf, sz len) {
  u64 (* bb)[2] = st->bit_bigram;
  sz i;
  for (i = 0; i < len; i++) {
    const unsigned byte = buf[i];
    int k;
    for (k = 7; k >= 0; k--) {
      const unsigned bit = (byte >> k) & 1u;
      if (st->dg_have) bb[st->dg_prev & 1u][bit]++;
      lr_one_(st, bit);
      if (!st->rn_have) { st->rn_have = 1;  st->rn_count = 1; }
      else if (bit != st->rn_last) st->rn_count++;
      st->rn_last  = (u8) bit;
      st->cs_sum  += bit ? 1 : -1;
      if (st->cs_sum < st->cs_min) st->cs_min = st->cs_sum;
      if (st->cs_sum > st->cs_max) st->cs_max = st->cs_sum;
      st->dg_prev  = (u8) bit;
      st->dg_have  = 1;
    }
  }
}

void fastent_digram_count(fastent_chunk_state * st, const u8 * buf,
                          sz len, int binary, int fold) {
  if (len == 0) return;
  if (!binary && !st->bigram) return;   /*  byte mode without -ee table  */

  if (!fold) {
    if (binary) digram_bits_(st, buf, len);
    else        digram_bytes_(st, buf, len);
    return;
  }

  /*  -f: prefold per chunk (mmap is read-only/shared, no in-place
      fold), then fold-free scan.  st->dg_* and the lr/rn/cs
      accumulators carry across chunks, so boundaries do not matter.  */
  fastent_fold_fn ff = dg_fold_fn_();
  u8 scratch[FASTENT_DG_FOLD_CHUNK];
  for (sz off = 0; off < len; off += FASTENT_DG_FOLD_CHUNK) {
    sz cl = len - off;
    if (cl > FASTENT_DG_FOLD_CHUNK) cl = FASTENT_DG_FOLD_CHUNK;
    memcpy(scratch, buf + off, cl);
    ff(scratch, cl);
    if (binary) digram_bits_(st, scratch, cl);
    else        digram_bytes_(st, scratch, cl);
  }
}
