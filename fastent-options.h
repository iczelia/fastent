/*  fastent: shared option struct.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_OPTIONS_H
#define FASTENT_OPTIONS_H

typedef enum {
  FASTENT_SORT_NONE = 0,
  FASTENT_SORT_PATH,
  FASTENT_SORT_SAMPLES,
  FASTENT_SORT_ENTROPY,
  FASTENT_SORT_CHI_SQUARE,
  FASTENT_SORT_MEAN,
  FASTENT_SORT_MONTE_PI,
  FASTENT_SORT_SCC
} fastent_sort_by;

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
  int recursive;      /*  -r / --recursive  */
  int sort_by;        /*  --sort-by=COL, value from fastent_sort_by  */
  int sort_desc;      /*  1 = descending; 0 = ascending  */
  const char * path;  /*  positional (NULL = stdin)  */
} fastent_options;

static inline int fastent_is_displayable(unsigned c) {
  return (c >= 0x21u && c <= 0x7Eu) || c >= 0xA1u;
}

#endif
