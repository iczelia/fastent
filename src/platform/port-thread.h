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
