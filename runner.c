/*  fastent: analysis drivers.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

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
  u64 * const *  bigrams;    /*  per-slab digram tables; NULL unless
                                 -ee byte mode  */
  fastent_analyze_fn fn;
  int            extended;
  int            binary;
  int            fold;
} mt_ctx;

static void mt_worker_(sz k, void * vctx) {
  mt_ctx * c = (mt_ctx *) vctx;
  u64 start = c->bounds[k];
  u64 end   = c->bounds[k + 1];
  fastent_chunk_state_init(&c->states[k]);
  if (c->bigrams) c->states[k].bigram = c->bigrams[k];
  c->fn(&c->states[k], c->data + start, (sz)(end - start));
  if (c->extended >= 2)
    fastent_digram_count(&c->states[k], c->data + start,
                         (sz)(end - start), c->binary, c->fold);
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

  /*  Per-slab digram tables (byte -ee only): one each, summed at
      merge.  Allocated up front so OOM is handled serially; the
      worker only wires the pointer in after init.  */
  u64 ** bgs = NULL;
  if (o->extended >= 2 && !o->binary) {
    bgs = (u64 **) calloc((sz) N, sizeof(*bgs));
    if (!bgs) { fprintf(stderr, "out of memory\n"); exit(2); }
    Fk(N, bgs[k] = fastent_bigram_alloc();
          if (!bgs[k]) { fprintf(stderr, "out of memory\n"); exit(2); })
  }

  mt_ctx ctx;
  ctx.data = data;  ctx.bounds = bounds;  ctx.states = states;
  ctx.bigrams = bgs;  ctx.fn = fn;
  ctx.extended = o->extended;  ctx.binary = o->binary;
  ctx.fold = o->fold;
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

  /*  Merge the -ee level-2 reductions.  Per-slab counts are
      partition-invariant; the pair, bit-run and cusum statistics
      additionally need the symbol straddling each slab boundary,
      stitched here in fixed slab order so j1 == jN.  The longest run
      is a segmented merge: a run can span a boundary, or whole slabs
      of one symbol (all-zeros input split N ways).  */
  if (o->extended >= 2) {
    u64 lr_gmax = 0, carry_len = 0;
    unsigned carry_sym = 0;
    int have_run = 0, seen = 0;
    i64 cs_off = 0, cs_min = 0, cs_max = 0;
    Fk(N,
       fastent_chunk_state * s = &states[k];
       if (s->total_bytes == 0) continue;
       const u64 start = bounds[k];
       if (out->bigram && s->bigram)
         Fi((int) FASTENT_BG_CELLS, out->bigram[i] += s->bigram[i])
       if (o->binary) {
         out->bit_bigram[0][0] += s->bit_bigram[0][0];
         out->bit_bigram[0][1] += s->bit_bigram[0][1];
         out->bit_bigram[1][0] += s->bit_bigram[1][0];
         out->bit_bigram[1][1] += s->bit_bigram[1][1];
       }
       if (seen && start > 0) {     /*  pair straddling this boundary  */
         /*  Fold to match the per-slab digram pass under -f.  */
         const unsigned bp = o->fold ? fastent_fold_byte(data[start - 1])
                                      : data[start - 1];
         const unsigned bc = o->fold ? fastent_fold_byte(data[start])
                                      : data[start];
         if (o->binary) {
           const unsigned pb = bp & 1u;
           const unsigned cb = (bc >> 7) & 1u;
           out->bit_bigram[pb][cb]++;
           if (pb == cb) out->rn_count--;       /*  two runs join  */
         } else if (out->bigram) {
           out->bigram[(bp << 8) | bc]++;
         }
       }
       {
         const u64 imax = s->lr_cur > s->lr_max ? s->lr_cur : s->lr_max;
         const unsigned hs = s->lr_head_sym;
         const u64 hl = s->lr_head_len;  const u64 tl = s->lr_cur;
         const int whole = s->lr_head_open;
         if (imax > lr_gmax) lr_gmax = imax;
         if (have_run && carry_sym == hs) {
           const u64 j = carry_len + hl;
           if (j > lr_gmax) lr_gmax = j;
           if (whole) carry_len = j;
           else { carry_sym = s->lr_sym;  carry_len = tl; }
         } else {
           if (have_run && carry_len > lr_gmax) lr_gmax = carry_len;
           if (whole) { carry_sym = hs;        carry_len = hl; }
           else       { carry_sym = s->lr_sym; carry_len = tl; }
           have_run = 1;
         }
       }
       if (o->binary) {
         out->rn_count += s->rn_count;  out->rn_have = 1;
         if (cs_off + s->cs_min < cs_min) cs_min = cs_off + s->cs_min;
         if (cs_off + s->cs_max > cs_max) cs_max = cs_off + s->cs_max;
         cs_off += s->cs_sum;
       }
       seen = 1;)
    if (have_run) {
      if (carry_len > lr_gmax) lr_gmax = carry_len;
      out->lr_have = 1;  out->lr_max = lr_gmax;  out->lr_cur = 0;
    }
    if (o->binary) {
      out->cs_min = cs_min;  out->cs_max = cs_max;  out->cs_sum = cs_off;
    }
  }

  if (bgs) { Fk(N, fastent_bigram_free(bgs[k])) free(bgs); }
  free(states);
  free(bounds);
}

/*  SPMC stream/uring pipeline.  W consumers; whichever holds the
    mutex performs the next serialized fastent_src_read (re-chunked
    to a multiple of 6 so no Monte Carlo hexad straddles a block) and
    claims the next seq, then processes its block lock-free.  Order-
    independent sums fold into per-consumer accumulators; the order-
    sensitive boundary quantities go into per-block edge records,
    stitched in seq order afterwards with the same merge run_mmap_mt_
    uses, so the result is bit-identical to the serial path and to
    -j1.  */

typedef struct {
  u64 seq, n;
  u8  raw_first, raw_last;        /*  digram / bit boundary  */
  u8  o0_first, o0_last;          /*  order-0 SCC boundary   */
  u64 lr_internal, lr_head_len, lr_tail_len;
  u8  lr_head_sym, lr_tail_sym, lr_whole;
  u64 rn_count;                   /*  bit mode               */
  i64 cs_sum, cs_min, cs_max;     /*  bit mode               */
  int mc_pos;  u8 mc_buf[6];      /*  trailing ring (final)  */
} stream_edge;

typedef struct {
  fastent_source *   src;
  fastent_mutex *    mtx;
  fastent_analyze_fn fn;
  int                extended, binary, fold;
  sz                 blocksz;
  u64                next_seq;          /*  guarded by mtx  */
  int                eof, err;          /*  guarded by mtx  */
  const u8 *         stage;             /*  guarded by mtx  */
  sz                 stage_len;
  u8 **              bufs;
  void **            bufs_raw;
  int *              freelist;  int free_n;
  stream_edge *      edges;  sz ne, ecap;
  fastent_chunk_state * accs;           /*  W, per-consumer  */
} stream_ctx;

/*  Caller holds mtx.  Fills buf with blocksz bytes (multiple of 6),
    or fewer at EOF; (sz)-1 on error.  Re-chunks the source's own
    reads so a Monte Carlo hexad never straddles a block.  */
static sz stream_fill_(stream_ctx * c, u8 * buf) {
  sz filled = 0;
  while (filled < c->blocksz) {
    if (c->stage_len == 0) {
      sz n = fastent_src_read(c->src);
      if (n == (sz) -1) return (sz) -1;
      if (n == 0) break;                /*  EOF: short final block  */
      c->stage = c->src->stream_buf;  c->stage_len = n;
    }
    sz take = c->blocksz - filled;
    if (take > c->stage_len) take = c->stage_len;
    memcpy(buf + filled, c->stage, take);
    filled += take;  c->stage += take;  c->stage_len -= take;
  }
  return filled;
}

static void stream_consumer_(sz k, void * vctx) {
  stream_ctx * c = (stream_ctx *) vctx;
  fastent_chunk_state * acc = &c->accs[k];
  for (;;) {
    fastent_mutex_lock(c->mtx);
    if (c->eof || c->err) { fastent_mutex_unlock(c->mtx);  break; }
    int bi = c->freelist[--c->free_n];
    u8 * buf = c->bufs[bi];
    sz got = stream_fill_(c, buf);
    if (got == (sz) -1) {
      c->err = errno ? errno : EIO;  c->freelist[c->free_n++] = bi;
      fastent_mutex_unlock(c->mtx);  break;
    }
    if (got == 0) {
      c->eof = 1;  c->freelist[c->free_n++] = bi;
      fastent_mutex_unlock(c->mtx);  break;
    }
    u64 myseq = c->next_seq++;
    fastent_mutex_unlock(c->mtx);

    fastent_chunk_state blk;
    fastent_chunk_state_init(&blk);
    blk.bigram = acc->bigram;           /*  byte -ee: accumulate here  */
    c->fn(&blk, buf, got);
    if (c->extended >= 2)
      fastent_digram_count(&blk, buf, got, c->binary, c->fold);

    Fi(FASTENT_BANKS, Fj(256, acc->bank[i][j] += blk.bank[i][j]))
    acc->bit_hist[0] += blk.bit_hist[0];
    acc->bit_hist[1] += blk.bit_hist[1];
    acc->bit_bigram[0][0] += blk.bit_bigram[0][0];
    acc->bit_bigram[0][1] += blk.bit_bigram[0][1];
    acc->bit_bigram[1][0] += blk.bit_bigram[1][0];
    acc->bit_bigram[1][1] += blk.bit_bigram[1][1];
    acc->cross_product += blk.cross_product;
    acc->mc_count  += blk.mc_count;
    acc->mc_inside += blk.mc_inside;
    acc->total_bytes += blk.total_bytes;

    stream_edge e;
    e.seq = myseq;  e.n = got;
    /*  Stitch boundary bytes; folded under -f to match the scan.  */
    e.raw_first = c->fold ? fastent_fold_byte(buf[0]) : buf[0];
    e.raw_last  = c->fold ? fastent_fold_byte(buf[got - 1]) : buf[got - 1];
    e.o0_first = blk.first_byte;  e.o0_last = blk.last_byte;
    e.lr_internal = blk.lr_cur > blk.lr_max ? blk.lr_cur : blk.lr_max;
    e.lr_head_sym = blk.lr_head_sym;  e.lr_head_len = blk.lr_head_len;
    e.lr_tail_sym = blk.lr_sym;       e.lr_tail_len = blk.lr_cur;
    e.lr_whole = blk.lr_head_open;
    e.rn_count = blk.rn_count;
    e.cs_sum = blk.cs_sum;  e.cs_min = blk.cs_min;  e.cs_max = blk.cs_max;
    e.mc_pos = blk.mc_pos;  memcpy(e.mc_buf, blk.mc_buf, sizeof(e.mc_buf));

    fastent_mutex_lock(c->mtx);
    if (c->ne == c->ecap) {
      sz nc = c->ecap ? c->ecap * 2 : 64;
      stream_edge * grow =
        (stream_edge *) realloc(c->edges, nc * sizeof(*grow));
      if (!grow) {
        c->err = ENOMEM;  c->freelist[c->free_n++] = bi;
        fastent_mutex_unlock(c->mtx);  break;
      }
      c->edges = grow;  c->ecap = nc;
    }
    c->edges[c->ne++] = e;
    c->freelist[c->free_n++] = bi;
    fastent_mutex_unlock(c->mtx);
  }
}

static int stream_edge_cmp_(const void * a, const void * b) {
  u64 sa = ((const stream_edge *) a)->seq;
  u64 sb = ((const stream_edge *) b)->seq;
  return sa < sb ? -1 : sa > sb ? 1 : 0;
}

static void run_stream_mt_(fastent_chunk_state * out,
                           const fastent_options * o,
                           fastent_analyze_fn fn, fastent_source * src) {
  int W = o->threads;
  fastent_set_num_threads(W);
  if (W < 2) {                          /*  no real pool: serial  */
    for (;;) {
      sz n = fastent_src_read(src);
      if (n == (sz) -1) { perror("read");  exit(2); }
      if (n == 0) break;
      fn(out, src->stream_buf, n);
      if (o->extended >= 2)
        fastent_digram_count(out, src->stream_buf, n, o->binary, o->fold);
    }
    return;
  }

  stream_ctx c;
  memset(&c, 0, sizeof c);
  c.src = src;  c.fn = fn;
  c.extended = o->extended;  c.binary = o->binary;  c.fold = o->fold;
  c.blocksz = (FASTENT_STREAM_BUF / 6u) * 6u;
  c.mtx = fastent_mutex_create();
  if (!c.mtx) { fprintf(stderr, "out of memory\n"); exit(2); }

  int P = W + 1;
  c.bufs     = (u8 **) calloc((sz) P, sizeof(*c.bufs));
  c.bufs_raw = (void **) calloc((sz) P, sizeof(*c.bufs_raw));
  c.freelist = (int *) malloc((sz) P * sizeof(*c.freelist));
  c.accs = (fastent_chunk_state *) calloc((sz) W, sizeof(*c.accs));
  if (!c.bufs || !c.bufs_raw || !c.freelist || !c.accs) {
    fprintf(stderr, "out of memory\n"); exit(2);
  }
  Fk(P,
     void * raw = NULL;  void * user = NULL;
     if (fastent_io_alloc_aligned(&raw, &user, c.blocksz) < 0) {
       fprintf(stderr, "out of memory\n"); exit(2);
     }
     c.bufs[k] = (u8 *) user;  c.bufs_raw[k] = raw;
     c.freelist[k] = k)
  c.free_n = P;
  if (o->extended >= 2 && !o->binary)
    Fk(W,
       c.accs[k].bigram = fastent_bigram_alloc();
       if (!c.accs[k].bigram) { fprintf(stderr, "out of memory\n"); exit(2); })

  fastent_parallel_for((sz) W, stream_consumer_, &c);

  if (c.err) {
    if (c.err == ENOMEM) fprintf(stderr, "out of memory\n");
    else { errno = c.err;  perror("read"); }
    exit(2);
  }

  /*  Order-independent sums.  */
  Fk(W,
     const fastent_chunk_state * a = &c.accs[k];
     Fi(FASTENT_BANKS, Fj(256, out->bank[i][j] += a->bank[i][j]))
     out->bit_hist[0] += a->bit_hist[0];
     out->bit_hist[1] += a->bit_hist[1];
     out->bit_bigram[0][0] += a->bit_bigram[0][0];
     out->bit_bigram[0][1] += a->bit_bigram[0][1];
     out->bit_bigram[1][0] += a->bit_bigram[1][0];
     out->bit_bigram[1][1] += a->bit_bigram[1][1];
     out->cross_product += a->cross_product;
     out->mc_count  += a->mc_count;
     out->mc_inside += a->mc_inside;
     out->total_bytes += a->total_bytes;
     if (out->bigram && a->bigram)
       Fi((int) FASTENT_BG_CELLS, out->bigram[i] += a->bigram[i]))

  qsort(c.edges, c.ne, sizeof(*c.edges), stream_edge_cmp_);

  /*  Ordered boundary stitch (same algebra as run_mmap_mt_).  */
  u64 lr_gmax = 0, carry_len = 0;
  unsigned carry_sym = 0;
  int have_run = 0;
  i64 cs_off = 0, cs_min = 0, cs_max = 0;
  for (sz i = 0; i < c.ne; i++) {
    const stream_edge * e = &c.edges[i];
    if (out->have_carry) {
      out->cross_product += (i64) out->carry_byte * (i64) e->o0_first;
      if (out->bigram)
        out->bigram[((unsigned) c.edges[i - 1].raw_last << 8)
                    | e->raw_first]++;
      if (o->binary && o->extended >= 2) {
        const unsigned pb = c.edges[i - 1].raw_last & 1u;
        const unsigned cb = (e->raw_first >> 7) & 1u;
        out->bit_bigram[pb][cb]++;
        if (pb == cb) out->rn_count--;
      }
    } else {
      out->first_byte = e->o0_first;  out->have_first = 1;
    }
    out->carry_byte = e->o0_last;  out->last_byte = e->o0_last;
    out->have_carry = 1;

    if (o->extended >= 2) {
      const u64 imax = e->lr_internal;
      if (imax > lr_gmax) lr_gmax = imax;
      if (have_run && carry_sym == e->lr_head_sym) {
        const u64 j = carry_len + e->lr_head_len;
        if (j > lr_gmax) lr_gmax = j;
        if (e->lr_whole) carry_len = j;
        else { carry_sym = e->lr_tail_sym;  carry_len = e->lr_tail_len; }
      } else {
        if (have_run && carry_len > lr_gmax) lr_gmax = carry_len;
        if (e->lr_whole) { carry_sym = e->lr_head_sym;
                           carry_len = e->lr_head_len; }
        else { carry_sym = e->lr_tail_sym;  carry_len = e->lr_tail_len; }
        have_run = 1;
      }
      if (o->binary) {
        out->rn_count += e->rn_count;  out->rn_have = 1;
        if (cs_off + e->cs_min < cs_min) cs_min = cs_off + e->cs_min;
        if (cs_off + e->cs_max > cs_max) cs_max = cs_off + e->cs_max;
        cs_off += e->cs_sum;
      }
    }
  }
  if (c.ne) {     /*  trailing MC ring lives in the final block  */
    out->mc_pos = c.edges[c.ne - 1].mc_pos;
    memcpy(out->mc_buf, c.edges[c.ne - 1].mc_buf, sizeof(out->mc_buf));
  }
  if (o->extended >= 2 && have_run) {
    if (carry_len > lr_gmax) lr_gmax = carry_len;
    out->lr_have = 1;  out->lr_max = lr_gmax;  out->lr_cur = 0;
  }
  if (o->extended >= 2 && o->binary) {
    out->cs_min = cs_min;  out->cs_max = cs_max;  out->cs_sum = cs_off;
  }

  if (o->extended >= 2 && !o->binary)
    Fk(W, fastent_bigram_free(c.accs[k].bigram))
  Fk(P, free(c.bufs_raw[k]))
  free(c.bufs);  free(c.bufs_raw);  free(c.freelist);
  free(c.accs);  free(c.edges);
  fastent_mutex_destroy(c.mtx);
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
  if (o->extended >= 2)
    fastent_digram_count(st, data, (sz) size, o->binary, o->fold);
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

#ifdef FASTENT_HAVE_THREADS
  if (o->threads > 1) {
    run_stream_mt_(st, o, body, src);
    return;
  }
#endif

  for (;;) {
    sz n = fastent_src_read(src);
    if (n == (sz) -1) {
      perror("read");
      exit(2);
    }
    if (n == 0) break;
    body(st, src->stream_buf, n);
    if (o->extended >= 2)
      fastent_digram_count(st, src->stream_buf, n, o->binary, o->fold);
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
  fastent_result r;
  fastent_finalize(&st, c->o->binary, &r);

  fastent_src_close(&src);
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
