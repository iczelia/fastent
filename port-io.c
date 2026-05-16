/*  fastent: portable I/O helpers.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "port-io.h"

#include <stdint.h>
#include <stdlib.h>

int fastent_io_alloc_aligned(void ** out_raw, void ** out_user, sz cap) {
  void * raw  = NULL;
  void * user = NULL;
#if HAVE_POSIX_MEMALIGN
  if (posix_memalign(&raw, FASTENT_STREAM_ALIGN, cap) != 0) raw = NULL;
  user = raw;
#elif HAVE_ALIGNED_ALLOC
  raw = aligned_alloc(FASTENT_STREAM_ALIGN, cap);
  user = raw;
#else
  raw = malloc(cap + FASTENT_STREAM_ALIGN);
  if (raw) {
    uintptr_t a = ((uintptr_t) raw + (FASTENT_STREAM_ALIGN - 1))
                  & ~(uintptr_t)(FASTENT_STREAM_ALIGN - 1);
    user = (void *) a;
  }
#endif
  if (!raw) return -1;
  *out_raw  = raw;
  *out_user = user;
  return 0;
}
