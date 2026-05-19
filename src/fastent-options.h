/*  fastent: shared option struct.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_OPTIONS_H
#define FASTENT_OPTIONS_H

#include "common.h"

typedef enum {
  FASTENT_SORT_NONE = 0,
  FASTENT_SORT_PATH,
  FASTENT_SORT_SAMPLES,
  FASTENT_SORT_ENTROPY,
  FASTENT_SORT_CHI_SQUARE,
  FASTENT_SORT_MEAN,
  FASTENT_SORT_MONTE_PI,
  FASTENT_SORT_SCC,
  FASTENT_SORT_MIN_ENTROPY,
  FASTENT_SORT_COLLISION,
  FASTENT_SORT_IC,
  FASTENT_SORT_POKER,
  FASTENT_SORT_VARIANCE,
  FASTENT_SORT_REDUNDANCY,
  FASTENT_SORT_DISTINCT,
  FASTENT_SORT_BIT_BIAS,
  FASTENT_SORT_COND_ENTROPY,
  FASTENT_SORT_MUTUAL_INFO,
  FASTENT_SORT_LZ_DEVIATION,
  FASTENT_SORT_LZ_CR,
  FASTENT_SORT_LZ_MATCH_COV,
  FASTENT_SORT_BM_DEVIATION,
  FASTENT_SORT_BM_MEAN_LC,
  FASTENT_SORT_MAURER_DEVIATION
} fastent_sort_by;

typedef struct {
  int binary;         /*  -b  */
  int counts;         /*  -c  */
  int fold;           /*  -f  */
  int terse;          /*  -t  */
  int json;           /*  --json  */
  int full_precision; /*  -p / --full-precision  */
  int io_mode;        /*  --io={auto,mmap,stream,uring}  */
  int histogram;      /*  -H / --histogram  */
  int histogram_log;  /*  --log  (log-y for the histogram)  */
  int color;          /*  --color mode: 0=never 1=auto 2=always  */
  int threads;        /*  -j / --threads  (0 = auto, 1 = default)  */
  int recursive;      /*  -r / --recursive  */
  int extended;       /*  -e / --extended level (repeatable): >=1 emits
                          the extended stats; >=2 (-ee) also computes the
                          order-1 bigram H(cur|prev)+MI  */
  int annotate;       /*  --annotate (interpretive report; implies -e)  */
  int fips140;        /*  --fips-140-2 (RNG power-up self-tests)  */
  int sort_by;        /*  --sort-by=COL, value from fastent_sort_by  */
  int sort_desc;      /*  1 = descending; 0 = ascending  */
  const char * path;  /*  positional (NULL = stdin)  */
} fastent_options;

static inline int fastent_is_displayable(u32 c) {
  return (c >= 0x21u && c <= 0x7Eu) || c >= 0xA1u;
}

#endif
