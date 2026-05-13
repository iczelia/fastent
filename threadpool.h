/*  fastent  --  tiny static-partition pthread pool.
    Copyright (C) 2026 Kamila Szewczyk.  GPLv3 only (see COPYING).  */

#ifndef FASTENT_THREADPOOL_H
#define FASTENT_THREADPOOL_H

#include "common.h"

#ifdef FASTENT_HAVE_PTHREAD

/*  Set the worker count for the next/first parallel_for. n <= 1 disables
    parallel execution (everything runs in the calling thread).  */
void fastent_set_num_threads(int n);

/*  Returns the configured worker count (1 if disabled or unset).  */
int  fastent_num_threads(void);

/*  Synchronously run fn(i, ctx) for i in [0, n). Workers are spawned
    on first call; subsequent calls reuse them.  */
typedef void (* fastent_parfor_fn)(sz, void *);
void fastent_parallel_for(sz n, fastent_parfor_fn fn, void * ctx);

#endif

#endif
