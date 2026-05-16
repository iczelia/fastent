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
  printf("0,File-%ss,Entropy,Chi-square,Mean,Monte-Carlo-Pi,Serial-Correlation",
         o->binary ? "bit" : "byte");
  if (o->extended)
    printf(",Min-Entropy,Collision-Entropy,IC,Poker,Poker-p,Variance,Stddev,"
           "Redundancy,Distinct,Mode,Mode-Count,Rarest,Rarest-Count,"
           "Bit0,Bit1,Bit2,Bit3,Bit4,Bit5,Bit6,Bit7,"
           "Bit-Bias-Max,Bit-Bias-Worst,"
           "Conditional-Entropy,Mutual-Information");
  putchar('\n');

  const char * f = o->full_precision ? "%.17g" : "%f";
  if (o->full_precision) {
    printf("1,%llu,%.17g,%.17g,%.17g,%.17g,%.17g",
           (unsigned long long) r->total_samples,
           r->entropy, r->chi_square, r->mean, r->monte_pi, r->scc);
  } else {
    printf("1,%llu,%f,%f,%f,%f,%f",
           (unsigned long long) r->total_samples,
           r->entropy, r->chi_square, r->mean, r->monte_pi, r->scc);
  }
  if (o->extended) {
    putchar(','); printf(f, r->min_entropy);
    putchar(','); printf(f, r->collision_entropy);
    putchar(','); printf(f, r->ic);
    putchar(','); printf(f, r->poker_chisq);
    putchar(','); printf(f, r->poker_p);
    putchar(','); printf(f, r->variance);
    putchar(','); printf(f, r->stddev);
    putchar(','); printf(f, r->redundancy);
    printf(",%u,%d,%llu,%d,%llu",
           r->distinct, r->mode_value, (unsigned long long) r->mode_count,
           r->rarest_value, (unsigned long long) r->rarest_count);
    Fi(8, putchar(','); printf(f, r->bit_freq[i]))
    putchar(','); printf(f, r->bit_bias_max);
    printf(",%d", r->bit_bias_worst);
    putchar(','); printf(f, r->conditional_entropy);
    putchar(','); printf(f, r->mutual_information);
  }
  putchar('\n');
  if (o->counts) print_counts_(r, o->binary);
}
