/*  fastent: analysis drivers.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "runner.h"

#include "port-thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FASTENT_HAVE_THREADS
typedef struct {
  const u8 *     data;
  const u64 *    bounds;     /*  N+1 entries, multiples of 6 except last  */
  fastent_chunk_state * states;
  fastent_analyze_fn fn;
} mt_ctx;

static void mt_worker_(sz k, void * vctx) {
  mt_ctx * c = (mt_ctx *) vctx;
  u64 start = c->bounds[k];
  u64 end   = c->bounds[k + 1];
  fastent_chunk_state_init(&c->states[k]);
  c->fn(&c->states[k], c->data + start, (sz)(end - start));
}

static void run_mmap_mt_(fastent_chunk_state * out, const fastent_options * o,
                         fastent_analyze_fn fn,
                         const u8 * data, u64 size) {
  int N = o->threads;
  fastent_set_num_threads(N);

  /*  6-byte-aligned slab boundaries so each thread's MC Pi state
      starts at mc_pos == 0; no cross-slab hexads.  */
  u64 * bounds = (u64 *) malloc((sz)(N + 1) * sizeof(u64));
  if (!bounds) { fprintf(stderr, "out of memory\n"); exit(2); }
  bounds[0] = 0;
  bounds[N] = size;
  Fk0(N, 1,
      u64 raw = (u64)((double) size * (double) k / (double) N);
      bounds[k] = (raw / 6ULL) * 6ULL;
      if (bounds[k] < bounds[k - 1]) bounds[k] = bounds[k - 1])

  fastent_chunk_state * states = (fastent_chunk_state *) calloc((sz) N, sizeof(*states));
  if (!states) { fprintf(stderr, "out of memory\n"); exit(2); }

  mt_ctx ctx = { data, bounds, states, fn };
  fastent_parallel_for((sz) N, mt_worker_, &ctx);

  /*  Merge slabs: adjacent slab pairs contribute b[end-1] *
      b[start_next] to the SCC; final wrap (last * first) is added in
      fastent_finalize().  In bit mode the carry/first/last fields are
      single bits, so the same expression covers the cross-slab bit
      pair.  */
  Fk(N,
     const fastent_chunk_state * s = &states[k];
     if (s->total_bytes == 0) continue;
     Fi(FASTENT_BANKS, Fj(256, out->bank[i][j] += s->bank[i][j]))
     out->bit_hist[0] += s->bit_hist[0];
     out->bit_hist[1] += s->bit_hist[1];
     if (out->have_carry) {
       out->cross_product += (i64) out->carry_byte * (i64) s->first_byte;
     } else {
       out->first_byte = s->first_byte;
       out->have_first = 1;
     }
     out->cross_product += s->cross_product;
     out->carry_byte    = s->last_byte;
     out->last_byte     = s->last_byte;
     out->have_carry    = 1;
     out->total_bytes  += s->total_bytes;
     out->mc_count     += s->mc_count;
     out->mc_inside    += s->mc_inside;
     /*  Only the last slab can carry trailing MC ring bytes; the
         others end on a 6-aligned boundary.  */
     if (k == N - 1) {
       out->mc_pos = s->mc_pos;
       memcpy(out->mc_buf, s->mc_buf, sizeof(out->mc_buf));
     })

  free(states);
  free(bounds);
}
#endif

void fastent_run_mmap(fastent_chunk_state * st, const fastent_options * o,
                      fastent_analyze_fn fn_byte,
                      fastent_analyze_fn fn_bits,
                      fastent_analyze_fn fn_byte_fold,
                      fastent_analyze_fn fn_bits_fold,
                      const u8 * data, u64 size) {
  fastent_analyze_fn body = o->binary
    ? (o->fold ? fn_bits_fold : fn_bits)
    : (o->fold ? fn_byte_fold : fn_byte);

#ifdef FASTENT_HAVE_THREADS
  if (o->threads > 1 && size >= (u64)(o->threads) * 1024u * 1024u) {
    run_mmap_mt_(st, o, body, data, size);
    return;
  }
#endif

  body(st, data, (sz) size);
}

void fastent_run_stream(fastent_chunk_state * st, const fastent_options * o,
                        fastent_analyze_fn fn_byte,
                        fastent_analyze_fn fn_bits,
                        fastent_analyze_fn fn_byte_fold,
                        fastent_analyze_fn fn_bits_fold,
                        fastent_source * src) {
  fastent_analyze_fn body = o->binary
    ? (o->fold ? fn_bits_fold : fn_bits)
    : (o->fold ? fn_byte_fold : fn_byte);
  for (;;) {
    sz n = fastent_src_read(src);
    if (n == (sz) -1) {
      perror("read");
      exit(2);
    }
    if (n == 0) break;
    body(st, src->stream_buf, n);
  }
}
