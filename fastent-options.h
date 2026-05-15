/*  fastent: shared option struct.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_OPTIONS_H
#define FASTENT_OPTIONS_H

typedef struct {
  int binary;         /*  -b  */
  int counts;         /*  -c  */
  int fold;           /*  -f  */
  int terse;          /*  -t  */
  int json;           /*  --json  */
  int full_precision; /*  -p / --full-precision  */
  int no_mmap;        /*  --no-mmap (deprecated alias for --io=stream)  */
  int io_mode;        /*  --io={auto,mmap,stream,uring}  */
  int histogram;      /*  -H / --histogram  */
  int histogram_log;  /*  --log  (log-y for the histogram)  */
  int color;          /*  --color={auto,always,never}, 0=never 1=auto 2=always  */
  int threads;        /*  -j / --threads  (0 = auto, 1 = default)  */
  const char * path;  /*  positional (NULL = stdin)  */
} fastent_options;

static inline int fastent_is_displayable(unsigned c) {
  return (c >= 0x21u && c <= 0x7Eu) || c >= 0xA1u;
}

#endif
