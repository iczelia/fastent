/*  fastent: I/O layer: mmap fast path, read(2) fallback, io_uring opt-in.

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

/*  I/O mode selection.  AUTO is the historical default: mmap if the
    fd is a regular non-empty file, read(2) otherwise.  Explicit modes
    force the choice (and error out if impossible).  */
typedef enum {
  FASTENT_IO_AUTO   = 0,
  FASTENT_IO_MMAP   = 1,
  FASTENT_IO_STREAM = 2,
  FASTENT_IO_URING  = 3
} fastent_io_mode;

typedef struct {
  fastent_src_kind kind;
  int          fd;
  void *       map;            /*  FASTENT_SRC_MMAP: base pointer  */
  u64          size;           /*  FASTENT_SRC_MMAP / URING: file length  */
  void *       map_handle;     /*  FASTENT_SRC_MMAP: Win32 HANDLE for the
                                   CreateFileMapping object; NULL on POSIX
                                   (munmap doesn't need a handle).  */
  u8 *         stream_buf;     /*  FASTENT_SRC_STREAM / URING: current buffer
                                   (URING rotates this across slots)  */
  void *       stream_buf_raw; /*  FASTENT_SRC_STREAM: pointer to free  */
  sz           stream_buf_cap; /*  FASTENT_SRC_STREAM / URING: buffer size  */
  int          opened_fd;      /*  1 if we open()d (so we close)  */
  void *       uring_state;    /*  FASTENT_SRC_URING: opaque  */
} fastent_source;

/*  Open `path` (NULL = stdin), selecting the requested I/O mode.
    Returns 0 on success, -1 on failure with errno set.

    URING and MMAP error out if their preconditions aren't met (e.g.
    URING on stdin or on a kernel that doesn't support io_uring; MMAP
    on a pipe).  AUTO and STREAM never fail for I/O-mode reasons.  */
int  fastent_src_open(fastent_source * s, const char * path,
                      fastent_io_mode mode);

/*  Stream / uring mode: pull next chunk into s->stream_buf.
    Returns bytes read (0 = EOF, <0 = error with errno set).  */
sz   fastent_src_read(fastent_source * s);

/*  Release resources.  */
void fastent_src_close(fastent_source * s);

#endif
