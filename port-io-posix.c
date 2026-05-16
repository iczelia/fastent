/*  fastent: POSIX I/O backend (mmap, stream read, optional io_uring).

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "port-io.h"

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
  int     ring_fd;
  void *  sq_ring;
  size_t  sq_ring_size;
  void *  cq_ring;
  size_t  cq_ring_size;
  struct io_uring_sqe * sqes;
  size_t  sqes_size;
  volatile uint32_t * sq_khead;
  volatile uint32_t * sq_ktail;
  volatile uint32_t * sq_ring_mask;
           uint32_t * sq_array;
  volatile uint32_t * cq_khead;
  volatile uint32_t * cq_ktail;
  volatile uint32_t * cq_ring_mask;
  struct io_uring_cqe * cqes;
  u8 *     slot_buf[FASTENT_URING_SLOTS];
  void *   slot_buf_raw[FASTENT_URING_SLOTS];
  int      slot_done[FASTENT_URING_SLOTS];
  int32_t  slot_result[FASTENT_URING_SLOTS];
  int      next_consume;
  int      prev_returned;
  u64      next_submit_offset;
  u64      file_size;
} uring_state;

static long io_uring_setup_sys(unsigned entries, struct io_uring_params * p) {
  return syscall(__NR_io_uring_setup, entries, p);
}
static long io_uring_enter_sys(int fd, unsigned to_submit,
                               unsigned min_complete, unsigned flags) {
  return syscall(__NR_io_uring_enter, fd, to_submit, min_complete,
                 flags, NULL, (size_t) 0);
}

static int uring_submit_read(uring_state * u, int src_fd, int slot) {
  uint32_t tail = *u->sq_ktail;
  uint32_t mask = *u->sq_ring_mask;
  uint32_t idx  = tail & mask;
  struct io_uring_sqe * sqe = &u->sqes[idx];

  u64 off = u->next_submit_offset;
  sz  to_read = FASTENT_STREAM_BUF;
  if (off >= u->file_size) to_read = 0;
  else if (off + to_read > u->file_size) to_read = (sz)(u->file_size - off);

  memset(sqe, 0, sizeof(*sqe));
  sqe->opcode    = IORING_OP_READ;
  sqe->fd        = src_fd;
  sqe->off       = off;
  sqe->addr      = (uint64_t)(uintptr_t) u->slot_buf[slot];
  sqe->len       = (uint32_t) to_read;
  sqe->user_data = (uint64_t) slot;

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

static int uring_wait(uring_state * u, int target_slot) {
  while (!u->slot_done[target_slot]) {
    uint32_t head = *u->cq_khead;
    uint32_t tail = *u->cq_ktail;

    while (head != tail) {
      uint32_t mask = *u->cq_ring_mask;
      struct io_uring_cqe * cqe = &u->cqes[head & mask];
      int slot = (int) cqe->user_data;
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
  if (!u) return;
  Fi((int) FASTENT_URING_SLOTS,
     if (u->slot_buf_raw[i]) free(u->slot_buf_raw[i]))
  if (u->sqes && u->sqes != MAP_FAILED) munmap(u->sqes, u->sqes_size);
  if (u->cq_ring && u->cq_ring != u->sq_ring && u->cq_ring != MAP_FAILED)
    munmap(u->cq_ring, u->cq_ring_size);
  if (u->sq_ring && u->sq_ring != MAP_FAILED)
    munmap(u->sq_ring, u->sq_ring_size);
  if (u->ring_fd >= 0) close(u->ring_fd);
  free(u);
}

static uring_state * uring_setup(int src_fd, u64 file_size) {
  uring_state * u = calloc(1, sizeof(*u));
  if (!u) return NULL;
  u->ring_fd = -1;

  struct io_uring_params p;
  memset(&p, 0, sizeof(p));
  long r = io_uring_setup_sys(FASTENT_URING_ENTRIES, &p);
  if (r < 0) { free(u); return NULL; }
  u->ring_fd = (int) r;

  u->sq_ring_size = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
  u->cq_ring_size = p.cq_off.cqes  + p.cq_entries * sizeof(struct io_uring_cqe);
  if (p.features & IORING_FEAT_SINGLE_MMAP) {
    if (u->cq_ring_size > u->sq_ring_size) u->sq_ring_size = u->cq_ring_size;
    u->cq_ring_size = u->sq_ring_size;
  }

  u->sq_ring = mmap(NULL, u->sq_ring_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, u->ring_fd, IORING_OFF_SQ_RING);
  if (u->sq_ring == MAP_FAILED) { uring_destroy(u); return NULL; }

  if (p.features & IORING_FEAT_SINGLE_MMAP) {
    u->cq_ring = u->sq_ring;
  } else {
    u->cq_ring = mmap(NULL, u->cq_ring_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, u->ring_fd,
                      IORING_OFF_CQ_RING);
    if (u->cq_ring == MAP_FAILED) { uring_destroy(u); return NULL; }
  }

  u->sqes_size = p.sq_entries * sizeof(struct io_uring_sqe);
  u->sqes = mmap(NULL, u->sqes_size, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_POPULATE, u->ring_fd, IORING_OFF_SQES);
  if (u->sqes == MAP_FAILED) { uring_destroy(u); return NULL; }

  u->sq_khead     = (uint32_t *)((char *) u->sq_ring + p.sq_off.head);
  u->sq_ktail     = (uint32_t *)((char *) u->sq_ring + p.sq_off.tail);
  u->sq_ring_mask = (uint32_t *)((char *) u->sq_ring + p.sq_off.ring_mask);
  u->sq_array     = (uint32_t *)((char *) u->sq_ring + p.sq_off.array);

  u->cq_khead     = (uint32_t *)((char *) u->cq_ring + p.cq_off.head);
  u->cq_ktail     = (uint32_t *)((char *) u->cq_ring + p.cq_off.tail);
  u->cq_ring_mask = (uint32_t *)((char *) u->cq_ring + p.cq_off.ring_mask);
  u->cqes = (struct io_uring_cqe *)
            ((char *) u->cq_ring + p.cq_off.cqes);

  Fi((int) FASTENT_URING_SLOTS,
     void * raw = NULL;
     void * user = NULL;
     if (fastent_io_alloc_aligned(&raw, &user, FASTENT_STREAM_BUF) < 0) {
       uring_destroy(u);  return NULL;
     }
     u->slot_buf[i]     = (u8 *) user;
     u->slot_buf_raw[i] = raw)

  u->next_consume       = 0;
  u->prev_returned      = -1;
  u->next_submit_offset = 0;
  u->file_size          = file_size;

#ifdef HAVE_POSIX_FADVISE
  if (src_fd >= 0)
    posix_fadvise(src_fd, 0, (off_t) file_size, POSIX_FADV_SEQUENTIAL);
#endif

  Fi((int) FASTENT_URING_SLOTS,
     if (uring_submit_read(u, src_fd, (int) i) < 0) {
       uring_destroy(u);  return NULL;
     })
  return u;
}

#endif  /*  HAVE_IO_URING  */

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
    void * p = mmap(NULL, (size_t) file_size,
                    PROT_READ, MAP_PRIVATE, fd, 0);
    if (p != MAP_FAILED) {
      s->kind = FASTENT_SRC_MMAP;
      s->map  = p;
      s->size = file_size;
#ifdef HAVE_MADVISE
      madvise(p, (size_t) file_size, MADV_SEQUENTIAL | MADV_WILLNEED);
  #ifdef MADV_HUGEPAGE
      madvise(p, (size_t) file_size, MADV_HUGEPAGE);
  #endif
#endif
#ifdef HAVE_POSIX_FADVISE
      posix_fadvise(fd, 0, (off_t) file_size, POSIX_FADV_SEQUENTIAL);
#endif
      return 0;
    }
    if (mode == FASTENT_IO_MMAP) goto close_fail;
#else
    if (mode == FASTENT_IO_MMAP) { errno = ENOSYS; goto close_fail; }
#endif
  } else if (mode == FASTENT_IO_MMAP && !is_regular) {
    errno = ESPIPE; goto close_fail;
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
      long n = (long) read(s->fd, s->stream_buf + off,
                           s->stream_buf_cap - off);
      if (n < 0)  { if (errno == EINTR) continue;  return (sz) -1; }
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

    int slot = u->next_consume;
    if (uring_wait(u, slot) < 0) return (sz) -1;

    int32_t res = u->slot_result[slot];
    if (res < 0) { errno = -res; return (sz) -1; }

    s->stream_buf    = u->slot_buf[slot];
    u->prev_returned = slot;
    u->next_consume  = (slot + 1) % (int) FASTENT_URING_SLOTS;

    return (sz) res;
  }
#endif
  return 0;
}

void fastent_src_close(fastent_source * s) {
  if (!s) return;

  if (s->kind == FASTENT_SRC_MMAP && s->map) {
#ifdef HAVE_MMAP
    munmap(s->map, (size_t) s->size);
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
