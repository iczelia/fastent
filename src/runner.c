/*  Copyright (C) 2023-2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.  */

#include "common.h"
#include "runner.h"

#include "lzest.h"
#include "bm.h"
#include "maurer.h"
#include "mrank.h"
#include "perment.h"
#include "output.h"
#include "port-thread.h"
#include "port-walk.h"
#include "port-io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FASTENT_FUSE_BLOCK (64u * 1024u)

/*  Run `body` over [data,len) split at FASTENT_HIST_CHUNK boundaries,
    draining the u32 order-0 banks before a chunk overflows.  */
static void body_drained_(
    fastent_chunk_state * st, fastent_analyze_fn body, const u8 * data,
    sz len) {
  sz off = 0;
  while (off < len) {
    if (st->hist_chunk_bytes >= FASTENT_HIST_CHUNK)
      fastent_hist_flush_(st);
    sz room = (sz) (FASTENT_HIST_CHUNK - st->hist_chunk_bytes);
    sz n = len - off;
    if (n > room) n = room;
    body(st, data + off, n);
    st->hist_chunk_bytes += n;
    off += n;
  }
}

/*  Cached vectorised fold variant for the fold-once -ee byte path.
    Pick is process-stable, so the lazy-init race stores the same
    pointer (same discipline as digram.c's dg_fold_fn_).  */
static fastent_fold_fn fused_fold_fn_(void) {
  static fastent_fold_fn ff = NULL;
  fastent_fold_fn f = ff;
  if (!f) { f = fastent_pick_fold_variant(NULL);  ff = f; }
  return f;
}

/*  Cache-blocked: order-0 body then -ee extras per L1/L2-resident sub-block
    so extras hit cache not DRAM; state threads as the stream path,
    byte-identical to one whole pass.  */
static void analyze_fused_(
    fastent_chunk_state * st, fastent_analyze_fn body,
    fastent_analyze_fn body_plain, const u8 * data, sz len, int extended,
    int binary, int fold) {
  if (extended < 2) { body_drained_(st, body, data, len);  return; }

  if (fold && !binary && body_plain) {
    ALIGN(64) u8 fb[FASTENT_FUSE_BLOCK];
    fastent_fold_fn ff = fused_fold_fn_();
    sz off = 0;
    while (off < len) {
      sz n = len - off;
      if (n > FASTENT_FUSE_BLOCK) n = FASTENT_FUSE_BLOCK;
      memcpy(fb, data + off, n);
      ff(fb, n);
      if (st->hist_chunk_bytes >= FASTENT_HIST_CHUNK)
        fastent_hist_flush_(st);
      body_plain(st, fb, n);
      st->hist_chunk_bytes += n;
      fastent_digram_count(st, fb, n, 0, 0);
      off += n;
    }
    return;
  }

  sz off = 0;
  while (off < len) {
    sz n = len - off;
    if (n > FASTENT_FUSE_BLOCK) n = FASTENT_FUSE_BLOCK;
    if (st->hist_chunk_bytes >= FASTENT_HIST_CHUNK)
      fastent_hist_flush_(st);
    body(st, data + off, n);
    st->hist_chunk_bytes += n;
    fastent_digram_count(st, data + off, n, binary, fold);
    off += n;
  }
}

#ifdef FASTENT_HAVE_THREADS
typedef struct {
  const u8 *     data;
  const u64 *    bounds;     /*  N+1 entries, multiples of 6 except last  */
  fastent_chunk_state * states;
  /*  per-slab digram tables; NULL unless -ee byte mode  */
  u64 * const *  bigrams;
  u32 * const *  dg_u32s;    /*  parallel u32 chunk shadows  */
  fastent_analyze_fn fn;
  fastent_analyze_fn fn_plain;   /*  non-fold byte body (fold-once)  */
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
  if (c->dg_u32s) c->states[k].dg_u32 = c->dg_u32s[k];
  analyze_fused_(&c->states[k], c->fn, c->fn_plain, c->data + start,
                 (sz) (end - start), c->extended, c->binary, c->fold);
  /*  Settle this slab's residual u32 banks into its u64 master so the
      serial merge below sums masters (order-independent, == j1).  */
  fastent_hist_flush_(&c->states[k]);
}

static void run_mmap_mt_(
    fastent_chunk_state * out, const fastent_options * o,
    fastent_analyze_fn fn, fastent_analyze_fn fn_plain, const u8 * data,
    u64 size) {
  i32 N = o->threads;
  i32 i, k;
  fastent_set_num_threads(N);

  /*  6-byte-aligned slab boundaries so each thread's MC Pi state
      starts at mc_pos == 0; no cross-slab hexads.  */
  u64 * bounds = (u64 *) malloc((sz) (N + 1) * sizeof (u64));
  if (!bounds) { fastent_message("out of memory"); exit(2); }
  bounds[0] = 0;
  bounds[N] = size;
  for (k = 1; k < N; k++) {
    u64 raw = (u64) ((f64) size * (f64) k / (f64) N);
    bounds[k] = (raw / 6ULL) * 6ULL;
    if (bounds[k] < bounds[k - 1]) bounds[k] = bounds[k - 1];
  }

  fastent_chunk_state * states =
    (fastent_chunk_state *) calloc((sz) N, sizeof (*states));
  if (!states) { fastent_message("out of memory"); exit(2); }

  /*  Per-slab digram tables (byte -ee only), summed at merge.
      Allocated up front so OOM is handled serially.  */
  u64 ** bgs = NULL;
  u32 ** dgs = NULL;
  if (o->extended >= 2 && !o->binary) {
    bgs = (u64 **) calloc((sz) N, sizeof (*bgs));
    dgs = (u32 **) calloc((sz) N, sizeof (*dgs));
    if (!bgs || !dgs) { fastent_message("out of memory"); exit(2); }
    Fk(N, bgs[k] = fastent_bigram_alloc();
         dgs[k] = fastent_dg_u32_alloc();
         if (!bgs[k] || !dgs[k]) {
           fastent_message("out of memory"); exit(2); });
  }

  mt_ctx ctx;
  ctx.data = data;  ctx.bounds = bounds;  ctx.states = states;
  ctx.bigrams = bgs;  ctx.dg_u32s = dgs;
  ctx.fn = fn;  ctx.fn_plain = fn_plain;
  ctx.extended = o->extended;  ctx.binary = o->binary;
  ctx.fold = o->fold;
  fastent_parallel_for((sz) N, mt_worker_, &ctx);

  /*  Fixed-order slab merge, bit-identical to -j1.  */
  Fk(N,
    const fastent_chunk_state * s = &states[k];
    if (s->total_bytes == 0) continue;
    Fi(256, out->hist_master[i] += s->hist_master[i]);
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
    /*  The last non-empty slab's trailing ring (non-final slabs are
        6-aligned, so their mc_pos is 0 and this is a no-op for
        them); robust if the final slab is empty.  */
    out->mc_pos = s->mc_pos;
    memcpy(out->mc_buf, s->mc_buf, sizeof (out->mc_buf)));

  /*  Merge -ee level-2 reductions.  */
  if (o->extended >= 2) {
    u64 lr_gmax = 0, carry_len = 0;
    u32 carry_sym = 0;
    int have_run = 0, seen = 0;
    i64 cs_off = 0, cs_min = 0, cs_max = 0;
    Fk(N,
      fastent_chunk_state * s = &states[k];
      if (s->total_bytes == 0) continue;
      const u64 start = bounds[k];
      fastent_dg_drain(s);   /*  flush this slab's u32 chunk first  */
      if (out->bigram && s->bigram)
        Fi((int) FASTENT_BG_CELLS, out->bigram[i] += s->bigram[i]);
      if (o->binary) {
        out->bit_bigram[0][0] += s->bit_bigram[0][0];
        out->bit_bigram[0][1] += s->bit_bigram[0][1];
        out->bit_bigram[1][0] += s->bit_bigram[1][0];
        out->bit_bigram[1][1] += s->bit_bigram[1][1];
      }
      if (seen && start > 0) {     /*  pair straddling this boundary  */
        /*  Fold to match the per-slab digram pass under -f.  */
        const u32 bp = o->fold ? fastent_fold_byte(data[start - 1])
                                : data[start - 1];
        const u32 bc = o->fold ? fastent_fold_byte(data[start])
                                : data[start];
        if (o->binary) {
          const u32 pb = bp & 1u;
          const u32 cb = (bc >> 7) & 1u;
          out->bit_bigram[pb][cb]++;
          if (pb == cb) out->rn_count--;       /*  two runs join  */
        } else if (out->bigram) {
          out->bigram[(bp << 8) | bc]++;
        }
      }
      {
        const u64 imax = s->lr_cur > s->lr_max ? s->lr_cur : s->lr_max;
        const u32 hs = s->lr_head_sym;
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
      seen = 1);
    if (have_run) {
      if (carry_len > lr_gmax) lr_gmax = carry_len;
      out->lr_have = 1;  out->lr_max = lr_gmax;  out->lr_cur = 0;
    }
    if (o->binary) { out->cs_min = cs_min;  out->cs_max = cs_max;  out->cs_sum = cs_off; }
  }

  if (bgs) { Fk(N, fastent_bigram_free(bgs[k])); free(bgs); }
  if (dgs) { Fk(N, fastent_dg_u32_free(dgs[k])); free(dgs); }
  free(states);
  free(bounds);
}

/*  SPMC stream/uring pipeline.  */

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
  fastent_analyze_fn fn_plain;   /*  non-fold byte body (fold-once)  */
  int                extended, binary, fold;
  sz                 blocksz;
  u64                next_seq;          /*  guarded by mtx  */
  int                eof, err;          /*  guarded by mtx  */
  const u8 *         stage;             /*  guarded by mtx  */
  sz                 stage_len;
  u8 **              bufs;
  void **            bufs_raw;
  i32 *              freelist;  i32 free_n;
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
  i32 i;
  for (;;) {
    fastent_mutex_lock(c->mtx);
    if (c->eof || c->err) { fastent_mutex_unlock(c->mtx);  break; }
    i32 bi = c->freelist[--c->free_n];
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
    blk.dg_u32 = acc->dg_u32;           /*  per-consumer u32 shadow    */
    analyze_fused_(&blk, c->fn, c->fn_plain, buf, got,
                   c->extended, c->binary, c->fold);
    /*  Flush this block's u32 chunk into acc->bigram before the
        per-consumer accumulate; the shadow is zeroed for reuse.  */
    fastent_dg_drain(&blk);

    /*  Settle this block's u32 banks into its u64 master, then add the
        master into the per-consumer accumulator (order-independent).  */
    fastent_hist_flush_(&blk);
    Fi(256, acc->hist_master[i] += blk.hist_master[i]);
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
    e.mc_pos = blk.mc_pos;  memcpy(e.mc_buf, blk.mc_buf, sizeof (e.mc_buf));

    fastent_mutex_lock(c->mtx);
    if (c->ne == c->ecap) {
      sz nc = c->ecap ? c->ecap * 2 : 64;
      stream_edge * grow =
        (stream_edge *) realloc(c->edges, nc * sizeof (*grow));
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

static void run_stream_mt_(
    fastent_chunk_state * out, const fastent_options * o,
    fastent_analyze_fn fn, fastent_analyze_fn fn_plain, fastent_source * src) {
  i32 W = o->threads;
  i32 k;
  sz i;
  fastent_set_num_threads(W);
  if (W < 2) {                          /*  no real pool: serial  */
    for (;;) {
      sz n = fastent_src_read(src);
      if (n == (sz) -1) { fastent_message("read error: %s", strerror(errno));  exit(2); }
      if (n == 0) break;
      analyze_fused_(out, fn, fn_plain, src->stream_buf, n,
                     o->extended, o->binary, o->fold);
    }
    return;
  }

  stream_ctx c;
  memset(&c, 0, sizeof c);
  c.src = src;  c.fn = fn;  c.fn_plain = fn_plain;
  c.extended = o->extended;  c.binary = o->binary;  c.fold = o->fold;
  c.blocksz = (FASTENT_STREAM_BUF / 6u) * 6u;
  c.mtx = fastent_mutex_create();
  if (!c.mtx) { fastent_message("out of memory"); exit(2); }

  i32 P = W + 1;
  c.bufs     = (u8 **) calloc((sz) P, sizeof (*c.bufs));
  c.bufs_raw = (void **) calloc((sz) P, sizeof (*c.bufs_raw));
  c.freelist = (i32 *) malloc((sz) P * sizeof (*c.freelist));
  c.accs = (fastent_chunk_state *) calloc((sz) W, sizeof (*c.accs));
  if (!c.bufs || !c.bufs_raw || !c.freelist || !c.accs) {
    fastent_message("out of memory"); exit(2);
  }
  Fk(P,
    void * raw = NULL;  void * user = NULL;
    if (fastent_io_alloc_aligned(&raw, &user, c.blocksz) < 0) {
      fastent_message("out of memory"); exit(2);
    }
    c.bufs[k] = (u8 *) user;  c.bufs_raw[k] = raw;
    c.freelist[k] = k);
  c.free_n = P;
  if (o->extended >= 2 && !o->binary)
    Fk(W,
      c.accs[k].bigram = fastent_bigram_alloc();
      c.accs[k].dg_u32 = fastent_dg_u32_alloc();
      if (!c.accs[k].bigram || !c.accs[k].dg_u32) {
        fastent_message("out of memory"); exit(2); });

  fastent_parallel_for((sz) W, stream_consumer_, &c);

  if (c.err) {
    if (c.err == ENOMEM) fastent_message("out of memory");
    else fastent_message("read error: %s", strerror(c.err));
    exit(2);
  }

  /*  Order-independent sums.  */
  Fk(W,
    const fastent_chunk_state * a = &c.accs[k];
    Fi(256, out->hist_master[i] += a->hist_master[i]);
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
      Fi((int) FASTENT_BG_CELLS, out->bigram[i] += a->bigram[i]););

  /*  c.edges is allocated lazily on the first pushed edge; with no
      edges (empty / single-block input) it stays NULL and qsort(NULL,
      0, ...) is undefined (glibc marks the base nonnull).  */
  if (c.ne) qsort(c.edges, c.ne, sizeof (*c.edges), stream_edge_cmp_);

  /*  Ordered boundary stitch (same algebra as run_mmap_mt_).  */
  u64 lr_gmax = 0, carry_len = 0;
  u32 carry_sym = 0;
  int have_run = 0;
  i64 cs_off = 0, cs_min = 0, cs_max = 0;
  for (i = 0; i < c.ne; i++) {
    const stream_edge * e = &c.edges[i];
    if (out->have_carry) {
      out->cross_product += (i64) out->carry_byte * (i64) e->o0_first;
      if (out->bigram)
        out->bigram[((u32) c.edges[i - 1].raw_last << 8)
                    | e->raw_first]++;
      if (o->binary && o->extended >= 2) {
        const u32 pb = c.edges[i - 1].raw_last & 1u;
        const u32 cb = (e->raw_first >> 7) & 1u;
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
    memcpy(out->mc_buf, c.edges[c.ne - 1].mc_buf, sizeof (out->mc_buf));
  }
  if (o->extended >= 2 && have_run) {
    if (carry_len > lr_gmax) lr_gmax = carry_len;
    out->lr_have = 1;  out->lr_max = lr_gmax;  out->lr_cur = 0;
  }
  if (o->extended >= 2 && o->binary) {
    out->cs_min = cs_min;  out->cs_max = cs_max;  out->cs_sum = cs_off;
  }

  if (o->extended >= 2 && !o->binary)
    Fk(W, fastent_bigram_free(c.accs[k].bigram);
         fastent_dg_u32_free(c.accs[k].dg_u32));
  Fk(P, free(c.bufs_raw[k]));
  free(c.bufs);  free(c.bufs_raw);  free(c.freelist);
  free(c.accs);  free(c.edges);
  fastent_mutex_destroy(c.mtx);
}

/*  Per-worker io_uring slab driver.  */
typedef struct {
  int                fd;
  const char *       path;
  const u64 *        bounds;
  fastent_chunk_state * accs;
  stream_edge *      edges;
  u64 * const *      bigrams;
  u32 * const *      dg_u32s;
  fastent_analyze_fn fn, fn_plain;
  int                extended, binary, fold;
  volatile int       failed;
} uring_mt_ctx;

static void uring_mt_worker_(sz k, void * vctx) {
  uring_mt_ctx * c = (uring_mt_ctx *) vctx;
  u64 start = c->bounds[k], end = c->bounds[k + 1];
  fastent_chunk_state * acc = &c->accs[k];
  fastent_chunk_state_init(acc);
  if (c->bigrams) acc->bigram = c->bigrams[k];
  if (c->dg_u32s) acc->dg_u32 = c->dg_u32s[k];
  stream_edge * e = &c->edges[k];
  memset(e, 0, sizeof (*e));
  e->seq = (u64) k;
  if (end <= start) return;            /*  empty slab: n stays 0  */

  fastent_uring_slab * r =
      fastent_uring_slab_open(c->fd, c->path, start, end - start);
  if (!r) { c->failed = 1;  return; }
  u8 first = 0, last = 0;
  int seen = 0;
  for (;;) {
    const u8 * blk = NULL;
    sz n = fastent_uring_slab_next(r, &blk);
    if (n == (sz) -1) { c->failed = 1;  fastent_uring_slab_close(r);  return; }
    if (n == 0) break;
    if (!seen) { first = blk[0];  seen = 1; }
    last = blk[n - 1];
    analyze_fused_(acc, c->fn, c->fn_plain, blk, n,
                   c->extended, c->binary, c->fold);
  }
  fastent_uring_slab_close(r);
  if (!seen) return;                   /*  short read of nothing  */

  fastent_dg_drain(acc);
  fastent_hist_flush_(acc);
  e->n = end - start;
  e->raw_first = c->fold ? fastent_fold_byte(first) : first;
  e->raw_last  = c->fold ? fastent_fold_byte(last)  : last;
  e->o0_first = acc->first_byte;  e->o0_last = acc->last_byte;
  e->lr_internal = acc->lr_cur > acc->lr_max ? acc->lr_cur : acc->lr_max;
  e->lr_head_sym = acc->lr_head_sym;  e->lr_head_len = acc->lr_head_len;
  e->lr_tail_sym = acc->lr_sym;       e->lr_tail_len = acc->lr_cur;
  e->lr_whole = acc->lr_head_open;
  e->rn_count = acc->rn_count;
  e->cs_sum = acc->cs_sum;  e->cs_min = acc->cs_min;  e->cs_max = acc->cs_max;
  e->mc_pos = acc->mc_pos;  memcpy(e->mc_buf, acc->mc_buf, sizeof (e->mc_buf));
}

/*  Returns 0 on success, -1 if uring is unavailable (caller falls back
    to the stream driver, preserving the graceful-degradation gate).  */
static int run_uring_mt_(
    fastent_chunk_state * out, const fastent_options * o,
    fastent_analyze_fn fn, fastent_analyze_fn fn_plain, fastent_source * src) {
  i32 N = o->threads;
  i32 i, k;
  fastent_set_num_threads(N);
  u64 size = src->size;

  u64 * bounds = (u64 *) malloc((sz) (N + 1) * sizeof (u64));
  if (!bounds) { fastent_message("out of memory"); exit(2); }
  bounds[0] = 0;  bounds[N] = size;
  for (k = 1; k < N; k++) {
    u64 raw = (u64) ((f64) size * (f64) k / (f64) N);
    bounds[k] = (raw / 6ULL) * 6ULL;
    if (bounds[k] < bounds[k - 1]) bounds[k] = bounds[k - 1];
  }

  fastent_chunk_state * accs =
    (fastent_chunk_state *) calloc((sz) N, sizeof (*accs));
  stream_edge * edges =
    (stream_edge *) calloc((sz) N, sizeof (*edges));
  if (!accs || !edges) { fastent_message("out of memory"); exit(2); }

  u64 ** bgs = NULL;  u32 ** dgs = NULL;
  if (o->extended >= 2 && !o->binary) {
    bgs = (u64 **) calloc((sz) N, sizeof (*bgs));
    dgs = (u32 **) calloc((sz) N, sizeof (*dgs));
    if (!bgs || !dgs) { fastent_message("out of memory"); exit(2); }
    Fk(N, bgs[k] = fastent_bigram_alloc();
         dgs[k] = fastent_dg_u32_alloc();
         if (!bgs[k] || !dgs[k]) {
           fastent_message("out of memory"); exit(2); });
  }

  uring_mt_ctx c;
  c.fd = src->fd;  c.path = o->path;
  c.bounds = bounds;  c.accs = accs;  c.edges = edges;
  c.bigrams = bgs;  c.dg_u32s = dgs;
  c.fn = fn;  c.fn_plain = fn_plain;
  c.extended = o->extended;  c.binary = o->binary;  c.fold = o->fold;
  c.failed = 0;
  fastent_parallel_for((sz) N, uring_mt_worker_, &c);

  if (c.failed) {                      /*  graceful fallback  */
    if (bgs) { Fk(N, fastent_bigram_free(bgs[k])); free(bgs); }
    if (dgs) { Fk(N, fastent_dg_u32_free(dgs[k])); free(dgs); }
    free(accs);  free(edges);  free(bounds);
    return -1;
  }

  /*  Order-independent sums (same as run_stream_mt_).  */
  Fk(N,
    const fastent_chunk_state * a = &accs[k];
    Fi(256, out->hist_master[i] += a->hist_master[i]);
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
      Fi((int) FASTENT_BG_CELLS, out->bigram[i] += a->bigram[i]););

  /*  Ordered boundary stitch in slab order (seq == k); same algebra as
      run_stream_mt_, edges already laid out by index so no qsort.  */
  u64 lr_gmax = 0, carry_len = 0;
  u32 carry_sym = 0;
  int have_run = 0, seen = 0;
  i64 cs_off = 0, cs_min = 0, cs_max = 0;
  i32 prev = -1;
  Fk(N,
    const stream_edge * e = &edges[k];
    if (e->n == 0 || accs[k].total_bytes == 0) continue;
    if (seen) {
      out->cross_product += (i64) out->carry_byte * (i64) e->o0_first;
      if (out->bigram)
        out->bigram[((u32) edges[prev].raw_last << 8) | e->raw_first]++;
      if (o->binary && o->extended >= 2) {
        const u32 pb = edges[prev].raw_last & 1u;
        const u32 cb = (e->raw_first >> 7) & 1u;
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
    out->mc_pos = e->mc_pos;
    memcpy(out->mc_buf, e->mc_buf, sizeof (out->mc_buf));
    prev = (i32) k;  seen = 1;);
  if (o->extended >= 2 && have_run) {
    if (carry_len > lr_gmax) lr_gmax = carry_len;
    out->lr_have = 1;  out->lr_max = lr_gmax;  out->lr_cur = 0;
  }
  if (o->extended >= 2 && o->binary) {
    out->cs_min = cs_min;  out->cs_max = cs_max;  out->cs_sum = cs_off;
  }

  if (bgs) { Fk(N, fastent_bigram_free(bgs[k])); free(bgs); }
  if (dgs) { Fk(N, fastent_dg_u32_free(dgs[k])); free(dgs); }
  free(accs);  free(edges);  free(bounds);
  return 0;
}
#endif

/*  Grid drivers (LZ77F / linear-complexity / Maurer): the parse is decomposed
    on the absolute 4 MiB grid (lzest.h); each block is scored whole with
    fresh per-block state, so the accumulators combine order-independent and
    -j1 == -jN == ref.  */

#define FASTENT_LZ_GRID_U64 ((u64) FASTENT_LZ_GRID)

#define FASTENT_GRID_EST lz
#include "runner-grid.h"

#define FASTENT_GRID_EST bm
#include "runner-grid.h"

#define FASTENT_GRID_EST maurer
#include "runner-grid.h"

#define FASTENT_GRID_EST mrank
#include "runner-grid.h"

#define FASTENT_GRID_EST perment
#include "runner-grid.h"

void fastent_run_mmap(
    fastent_chunk_state * st, const fastent_options * o,
    fastent_analyze_fn fn_byte, fastent_analyze_fn fn_bits,
    fastent_analyze_fn fn_byte_fold, fastent_analyze_fn fn_bits_fold,
    const u8 * data, u64 size) {
  fastent_analyze_fn body = o->binary
    ? (o->fold ? fn_bits_fold : fn_bits)
    : (o->fold ? fn_byte_fold : fn_byte);
  /*  Non-fold byte body for the fold-once -ee path.  */
  fastent_analyze_fn body_plain = (!o->binary && o->fold) ? fn_byte : NULL;

#ifdef FASTENT_HAVE_THREADS
  if (o->threads > 1 && size >= (u64) (o->threads) * 1024u * 1024u) {
    run_mmap_mt_(st, o, body, body_plain, data, size);
    return;
  }
#endif

  analyze_fused_(st, body, body_plain, data, (sz) size,
                 o->extended, o->binary, o->fold);
}

void fastent_run_stream(
    fastent_chunk_state * st, const fastent_options * o,
    fastent_analyze_fn fn_byte, fastent_analyze_fn fn_bits,
    fastent_analyze_fn fn_byte_fold, fastent_analyze_fn fn_bits_fold,
    fastent_source * src) {
  fastent_analyze_fn body = o->binary
    ? (o->fold ? fn_bits_fold : fn_bits)
    : (o->fold ? fn_byte_fold : fn_byte);
  /*  Non-fold byte body for the fold-once -ee path.  */
  fastent_analyze_fn body_plain = (!o->binary && o->fold) ? fn_byte : NULL;

#ifdef FASTENT_HAVE_THREADS
  if (o->threads > 1) {
    /*  io_uring regular-file source: per-worker private rings over
        disjoint slabs (scales like mmap with async overlap).  Falls
        back to the shared-feed SPMC path if a ring cannot be made.  */
    if (src->kind == FASTENT_SRC_URING && src->fd >= 0 && src->size > 0
        && run_uring_mt_(st, o, body, body_plain, src) == 0)
      return;
    run_stream_mt_(st, o, body, body_plain, src);
    return;
  }
#endif

  for (;;) {
    sz n = fastent_src_read(src);
    if (n == (sz) -1) {
      fastent_message("read error: %s", strerror(errno));
      exit(2);
    }
    if (n == 0) break;
    analyze_fused_(st, body, body_plain, src->stream_buf, n,
                   o->extended, o->binary, o->fold);
  }
}

/*  -eee non-mmap tee: one bounded pass feeds each chunk to the order-0/-ee
    analyzer (analyze_fused_), the LZ77F acc, the linear-complexity acc, the
    Maurer acc, the matrix-rank acc and the permutation-entropy acc, O(chunk +
    grid block).  */
void fastent_run_stream_lz_tee(
    fastent_chunk_state * st, fastent_lz_acc * lz, fastent_bm_acc * bm,
    fastent_maurer_acc * ma, fastent_mrank_acc * mr,
    fastent_perment_acc * pe, const fastent_options * o,
    fastent_analyze_fn fn_byte, fastent_analyze_fn fn_bits,
    fastent_analyze_fn fn_byte_fold, fastent_analyze_fn fn_bits_fold,
    fastent_source * src) {
  fastent_analyze_fn body = o->binary
    ? (o->fold ? fn_bits_fold : fn_bits)
    : (o->fold ? fn_byte_fold : fn_byte);
  fastent_analyze_fn body_plain = (!o->binary && o->fold) ? fn_byte : NULL;

  for (;;) {
    sz n = fastent_src_read(src);
    if (n == (sz) -1) { fastent_message("read error: %s", strerror(errno));  exit(2); }
    if (n == 0) break;
    analyze_fused_(st, body, body_plain, src->stream_buf, n,
                   o->extended, o->binary, o->fold);
    if (lz && fastent_lz_acc_feed(lz, src->stream_buf, n) != 0) { lz->oom = 1;  return; }
    if (bm && fastent_bm_acc_feed(bm, src->stream_buf, n) != 0) { bm->oom = 1;  return; }
    if (ma && fastent_maurer_acc_feed(ma, src->stream_buf, n) != 0) { ma->oom = 1;  return; }
    if (mr && fastent_mrank_acc_feed(mr, src->stream_buf, n) != 0) { mr->oom = 1;  return; }
    if (pe && fastent_perment_acc_feed(pe, src->stream_buf, n) != 0) { pe->oom = 1;  return; }
  }
  if (lz && fastent_lz_acc_flush(lz) != 0) lz->oom = 1;
  if (bm && fastent_bm_acc_flush(bm) != 0) bm->oom = 1;
  if (ma && fastent_maurer_acc_flush(ma) != 0) ma->oom = 1;
  if (mr && fastent_mrank_acc_flush(mr) != 0) mr->oom = 1;
  if (pe && fastent_perment_acc_flush(pe) != 0) pe->oom = 1;
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
  /*  Skip files we can't open rather than aborting the whole walk.  */
  fastent_source src;
  fastent_io_mode io = (fastent_io_mode) c->o->io_mode;
  if (fastent_src_open(&src, path, io) != 0) {
    fastent_message("skip %s: %s", path, strerror(errno));
    return 0;
  }

  fastent_chunk_state st;
  fastent_chunk_state_init(&st);
  if (c->o->extended >= 2 && !c->o->binary) {
    st.bigram = fastent_bigram_alloc();
    st.dg_u32 = fastent_dg_u32_alloc();
    if (!st.bigram || !st.dg_u32) { fastent_message("out of memory"); exit(2); }
  }
  /*  Grid estimators by speed band (matches fastent.c::main): LZ77F and
      perment ride -e; BM / Maurer / mrank stay at -eee.  */
  const int do_lzp = (c->o->extended >= 1);
  const int do_bmm = (c->o->extended >= 3);
  /*  -H per-file plots: human side output in walk order before the
      sorted rows.  JSON stays pure (no plot), as single-file does.  */
  const int do_plot = do_lzp && c->o->histogram && !c->o->json;
  fastent_lz_acc * lz = NULL;
  fastent_bm_acc * bm = NULL;
  fastent_maurer_acc * ma = NULL;
  fastent_mrank_acc * mr = NULL;
  fastent_perment_acc * pe = NULL;
  if (do_lzp) {
    lz = (fastent_lz_acc *) malloc(sizeof *lz);
    pe = (fastent_perment_acc *) malloc(sizeof *pe);
    if (!lz || !pe) { fastent_message("out of memory");  exit(2); }
    fastent_lz_acc_init(lz, 0);
    fastent_perment_acc_init(pe, 0);
  }
  if (do_bmm) {
    bm = (fastent_bm_acc *) malloc(sizeof *bm);
    ma = (fastent_maurer_acc *) malloc(sizeof *ma);
    mr = (fastent_mrank_acc *) malloc(sizeof *mr);
    if (!bm || !ma || !mr) { fastent_message("out of memory");  exit(2); }
    fastent_bm_acc_init(bm, 0);
    fastent_maurer_acc_init(ma, 0);
    fastent_mrank_acc_init(mr, 0);
  }

  if (do_lzp && src.kind == FASTENT_SRC_MMAP) {
    fastent_run_mmap(&st, c->o, c->fn_byte, c->fn_bits,
                     c->fn_byte_fold, c->fn_bits_fold,
                     (const u8 *) src.map, src.size);
    fastent_run_lz(lz, c->o, &src);
    fastent_run_perment(pe, c->o, &src);
    if (do_bmm) {
      fastent_run_bm(bm, c->o, &src);
      fastent_run_maurer(ma, c->o, &src);
      fastent_run_mrank(mr, c->o, &src);
    }
  } else if (do_lzp) {
    fastent_run_stream_lz_tee(&st, lz,
                              do_bmm ? bm : NULL,
                              do_bmm ? ma : NULL,
                              do_bmm ? mr : NULL,
                              pe, c->o,
                              c->fn_byte, c->fn_bits,
                              c->fn_byte_fold, c->fn_bits_fold, &src);
  } else if (src.kind == FASTENT_SRC_MMAP) {
    fastent_run_mmap(&st, c->o, c->fn_byte, c->fn_bits,
                     c->fn_byte_fold, c->fn_bits_fold,
                     (const u8 *) src.map, src.size);
  } else {
    fastent_run_stream(&st, c->o, c->fn_byte, c->fn_bits,
                       c->fn_byte_fold, c->fn_bits_fold, &src);
  }
  fastent_result r;
  fastent_finalize(&st, c->o->binary, &r);

  if (do_lzp) {
    if (lz->oom || pe->oom
        || (do_bmm && (bm->oom || ma->oom || mr->oom))) {
      fastent_message("out of memory"); exit(2);
    }
    if (do_plot) {
      r.lz = fastent_lz77f_tables_alloc();
      if (!r.lz) { fastent_message("out of memory"); exit(2); }
    }
    fastent_lz_finalize(lz, st.total_bytes, &r);
    fastent_perment_finalize(pe, st.total_bytes, &r);
    fastent_lz_acc_free(lz);  free(lz);
    fastent_perment_acc_free(pe);  free(pe);
    if (do_bmm) {
      fastent_bm_finalize(bm, st.total_bytes, &r);
      fastent_maurer_finalize(ma, st.total_bytes, &r);
      fastent_mrank_finalize(mr, st.total_bytes, &r);
      fastent_bm_acc_free(bm);  free(bm);
      fastent_maurer_acc_free(ma);  free(ma);
      fastent_mrank_acc_free(mr);  free(mr);
    }
    if (do_plot) {
      printf("%s\n", path);
      fastent_print_histogram(&r, c->o);
      fastent_lz77f_tables_free(r.lz);
      r.lz = NULL;                        /*  no per-row tables  */
    }
  }

  fastent_src_close(&src);
  fastent_bigram_free(st.bigram);
  fastent_dg_u32_free(st.dg_u32);

  if (c->count == c->cap) {
    sz nc = c->cap ? c->cap * 2 : 32;
    fastent_recursive_row * nr =
      (fastent_recursive_row *) realloc(c->rows, nc * sizeof (*nr));
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

int fastent_run_recursive(
    const char * root, const fastent_options * o,
    fastent_analyze_fn fn_byte, fastent_analyze_fn fn_bits,
    fastent_analyze_fn fn_byte_fold, fastent_analyze_fn fn_bits_fold,
    fastent_recursive_row ** out_rows, sz * out_n) {
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
  sz i;
  if (!rows) return;
  for (i = 0; i < n; i++) free(rows[i].path);
  free(rows);
}

static int g_sort_by_  = 0;
static int g_sort_desc_ = 0;

static f64 row_key_(const fastent_recursive_row * r) {
  switch ((fastent_sort_by) g_sort_by_) {
    case FASTENT_SORT_SAMPLES:    return (f64) r->result.total_samples;
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
    case FASTENT_SORT_DISTINCT:   return (f64) r->result.distinct;
    case FASTENT_SORT_BIT_BIAS:   return r->result.bit_bias_max;
    case FASTENT_SORT_COND_ENTROPY: return r->result.conditional_entropy;
    case FASTENT_SORT_MUTUAL_INFO:  return r->result.mutual_information;
    case FASTENT_SORT_LZ_DEVIATION: return r->result.lz_deviation;
    case FASTENT_SORT_LZ_CR:        return r->result.lz_cr_excess;
    case FASTENT_SORT_LZ_MATCH_COV: return r->result.lz_match_cov;
    case FASTENT_SORT_BM_DEVIATION: return r->result.bm_deviation;
    case FASTENT_SORT_BM_MEAN_LC:   return r->result.bm_mean_lc;
    case FASTENT_SORT_MAURER_DEVIATION: return r->result.maurer_dev;
    case FASTENT_SORT_MRANK_DEV:    return r->result.mrank_dev;
    case FASTENT_SORT_PERMENT_DEV:  return r->result.perment_deviation;
    default:                      return 0.0;
  }
}

static int cmp_(const void * a, const void * b) {
  const fastent_recursive_row * ra = (const fastent_recursive_row *) a;
  const fastent_recursive_row * rb = (const fastent_recursive_row *) b;
  int rc;
  if (g_sort_by_ == FASTENT_SORT_PATH) { rc = strcmp(ra->path, rb->path); } else {
    f64 ka = row_key_(ra);
    f64 kb = row_key_(rb);
    rc = (ka < kb) ? -1 : (ka > kb) ? 1 : 0;
  }
  return g_sort_desc_ ? -rc : rc;
}

void fastent_rows_sort(
    fastent_recursive_row * rows, sz n, const fastent_options * o) {
  if (o->sort_by == FASTENT_SORT_NONE || n < 2) return;
  g_sort_by_  = o->sort_by;
  g_sort_desc_ = o->sort_desc;
  qsort(rows, n, sizeof *rows, cmp_);
}
