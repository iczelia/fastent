/*  fastent: Win32-backed thread pool.  Uses CreateThread plus Vista's
    SRWLOCK + CONDITION_VARIABLE for the same generation-counter wait
    protocol as the POSIX backend.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "port-thread.h"

#if defined(FASTENT_HAVE_THREADS) && defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

static i32                g_n_threads = 0;
static i32                g_pending   = 0;
static HANDLE *           g_workers   = NULL;
static SRWLOCK            g_m         = SRWLOCK_INIT;
static CONDITION_VARIABLE g_work_ready = CONDITION_VARIABLE_INIT;
static CONDITION_VARIABLE g_work_done  = CONDITION_VARIABLE_INIT;

static fastent_parfor_fn g_fn  = NULL;
static void *        g_ctx = NULL;
static sz            g_n   = 0;
static i32           g_busy = 0;
static u64           g_gen  = 0;

struct worker_args { i32 k; };

static DWORD WINAPI worker_main(LPVOID arg) {
  i32 k = ((struct worker_args *) arg)->k;
  free(arg);

  u64 last_gen = 0;
  for (;;) {
    AcquireSRWLockExclusive(&g_m);
    while (g_gen == last_gen)
      SleepConditionVariableSRW(&g_work_ready, &g_m, INFINITE, 0);
    last_gen = g_gen;
    fastent_parfor_fn fn = g_fn;
    void * ctx = g_ctx;
    sz     n   = g_n;
    i32    T   = g_n_threads;
    ReleaseSRWLockExclusive(&g_m);

    sz start = (sz) k * n / (sz) T;
    sz end   = (sz)(k + 1) * n / (sz) T;
    for (sz i = start; i < end; i++) fn(i, ctx);

    AcquireSRWLockExclusive(&g_m);
    if (--g_busy == 0) WakeAllConditionVariable(&g_work_done);
    ReleaseSRWLockExclusive(&g_m);
  }
  return 0;
}

static void lazy_init(void) {
  if (g_n_threads != 0) return;
  i32 T = g_pending > 0 ? g_pending : 1;
  if (T < 1) T = 1;
  if (T > 1) {
    g_workers = (HANDLE *) malloc((sz) T * sizeof(HANDLE));
    if (!g_workers) { g_n_threads = 1; return; }
    Fk(T,
       struct worker_args * a = (struct worker_args *) malloc(sizeof(*a));
       if (!a) { g_n_threads = 1;  return; }
       a->k = k;
       g_workers[k] = CreateThread(NULL, 0, worker_main, a, 0, NULL);
       if (g_workers[k] == NULL) {
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

  AcquireSRWLockExclusive(&g_m);
  while (g_busy > 0)
    SleepConditionVariableSRW(&g_work_done, &g_m, INFINITE, 0);
  g_fn = fn;  g_ctx = ctx;  g_n = n;
  g_busy = g_n_threads;
  g_gen++;
  WakeAllConditionVariable(&g_work_ready);
  while (g_busy > 0)
    SleepConditionVariableSRW(&g_work_done, &g_m, INFINITE, 0);
  ReleaseSRWLockExclusive(&g_m);
}

struct fastent_mutex { SRWLOCK l; };

fastent_mutex * fastent_mutex_create(void) {
  fastent_mutex * x = (fastent_mutex *) malloc(sizeof(*x));
  if (!x) return NULL;
  InitializeSRWLock(&x->l);
  return x;
}
void fastent_mutex_lock(fastent_mutex * m)   { AcquireSRWLockExclusive(&m->l); }
void fastent_mutex_unlock(fastent_mutex * m) { ReleaseSRWLockExclusive(&m->l); }
void fastent_mutex_destroy(fastent_mutex * m) { free(m); }

#endif
