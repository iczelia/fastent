/*  fastent: JSON output.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "output.h"

#include <math.h>
#include <stdio.h>

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

static void jnum_(const char * fmt, double v) {
  if (!(v == v)) { fputs("null", stdout); return; }  /*  NaN -> null  */
  printf(fmt, v);
}

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
  if (o->extended) {
    printf(",\n  \"min_entropy\": ");          jnum_(fmt_fp, r->min_entropy);
    printf(",\n  \"collision_entropy\": ");    jnum_(fmt_fp, r->collision_entropy);
    printf(",\n  \"index_of_coincidence\": "); jnum_(fmt_fp, r->ic);
    printf(",\n  \"poker\": ");
    if (!(r->poker_chisq == r->poker_chisq)) {
      fputs("null", stdout);
    } else {
      printf("{ \"statistic\": "); printf(fmt_fp, r->poker_chisq);
      printf(", \"df\": 15, \"p_exceed\": "); printf(fmt_fp, r->poker_p);
      printf(" }");
    }
    printf(",\n  \"variance\": ");        jnum_(fmt_fp, r->variance);
    printf(",\n  \"stddev\": ");          jnum_(fmt_fp, r->stddev);
    printf(",\n  \"redundancy\": ");      jnum_(fmt_fp, r->redundancy);
    printf(",\n  \"distinct_symbols\": %u", r->distinct);
    printf(",\n  \"most_common\": ");
    if (r->mode_value < 0) fputs("null", stdout);
    else printf("{ \"value\": %d, \"count\": %llu }",
                r->mode_value, (unsigned long long) r->mode_count);
    printf(",\n  \"rarest\": ");
    if (r->rarest_value < 0) fputs("null", stdout);
    else printf("{ \"value\": %d, \"count\": %llu }",
                r->rarest_value, (unsigned long long) r->rarest_count);
    printf(",\n  \"bit_frequencies\": ");
    if (r->bit_bias_worst < 0) {
      fputs("null", stdout);
    } else {
      putchar('[');
      Fi(8, if (i) putchar(','); putchar(' '); printf(fmt_fp, r->bit_freq[i]))
      fputs(" ]", stdout);
    }
    printf(",\n  \"bit_bias\": ");
    if (r->bit_bias_worst < 0) fputs("null", stdout);
    else { printf("{ \"max\": "); jnum_(fmt_fp, r->bit_bias_max);
           printf(", \"worst_bit\": %d }", r->bit_bias_worst); }
    printf(",\n  \"conditional_entropy\": ");
    jnum_(fmt_fp, r->conditional_entropy);
    printf(",\n  \"mutual_information\": ");
    jnum_(fmt_fp, r->mutual_information);
  }
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
