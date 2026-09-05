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

#ifndef FASTENT_PORT_IO_H
#define FASTENT_PORT_IO_H

#include "common.h"

#define FASTENT_STREAM_BUF   (2u * 1024u * 1024u)
#define FASTENT_STREAM_ALIGN 4096u

typedef enum {
  FASTENT_SRC_NONE,
  FASTENT_SRC_MMAP,
  FASTENT_SRC_STREAM,
  FASTENT_SRC_URING
} fastent_src_kind;

typedef enum {
  FASTENT_IO_AUTO   = 0,
  FASTENT_IO_MMAP   = 1,
  FASTENT_IO_STREAM = 2,
  FASTENT_IO_URING  = 3
} fastent_io_mode;

typedef struct {
  fastent_src_kind kind;
  int fd;
  void * map;
  u64 size;
  void * map_handle;
  u8 * stream_buf;
  void * stream_buf_raw;
  sz stream_buf_cap;
  int opened_fd;
  void * uring_state;
} fastent_source;

/*  AUTO/STREAM never fail for I/O-mode reasons; MMAP/URING error out
    if their preconditions are missing (stdin, old kernel, etc).  */
int fastent_src_open(
    fastent_source * s, const char * path, fastent_io_mode mode);

/*  STREAM/URING: bytes read, 0 = EOF, <0 = errno set.  */
sz   fastent_src_read(fastent_source * s);

void fastent_src_close(fastent_source * s);

/*  Aligned buffer alloc used by both backends; lives in port-io.c.  */
int  fastent_io_alloc_aligned(void ** out_raw, void ** out_user, sz cap);

/*  Per-worker io_uring slab reader: a private ring double-buffering
    sequential reads over one disjoint [off,off+len) range (N workers, no
    shared feed).  */
typedef struct fastent_uring_slab fastent_uring_slab;
fastent_uring_slab * fastent_uring_slab_open(
    int fd, const char * path, u64 off, u64 len);
sz   fastent_uring_slab_next(fastent_uring_slab * r, const u8 ** out);
void fastent_uring_slab_close(fastent_uring_slab * r);

#endif
