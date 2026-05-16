/*  fastent: order-1 digram counter (opt-in via -ee).

    Runs as its own tight pass over the same bytes the fast SIMD
    analyser already consumed, so the order-0 statistics keep full
    SIMD throughput.  Byte mode is an order-0 histogram of the 16-bit
    key (prev<<8)|cur over FASTENT_BG_NB round-robin shadow tables, so
    FASTENT_BG_NB consecutive pairs land in independent tables and the
    store-to-load-forward dependency does not serialise (the htscodecs
    hist1_4 technique); the key is built explicitly rather than via an
    endian-specific 16-bit load so big-endian hosts agree bit-for-bit.
    Bit mode keeps the 2x2 bit_bigram.  Tables/counts are merged at
    finalize.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "analyze.h"

#if FASTENT_BG_NB < 4
#error "fastent_digram_count unrolls 4 shadow tables; FASTENT_BG_NB >= 4"
#endif

static void digram_count_bytes_(fastent_chunk_state * st,
                                const u8 * FASTENT_RESTRICT buf, sz len) {
  u64 * FASTENT_RESTRICT t = st->bigram;
  unsigned prev;
  sz i = 0;

  if (st->dg_have) {
    prev = st->dg_prev;
  } else {
    /*  Very first byte of the stream: no left neighbour.  Its pair
        with the previous slab's last byte (if any) is stitched once
        at the multi-thread merge, never here.  */
    prev = buf[0];
    i = 1;
    st->dg_have = 1;
  }

  for (; i + 4 <= len; i += 4) {
    unsigned c0 = buf[i], c1 = buf[i + 1];
    unsigned c2 = buf[i + 2], c3 = buf[i + 3];
    t[0u * FASTENT_BG_TABLE + ((prev << 8) | c0)]++;
    t[1u * FASTENT_BG_TABLE + ((c0   << 8) | c1)]++;
    t[2u * FASTENT_BG_TABLE + ((c1   << 8) | c2)]++;
    t[3u * FASTENT_BG_TABLE + ((c2   << 8) | c3)]++;
    prev = c3;
  }
  for (; i < len; i++) {
    unsigned c = buf[i];
    t[(prev << 8) | c]++;
    prev = c;
  }
  st->dg_prev = (u8) prev;
}

static void digram_count_bits_(fastent_chunk_state * st,
                               const u8 * FASTENT_RESTRICT buf, sz len) {
  u64 (* bb)[2] = st->bit_bigram;
  int have = st->dg_have;
  unsigned prev = st->dg_have ? st->dg_prev : 0u;
  sz i;
  for (i = 0; i < len; i++) {
    const unsigned byte = buf[i];
    if (have)
      bb[prev & 1u][(byte >> 7) & 1u]++;   /*  prev byte LSB, this MSB  */
    Fi0(8, 1, bb[(byte >> i) & 1u][(byte >> (i - 1)) & 1u]++)
    prev = byte;
    have = 1;
  }
  st->dg_prev = (u8) prev;
  st->dg_have = 1;
}

void fastent_digram_count(fastent_chunk_state * st, const u8 * buf,
                          sz len, int binary) {
  if (len == 0) return;
  if (binary)            digram_count_bits_(st, buf, len);
  else if (st->bigram)   digram_count_bytes_(st, buf, len);
}
