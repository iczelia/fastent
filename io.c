/*  fastent: I/O layer.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "io.h"   /*  Pulls common.h with feature macros (must be first).  */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef HAVE_SYS_STAT_H
  #include <sys/stat.h>
#endif
#ifdef HAVE_SYS_MMAN_H
  #include <sys/mman.h>
#endif

#define FASTENT_STREAM_BUF (2u * 1024u * 1024u)  /*  2 MiB per read()  */

#ifndef MADV_SEQUENTIAL
  #define MADV_SEQUENTIAL 0
#endif
#ifndef MADV_WILLNEED
  #define MADV_WILLNEED 0
#endif
#ifndef POSIX_FADV_SEQUENTIAL
  #define POSIX_FADV_SEQUENTIAL 0
#endif

static int alloc_stream_buf(fastent_source * s) {
  void * p = NULL;
#if HAVE_POSIX_MEMALIGN
  if (posix_memalign(&p, 4096, FASTENT_STREAM_BUF) != 0) p = NULL;
#else
  p = malloc(FASTENT_STREAM_BUF);
#endif
  if (!p) return -1;
  s->stream_buf = (u8 *) p;
  s->stream_buf_cap = FASTENT_STREAM_BUF;
  return 0;
}

int fastent_src_open(fastent_source * s, const char * path, int no_mmap) {
  memset(s, 0, sizeof(*s));
  s->kind = FASTENT_SRC_NONE;
  s->fd = -1;
  s->opened_fd = 0;

  if (!path) {
    s->fd = 0;  /*  STDIN_FILENO  */
    s->kind = FASTENT_SRC_STREAM;
    if (alloc_stream_buf(s) < 0) return -1;
    return 0;
  }

  int fd = open(path, O_RDONLY
#ifdef O_BINARY
                          | O_BINARY
#endif
                );
  if (fd < 0) return -1;
  s->opened_fd = 1;
  s->fd = fd;

#ifdef HAVE_SYS_STAT_H
  struct stat st;
  if (!no_mmap && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
#ifdef HAVE_MMAP
    /*  No MAP_POPULATE: lazy faulting lets the workers parallelise
        the page faults and overlap them with the SIMD body.  The
        MADV_SEQUENTIAL / MADV_WILLNEED hints below still trigger
        kernel read-ahead so cold-cache throughput is unaffected.  */
    void * p = mmap(NULL, (size_t) st.st_size,
                    PROT_READ, MAP_PRIVATE, fd, 0);
    if (p != MAP_FAILED) {
      s->kind = FASTENT_SRC_MMAP;
      s->map  = p;
      s->size = (u64) st.st_size;
#ifdef HAVE_MADVISE
      madvise(p, (size_t) st.st_size,
              MADV_SEQUENTIAL | MADV_WILLNEED);
      /*  Promote to transparent huge pages where available; cuts TLB
          misses by 512x at 4 KiB-page granularity. Ignored on kernels
          without THP support.  */
  #ifdef MADV_HUGEPAGE
      madvise(p, (size_t) st.st_size, MADV_HUGEPAGE);
  #endif
#endif
#ifdef HAVE_POSIX_FADVISE
      posix_fadvise(fd, 0, st.st_size, POSIX_FADV_SEQUENTIAL);
#endif
      return 0;
    }
#endif
  }
#endif

  s->kind = FASTENT_SRC_STREAM;
  if (alloc_stream_buf(s) < 0) {
    close(fd);  s->fd = -1;  s->opened_fd = 0;
    return -1;
  }
  return 0;
}

sz fastent_src_read(fastent_source * s) {
  if (s->kind != FASTENT_SRC_STREAM) return 0;
  sz off = 0;
  while (off < s->stream_buf_cap) {
    ssize_t n = read(s->fd, s->stream_buf + off, s->stream_buf_cap - off);
    if (n < 0)  { if (errno == EINTR) continue;  return (sz) -1; }
    if (n == 0) break;
    off += (sz) n;
  }
  return off;
}

void fastent_src_close(fastent_source * s) {
  if (!s) return;
  if (s->kind == FASTENT_SRC_MMAP && s->map) {
#ifdef HAVE_MMAP
    munmap(s->map, (size_t) s->size);
#endif
    s->map = NULL;
  }
  if (s->stream_buf) { free(s->stream_buf);  s->stream_buf = NULL; }
  if (s->opened_fd && s->fd >= 0) close(s->fd);
  s->fd = -1;
  s->kind = FASTENT_SRC_NONE;
}
