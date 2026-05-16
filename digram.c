/*  fastent: the -ee level-2 extra pass.

    One scalar pass over the same bytes the fast SIMD analyser already
    consumed, so the order-0 statistics keep full SIMD throughput.  It
    folds every per-symbol level-2 reduction into a single scan:

      digram     : order-0 histogram of the 16-bit key (prev<<8)|cur
                   over FASTENT_BG_NB round-robin shadow tables, so NB
                   consecutive pairs land in independent tables and
                   the store-to-load-forward dependency does not
                   serialise (the htscodecs hist1_4 technique).  Bit
                   mode uses the 2x2 bit_bigram.
      longest run: longest identical-symbol run (bytes or bits).
      runs       : number of 0/1 runs                (bit mode).
      cusum_max  : extent of the +-1 bit walk         (bit mode).

    Stream calls this per chunk on one persistent state.  Single-
    thread mmap calls it once over the whole resident buffer.  With
    -j each worker calls it on its own slab and run_mmap_mt_ merges
    the per-slab reductions with a boundary stitch, so the result is
    bit-identical regardless of thread count.  Byte runs-vs-median is
    then derived in fastent_finalize from the digram joint counts (no
    rescan).

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "analyze.h"

#if FASTENT_BG_NB < 4
#error "fastent_digram_count unrolls 4 shadow tables; FASTENT_BG_NB >= 4"
#endif

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
                          const u8 * FASTENT_RESTRICT buf, sz len,
                          int fold) {
  u64 * FASTENT_RESTRICT t = st->bigram;
  unsigned prev;
  sz i = 0;

  /*  fold is a compile-time constant at both call sites, so the
      conditional dead-eliminates per trampoline.  */
  #define DG_LD(IDX) (fold ? fastent_fold_byte(buf[IDX]) : buf[IDX])

  if (st->dg_have) {
    prev = st->dg_prev;
  } else {
    /*  First byte of the stream: no left neighbour for a pair, but
        it is still a symbol for the longest-run scan.  */
    prev = DG_LD(0);
    lr_one_(st, prev);
    i = 1;
    st->dg_have = 1;
  }

  for (; i + 4 <= len; i += 4) {
    unsigned c0 = DG_LD(i),     c1 = DG_LD(i + 1);
    unsigned c2 = DG_LD(i + 2), c3 = DG_LD(i + 3);
    t[0u * FASTENT_BG_TABLE + ((prev << 8) | c0)]++;  lr_one_(st, c0);
    t[1u * FASTENT_BG_TABLE + ((c0   << 8) | c1)]++;  lr_one_(st, c1);
    t[2u * FASTENT_BG_TABLE + ((c1   << 8) | c2)]++;  lr_one_(st, c2);
    t[3u * FASTENT_BG_TABLE + ((c2   << 8) | c3)]++;  lr_one_(st, c3);
    prev = c3;
  }
  for (; i < len; i++) {
    unsigned c = DG_LD(i);
    t[(prev << 8) | c]++;  lr_one_(st, c);
    prev = c;
  }
  st->dg_prev = (u8) prev;
  #undef DG_LD
}

/*  Bit mode: every adjacent bit pair (MSB->LSB, across byte and chunk
    boundaries via the carried previous bit) feeds the 2x2 bigram,
    longest run, 0/1 run count and the +-1 cusum walk.  dg_prev holds
    the previous bit (0/1) here.  */
static void digram_bits_(fastent_chunk_state * st,
                         const u8 * FASTENT_RESTRICT buf, sz len,
                         int fold) {
  u64 (* bb)[2] = st->bit_bigram;
  sz i;
  for (i = 0; i < len; i++) {
    const unsigned byte = fold ? fastent_fold_byte(buf[i]) : buf[i];
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
  if (binary) {
    if (fold) digram_bits_(st, buf, len, 1);
    else      digram_bits_(st, buf, len, 0);
  } else if (st->bigram) {
    if (fold) digram_bytes_(st, buf, len, 1);
    else      digram_bytes_(st, buf, len, 0);
  }
}
