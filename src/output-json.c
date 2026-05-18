/*  fastent: JSON output.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "output.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

/*  JSON has no NaN/Infinity literal, so non-finite values render
    as null.  */
static void jnum_(const char * fmt, f64 v) {
  if (!isfinite(v)) { fputs("null", stdout); return; }
  printf(fmt, v);
}

/*  Integer-valued counts: exact, never %g.  */
static void jint_(f64 v) {
  if (!isfinite(v)) fputs("null", stdout);
  else              printf("%.0f", v);
}

void fastent_print_json(const fastent_result * r, const fastent_options * o) {
  const char * samp = o->binary ? "bit" : "byte";
  const int fp = o->full_precision;
  const char * fmt_fp = fp ? "%.17g" : "%g";
  const f64 per = o->binary ? 1.0 : 8.0;
  /*  Clamp to [0, 100]; non-finite -> 0 (int cast of NaN is UB).  */
  const f64 comp_raw = 100.0 * (per - r->entropy) / per;
  const i32 comp_pct = !isfinite(comp_raw) ? 0
                     : comp_raw < 0.0      ? 0
                     : comp_raw > 100.0    ? 100
                                           : (i32) comp_raw;

  printf("{\n");
  printf("  \"unit\": \"%s\",\n", samp);
  printf("  \"samples\": %" PRIu64 ",\n", (u64) r->total_samples);
  printf("  \"entropy\": "); jnum_(fmt_fp, r->entropy); printf(",\n");
  printf("  \"optimum_compression_percent\": %d,\n", comp_pct);
  printf("  \"chi_square\": {\n");
  printf("    \"statistic\": "); jnum_(fmt_fp, r->chi_square); printf(",\n");
  printf("    \"df\": %d,\n", o->binary ? 1 : 255);
  printf("    \"p_exceed\": ");
  jnum_(fmt_fp, r->chi_probability);  printf("\n");
  printf("  },\n");
  printf("  \"arithmetic_mean\": "); jnum_(fmt_fp, r->mean); printf(",\n");
  printf("  \"monte_carlo_pi\": {\n");
  printf("    \"value\": "); jnum_(fmt_fp, r->monte_pi); printf(",\n");
  printf("    \"error_percent\": ");
  jnum_(fmt_fp, 100.0 * (fabs(M_PI - r->monte_pi) / M_PI));
  printf("\n  },\n");
  printf("  \"serial_correlation\": ");
  if (!FASTENT_SCC_DEFINED(r->scc)) printf("null");
  else                 jnum_(fmt_fp, r->scc);
  if (o->extended) {
    printf(",\n  \"min_entropy\": ");          jnum_(fmt_fp, r->min_entropy);
    printf(",\n  \"collision_entropy\": ");
    jnum_(fmt_fp, r->collision_entropy);
    printf(",\n  \"index_of_coincidence\": "); jnum_(fmt_fp, r->ic);
    printf(",\n  \"poker\": ");
    if (!(r->poker_chisq == r->poker_chisq)) {
      fputs("null", stdout);
    } else {
      printf("{ \"statistic\": "); jnum_(fmt_fp, r->poker_chisq);
      printf(", \"df\": 15, \"p_exceed\": "); jnum_(fmt_fp, r->poker_p);
      printf(" }");
    }
    printf(",\n  \"variance\": ");        jnum_(fmt_fp, r->variance);
    printf(",\n  \"stddev\": ");          jnum_(fmt_fp, r->stddev);
    printf(",\n  \"redundancy\": ");      jnum_(fmt_fp, r->redundancy);
    printf(",\n  \"distinct_symbols\": %u", r->distinct);
    printf(",\n  \"most_common\": ");
    if (r->mode_value < 0) fputs("null", stdout);
    else printf("{ \"value\": %d, \"count\": %" PRIu64 " }",
                r->mode_value, (u64) r->mode_count);
    printf(",\n  \"rarest\": ");
    if (r->rarest_value < 0) fputs("null", stdout);
    else printf("{ \"value\": %d, \"count\": %" PRIu64 " }",
                r->rarest_value, (u64) r->rarest_count);
    printf(",\n  \"bit_frequencies\": ");
    if (r->bit_bias_worst < 0) {
      fputs("null", stdout);
    } else {
      putchar('[');
      Fi(8, if (i) putchar(','); putchar(' '); jnum_(fmt_fp, r->bit_freq[i]))
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
    printf(",\n  \"runs\": ");          jint_(r->runs);
    printf(",\n  \"longest_run\": ");   jint_(r->longest_run);
    printf(",\n  \"cusum_max\": ");     jint_(r->cusum_max);
  }
  if (o->extended >= 3) {
    printf(",\n  \"lz77f\": ");
    if (r->lz_deviation != r->lz_deviation) {
      fputs("null", stdout);
    } else {
      printf("{\n    \"cr_excess\": ");      jnum_(fmt_fp, r->lz_cr_excess);
      printf(",\n    \"literal_entropy\": "); jnum_(fmt_fp, r->lz_lit_h);
      printf(",\n    \"literal_kl\": ");      jnum_(fmt_fp, r->lz_lit_kl);
      printf(",\n    \"match_coverage\": ");  jnum_(fmt_fp, r->lz_match_cov);
      printf(",\n    \"offset_concentration\": ");
      jnum_(fmt_fp, r->lz_off_conc);
      printf(",\n    \"mlen_excess\": ");     jnum_(fmt_fp, r->lz_mlen_excess);
      printf(",\n    \"literal_chi_square\": { \"statistic\": ");
      jnum_(fmt_fp, r->lz_lit_chi);
      printf(", \"df\": 255, \"p_exceed\": ");
      jnum_(fmt_fp, r->lz_lit_chi_p);
      printf(" }");
      printf(",\n    \"deviation\": ");       jnum_(fmt_fp, r->lz_deviation);
      printf(",\n    \"matches\": %" PRIu64, (u64) r->lz_nmatch);
      printf(",\n    \"single_dominant_match\": %s",
             r->lz_megamatch ? "true" : "false");
      printf("\n  }");
    }
  }
  if (o->counts) {
    printf(",\n  \"occurrences\": [\n");
    const i32 bins = o->binary ? 2 : 256;
    int first = 1;
    Fi(bins,
       if (r->hist[i] == 0) continue;
       if (!first) printf(",\n");
       first = 0;
       printf("    { \"value\": %d, \"count\": %" PRIu64 ", \"fraction\": ",
              i, (u64) r->hist[i]);
       jnum_(fmt_fp, (f64) r->hist[i] / (f64) r->total_samples);
       printf(" }"))
    printf("\n  ]");
  }
  printf("\n}\n");
}
