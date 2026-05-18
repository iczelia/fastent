/*  fastent: Win32 I/O backend.

      mmap:   CreateFileMapping + MapViewOfFile.
      stream: _read on the CRT fd.
      uring:  IOCP-driven overlapped ReadFile pipeline (Vista+; the
              flag spelled --io=uring on the CLI for cross-platform
              symmetry).

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "port-io.h"
#include "port-thread.h"

#ifdef _WIN32

#include "fastent-win32.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

/*  IOCP backend.  CancelIoEx + Vista+ GQCS semantics, so disabled
    under FASTENT_WIN_LEGACY (Win95 target).  */
#ifndef FASTENT_WIN_LEGACY

#define FASTENT_IOCP_SLOTS 4u

typedef struct {
  u8 *       buf;
  void *     buf_raw;
  OVERLAPPED ov;
  DWORD      bytes;
  DWORD      err;
  int        done;
} iocp_slot;

typedef struct {
  HANDLE     file;
  HANDLE     iocp;
  u64        file_size;
  u64        next_submit_offset;
  i32        next_consume;
  i32        prev_returned;
  iocp_slot  slot[FASTENT_IOCP_SLOTS];
} iocp_state;

static void iocp_destroy(iocp_state * u) {
  if (!u) return;
  /*  CancelIoEx is asynchronous: the kernel may still be writing a
      slot buffer.  Wait for every op to actually drain before the
      buffers are freed below, else use-after-free on this path.  */
  if (u->file && u->file != INVALID_HANDLE_VALUE) {
    CancelIoEx(u->file, NULL);
    Fi((int) FASTENT_IOCP_SLOTS,
       DWORD got;
       GetOverlappedResult(u->file, &u->slot[i].ov, &got, TRUE))
    CloseHandle(u->file);
  }
  if (u->iocp) CloseHandle(u->iocp);
  Fi((int) FASTENT_IOCP_SLOTS,
     if (u->slot[i].buf_raw) free(u->slot[i].buf_raw))
  free(u);
}

static int iocp_submit_(iocp_state * u, i32 slot) {
  iocp_slot * s = &u->slot[slot];

  u64 off = u->next_submit_offset;
  DWORD to_read = FASTENT_STREAM_BUF;
  if (off >= u->file_size) to_read = 0;
  else if (off + to_read > u->file_size) to_read = (DWORD)(u->file_size - off);

  memset(&s->ov, 0, sizeof(s->ov));
  s->ov.Offset     = (DWORD)(off & 0xFFFFFFFFu);
  s->ov.OffsetHigh = (DWORD)(off >> 32);
  s->bytes = 0;
  s->err   = 0;
  s->done  = 0;
  u->next_submit_offset = off + (u64) to_read;

  if (to_read == 0) {
    /*  Post a zero-byte completion so the consumer sees EOF
        without blocking in GQCS.  */
    if (!PostQueuedCompletionStatus(u->iocp, 0, (ULONG_PTR) slot, &s->ov))
      return -1;
    return 0;
  }

  BOOL ok = ReadFile(u->file, s->buf, to_read, NULL, &s->ov);
  if (ok) return 0;                       /*  Completes via IOCP.  */
  DWORD e = GetLastError();
  if (e == ERROR_IO_PENDING) return 0;
  if (e == ERROR_HANDLE_EOF) {
    /*  Synchronous EOF; mark this slot done with 0 bytes.  */
    s->done = 1;
    s->bytes = 0;
    return 0;
  }
  s->err  = e;
  s->done = 1;
  return 0;
}

static int iocp_wait_(iocp_state * u, i32 target) {
  while (!u->slot[target].done) {
    DWORD       bytes = 0;
    ULONG_PTR   key   = 0;
    OVERLAPPED* ov    = NULL;
    BOOL ok = GetQueuedCompletionStatus(u->iocp, &bytes, &key, &ov, INFINITE);
    if (!ov) {
      /*  GQCS itself failed (timeout / closed port).  */
      return -1;
    }
    i32 slot = (i32) key;
    if (slot < 0 || (u32) slot >= FASTENT_IOCP_SLOTS) return -1;
    iocp_slot * s = &u->slot[slot];
    s->done  = 1;
    s->bytes = bytes;
    if (!ok) {
      DWORD e = GetLastError();
      if (e == ERROR_HANDLE_EOF) s->err = 0;  /*  Clean EOF.  */
      else                       s->err = e;
    }
  }
  return 0;
}

static iocp_state * iocp_setup(const char * path) {
  iocp_state * u = calloc(1, sizeof(*u));
  if (!u) return NULL;

  u64 sz_out = 0;
  u->file = (HANDLE) fastent_win32_open_overlapped(path, &sz_out);
  if (!u->file) { iocp_destroy(u); return NULL; }
  u->file_size = sz_out;

  u->iocp = CreateIoCompletionPort(u->file, NULL, 0, 1);
  if (!u->iocp) { iocp_destroy(u); return NULL; }

  Fi((int) FASTENT_IOCP_SLOTS,
     void * raw = NULL;
     void * user = NULL;
     if (fastent_io_alloc_aligned(&raw, &user, FASTENT_STREAM_BUF) < 0) {
       iocp_destroy(u);  return NULL;
     }
     u->slot[i].buf     = (u8 *) user;
     u->slot[i].buf_raw = raw)

  u->next_submit_offset = 0;
  u->next_consume       = 0;
  u->prev_returned      = -1;

  Fi((int) FASTENT_IOCP_SLOTS,
     if (iocp_submit_(u, (int) i) < 0) { iocp_destroy(u);  return NULL; })
  return u;
}

#endif  /*  !FASTENT_WIN_LEGACY  */

/*  Public API.  */

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

  if (mode == FASTENT_IO_URING) {
#ifndef FASTENT_WIN_LEGACY
    iocp_state * u = iocp_setup(path);
    if (!u) { errno = EIO; return -1; }
    s->kind           = FASTENT_SRC_URING;
    s->size           = u->file_size;
    s->stream_buf_cap = FASTENT_STREAM_BUF;
    s->uring_state    = u;
    return 0;
#else
    errno = ENOSYS;
    return -1;
#endif
  }

#ifndef FASTENT_WIN_LEGACY
  /*  auto picks IOCP only at -j1 (Win32 analogue of the POSIX
      io_uring-at-j1 rule): single-feed, does not parallelise, so -jN
      keeps the parallel file-mapping slab scan.  */
  if (mode == FASTENT_IO_AUTO && fastent_num_threads() == 1) {
    iocp_state * u = iocp_setup(path);
    if (u) {
      s->kind           = FASTENT_SRC_URING;
      s->size           = u->file_size;
      s->stream_buf_cap = FASTENT_STREAM_BUF;
      s->uring_state    = u;
      return 0;
    }
  }
#endif

  int fd = fastent_win32_open_utf8(path, O_RDONLY | O_BINARY);
  if (fd < 0) return -1;
  s->opened_fd = 1;
  s->fd = fd;

  if (mode == FASTENT_IO_STREAM) {
    s->kind = FASTENT_SRC_STREAM;
    if (alloc_stream_buf(s) < 0) goto close_fail;
    return 0;
  }

  {
    void * base   = NULL;
    void * handle = NULL;
    u64    sz_out = 0;
    int    mr     = fastent_win32_mmap(fd, &base, &sz_out, &handle);
    if (mr == 0) {
      s->kind       = FASTENT_SRC_MMAP;
      s->map        = base;
      s->size       = sz_out;
      s->map_handle = handle;
      fastent_win32_mmap_prefetch(base, sz_out);
      return 0;
    }
    /*  mr == 1: empty file, unmappable but valid; stream it (0 bytes,
        clean EOF) exactly as the POSIX backend does, even under an
        explicit --io=mmap.  Only a genuine failure (mr < 0) under
        --io=mmap is fatal.  */
    if (mr < 0 && mode == FASTENT_IO_MMAP) goto close_fail;
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
      i64 n = fastent_win32_read(s->fd, s->stream_buf + off,
                                 s->stream_buf_cap - off);
      if (n < 0) {
        if (errno == EINTR) continue;
        /*  Deliver the buffered prefix; the error resurfaces on the
            next call (read fails again at off == 0).  */
        if (off > 0) return off;
        return (sz) -1;
      }
      if (n == 0) break;
      off += (sz) n;
    }
    return off;
  }
#ifndef FASTENT_WIN_LEGACY
  if (s->kind == FASTENT_SRC_URING) {
    iocp_state * u = (iocp_state *) s->uring_state;
    if (u->prev_returned >= 0) {
      if (iocp_submit_(u, u->prev_returned) < 0) return (sz) -1;
      u->prev_returned = -1;
    }

    i32 slot = u->next_consume;
    if (iocp_wait_(u, slot) < 0) return (sz) -1;

    iocp_slot * sl = &u->slot[slot];
    if (sl->err) { errno = EIO; return (sz) -1; }

    s->stream_buf    = sl->buf;
    u->prev_returned = slot;
    u->next_consume  = (slot + 1) % (i32) FASTENT_IOCP_SLOTS;

    return (sz) sl->bytes;
  }
#endif
  return 0;
}

void fastent_src_close(fastent_source * s) {
  if (!s) return;

  if (s->kind == FASTENT_SRC_MMAP && s->map) {
    fastent_win32_munmap(s->map, s->map_handle);
    s->map_handle = NULL;
    s->map = NULL;
  }

#ifndef FASTENT_WIN_LEGACY
  if (s->kind == FASTENT_SRC_URING && s->uring_state) {
    iocp_destroy((iocp_state *) s->uring_state);
    s->uring_state = NULL;
    s->stream_buf  = NULL;
  }
#endif

  if (s->stream_buf_raw) {
    free(s->stream_buf_raw);
    s->stream_buf_raw = NULL;
    s->stream_buf     = NULL;
  }

  if (s->opened_fd && s->fd >= 0) fastent_win32_close(s->fd);
  s->fd = -1;
  s->kind = FASTENT_SRC_NONE;
}

#ifndef FASTENT_WIN_LEGACY

#define FASTENT_SLAB_SLOTS 2u

struct fastent_uring_slab {
  HANDLE     h;
  HANDLE     ev;
  u64        next_off;
  u64        end_off;
  OVERLAPPED ov[FASTENT_SLAB_SLOTS];
  u8 *       buf[FASTENT_SLAB_SLOTS];
  void *     buf_raw[FASTENT_SLAB_SLOTS];
  i32        inflight;
  int        started;
};

static int slab_submit(struct fastent_uring_slab * r, i32 slot) {
  if (r->next_off >= r->end_off) return 0;
  u64 off = r->next_off;
  DWORD to_read = (DWORD) FASTENT_STREAM_BUF;
  if (off + to_read > r->end_off) to_read = (DWORD)(r->end_off - off);
  memset(&r->ov[slot], 0, sizeof(r->ov[slot]));
  r->ov[slot].Offset     = (DWORD)(off & 0xFFFFFFFFu);
  r->ov[slot].OffsetHigh = (DWORD)(off >> 32);
  r->ov[slot].hEvent     = r->ev;
  ResetEvent(r->ev);
  r->next_off = off + (u64) to_read;
  if (ReadFile(r->h, r->buf[slot], to_read, NULL, &r->ov[slot])) return 1;
  DWORD e = GetLastError();
  if (e == ERROR_IO_PENDING) return 1;
  if (e == ERROR_HANDLE_EOF)  return 0;
  return -1;
}

/*  One read in flight over a disjoint range in strict offset order:
    output is bit-identical to -j1 / mmap.  */
fastent_uring_slab * fastent_uring_slab_open(int fd, const char * path,
                                             u64 off, u64 len) {
  (void) fd;
  if (!path || len == 0) return NULL;
  struct fastent_uring_slab * r = calloc(1, sizeof(*r));
  if (!r) return NULL;
  u64 fsz = 0;
  r->h = (HANDLE) fastent_win32_open_overlapped(path, &fsz);
  if (!r->h) { free(r); return NULL; }
  r->ev = CreateEventA(NULL, TRUE, FALSE, NULL);
  if (!r->ev) { CloseHandle(r->h); free(r); return NULL; }
  r->next_off = off;
  r->end_off  = off + len;
  r->inflight = -1;
  Fi((int) FASTENT_SLAB_SLOTS,
     void * raw = NULL;
     void * user = NULL;
     if (fastent_io_alloc_aligned(&raw, &user, FASTENT_STREAM_BUF) < 0) {
       fastent_uring_slab_close(r);  return NULL;
     }
     r->buf[i]     = (u8 *) user;
     r->buf_raw[i] = raw)
  return r;
}

sz fastent_uring_slab_next(fastent_uring_slab * r, const u8 ** out) {
  if (!r->started) {
    int s = slab_submit(r, 0);
    if (s < 0) return (sz) -1;
    if (s == 0) { r->started = 1;  return 0; }
    r->inflight = 0;  r->started = 1;
  }
  if (r->inflight < 0) return 0;
  i32 slot = r->inflight;
  DWORD got = 0;
  if (!GetOverlappedResult(r->h, &r->ov[slot], &got, TRUE)) {
    if (GetLastError() != ERROR_HANDLE_EOF) return (sz) -1;
    got = 0;
  }
  i32 nxt = (slot + 1) % (i32) FASTENT_SLAB_SLOTS;
  int s = slab_submit(r, nxt);
  if (s < 0) return (sz) -1;
  r->inflight = s ? nxt : -1;
  if (got == 0) return 0;
  *out = r->buf[slot];
  return (sz) got;
}

void fastent_uring_slab_close(fastent_uring_slab * r) {
  if (!r) return;
  if (r->h && r->h != INVALID_HANDLE_VALUE) {
    CancelIoEx(r->h, NULL);
    Fi((int) FASTENT_SLAB_SLOTS,
       DWORD g;  GetOverlappedResult(r->h, &r->ov[i], &g, TRUE))
    CloseHandle(r->h);
  }
  if (r->ev) CloseHandle(r->ev);
  Fi((int) FASTENT_SLAB_SLOTS, if (r->buf_raw[i]) free(r->buf_raw[i]))
  free(r);
}

#else
fastent_uring_slab * fastent_uring_slab_open(int fd, const char * path,
                                             u64 off, u64 len) {
  (void) fd;  (void) path;  (void) off;  (void) len;  return NULL;
}
sz fastent_uring_slab_next(fastent_uring_slab * r, const u8 ** out) {
  (void) r;  (void) out;  return (sz) -1;
}
void fastent_uring_slab_close(fastent_uring_slab * r) { (void) r; }
#endif

#endif  /*  _WIN32  */
