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
  const u8 * data;  /*  resident buffer (mmap)  */
  u64 size;
  u64 nblk;  /*  number of 4 MiB grid blocks  */
  FASTENT_G_ACC * accs;  /*  one per worker  */
  i32 nthreads;
  volatile int oom;
} FASTENT_G_FN(_mmap_ctx);

/*  Worker w scores grid blocks w, w+T, w+2T, ...  */
static void FASTENT_G_FN(_mmap_worker_)(sz w, void * vctx) {
  FASTENT_G_FN(_mmap_ctx) * c = (FASTENT_G_FN(_mmap_ctx) *) vctx;
  FASTENT_G_ACC * a = &c->accs[w];
  FASTENT_G_ACC * blk = (FASTENT_G_ACC *) malloc(sizeof *blk);
  u64 g;
  if (!blk) { c->oom = 1;  return; }
  /*  Reuse the block accumulator and its scratch buffers across this
      worker's blocks.  */
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

/*  Analyse mapped input, distributing whole grid blocks among workers.  */
static int FASTENT_G_FN(_run_resident_)(
    FASTENT_G_ACC * acc, const fastent_options * o, const u8 * data,
    u64 size) {
  if (size == 0) return 0;

#ifdef FASTENT_HAVE_THREADS
  i32 k;
  u64 nblk = (size + FASTENT_LZ_GRID_U64 - 1) / FASTENT_LZ_GRID_U64;
  if (o->threads > 1 && nblk > 1) {
    i32 N = o->threads;
    if ((u64) N > nblk) N = (i32) nblk;
    fastent_set_num_threads(N);
    FASTENT_G_ACC * accs =
      (FASTENT_G_ACC *) calloc((sz) N, sizeof *accs);
    if (!accs) return -1;
    Fk(N, FASTENT_G_ACCFN(_acc_init)(&accs[k], 0));
    FASTENT_G_FN(_mmap_ctx) c;
    c.data = data;  c.size = size;  c.nblk = nblk;
    c.accs = accs;  c.nthreads = N;  c.oom = 0;
    fastent_parallel_for((sz) N, FASTENT_G_FN(_mmap_worker_), &c);
    int rc = c.oom ? -1 : 0;
    /*  Merge accumulators in worker order.  */
    Fk(N, FASTENT_G_ACCFN(_acc_merge)(acc, &accs[k]);
         FASTENT_G_ACCFN(_acc_free)(&accs[k]));
    free(accs);
    if (acc->oom) rc = -1;
    return rc;
  }
#else
  (void) o;
#endif

  /*  The serial accumulator resets its state at each 4 MiB boundary.  */
  if (FASTENT_G_ACCFN(_acc_feed)(acc, data, (sz) size) != 0) return -1;
  if (FASTENT_G_ACCFN(_acc_flush)(acc) != 0) return -1;
  return 0;
}

void FASTENT_G_CAT(fastent_run_, FASTENT_GRID_EST)(
    FASTENT_G_ACC * acc, const fastent_options * o, fastent_source * src) {
  if (FASTENT_G_FN(_run_resident_)(acc, o, (const u8 *) src->map,
                                  src->size) != 0)
    acc->oom = 1;
}

#undef FASTENT_GRID_EST
#undef FASTENT_G_CAT_
#undef FASTENT_G_CAT
#undef FASTENT_G_ACC
#undef FASTENT_G_FN
#undef FASTENT_G_ACCFN
