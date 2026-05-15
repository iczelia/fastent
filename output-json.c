/*  fastent: JSON output.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "output.h"

#include <math.h>
#include <stdio.h>

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

void fastent_print_json(const fastent_result * r, const fastent_options * o) {
  const char * samp = o->binary ? "bit" : "byte";
  const int fp = o->full_precision;
  const char * fmt_fp = fp ? "%.17g" : "%g";
  const f64 per = o->binary ? 1.0 : 8.0;
  const int comp_pct = (int)(short)(100.0 * (per - r->entropy) / per);

  printf("{\n");
  printf("  \"unit\": \"%s\",\n", samp);
  printf("  \"samples\": %llu,\n", (unsigned long long) r->total_samples);
  printf("  \"entropy\": "); printf(fmt_fp, r->entropy); printf(",\n");
  printf("  \"optimum_compression_percent\": %d,\n", comp_pct);
  printf("  \"chi_square\": {\n");
  printf("    \"statistic\": "); printf(fmt_fp, r->chi_square); printf(",\n");
  printf("    \"df\": %d,\n", o->binary ? 1 : 255);
  printf("    \"p_exceed\": "); printf(fmt_fp, r->chi_probability); printf("\n");
  printf("  },\n");
  printf("  \"arithmetic_mean\": "); printf(fmt_fp, r->mean); printf(",\n");
  printf("  \"monte_carlo_pi\": {\n");
  printf("    \"value\": "); printf(fmt_fp, r->monte_pi); printf(",\n");
  printf("    \"error_percent\": ");
  printf(fmt_fp, 100.0 * (fabs(M_PI - r->monte_pi) / M_PI));
  printf("\n  },\n");
  printf("  \"serial_correlation\": ");
  if (r->scc < -99999) printf("null");
  else                 printf(fmt_fp, r->scc);
  if (o->counts) {
    printf(",\n  \"occurrences\": [\n");
    const int bins = o->binary ? 2 : 256;
    int first = 1;
    Fi(bins,
       if (r->hist[i] == 0) continue;
       if (!first) printf(",\n");
       first = 0;
       printf("    { \"value\": %d, \"count\": %llu, \"fraction\": ",
              i, (unsigned long long) r->hist[i]);
       printf(fmt_fp, (f64) r->hist[i] / (f64) r->total_samples);
       printf(" }"))
    printf("\n  ]");
  }
  printf("\n}\n");
}
