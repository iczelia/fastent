/*  fastent: terse CSV output.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "output.h"

#include <stdio.h>

static void print_counts_(const fastent_result * r, int binary) {
  const int bins = binary ? 2 : 256;
  printf("2,Value,Occurrences,Fraction\n");
  Fi(bins,
     printf("3,%d,%llu,%f\n", i,
            (unsigned long long) r->hist[i],
            (f64) r->hist[i] / (f64) r->total_samples))
}

void fastent_print_terse(const fastent_result * r, const fastent_options * o) {
  printf("0,File-%ss,Entropy,Chi-square,Mean,Monte-Carlo-Pi,Serial-Correlation\n",
         o->binary ? "bit" : "byte");
  if (o->full_precision) {
    printf("1,%llu,%.17g,%.17g,%.17g,%.17g,%.17g\n",
           (unsigned long long) r->total_samples,
           r->entropy, r->chi_square, r->mean, r->monte_pi, r->scc);
  } else {
    printf("1,%llu,%f,%f,%f,%f,%f\n",
           (unsigned long long) r->total_samples,
           r->entropy, r->chi_square, r->mean, r->monte_pi, r->scc);
  }
  if (o->counts) print_counts_(r, o->binary);
}
