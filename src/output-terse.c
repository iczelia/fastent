/*  fastent: terse CSV output.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "output.h"

#include <stdio.h>

/*  Emit literal "nan" regardless of sign bit: glibc prints "-nan"
    for a sign-set qNaN, which would otherwise vary the CSV.  */
static void tnum_(const char * fmt, f64 x) {
  if (x != x) fputs("nan", stdout);
  else        printf(fmt, x);
}

/*  Integer-valued counts (runs / longest run / cusum): exact %.0f,
    never %g (which would render 1048576 as 1.04858e+06).  */
static void tint_(f64 x) {
  if (x != x) fputs("nan", stdout);
  else        printf("%.0f", x);
}

static void print_counts_(const fastent_result * r, int binary) {
  const i32 bins = binary ? 2 : 256;
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
           "Conditional-Entropy,Mutual-Information,"
           "Runs,Longest-Run,Cusum-Max");
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
    putchar(','); tnum_(f, r->min_entropy);
    putchar(','); tnum_(f, r->collision_entropy);
    putchar(','); tnum_(f, r->ic);
    putchar(','); tnum_(f, r->poker_chisq);
    putchar(','); tnum_(f, r->poker_p);
    putchar(','); tnum_(f, r->variance);
    putchar(','); tnum_(f, r->stddev);
    putchar(','); tnum_(f, r->redundancy);
    printf(",%u,%d,%llu,%d,%llu",
           r->distinct, r->mode_value, (unsigned long long) r->mode_count,
           r->rarest_value, (unsigned long long) r->rarest_count);
    Fi(8, putchar(','); tnum_(f, r->bit_freq[i]))
    putchar(','); tnum_(f, r->bit_bias_max);
    printf(",%d", r->bit_bias_worst);
    putchar(','); tnum_(f, r->conditional_entropy);
    putchar(','); tnum_(f, r->mutual_information);
    putchar(','); tint_(r->runs);
    putchar(','); tint_(r->longest_run);
    putchar(','); tint_(r->cusum_max);
  }
  putchar('\n');
  if (o->counts) print_counts_(r, o->binary);
}
