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

#include "common.h"
#include "port-io.h"
#include "port-thread.h"

#ifndef _WIN32

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

#ifdef HAVE_IO_URING
#include <sys/syscall.h>
#include <linux/io_uring.h>
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif
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
  void * raw = NULL, * user = NULL;
  if (fastent_io_alloc_aligned(&raw, &user, FASTENT_STREAM_BUF) < 0) return -1;
  s->stream_buf     = (u8 *) user;
  s->stream_buf_raw = raw;
  s->stream_buf_cap = FASTENT_STREAM_BUF;
  return 0;
}

#ifdef HAVE_IO_URING

#define FASTENT_URING_SLOTS    4u
#define FASTENT_URING_ENTRIES  8u

typedef struct {
  int ring_fd;
  void * sq_ring;
  sz sq_ring_size;
  void * cq_ring;
  sz cq_ring_size;
  struct io_uring_sqe * sqes;
  sz sqes_size;
  volatile u32 * sq_khead;
  volatile u32 * sq_ktail;
  volatile u32 * sq_ring_mask;
           u32 * sq_array;
  volatile u32 * cq_khead;
  volatile u32 * cq_ktail;
  volatile u32 * cq_ring_mask;
  struct io_uring_cqe * cqes;
  u8 * slot_buf[FASTENT_URING_SLOTS];
  void * slot_buf_raw[FASTENT_URING_SLOTS];
  int slot_done[FASTENT_URING_SLOTS];
  i32 slot_result[FASTENT_URING_SLOTS];
  i32 next_consume;
  i32 prev_returned;
  u64 next_submit_offset;
  u64 file_size;
} uring_state;

static long io_uring_setup_sys(u32 entries, struct io_uring_params * p) {
  return syscall(__NR_io_uring_setup, entries, p);
}
static long io_uring_enter_sys(
    int fd, u32 to_submit, u32 min_complete, u32 flags) {
  return syscall(__NR_io_uring_enter, fd, to_submit, min_complete,
                 flags, NULL, (sz) 0);
}

static int uring_submit_read(uring_state * u, int src_fd, i32 slot) {
  u32 tail = *u->sq_ktail;
  u32 mask = *u->sq_ring_mask;
  u32 idx  = tail & mask;
  struct io_uring_sqe * sqe = &u->sqes[idx];

  u64 off = u->next_submit_offset;
  sz  to_read = FASTENT_STREAM_BUF;
  if (off >= u->file_size) to_read = 0;
  else if (off + to_read > u->file_size) to_read = (sz) (u->file_size - off);

  memset(sqe, 0, sizeof *sqe);
  sqe->opcode    = IORING_OP_READ;
  sqe->fd        = src_fd;
  sqe->off       = off;
  sqe->addr      = (u64) (uintptr_t) u->slot_buf[slot];
  sqe->len       = (u32) to_read;
  sqe->user_data = (u64) slot;

  u->sq_array[idx]     = idx;
  u->slot_done[slot]   = 0;
  u->slot_result[slot] = 0;

  *u->sq_ktail = tail + 1;
  u->next_submit_offset = off + (u64) to_read;

  for (;;) {
    long r = io_uring_enter_sys(u->ring_fd, 1, 0, 0);
    if (r >= 0) return 0;
    if (errno == EINTR) continue;
    return -1;
  }
}

static int uring_wait(uring_state * u, i32 target_slot) {
  while (!u->slot_done[target_slot]) {
    u32 head = *u->cq_khead;
    u32 tail = *u->cq_ktail;

    while (head != tail) {
      u32 mask = *u->cq_ring_mask;
      struct io_uring_cqe * cqe = &u->cqes[head & mask];
      i32 slot = (i32) cqe->user_data;
      u->slot_result[slot] = cqe->res;
      u->slot_done[slot]   = 1;
      head++;
    }
    *u->cq_khead = head;
    if (u->slot_done[target_slot]) break;
    long r = io_uring_enter_sys(u->ring_fd, 0, 1, IORING_ENTER_GETEVENTS);
    if (r < 0 && errno != EINTR) return -1;
  }
  return 0;
}

static void uring_destroy(uring_state * u) {
  i32 i;
  if (!u) return;
  Fi((int) FASTENT_URING_SLOTS,
    if (u->slot_buf_raw[i]) free(u->slot_buf_raw[i]));
  if (u->sqes && u->sqes != MAP_FAILED) munmap(u->sqes, u->sqes_size);
  if (u->cq_ring && u->cq_ring != u->sq_ring && u->cq_ring != MAP_FAILED)
    munmap(u->cq_ring, u->cq_ring_size);
  if (u->sq_ring && u->sq_ring != MAP_FAILED)
    munmap(u->sq_ring, u->sq_ring_size);
  if (u->ring_fd >= 0) close(u->ring_fd);
  free(u);
}

/*  Ring + slot buffers, no reads pre-submitted: shared by the single-
    feed source path and the per-worker slab reader.  */
static uring_state * uring_setup_bare(u64 file_size) {
  uring_state * u = calloc(1, sizeof *u);
  i32 i;
  if (!u) return NULL;
  u->ring_fd = -1;

  struct io_uring_params p;
  memset(&p, 0, sizeof (p));
  long r = io_uring_setup_sys(FASTENT_URING_ENTRIES, &p);
  if (r < 0) { free(u);  return NULL; }
  u->ring_fd = (int) r;

  u->sq_ring_size = p.sq_off.array + p.sq_entries * sizeof (u32);
  u->cq_ring_size = p.cq_off.cqes  + p.cq_entries * sizeof (struct io_uring_cqe);
  if (p.features & IORING_FEAT_SINGLE_MMAP) {
    if (u->cq_ring_size > u->sq_ring_size) u->sq_ring_size = u->cq_ring_size;
    u->cq_ring_size = u->sq_ring_size;
  }

  u->sq_ring = mmap(NULL, u->sq_ring_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, u->ring_fd, IORING_OFF_SQ_RING);
  if (u->sq_ring == MAP_FAILED) { uring_destroy(u);  return NULL; }

  if (p.features & IORING_FEAT_SINGLE_MMAP) u->cq_ring = u->sq_ring;
  else {
    u->cq_ring = mmap(NULL, u->cq_ring_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, u->ring_fd,
                      IORING_OFF_CQ_RING);
    if (u->cq_ring == MAP_FAILED) { uring_destroy(u);  return NULL; }
  }

  u->sqes_size = p.sq_entries * sizeof (struct io_uring_sqe);
  u->sqes = mmap(NULL, u->sqes_size, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_POPULATE, u->ring_fd, IORING_OFF_SQES);
  if (u->sqes == MAP_FAILED) { uring_destroy(u);  return NULL; }

  u->sq_khead     = (u32 *) ((char *) u->sq_ring + p.sq_off.head);
  u->sq_ktail     = (u32 *) ((char *) u->sq_ring + p.sq_off.tail);
  u->sq_ring_mask = (u32 *) ((char *) u->sq_ring + p.sq_off.ring_mask);
  u->sq_array     = (u32 *) ((char *) u->sq_ring + p.sq_off.array);

  u->cq_khead     = (u32 *) ((char *) u->cq_ring + p.cq_off.head);
  u->cq_ktail     = (u32 *) ((char *) u->cq_ring + p.cq_off.tail);
  u->cq_ring_mask = (u32 *) ((char *) u->cq_ring + p.cq_off.ring_mask);
  u->cqes = (struct io_uring_cqe *)
            ((char *) u->cq_ring + p.cq_off.cqes);

  Fi((int) FASTENT_URING_SLOTS,
    void * raw = NULL;
    void * user = NULL;
    if (fastent_io_alloc_aligned(&raw, &user, FASTENT_STREAM_BUF) < 0) {
      uring_destroy(u);  return NULL;
    }
    u->slot_buf[i]     = (u8 *) user;
    u->slot_buf_raw[i] = raw);

  u->next_consume       = 0;
  u->prev_returned      = -1;
  u->next_submit_offset = 0;
  u->file_size          = file_size;
  return u;
}

/*  Single-feed source ring: bare ring + sequential advice + the four
    pre-submitted reads the FASTENT_SRC_URING path consumes in order.  */
static uring_state * uring_setup(int src_fd, u64 file_size) {
  uring_state * u = uring_setup_bare(file_size);
  i32 i;
  if (!u) return NULL;
#ifdef HAVE_POSIX_FADVISE
  if (src_fd >= 0)
    posix_fadvise(src_fd, 0, (off_t) file_size, POSIX_FADV_SEQUENTIAL);
#endif
  Fi((int) FASTENT_URING_SLOTS,
    if (uring_submit_read(u, src_fd, (int) i) < 0) { uring_destroy(u);  return NULL; });
  return u;
}

/*  One read in flight over a disjoint range in strict offset order:
    output is bit-identical to -j1 / mmap.  */
#define FASTENT_SLAB_SLOTS 2u

struct fastent_uring_slab {
  uring_state * u;
  int fd;
  u64 end_off;  /*  one past the last byte of this slab  */
  i32 inflight;  /*  slot index with a submitted read, -1  */
  int started;
};

static int slab_submit(fastent_uring_slab * r, i32 slot) {
  uring_state * u = r->u;
  if (u->next_submit_offset >= r->end_off) return 0;
  u32 tail = *u->sq_ktail, mask = *u->sq_ring_mask, idx = tail & mask;
  struct io_uring_sqe * sqe = &u->sqes[idx];
  u64 off = u->next_submit_offset;
  sz  to_read = FASTENT_STREAM_BUF;
  if (off + to_read > r->end_off) to_read = (sz) (r->end_off - off);
  memset(sqe, 0, sizeof *sqe);
  sqe->opcode = IORING_OP_READ;  sqe->fd = r->fd;  sqe->off = off;
  sqe->addr = (u64) (uintptr_t) u->slot_buf[slot];
  sqe->len = (u32) to_read;  sqe->user_data = (u64) slot;
  u->sq_array[idx] = idx;  u->slot_done[slot] = 0;  u->slot_result[slot] = 0;
  *u->sq_ktail = tail + 1;
  u->next_submit_offset = off + (u64) to_read;
  for (;;) {
    long e = io_uring_enter_sys(u->ring_fd, 1, 0, 0);
    if (e >= 0) return 1;
    if (errno == EINTR) continue;
    return -1;
  }
}

fastent_uring_slab * fastent_uring_slab_open(
    int fd, const char * path, u64 off, u64 len) {
  (void) path;
  if (len == 0) return NULL;
  fastent_uring_slab * r = calloc(1, sizeof *r);
  if (!r) return NULL;
  r->u = uring_setup_bare(off + len);
  if (!r->u) { free(r);  return NULL; }
  r->u->next_submit_offset = off;
  r->fd = fd;  r->end_off = off + len;
  r->inflight = -1;  r->started = 0;
#ifdef HAVE_POSIX_FADVISE
  posix_fadvise(fd, (off_t) off, (off_t) len, POSIX_FADV_SEQUENTIAL);
#endif
  return r;
}

sz fastent_uring_slab_next(fastent_uring_slab * r, const u8 ** out) {
  uring_state * u = r->u;
  if (!r->started) {  /*  prime: submit slot 0  */
    int s = slab_submit(r, 0);
    if (s < 0) return (sz) -1;
    if (s == 0) { r->started = 1;  return 0; }  /*  empty slab  */
    r->inflight = 0;  r->started = 1;
  }
  if (r->inflight < 0) return 0;  /*  drained  */
  i32 slot = r->inflight;
  if (uring_wait(u, slot) < 0) return (sz) -1;
  i32 res = u->slot_result[slot];
  if (res < 0) { errno = -res;  return (sz) -1; }
  i32 nxt = (slot + 1) % (i32) FASTENT_SLAB_SLOTS;
  int s = slab_submit(r, nxt);  /*  overlap next read  */
  if (s < 0) return (sz) -1;
  r->inflight = s ? nxt : -1;
  *out = u->slot_buf[slot];
  return (sz) res;
}

void fastent_uring_slab_close(fastent_uring_slab * r) {
  if (!r) return;
  if (r->u) uring_destroy(r->u);
  free(r);
}

#endif  /*  HAVE_IO_URING  */

#ifndef HAVE_IO_URING
struct fastent_uring_slab { int unused; };
fastent_uring_slab * fastent_uring_slab_open(
    int fd, const char * path, u64 off, u64 len) {
  (void) fd;  (void) path;  (void) off;  (void) len;  return NULL;
}
sz fastent_uring_slab_next(fastent_uring_slab * r, const u8 ** out) {
  (void) r;  (void) out;  return (sz) -1;
}
void fastent_uring_slab_close(fastent_uring_slab * r) { (void) r; }
#endif

int fastent_src_open(
    fastent_source * s, const char * path, fastent_io_mode mode) {
  memset(s, 0, sizeof *s);
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

  int fd = open(path, O_RDONLY | O_BINARY);
  if (fd < 0) return -1;
  s->opened_fd = 1;
  s->fd = fd;

  u64 file_size = 0;
  int is_regular = 0;
#ifdef HAVE_SYS_STAT_H
  {
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
      file_size  = (u64) st.st_size;
      is_regular = 1;
    }
  }
#endif

#ifdef HAVE_IO_URING
  /*  auto: uring only at -j1 (it loses to the parallel mmap slab scan
      at -jN); explicit --io=uring still uses it at any -j.  */
  if (mode == FASTENT_IO_AUTO && is_regular && file_size > 0
#ifdef FASTENT_HAVE_THREADS
      && fastent_num_threads() == 1
#endif
     ) {
    uring_state * u = uring_setup(fd, file_size);
    if (u) {
      s->kind           = FASTENT_SRC_URING;
      s->size           = file_size;
      s->stream_buf_cap = FASTENT_STREAM_BUF;
      s->uring_state    = u;
      return 0;
    }
  }
  if (mode == FASTENT_IO_URING) {
    if (!is_regular) {
      errno = ESPIPE;
      goto close_fail;
    }
    if (file_size == 0) {
      s->kind = FASTENT_SRC_STREAM;
      if (alloc_stream_buf(s) < 0) goto close_fail;
      return 0;
    }
    uring_state * u = uring_setup(fd, file_size);
    if (!u) goto close_fail;
    s->kind           = FASTENT_SRC_URING;
    s->size           = file_size;
    s->stream_buf_cap = FASTENT_STREAM_BUF;
    s->uring_state    = u;
    return 0;
  }
#else
  if (mode == FASTENT_IO_URING) {
    errno = ENOSYS;
    goto close_fail;
  }
#endif

  if (mode == FASTENT_IO_STREAM) {
    s->kind = FASTENT_SRC_STREAM;
    if (alloc_stream_buf(s) < 0) goto close_fail;
    return 0;
  }

#ifdef HAVE_SYS_STAT_H
  if (mode != FASTENT_IO_STREAM && is_regular && file_size > 0) {
#ifdef HAVE_MMAP
    void * p = mmap(NULL, (sz) file_size,
                    PROT_READ, MAP_PRIVATE, fd, 0);
    if (p != MAP_FAILED) {
      s->kind = FASTENT_SRC_MMAP;
      s->map  = p;
      s->size = file_size;
#ifdef HAVE_MADVISE
      madvise(p, (sz) file_size, MADV_SEQUENTIAL | MADV_WILLNEED);
#ifdef MADV_HUGEPAGE
      madvise(p, (sz) file_size, MADV_HUGEPAGE);
#endif
#endif
#ifdef HAVE_POSIX_FADVISE
      posix_fadvise(fd, 0, (off_t) file_size, POSIX_FADV_SEQUENTIAL);
#endif
      return 0;
    }
    if (mode == FASTENT_IO_MMAP) goto close_fail;
#else
    if (mode == FASTENT_IO_MMAP) { errno = ENOSYS;  goto close_fail; }
#endif
  } else if (mode == FASTENT_IO_MMAP && !is_regular) {
    errno = ESPIPE;  goto close_fail;
  }
#endif

  s->kind = FASTENT_SRC_STREAM;
  if (alloc_stream_buf(s) < 0) goto close_fail;
  return 0;

close_fail:
  {
    int saved = errno;
    if (s->opened_fd && s->fd >= 0) close(s->fd);
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
      i64 n = (i64) read(s->fd, s->stream_buf + off,
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
#ifdef HAVE_IO_URING
  if (s->kind == FASTENT_SRC_URING) {
    uring_state * u = (uring_state *) s->uring_state;
    if (u->prev_returned >= 0) {
      if (uring_submit_read(u, s->fd, u->prev_returned) < 0) return (sz) -1;
      u->prev_returned = -1;
    }

    i32 slot = u->next_consume;
    if (uring_wait(u, slot) < 0) return (sz) -1;

    i32 res = u->slot_result[slot];
    if (res < 0) { errno = -res;  return (sz) -1; }

    s->stream_buf    = u->slot_buf[slot];
    u->prev_returned = slot;
    u->next_consume  = (slot + 1) % (i32) FASTENT_URING_SLOTS;

    return (sz) res;
  }
#endif
  return 0;
}

void fastent_src_close(fastent_source * s) {
  if (!s) return;

  if (s->kind == FASTENT_SRC_MMAP && s->map) {
#ifdef HAVE_MMAP
    munmap(s->map, (sz) s->size);
#endif
    s->map = NULL;
  }

#ifdef HAVE_IO_URING
  if (s->kind == FASTENT_SRC_URING && s->uring_state) {
    uring_destroy((uring_state *) s->uring_state);
    s->uring_state = NULL;
    s->stream_buf  = NULL;
  }
#endif

  if (s->stream_buf_raw) {
    free(s->stream_buf_raw);
    s->stream_buf_raw = NULL;
    s->stream_buf     = NULL;
  }

  if (s->opened_fd && s->fd >= 0) close(s->fd);
  s->fd = -1;
  s->kind = FASTENT_SRC_NONE;
}

#endif  /*  !_WIN32  */
