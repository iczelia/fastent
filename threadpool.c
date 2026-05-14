/*  fastent: threadpool implementation. Built only when FASTENT_THREADS is on.
    Static partition; workers wait on a condition variable for a generation
    counter to advance.  */

#include "threadpool.h"  /*  Pulls common.h with feature macros.  */

#ifdef FASTENT_HAVE_PTHREAD

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

static int             g_n_threads = 0;
static int             g_pending   = 0;
static pthread_t *     g_workers   = NULL;
static pthread_mutex_t g_m         = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_work_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_work_done  = PTHREAD_COND_INITIALIZER;

static fastent_parfor_fn g_fn  = NULL;
static void *        g_ctx = NULL;
static sz            g_n   = 0;
static int           g_busy = 0;
static u64           g_gen  = 0;

struct worker_args { int k; };

static void * worker_main(void * arg) {
  int k = ((struct worker_args *) arg)->k;
  free(arg);

  /*  No explicit affinity. Empirically on Zen 3 / CCD architectures
      pinning N=ncpu/2..ncpu workers to fixed CPU IDs straddles SMT
      pairs and CCD boundaries and slightly hurts -j auto throughput.
      Let the scheduler place threads.  */

  u64 last_gen = 0;
  for (;;) {
    pthread_mutex_lock(&g_m);
    while (g_gen == last_gen) pthread_cond_wait(&g_work_ready, &g_m);
    last_gen = g_gen;
    fastent_parfor_fn fn = g_fn;
    void * ctx = g_ctx;
    sz     n   = g_n;
    int    T   = g_n_threads;
    pthread_mutex_unlock(&g_m);

    sz start = (sz) k * n / (sz) T;
    sz end   = (sz)(k + 1) * n / (sz) T;
    for (sz i = start; i < end; i++) fn(i, ctx);

    pthread_mutex_lock(&g_m);
    if (--g_busy == 0) pthread_cond_broadcast(&g_work_done);
    pthread_mutex_unlock(&g_m);
  }
  return NULL;
}

static void lazy_init(void) {
  if (g_n_threads != 0) return;
  int T = g_pending > 0 ? g_pending : 1;
  if (T < 1) T = 1;
  if (T > 1) {
    g_workers = (pthread_t *) malloc((size_t) T * sizeof(pthread_t));
    if (!g_workers) { g_n_threads = 1; return; }
    Fk(T,
       struct worker_args * a = (struct worker_args *) malloc(sizeof(*a));
       if (!a) { g_n_threads = 1;  return; }
       a->k = k;
       if (pthread_create(&g_workers[k], NULL, worker_main, a) != 0) {
         free(a);  g_n_threads = 1;  return;
       })
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
  if (n == 0) return;
  if (g_n_threads == 0) lazy_init();
  if (n == 1 || g_n_threads <= 1) {
    for (sz i = 0; i < n; i++) fn(i, ctx);
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

#endif
