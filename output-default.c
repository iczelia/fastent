/*  fastent: default human-readable output.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

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

void fastent_print_default(const fastent_result * r, const fastent_options * o) {
  const char * samp = o->binary ? "bit" : "byte";
  const int fp = o->full_precision;

  if (o->counts)    print_counts_(r, o->binary);
  if (o->histogram) fastent_print_histogram(r, o);

  if (fp) printf("Entropy = %.17g bits per %s.\n", r->entropy, samp);
  else    printf("Entropy = %f bits per %s.\n",    r->entropy, samp);

  printf("\nOptimum compression would reduce the size\n");
  const f64 per = o->binary ? 1.0 : 8.0;
  const int comp_pct = (int)(short)(100.0 * (per - r->entropy) / per);
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
}
