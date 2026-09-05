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
#include "port-thread.h"

#if defined(FASTENT_HAVE_THREADS) && !defined(_WIN32)

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

static i32             g_n_threads = 0;
static i32             g_pending   = 0;
static pthread_t *     g_workers   = NULL;
static pthread_mutex_t g_m         = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_work_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_work_done  = PTHREAD_COND_INITIALIZER;

static fastent_parfor_fn g_fn  = NULL;
static void *        g_ctx = NULL;
static sz            g_n   = 0;
static i32           g_busy = 0;
static u64           g_gen  = 0;

struct worker_args { i32 k; };

static void * worker_main(void * arg) {
  i32 k = ((struct worker_args *) arg)->k;
  sz i;
  free(arg);

  /*  No explicit affinity; let the scheduler place threads.  */

  u64 last_gen = 0;
  for (;;) {
    pthread_mutex_lock(&g_m);
    while (g_gen == last_gen) pthread_cond_wait(&g_work_ready, &g_m);
    last_gen = g_gen;
    fastent_parfor_fn fn = g_fn;
    void * ctx = g_ctx;
    sz     n   = g_n;
    i32    T   = g_n_threads;
    pthread_mutex_unlock(&g_m);

    sz start = (sz) k * n / (sz) T;
    sz end   = (sz) (k + 1) * n / (sz) T;
    for (i = start; i < end; i++) fn(i, ctx);

    pthread_mutex_lock(&g_m);
    if (--g_busy == 0) pthread_cond_broadcast(&g_work_done);
    pthread_mutex_unlock(&g_m);
  }
  return NULL;
}

static void lazy_init(void) {
  i32 k;
  if (g_n_threads != 0) return;
  i32 T = g_pending > 0 ? g_pending : 1;
  if (T < 1) T = 1;
  if (T > 1) {
    g_workers = (pthread_t *) malloc((sz) T * sizeof (pthread_t));
    if (!g_workers) { g_n_threads = 1;  return; }
    /*  Partial failure: keep the k workers already created and run with that
        count so g_n_threads matches the live pool instead of orphaning live
        workers behind a serial g_n_threads == 1 (k < 2 -> serial).  */
    Fk(T,
      struct worker_args * a = (struct worker_args *) malloc(sizeof *a);
      if (!a) { g_n_threads = (k >= 2) ? k : 1;  return; }
      a->k = k;
      if (pthread_create(&g_workers[k], NULL, worker_main, a) != 0) {
        free(a);  g_n_threads = (k >= 2) ? k : 1;  return;
      });
  }
  g_n_threads = T;
}

void fastent_set_num_threads(int n) {
  if (g_n_threads != 0) return;
  g_pending = n > 0 ? n : 0;
}

int fastent_num_threads(void) {
  return g_n_threads > 0 ? g_n_threads : (g_pending > 0 ? g_pending : 1);
}

void fastent_parallel_for(sz n, fastent_parfor_fn fn, void * ctx) {
  sz i;
  if (n == 0) return;
  if (g_n_threads == 0) lazy_init();
  if (n == 1 || g_n_threads <= 1) {
    Fi(n, fn(i, ctx));
    return;
  }

  pthread_mutex_lock(&g_m);
  while (g_busy > 0) pthread_cond_wait(&g_work_done, &g_m);
  g_fn = fn;  g_ctx = ctx;  g_n = n;
  g_busy = g_n_threads;
  g_gen++;
  pthread_cond_broadcast(&g_work_ready);
  while (g_busy > 0) pthread_cond_wait(&g_work_done, &g_m);
  pthread_mutex_unlock(&g_m);
}

struct fastent_mutex { pthread_mutex_t m; };

fastent_mutex * fastent_mutex_create(void) {
  fastent_mutex * x = (fastent_mutex *) malloc(sizeof *x);
  if (!x) return NULL;
  if (pthread_mutex_init(&x->m, NULL) != 0) { free(x);  return NULL; }
  return x;
}
void fastent_mutex_lock(fastent_mutex * m)   { pthread_mutex_lock(&m->m); }
void fastent_mutex_unlock(fastent_mutex * m) { pthread_mutex_unlock(&m->m); }
void fastent_mutex_destroy(fastent_mutex * m) {
  if (!m) return;
  pthread_mutex_destroy(&m->m);  free(m);
}

#endif
