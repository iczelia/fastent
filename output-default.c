/*  fastent: default human-readable output.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "output.h"

#include <math.h>
#include <stdio.h>

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

static void print_counts_(const fastent_result * r, int binary) {
  const int bins = binary ? 2 : 256;
  printf("Value Char Occurrences Fraction\n");
  Fi(bins,
     if (r->hist[i] == 0) continue;
     char ch = fastent_is_displayable((unsigned) i) ? (char) i : ' ';
     printf("%3d   %c   %10llu   %f\n", i, ch,
            (unsigned long long) r->hist[i],
            (f64) r->hist[i] / (f64) r->total_samples))
  printf("\nTotal:    %10llu   %f\n\n",
         (unsigned long long) r->total_samples, 1.0);
}

void fastent_print_default(const fastent_result * r,
                           const fastent_options * o) {
  const char * samp = o->binary ? "bit" : "byte";
  const int fp = o->full_precision;

  if (o->counts)    print_counts_(r, o->binary);
  if (o->histogram) fastent_print_histogram(r, o);

  if (fp) printf("Entropy = %.17g bits per %s.\n", r->entropy, samp);
  else    printf("Entropy = %f bits per %s.\n",    r->entropy, samp);

  printf("\nOptimum compression would reduce the size\n");
  const f64 per = o->binary ? 1.0 : 8.0;
  /*  Clamp to [0, 100]; non-finite -> 0 (cast of NaN/out-of-range
      to int is UB).  */
  const f64 comp_raw = 100.0 * (per - r->entropy) / per;
  const int comp_pct = !isfinite(comp_raw) ? 0
                     : comp_raw < 0.0      ? 0
                     : comp_raw > 100.0    ? 100
                                           : (int) comp_raw;
  printf("of this %llu %s file by %d percent.\n\n",
         (unsigned long long) r->total_samples, samp, comp_pct);

  if (fp) {
    printf("Chi square distribution for %llu samples is %.17g, and randomly\n",
           (unsigned long long) r->total_samples, r->chi_square);
  } else {
    printf("Chi square distribution for %llu samples is %1.2f, and randomly\n",
           (unsigned long long) r->total_samples, r->chi_square);
  }
  if      (r->chi_probability < 0.0001)
    printf("would exceed this value less than 0.01 percent of the times.\n\n");
  else if (r->chi_probability > 0.9999)
    printf("would exceed this value more than 99.99 percent of the times.\n\n");
  else if (fp)
    printf("would exceed this value %.17g percent of the times.\n\n",
           r->chi_probability * 100);
  else
    printf("would exceed this value %1.2f percent of the times.\n\n",
           r->chi_probability * 100);

  if (fp) {
    printf("Arithmetic mean value of data %ss is %.17g (%.17g = random).\n",
           samp, r->mean, o->binary ? 0.5 : 127.5);
  } else {
    printf("Arithmetic mean value of data %ss is %1.4f (%.1f = random).\n",
           samp, r->mean, o->binary ? 0.5 : 127.5);
  }
  if (fp) {
    printf("Monte Carlo value for Pi is %.17g (error %.17g percent).\n",
           r->monte_pi, 100.0 * (fabs(M_PI - r->monte_pi) / M_PI));
  } else {
    printf("Monte Carlo value for Pi is %1.9f (error %1.2f percent).\n",
           r->monte_pi, 100.0 * (fabs(M_PI - r->monte_pi) / M_PI));
  }
  printf("Serial correlation coefficient is ");
  if (r->scc >= -99999) {
    if (fp) printf("%.17g (totally uncorrelated = 0.0).\n", r->scc);
    else    printf("%1.6f (totally uncorrelated = 0.0).\n", r->scc);
  } else {
    printf("undefined (all values equal!).\n");
  }

  if (!o->extended) return;

  const int bins = o->binary ? 2 : 256;
  printf("\n");
  if (fp) {
    printf("Min-entropy is %.17g bits per %s.\n", r->min_entropy, samp);
    printf("Collision entropy is %.17g bits per %s.\n",
           r->collision_entropy, samp);
    printf("Index of coincidence is %.17g (%.17g = uniform).\n",
           r->ic, 1.0 / (f64) bins);
  } else {
    printf("Min-entropy is %f bits per %s.\n", r->min_entropy, samp);
    printf("Collision entropy is %f bits per %s.\n",
           r->collision_entropy, samp);
    printf("Index of coincidence is %f (%f = uniform).\n",
           r->ic, 1.0 / (f64) bins);
  }

  if (o->binary) {
    printf("Poker test is not applicable to bit streams.\n");
  } else {
    if (fp)
      printf("Poker chi square (16 bins, df=15) is %.17g, and randomly\n",
             r->poker_chisq);
    else
      printf("Poker chi square (16 bins, df=15) is %1.2f, and randomly\n",
             r->poker_chisq);
    if      (r->poker_p < 0.0001)
      printf("would exceed this value less than 0.01 percent of the times.\n");
    else if (r->poker_p > 0.9999)
      printf("would exceed this value more than 99.99 percent of the times.\n");
    else if (fp)
      printf("would exceed this value %.17g percent of the times.\n",
             r->poker_p * 100.0);
    else
      printf("would exceed this value %1.2f percent of the times.\n",
             r->poker_p * 100.0);
  }

  if (fp) {
    printf("Variance is %.17g (standard deviation %.17g).\n",
           r->variance, r->stddev);
    printf("Redundancy is %.17g percent.\n", r->redundancy * 100.0);
  } else {
    printf("Variance is %1.4f (standard deviation %1.4f).\n",
           r->variance, r->stddev);
    printf("Redundancy is %1.4f percent.\n", r->redundancy * 100.0);
  }
  printf("Distinct symbols: %u of %d.\n", r->distinct, bins);
  if (r->mode_value >= 0)
    printf("Most common symbol is %d (%llu times); "
           "rarest is %d (%llu times).\n",
           r->mode_value, (unsigned long long) r->mode_count,
           r->rarest_value, (unsigned long long) r->rarest_count);

  if (o->binary) {
    printf("Per-bit-position bias is not applicable to bit streams.\n");
  } else {
    printf("Per-bit-position P(1) [bit 0 = LSB]:\n");
    Fi(8, printf(fp ? "  bit %d = %.17g\n" : "  bit %d = %.4f\n",
                 i, r->bit_freq[i]))
    printf(fp ? "Worst bit is %d (bias %.17g from 0.5).\n"
              : "Worst bit is %d (bias %.4f from 0.5).\n",
           r->bit_bias_worst, r->bit_bias_max);
  }

  if (r->conditional_entropy != r->conditional_entropy) {
    printf("Order-1 bigram stats: pass -ee to compute them.\n");
  } else {
    printf(fp ? "Conditional entropy H(cur|prev) is %.17g bits per %s.\n"
              : "Conditional entropy H(cur|prev) is %f bits per %s.\n",
           r->conditional_entropy, samp);
    printf(fp ? "Adjacent mutual information I(prev;cur) is %.17g bits.\n"
              : "Adjacent mutual information I(prev;cur) is %f bits.\n",
           r->mutual_information);
  }

  if (r->longest_run == r->longest_run) {
    printf("Longest run is %.0f %ss.\n", r->longest_run, samp);
    if (r->runs == r->runs)
      printf("Runs is %.0f.\n", r->runs);
    if (r->cusum_max == r->cusum_max)
      printf("Cusum max excursion is %.0f.\n", r->cusum_max);
  }
}
