/*  fastent: thread pool port.  pthread on POSIX, Win32 threads on
    Windows Vista+.  FASTENT_HAVE_THREADS is set by configure when a
    backend is available.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_PORT_THREAD_H
#define FASTENT_PORT_THREAD_H

#include "common.h"

#ifdef FASTENT_HAVE_THREADS

void fastent_set_num_threads(int n);
int  fastent_num_threads(void);

typedef void (* fastent_parfor_fn)(sz, void *);
void fastent_parallel_for(sz n, fastent_parfor_fn fn, void * ctx);

/*  Minimal opaque mutex (pthread_mutex / SRWLOCK) for the SPMC
    stream/uring pipeline's serialized read + result append.  */
typedef struct fastent_mutex fastent_mutex;
fastent_mutex * fastent_mutex_create(void);
void fastent_mutex_lock(fastent_mutex * m);
void fastent_mutex_unlock(fastent_mutex * m);
void fastent_mutex_destroy(fastent_mutex * m);

#endif

#endif
