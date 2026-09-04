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

/*  Included from runner.c once per estimator.  */

#ifndef FASTENT_GRID_EST
#error "runner-grid.h requires FASTENT_GRID_EST"
#endif

#define FASTENT_G_CAT_(a, b) a##b
#define FASTENT_G_CAT(a, b)  FASTENT_G_CAT_(a, b)
#define FASTENT_G_ACC        FASTENT_G_CAT(fastent_, \
                               FASTENT_G_CAT(FASTENT_GRID_EST, _acc))
#define FASTENT_G_FN(suf)    FASTENT_G_CAT(FASTENT_GRID_EST, suf)
#define FASTENT_G_ACCFN(suf) FASTENT_G_CAT(fastent_, \
                               FASTENT_G_CAT(FASTENT_GRID_EST, suf))

#ifdef FASTENT_HAVE_THREADS
typedef struct {
  const u8 *      data;     /*  resident buffer (mmap)  */
  u64             size;
  u64             nblk;     /*  number of 4 MiB grid blocks  */
  FASTENT_G_ACC * accs;     /*  one per worker  */
  i32             nthreads;
  volatile int    oom;
} FASTENT_G_FN(_mmap_ctx);

/*  Worker w scores grid blocks w, w+T, w+2T, ...  */
static void FASTENT_G_FN(_mmap_worker_)(sz w, void * vctx) {
  FASTENT_G_FN(_mmap_ctx) * c = (FASTENT_G_FN(_mmap_ctx) *) vctx;
  FASTENT_G_ACC * a = &c->accs[w];
  FASTENT_G_ACC * blk = (FASTENT_G_ACC *) malloc(sizeof *blk);
  u64 g;
  if (!blk) { c->oom = 1;  return; }
  /*  One acc reused across this worker's blocks: init once, reset per
      block (keeps the lazily-grown scratch), free once.  */
  FASTENT_G_ACCFN(_acc_init)(blk, 0);
  for (g = (u64) w; g < c->nblk; g += (u64) c->nthreads) {
    u64 off = g * FASTENT_LZ_GRID_U64;
    u64 len = c->size - off;
    if (len > FASTENT_LZ_GRID_U64) len = FASTENT_LZ_GRID_U64;
    FASTENT_G_ACCFN(_acc_reset)(blk, off);
    if (FASTENT_G_ACCFN(_acc_feed)(blk, c->data + off, (sz) len) != 0 ||
        FASTENT_G_ACCFN(_acc_flush)(blk) != 0) {
      c->oom = 1;  FASTENT_G_ACCFN(_acc_free)(blk);  free(blk);  return;
    }
    FASTENT_G_ACCFN(_acc_merge)(a, blk);
  }
  FASTENT_G_ACCFN(_acc_free)(blk);
  free(blk);
}
#endif

/*  Resident-buffer path (mmap): grid-block parallel, else serial.  */
static int FASTENT_G_FN(_run_resident_)(
    FASTENT_G_ACC * acc, const fastent_options * o, const u8 * data,
    u64 size) {
  i32 k;
  if (size == 0) return 0;

#ifdef FASTENT_HAVE_THREADS
  u64 nblk = (size + FASTENT_LZ_GRID_U64 - 1) / FASTENT_LZ_GRID_U64;
  if (o->threads > 1 && nblk > 1) {
    i32 N = o->threads;
    if ((u64) N > nblk) N = (i32) nblk;
    fastent_set_num_threads(N);
    FASTENT_G_ACC * accs =
      (FASTENT_G_ACC *) calloc((sz) N, sizeof (*accs));
    if (!accs) return -1;
    Fk(N, FASTENT_G_ACCFN(_acc_init)(&accs[k], 0));
    FASTENT_G_FN(_mmap_ctx) c;
    c.data = data;  c.size = size;  c.nblk = nblk;
    c.accs = accs;  c.nthreads = N;  c.oom = 0;
    fastent_parallel_for((sz) N, FASTENT_G_FN(_mmap_worker_), &c);
    int rc = c.oom ? -1 : 0;
    /*  Fixed worker-index merge order (sum is order-independent; the
        fixed order keeps the reduction itself deterministic).  */
    Fk(N, FASTENT_G_ACCFN(_acc_merge)(acc, &accs[k]);
         FASTENT_G_ACCFN(_acc_free)(&accs[k]));
    free(accs);
    if (acc->oom) rc = -1;
    return rc;
  }
#else
  (void) o;
#endif

  /*  Serial: one acc fed the whole resident range; its grid logic
      resets per-block state at every absolute 4 MiB line.  */
  if (FASTENT_G_ACCFN(_acc_feed)(acc, data, (sz) size) != 0) return -1;
  if (FASTENT_G_ACCFN(_acc_flush)(acc) != 0) return -1;
  return 0;
}

#ifdef FASTENT_HAVE_THREADS
typedef struct {
  int             fd;
  const char *    path;
  const u64 *     bounds;   /*  N+1 grid-aligned slab edges  */
  FASTENT_G_ACC * accs;
  volatile int    failed;   /*  uring unavailable  */
  volatile int    oom;
} FASTENT_G_FN(_uring_ctx);

/*  Each worker owns a grid-aligned slab, so every 4 MiB block is
    scored whole by one worker and matches the serial reference.  */
static void FASTENT_G_FN(_uring_worker_)(sz k, void * vctx) {
  FASTENT_G_FN(_uring_ctx) * c = (FASTENT_G_FN(_uring_ctx) *) vctx;
  u64 start = c->bounds[k], end = c->bounds[k + 1];
  FASTENT_G_ACC * a = &c->accs[k];
  if (end <= start) return;
  fastent_uring_slab * r =
    fastent_uring_slab_open(c->fd, c->path, start, end - start);
  if (!r) { c->failed = 1;  return; }
  for (;;) {
    const u8 * blk = NULL;
    sz n = fastent_uring_slab_next(r, &blk);
    if (n == (sz) -1) { c->failed = 1;  fastent_uring_slab_close(r);  return; }
    if (n == 0) break;
    if (FASTENT_G_ACCFN(_acc_feed)(a, blk, n) != 0) {
      c->oom = 1;  fastent_uring_slab_close(r);  return;
    }
  }
  fastent_uring_slab_close(r);
  if (FASTENT_G_ACCFN(_acc_flush)(a) != 0) c->oom = 1;
}

/*  Returns 0 on success, -1 if io_uring is unavailable (caller falls
    back to the serial stream feed), -2 on OOM.  */
static int FASTENT_G_FN(_run_uring_)(
    FASTENT_G_ACC * acc, const fastent_options * o, fastent_source * src) {
  u64 size = src->size;
  i32 k;
  if (size == 0) return 0;
  i32 N = o->threads;
  if (N < 1) N = 1;
  u64 nblk = (size + FASTENT_LZ_GRID_U64 - 1) / FASTENT_LZ_GRID_U64;
  if ((u64) N > nblk) N = (i32) nblk;
  fastent_set_num_threads(N);

  u64 * bounds = (u64 *) malloc((sz) (N + 1) * sizeof (u64));
  FASTENT_G_ACC * accs =
    (FASTENT_G_ACC *) calloc((sz) N, sizeof (*accs));
  if (!bounds || !accs) { free(bounds);  free(accs);  return 0; }
  bounds[0] = 0;  bounds[N] = size;
  for (k = 1; k < N; k++) {
    u64 blk = (u64) ((f64) nblk * (f64) k / (f64) N);
    u64 b = blk * FASTENT_LZ_GRID_U64;
    if (b > size) b = size;
    bounds[k] = b;
  }
  Fk(N, FASTENT_G_ACCFN(_acc_init)(&accs[k], bounds[k]));

  FASTENT_G_FN(_uring_ctx) c;
  c.fd = src->fd;  c.path = o->path;  c.bounds = bounds;
  c.accs = accs;  c.failed = 0;  c.oom = 0;
  fastent_parallel_for((sz) N, FASTENT_G_FN(_uring_worker_), &c);

  int rc;
  if (c.failed) {
    rc = -1;                              /*  graceful fallback  */
  } else {
    Fk(N, FASTENT_G_ACCFN(_acc_merge)(acc, &accs[k]));
    rc = (c.oom || acc->oom) ? -2 : 0;
  }
  Fk(N, FASTENT_G_ACCFN(_acc_free)(&accs[k]));
  free(accs);  free(bounds);
  return rc;
}
#endif

void FASTENT_G_CAT(fastent_run_, FASTENT_GRID_EST)(
    FASTENT_G_ACC * acc, const fastent_options * o, fastent_source * src) {
  int rc = 0;
  if (src->kind == FASTENT_SRC_MMAP) {
    rc = FASTENT_G_FN(_run_resident_)(acc, o, (const u8 *) src->map,
                                      src->size);
  } else {
#ifdef FASTENT_HAVE_THREADS
    if (src->kind == FASTENT_SRC_URING && src->fd >= 0 && src->size > 0
        && o->threads != 1) {
      int u = FASTENT_G_FN(_run_uring_)(acc, o, src);
      if (u == 0)  return;
      if (u == -2) { acc->oom = 1;  return; }
      /*  u == -1: uring absent, fall through to the serial feed.  */
    }
#endif
    /*  Stream/pipe: read sequentially, feed one acc in absolute order.
        It buffers whole grid blocks, so the parse matches the ref.  */
    for (;;) {
      sz n = fastent_src_read(src);
      if (n == (sz) -1) { fastent_message("read error: %s", strerror(errno));  exit(2); }
      if (n == 0) break;
      if (FASTENT_G_ACCFN(_acc_feed)(acc, src->stream_buf, n) != 0) { acc->oom = 1;  return; }
    }
    if (FASTENT_G_ACCFN(_acc_flush)(acc) != 0) acc->oom = 1;
  }
  if (rc != 0) acc->oom = 1;
}

#undef FASTENT_GRID_EST
#undef FASTENT_G_CAT_
#undef FASTENT_G_CAT
#undef FASTENT_G_ACC
#undef FASTENT_G_FN
#undef FASTENT_G_ACCFN
