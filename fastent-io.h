/*  fastent: I/O layer: mmap fast path + read(2) fallback.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_IO_H
#define FASTENT_IO_H

#include "common.h"

typedef enum {
  FASTENT_SRC_NONE,
  FASTENT_SRC_MMAP,
  FASTENT_SRC_STREAM
} fastent_src_kind;

typedef struct {
  fastent_src_kind kind;
  int          fd;
  void *       map;            /*  FASTENT_SRC_MMAP: base pointer  */
  u64          size;           /*  FASTENT_SRC_MMAP: file length    */
  u8 *         stream_buf;     /*  FASTENT_SRC_STREAM: aligned read buffer  */
  void *       stream_buf_raw; /*  FASTENT_SRC_STREAM: pointer to free  */
  sz           stream_buf_cap; /*  FASTENT_SRC_STREAM: buffer size  */
  int          opened_fd;      /*  1 if we open()d (so we close)  */
} fastent_source;

/*  Open `path` (NULL = stdin). If `no_mmap` is nonzero, never mmap.
    Returns 0 on success, -1 on failure with errno set.  */
int  fastent_src_open(fastent_source * s, const char * path, int no_mmap);

/*  Stream mode: pull next chunk into s->stream_buf.
    Returns bytes read (0 = EOF, <0 = error with errno set).  */
sz   fastent_src_read(fastent_source * s);

/*  Release resources.  */
void fastent_src_close(fastent_source * s);

#endif
