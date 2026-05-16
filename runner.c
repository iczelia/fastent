/*  fastent: analysis drivers.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "runner.h"

#include "port-thread.h"
#include "port-walk.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FASTENT_HAVE_THREADS
typedef struct {
  const u8 *     data;
  const u64 *    bounds;     /*  N+1 entries, multiples of 6 except last  */
  fastent_chunk_state * states;
  fastent_analyze_fn fn;
  int            want_bigram;
} mt_ctx;

static void mt_worker_(sz k, void * vctx) {
  mt_ctx * c = (mt_ctx *) vctx;
  u64 start = c->bounds[k];
  u64 end   = c->bounds[k + 1];
  fastent_chunk_state_init(&c->states[k]);
  if (c->want_bigram) {
    c->states[k].bigram = fastent_bigram_alloc();
    if (!c->states[k].bigram) { fprintf(stderr, "out of memory\n"); exit(2); }
  }
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

  fastent_chunk_state * states =
    (fastent_chunk_state *) calloc((sz) N, sizeof(*states));
  if (!states) { fprintf(stderr, "out of memory\n"); exit(2); }

  mt_ctx ctx = { data, bounds, states, fn,
                 out->bigram != NULL };
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
     if (out->bigram && s->bigram) {
       sz t;
       for (t = 0; t < (sz) FASTENT_BG_CELLS; t++)
         out->bigram[t] += s->bigram[t];
     }
     out->bit_bigram[0][0] += s->bit_bigram[0][0];
     out->bit_bigram[0][1] += s->bit_bigram[0][1];
     out->bit_bigram[1][0] += s->bit_bigram[1][0];
     out->bit_bigram[1][1] += s->bit_bigram[1][1];
     if (out->have_carry) {
       out->cross_product += (i64) out->carry_byte * (i64) s->first_byte;
       /*  Boundary pair (prev slab last, this slab first), once per
           adjacency, no wrap.  Byte: plane 0; bit: the 2x2 table.  */
       if (out->bigram)
         FASTENT_BG_AT(out->bigram, 0, out->carry_byte, s->first_byte)++;
       if (o->binary)
         out->bit_bigram[out->carry_byte & 1u][s->first_byte & 1u]++;
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

  Fk(N, fastent_bigram_free(states[k].bigram))
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

/*  Recursive mode.  */

typedef struct {
  const fastent_options * o;
  fastent_analyze_fn fn_byte;
  fastent_analyze_fn fn_bits;
  fastent_analyze_fn fn_byte_fold;
  fastent_analyze_fn fn_bits_fold;
  fastent_recursive_row * rows;
  sz                       count;
  sz                       cap;
} recursive_ctx;

static int analyse_one_(const char * path, recursive_ctx * c) {
  /*  Force mmap when possible (regular files); fall back to stream on
      open errors so we don't abort the whole walk.  */
  fastent_source src;
  fastent_io_mode io = (fastent_io_mode) c->o->io_mode;
  if (fastent_src_open(&src, path, io) != 0) {
    fprintf(stderr, "skip %s: %s\n", path, strerror(errno));
    return 0;
  }

  fastent_chunk_state st;
  fastent_chunk_state_init(&st);
  if (c->o->extended >= 2 && !c->o->binary) {
    st.bigram = fastent_bigram_alloc();
    if (!st.bigram) { fprintf(stderr, "out of memory\n"); exit(2); }
  }
  if (src.kind == FASTENT_SRC_MMAP) {
    fastent_run_mmap(&st, c->o, c->fn_byte, c->fn_bits,
                     c->fn_byte_fold, c->fn_bits_fold,
                     (const u8 *) src.map, src.size);
  } else {
    fastent_run_stream(&st, c->o, c->fn_byte, c->fn_bits,
                       c->fn_byte_fold, c->fn_bits_fold, &src);
  }
  fastent_src_close(&src);

  fastent_result r;
  fastent_finalize(&st, c->o->binary, &r);
  fastent_bigram_free(st.bigram);

  if (c->count == c->cap) {
    sz nc = c->cap ? c->cap * 2 : 32;
    fastent_recursive_row * nr =
      (fastent_recursive_row *) realloc(c->rows, nc * sizeof(*nr));
    if (!nr) { errno = ENOMEM; return -1; }
    c->rows = nr;
    c->cap  = nc;
  }
  c->rows[c->count].path   = (char *) malloc(strlen(path) + 1);
  if (!c->rows[c->count].path) { errno = ENOMEM; return -1; }
  memcpy(c->rows[c->count].path, path, strlen(path) + 1);
  c->rows[c->count].result = r;
  c->count++;
  return 0;
}

static int walk_cb_(const char * path, void * vctx) {
  return analyse_one_(path, (recursive_ctx *) vctx);
}

int fastent_run_recursive(const char * root, const fastent_options * o,
                          fastent_analyze_fn fn_byte,
                          fastent_analyze_fn fn_bits,
                          fastent_analyze_fn fn_byte_fold,
                          fastent_analyze_fn fn_bits_fold,
                          fastent_recursive_row ** out_rows,
                          sz * out_n) {
  recursive_ctx c;
  c.o            = o;
  c.fn_byte      = fn_byte;
  c.fn_bits      = fn_bits;
  c.fn_byte_fold = fn_byte_fold;
  c.fn_bits_fold = fn_bits_fold;
  c.rows         = NULL;
  c.count        = 0;
  c.cap          = 0;
  int rc = fastent_walk(root, walk_cb_, &c);
  if (rc != 0) {
    fastent_rows_free(c.rows, c.count);
    return -1;
  }
  *out_rows = c.rows;
  *out_n    = c.count;
  return 0;
}

void fastent_rows_free(fastent_recursive_row * rows, sz n) {
  if (!rows) return;
  for (sz i = 0; i < n; i++) free(rows[i].path);
  free(rows);
}

static int g_sort_by_  = 0;
static int g_sort_desc_ = 0;

static double row_key_(const fastent_recursive_row * r) {
  switch ((fastent_sort_by) g_sort_by_) {
    case FASTENT_SORT_SAMPLES:    return (double) r->result.total_samples;
    case FASTENT_SORT_ENTROPY:    return r->result.entropy;
    case FASTENT_SORT_CHI_SQUARE: return r->result.chi_square;
    case FASTENT_SORT_MEAN:       return r->result.mean;
    case FASTENT_SORT_MONTE_PI:   return r->result.monte_pi;
    case FASTENT_SORT_SCC:        return r->result.scc;
    case FASTENT_SORT_MIN_ENTROPY:return r->result.min_entropy;
    case FASTENT_SORT_COLLISION:  return r->result.collision_entropy;
    case FASTENT_SORT_IC:         return r->result.ic;
    case FASTENT_SORT_POKER:      return r->result.poker_chisq;
    case FASTENT_SORT_VARIANCE:   return r->result.variance;
    case FASTENT_SORT_REDUNDANCY: return r->result.redundancy;
    case FASTENT_SORT_DISTINCT:   return (double) r->result.distinct;
    case FASTENT_SORT_BIT_BIAS:   return r->result.bit_bias_max;
    case FASTENT_SORT_COND_ENTROPY: return r->result.conditional_entropy;
    case FASTENT_SORT_MUTUAL_INFO:  return r->result.mutual_information;
    default:                      return 0.0;
  }
}

static int cmp_(const void * a, const void * b) {
  const fastent_recursive_row * ra = (const fastent_recursive_row *) a;
  const fastent_recursive_row * rb = (const fastent_recursive_row *) b;
  int rc;
  if (g_sort_by_ == FASTENT_SORT_PATH) {
    rc = strcmp(ra->path, rb->path);
  } else {
    double ka = row_key_(ra);
    double kb = row_key_(rb);
    rc = (ka < kb) ? -1 : (ka > kb) ? 1 : 0;
  }
  return g_sort_desc_ ? -rc : rc;
}

void fastent_rows_sort(fastent_recursive_row * rows, sz n,
                       const fastent_options * o) {
  if (o->sort_by == FASTENT_SORT_NONE || n < 2) return;
  g_sort_by_  = o->sort_by;
  g_sort_desc_ = o->sort_desc;
  qsort(rows, n, sizeof *rows, cmp_);
}
