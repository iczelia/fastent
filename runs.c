/*  fastent: -ee sequential extras (runs / longest run / cusum).

    A simple scalar pass over the same bytes the SIMD analyser already
    consumed.  Stream calls it per chunk on one persistent state; mmap
    calls it once over the whole resident buffer (single or -j), so no
    cross-slab stitch is needed in this first cut.  Symbols are bits
    (MSB->LSB) in bit mode, byte values in byte mode.

      longest_run : longest identical-symbol run (both modes)
      runs        : number of 0/1 runs                (bit mode)
      cusum_max   : max |S| of the +-1 bit walk        (bit mode)
      byte runs-vs-median: needs the median, a finalize quantity, so
                    it is a separate post-finalize rescan, see
                    fastent_byte_runs_median (mmap only).

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "analyze.h"

static void runs_one_(fastent_chunk_state * st, unsigned s) {
  if (!st->lr_have) {
    st->lr_have = 1;  st->lr_sym = (u8) s;  st->lr_cur = 1;
  } else if (s == st->lr_sym) {
    st->lr_cur++;
  } else {
    if (st->lr_cur > st->lr_max) st->lr_max = st->lr_cur;
    st->lr_sym = (u8) s;  st->lr_cur = 1;
  }
}

void fastent_runs_count(fastent_chunk_state * st, const u8 * buf,
                        sz len, int binary) {
  sz i;
  if (len == 0) return;

  if (!binary) {
    for (i = 0; i < len; i++) runs_one_(st, buf[i]);
    return;
  }

  for (i = 0; i < len; i++) {
    const unsigned byte = buf[i];
    int k;
    for (k = 7; k >= 0; k--) {
      const unsigned bit = (byte >> k) & 1u;
      runs_one_(st, bit);
      if (!st->rn_have) { st->rn_have = 1;  st->rn_count = 1; }
      else if (bit != st->rn_last) st->rn_count++;
      st->rn_last = (u8) bit;
      st->cs_sum += bit ? 1 : -1;
      if (st->cs_sum < st->cs_min) st->cs_min = st->cs_sum;
      if (st->cs_sum > st->cs_max) st->cs_max = st->cs_sum;
    }
  }
}

u64 fastent_byte_runs_median(const u8 * buf, sz len, int median) {
  sz i;
  u64 runs;
  int prev;
  if (len == 0) return 0;
  prev = buf[0] >= median;
  runs = 1;
  for (i = 1; i < len; i++) {
    const int cls = buf[i] >= median;
    if (cls != prev) { runs++;  prev = cls; }
  }
  return runs;
}
