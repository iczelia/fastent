/*  fastent: I/O layer.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "fastent-io.h"   /*  Pulls common.h with feature macros (must be first).  */

#include <errno.h>
#include <stdint.h>
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

/*  Windows MSVCRT's open(2) re-narrows the path argument through the
    active code page on its way to NtCreateFile, which breaks any path
    whose codepoints don't round-trip through CP_ACP.  Route the
    open, close and read calls through the fastent_win32_* helpers on
    Windows; on POSIX hosts they're just the libc names.  We also
    need this indirection because -std=c99 sets __STRICT_ANSI__ which
    hides POSIX names like close/read/_setmode in MinGW's headers.  */
#ifdef _WIN32
  #include "fastent-win32.h"
  #define FASTENT_OPEN_RD(p)      fastent_win32_open_utf8((p), O_RDONLY | O_BINARY)
  #define FASTENT_CLOSE(fd)       fastent_win32_close((fd))
  #define FASTENT_READ(fd, b, n)  fastent_win32_read((fd), (b), (n))
#else
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
  #define FASTENT_OPEN_RD(p)      open((p), O_RDONLY | O_BINARY)
  #define FASTENT_CLOSE(fd)       close((fd))
  #define FASTENT_READ(fd, b, n)  read((fd), (b), (n))
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

/*  Get a 4 KiB-aligned 2 MiB read buffer.  Preference order:
      posix_memalign  -> aligned_alloc  -> malloc + manual round-up.
    The last fallback over-allocates by FASTENT_STREAM_ALIGN bytes and
    rounds the user-visible pointer up; the raw malloc'd pointer is
    kept in stream_buf_raw for the matching free().  */
#define FASTENT_STREAM_ALIGN 4096u

static int alloc_stream_buf(fastent_source * s) {
  void * raw  = NULL;
  void * user = NULL;
#if HAVE_POSIX_MEMALIGN
  if (posix_memalign(&raw, FASTENT_STREAM_ALIGN, FASTENT_STREAM_BUF) != 0)
    raw = NULL;
  user = raw;
#elif HAVE_ALIGNED_ALLOC
  raw = aligned_alloc(FASTENT_STREAM_ALIGN, FASTENT_STREAM_BUF);
  user = raw;
#else
  raw = malloc(FASTENT_STREAM_BUF + FASTENT_STREAM_ALIGN);
  if (raw) {
    uintptr_t a = ((uintptr_t) raw + (FASTENT_STREAM_ALIGN - 1))
                  & ~(uintptr_t)(FASTENT_STREAM_ALIGN - 1);
    user = (void *) a;
  }
#endif
  if (!raw) return -1;
  s->stream_buf     = (u8 *) user;
  s->stream_buf_raw = raw;
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

  int fd = FASTENT_OPEN_RD(path);
  if (fd < 0) return -1;
  s->opened_fd = 1;
  s->fd = fd;

#ifdef _WIN32
  if (!no_mmap) {
    void *             base   = NULL;
    void *             handle = NULL;
    unsigned long long sz_out = 0;
    if (fastent_win32_mmap(fd, &base, &sz_out, &handle) == 0) {
      s->kind       = FASTENT_SRC_MMAP;
      s->map        = base;
      s->size       = (u64) sz_out;
      s->map_handle = handle;
      fastent_win32_mmap_prefetch(base, sz_out);
      return 0;
    }
  }
#else
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
#endif

  s->kind = FASTENT_SRC_STREAM;
  if (alloc_stream_buf(s) < 0) {
    FASTENT_CLOSE(fd);  s->fd = -1;  s->opened_fd = 0;
    return -1;
  }
  return 0;
}

sz fastent_src_read(fastent_source * s) {
  if (s->kind != FASTENT_SRC_STREAM) return 0;
  sz off = 0;
  while (off < s->stream_buf_cap) {
    long n = (long) FASTENT_READ(s->fd, s->stream_buf + off,
                                  s->stream_buf_cap - off);
    if (n < 0)  { if (errno == EINTR) continue;  return (sz) -1; }
    if (n == 0) break;
    off += (sz) n;
  }
  return off;
}

void fastent_src_close(fastent_source * s) {
  if (!s) return;
  if (s->kind == FASTENT_SRC_MMAP && s->map) {
#ifdef _WIN32
    fastent_win32_munmap(s->map, s->map_handle);
    s->map_handle = NULL;
#else
#ifdef HAVE_MMAP
    munmap(s->map, (size_t) s->size);
#endif
#endif
    s->map = NULL;
  }
  if (s->stream_buf_raw) {
    free(s->stream_buf_raw);
    s->stream_buf_raw = NULL;
    s->stream_buf     = NULL;
  }
  if (s->opened_fd && s->fd >= 0) FASTENT_CLOSE(s->fd);
  s->fd = -1;
  s->kind = FASTENT_SRC_NONE;
}
