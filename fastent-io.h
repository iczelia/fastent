/*  fastent: I/O layer.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_IO_H
#define FASTENT_IO_H

#include "common.h"

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
  int          fd;
  void *       map;
  u64          size;
  void *       map_handle;     /*  Win32 CreateFileMapping handle; NULL on POSIX.  */
  u8 *         stream_buf;     /*  URING rotates across N slots.  */
  void *       stream_buf_raw;
  sz           stream_buf_cap;
  int          opened_fd;
  void *       uring_state;
} fastent_source;

/*  AUTO/STREAM never fail for I/O-mode reasons; MMAP/URING error out
    if their preconditions are missing (stdin, old kernel, etc).  */
int  fastent_src_open(fastent_source * s, const char * path,
                      fastent_io_mode mode);

/*  STREAM/URING: bytes read, 0 = EOF, <0 = errno set.  */
sz   fastent_src_read(fastent_source * s);

void fastent_src_close(fastent_source * s);

#endif
