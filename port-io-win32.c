/*  fastent: Win32 I/O backend (CreateFileMapping, _read).

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "port-io.h"

#ifdef _WIN32

#include "fastent-win32.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

static int alloc_stream_buf(fastent_source * s) {
  void * raw = NULL, * user = NULL;
  if (fastent_io_alloc_aligned(&raw, &user, FASTENT_STREAM_BUF) < 0) return -1;
  s->stream_buf     = (u8 *) user;
  s->stream_buf_raw = raw;
  s->stream_buf_cap = FASTENT_STREAM_BUF;
  return 0;
}

int fastent_src_open(fastent_source * s, const char * path,
                     fastent_io_mode mode) {
  memset(s, 0, sizeof(*s));
  s->kind = FASTENT_SRC_NONE;
  s->fd = -1;
  s->opened_fd = 0;

  if (!path) {
    if (mode == FASTENT_IO_URING || mode == FASTENT_IO_MMAP) {
      errno = EINVAL;
      return -1;
    }
    s->fd   = 0;
    s->kind = FASTENT_SRC_STREAM;
    if (alloc_stream_buf(s) < 0) return -1;
    return 0;
  }

  int fd = fastent_win32_open_utf8(path, O_RDONLY | O_BINARY);
  if (fd < 0) return -1;
  s->opened_fd = 1;
  s->fd = fd;

  /*  CreateFileMapping rejects non-disk fds; --io=uring is Linux-only.  */
  if (mode == FASTENT_IO_URING) {
    errno = ENOSYS;
    goto close_fail;
  }

  if (mode == FASTENT_IO_STREAM) {
    s->kind = FASTENT_SRC_STREAM;
    if (alloc_stream_buf(s) < 0) goto close_fail;
    return 0;
  }

  {
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
    if (mode == FASTENT_IO_MMAP) goto close_fail;
  }

  s->kind = FASTENT_SRC_STREAM;
  if (alloc_stream_buf(s) < 0) goto close_fail;
  return 0;

close_fail:
  {
    int saved = errno;
    if (s->opened_fd && s->fd >= 0) fastent_win32_close(s->fd);
    s->fd = -1;
    s->opened_fd = 0;
    errno = saved;
    return -1;
  }
}

sz fastent_src_read(fastent_source * s) {
  if (s->kind == FASTENT_SRC_STREAM) {
    sz off = 0;
    while (off < s->stream_buf_cap) {
      long n = fastent_win32_read(s->fd, s->stream_buf + off,
                                  s->stream_buf_cap - off);
      if (n < 0)  { if (errno == EINTR) continue;  return (sz) -1; }
      if (n == 0) break;
      off += (sz) n;
    }
    return off;
  }
  return 0;
}

void fastent_src_close(fastent_source * s) {
  if (!s) return;

  if (s->kind == FASTENT_SRC_MMAP && s->map) {
    fastent_win32_munmap(s->map, s->map_handle);
    s->map_handle = NULL;
    s->map = NULL;
  }

  if (s->stream_buf_raw) {
    free(s->stream_buf_raw);
    s->stream_buf_raw = NULL;
    s->stream_buf     = NULL;
  }

  if (s->opened_fd && s->fd >= 0) fastent_win32_close(s->fd);
  s->fd = -1;
  s->kind = FASTENT_SRC_NONE;
}

#endif  /*  _WIN32  */
