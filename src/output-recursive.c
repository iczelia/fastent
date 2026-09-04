/*  Copyright (C) 2023-2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.  */

#include "common.h"
#include "output.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

static void csv_escape_(const char * s) {
  /*  RFC 4180 quoting (doubled quotes) plus a formula-injection guard:
      a leading = + - @ tab is prefixed with an apostrophe inside the
      quotes so spreadsheets keep it literal, still RFC 4180.  */
  int formula = (*s == '=' || *s == '+' || *s == '-'
              || *s == '@' || *s == '\t');
  int need_quote = formula;
  const char * p;
  for (p = s; *p; p++) {
    if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
      need_quote = 1;
      break;
    }
  }
  if (!need_quote) { fputs(s, stdout); return; }
  putchar('"');
  if (formula) putchar('\'');
  for (p = s; *p; p++) {
    if (*p == '"') putchar('"');
    putchar(*p);
  }
  putchar('"');
}

/*  CSV: non-finite renders as the literal "nan" (never glibc's
    sign-set "-nan"), matching terse, so the column stays parseable.  */
static void cnum_(const char * fmt, f64 v) {
  if (!isfinite(v)) { fputs("nan", stdout); return; }
  printf(fmt, v);
}

void fastent_print_recursive_csv(
    const fastent_recursive_row * rows, sz n, const fastent_options * o) {
  const int fp = o->full_precision;
  const char * f = fp ? "%.17g" : "%g";
  sz k;
  i32 i;
  fputs("path,unit,samples,entropy,chi_square,p_exceed,"
        "mean,monte_carlo_pi,serial_correlation", stdout);
  if (o->extended)
    fputs(",min_entropy,collision_entropy,ic,poker,poker_p,variance,stddev,"
          "redundancy,distinct,mode,mode_count,rarest,rarest_count,"
          "bit0,bit1,bit2,bit3,bit4,bit5,bit6,bit7,"
          "bit_bias_max,bit_bias_worst,"
          "conditional_entropy,mutual_information,"
          "runs,longest_run,cusum_max", stdout);
  if (o->extended >= 1)
    fputs(",lz_cr_excess,lz_lit_h,lz_lit_kl,lz_match_cov,lz_off_conc,"
          "lz_mlen_excess,lz_lit_chi,lz_lit_chi_p,lz_deviation,"
          "lz_matches,lz_megamatch,"
          "perment_hnorm,perment_deviation,perment_windows", stdout);
  if (o->extended >= 3)
    fputs(",bm_mean_lc,bm_mu,bm_chi,bm_chi_p,bm_deviation,"
          "bm_windows,bm_degenerate,"
          "maurer_fn,maurer_expected,maurer_deviation,"
          "maurer_k,maurer_degenerate,"
          "mrank_dev,mrank_chi,mrank_matrices,mrank_underpowered", stdout);
  putchar('\n');
  for (k = 0; k < n; k++) {
    const fastent_recursive_row * r = &rows[k];
    csv_escape_(r->path);
    printf(",%s,%" PRIu64 ",", o->binary ? "bit" : "byte",
           (u64) r->result.total_samples);
    cnum_(f, r->result.entropy);     putchar(',');
    cnum_(f, r->result.chi_square);  putchar(',');
    cnum_(f, r->result.chi_probability); putchar(',');
    cnum_(f, r->result.mean);        putchar(',');
    cnum_(f, r->result.monte_pi);    putchar(',');
    if (FASTENT_SCC_DEFINED(r->result.scc)) cnum_(f, r->result.scc);
    else                                    fputs("nan", stdout);
    if (o->extended) {
      putchar(','); cnum_(f, r->result.min_entropy);
      putchar(','); cnum_(f, r->result.collision_entropy);
      putchar(','); cnum_(f, r->result.ic);
      putchar(','); cnum_(f, r->result.poker_chisq);
      putchar(','); cnum_(f, r->result.poker_p);
      putchar(','); cnum_(f, r->result.variance);
      putchar(','); cnum_(f, r->result.stddev);
      putchar(','); cnum_(f, r->result.redundancy);
      printf(",%u,%d,%" PRIu64 ",%d,%" PRIu64,
             r->result.distinct, r->result.mode_value,
             (u64) r->result.mode_count,
             r->result.rarest_value,
             (u64) r->result.rarest_count);
      Fi(8, putchar(','); cnum_(f, r->result.bit_freq[i]));
      putchar(','); cnum_(f, r->result.bit_bias_max);
      printf(",%d", r->result.bit_bias_worst);
      putchar(','); cnum_(f, r->result.conditional_entropy);
      putchar(','); cnum_(f, r->result.mutual_information);
      putchar(',');
      if (r->result.runs == r->result.runs) printf("%.0f", r->result.runs);
      else fputs("nan", stdout);
      putchar(',');
      if (r->result.longest_run == r->result.longest_run)
        printf("%.0f", r->result.longest_run);
      else fputs("nan", stdout);
      putchar(',');
      if (r->result.cusum_max == r->result.cusum_max)
        printf("%.0f", r->result.cusum_max);
      else fputs("nan", stdout);
    }
    if (o->extended >= 1) {
      putchar(','); cnum_(f, r->result.lz_cr_excess);
      putchar(','); cnum_(f, r->result.lz_lit_h);
      putchar(','); cnum_(f, r->result.lz_lit_kl);
      putchar(','); cnum_(f, r->result.lz_match_cov);
      putchar(','); cnum_(f, r->result.lz_off_conc);
      putchar(','); cnum_(f, r->result.lz_mlen_excess);
      putchar(','); cnum_(f, r->result.lz_lit_chi);
      putchar(','); cnum_(f, r->result.lz_lit_chi_p);
      putchar(','); cnum_(f, r->result.lz_deviation);
      printf(",%" PRIu64 ",%d",
             (u64) r->result.lz_nmatch, r->result.lz_megamatch);
      putchar(','); cnum_(f, r->result.perment_h_norm);
      putchar(','); cnum_(f, r->result.perment_deviation);
      printf(",%" PRIu64, (u64) r->result.perment_windows);
    }
    if (o->extended >= 3) {
      putchar(','); cnum_(f, r->result.bm_mean_lc);
      putchar(','); cnum_(f, r->result.bm_mu);
      putchar(','); cnum_(f, r->result.bm_chi);
      putchar(','); cnum_(f, r->result.bm_chi_p);
      putchar(','); cnum_(f, r->result.bm_deviation);
      printf(",%" PRIu64 ",%d",
             (u64) r->result.bm_windows, r->result.bm_degenerate);
      putchar(','); cnum_(f, r->result.maurer_fn);
      putchar(','); cnum_(f, r->result.maurer_expected);
      putchar(','); cnum_(f, r->result.maurer_dev);
      printf(",%" PRIu64 ",%d",
             (u64) r->result.maurer_k, r->result.maurer_degenerate);
      putchar(','); cnum_(f, r->result.mrank_dev);
      putchar(','); cnum_(f, r->result.mrank_chi);
      printf(",%" PRIu64 ",%d",
             (u64) r->result.mrank_matrices, r->result.mrank_underpowered);
    }
    putchar('\n');
  }
}

static void json_escape_(const char * s) {
  putchar('"');
  for (; *s; s++) {
    u8 c = (u8) *s;
    switch (c) {
      case '"':  fputs("\\\"", stdout); break;
      case '\\': fputs("\\\\", stdout); break;
      case '\b': fputs("\\b",  stdout); break;
      case '\f': fputs("\\f",  stdout); break;
      case '\n': fputs("\\n",  stdout); break;
      case '\r': fputs("\\r",  stdout); break;
      case '\t': fputs("\\t",  stdout); break;
      default:
        /*  Escape controls and every non-ASCII byte: paths are arbitrary byte
            strings, so a raw >= 0x80 byte would produce invalid UTF-8 (and
            U+2028/U+2029) in the JSON.  */
        if (c < 0x20 || c >= 0x7F) printf("\\u%04x", c);
        else                       putchar((int) c);
    }
  }
  putchar('"');
}

/*  JSON has no NaN/Infinity literal, so non-finite values render
    as null.  */
static void json_num_(const char * fmt, f64 v) {
  if (!isfinite(v)) { fputs("null", stdout); return; }
  printf(fmt, v);
}

/*  Integer-valued counts: exact, never %g.  */
static void json_int_(f64 v) {
  if (!isfinite(v)) fputs("null", stdout);
  else              printf("%.0f", v);
}

void fastent_print_recursive_json(
    const fastent_recursive_row * rows, sz n, const fastent_options * o) {
  const int fp = o->full_precision;
  const char * f = fp ? "%.17g" : "%g";
  sz k;
  i32 i;
  printf("{\n  \"unit\": \"%s\",\n  \"files\": [\n",
         o->binary ? "bit" : "byte");
  for (k = 0; k < n; k++) {
    const fastent_recursive_row * r = &rows[k];
    fputs("    { \"path\": ", stdout);
    json_escape_(r->path);
    printf(", \"samples\": %" PRIu64,
           (u64) r->result.total_samples);
    fputs(", \"entropy\": ", stdout);        json_num_(f, r->result.entropy);
    fputs(", \"chi_square\": ", stdout);     json_num_(f, r->result.chi_square);
    fputs(", \"p_exceed\": ", stdout);
    json_num_(f, r->result.chi_probability);
    fputs(", \"mean\": ", stdout);           json_num_(f, r->result.mean);
    fputs(", \"monte_carlo_pi\": ", stdout); json_num_(f, r->result.monte_pi);
    fputs(", \"serial_correlation\": ", stdout);
    if (!FASTENT_SCC_DEFINED(r->result.scc)) fputs("null", stdout);
    else                                     json_num_(f, r->result.scc);
    if (o->extended) {
      fputs(", \"min_entropy\": ", stdout);
      json_num_(f, r->result.min_entropy);
      fputs(", \"collision_entropy\": ", stdout);
      json_num_(f, r->result.collision_entropy);
      fputs(", \"index_of_coincidence\": ", stdout);
      json_num_(f, r->result.ic);
      fputs(", \"poker\": ", stdout);
      if (!(r->result.poker_chisq == r->result.poker_chisq)) { fputs("null", stdout); } else {
        printf("{ \"statistic\": "); json_num_(f, r->result.poker_chisq);
        printf(", \"df\": 15, \"p_exceed\": ");
        json_num_(f, r->result.poker_p);
        printf(" }");
      }
      fputs(", \"variance\": ", stdout);   json_num_(f, r->result.variance);
      fputs(", \"stddev\": ", stdout);     json_num_(f, r->result.stddev);
      fputs(", \"redundancy\": ", stdout); json_num_(f, r->result.redundancy);
      printf(", \"distinct_symbols\": %u", r->result.distinct);
      fputs(", \"most_common\": ", stdout);
      if (r->result.mode_value < 0) fputs("null", stdout);
      else printf("{ \"value\": %d, \"count\": %" PRIu64 " }",
                  r->result.mode_value,
                  (u64) r->result.mode_count);
      fputs(", \"rarest\": ", stdout);
      if (r->result.rarest_value < 0) fputs("null", stdout);
      else printf("{ \"value\": %d, \"count\": %" PRIu64 " }",
                  r->result.rarest_value,
                  (u64) r->result.rarest_count);
      fputs(", \"bit_frequencies\": ", stdout);
      if (r->result.bit_bias_worst < 0) { fputs("null", stdout); } else {
        putchar('[');
        Fi(8, if (i) putchar(',');
             putchar(' '); json_num_(f, r->result.bit_freq[i]));
        fputs(" ]", stdout);
      }
      fputs(", \"bit_bias\": ", stdout);
      if (r->result.bit_bias_worst < 0) { fputs("null", stdout); } else {
        printf("{ \"max\": "); json_num_(f, r->result.bit_bias_max);
        printf(", \"worst_bit\": %d }", r->result.bit_bias_worst);
      }
      fputs(", \"conditional_entropy\": ", stdout);
      json_num_(f, r->result.conditional_entropy);
      fputs(", \"mutual_information\": ", stdout);
      json_num_(f, r->result.mutual_information);
      fputs(", \"runs\": ", stdout);         json_int_(r->result.runs);
      fputs(", \"longest_run\": ", stdout);  json_int_(r->result.longest_run);
      fputs(", \"cusum_max\": ", stdout);    json_int_(r->result.cusum_max);
    }
    if (o->extended >= 1) {
      fputs(", \"lz77f\": ", stdout);
      if (r->result.lz_deviation != r->result.lz_deviation) { fputs("null", stdout); } else {
        fputs("{ \"cr_excess\": ", stdout);
        json_num_(f, r->result.lz_cr_excess);
        fputs(", \"literal_entropy\": ", stdout);
        json_num_(f, r->result.lz_lit_h);
        fputs(", \"literal_kl\": ", stdout);
        json_num_(f, r->result.lz_lit_kl);
        fputs(", \"match_coverage\": ", stdout);
        json_num_(f, r->result.lz_match_cov);
        fputs(", \"offset_concentration\": ", stdout);
        json_num_(f, r->result.lz_off_conc);
        fputs(", \"mlen_excess\": ", stdout);
        json_num_(f, r->result.lz_mlen_excess);
        fputs(", \"literal_chi_square\": { \"statistic\": ", stdout);
        json_num_(f, r->result.lz_lit_chi);
        fputs(", \"df\": 255, \"p_exceed\": ", stdout);
        json_num_(f, r->result.lz_lit_chi_p);
        fputs(" }", stdout);
        fputs(", \"deviation\": ", stdout);
        json_num_(f, r->result.lz_deviation);
        printf(", \"matches\": %" PRIu64, (u64) r->result.lz_nmatch);
        printf(", \"single_dominant_match\": %s",
               r->result.lz_megamatch ? "true" : "false");
        fputs(" }", stdout);
      }
      fputs(", \"permutation_entropy\": ", stdout);
      if (r->result.perment_deviation != r->result.perment_deviation) {
        fputs("null", stdout);
      } else {
        fputs("{ \"h_norm\": ", stdout);
        json_num_(f, r->result.perment_h_norm);
        fputs(", \"deviation\": ", stdout);
        json_num_(f, r->result.perment_deviation);
        fputs(", \"chi_square\": ", stdout);
        json_num_(f, r->result.perment_chi);
        fputs(", \"chi_p\": ", stdout);
        json_num_(f, r->result.perment_chi_p);
        printf(", \"windows\": %" PRIu64, (u64) r->result.perment_windows);
        fputs(", \"histogram\": [", stdout);
        Fi(24, if (i) putchar(',');
             printf(" %u", r->result.perment_hist[i]));
        fputs(" ]", stdout);
        fputs(" }", stdout);
      }
    }
    if (o->extended >= 3) {
      fputs(", \"linear_complexity\": ", stdout);
      if (r->result.bm_deviation != r->result.bm_deviation) { fputs("null", stdout); } else {
        fputs("{ \"mean_lc\": ", stdout);
        json_num_(f, r->result.bm_mean_lc);
        fputs(", \"mu\": ", stdout);
        json_num_(f, r->result.bm_mu);
        fputs(", \"class_chi_square\": { \"statistic\": ", stdout);
        json_num_(f, r->result.bm_chi);
        fputs(", \"df\": 5, \"p_exceed\": ", stdout);
        json_num_(f, r->result.bm_chi_p);
        fputs(" }", stdout);
        fputs(", \"deviation\": ", stdout);
        json_num_(f, r->result.bm_deviation);
        printf(", \"windows\": %" PRIu64, (u64) r->result.bm_windows);
        printf(", \"near_constant\": %s",
               r->result.bm_degenerate ? "true" : "false");
        fputs(" }", stdout);
      }
      fputs(", \"maurer_universal\": ", stdout);
      if (r->result.maurer_dev != r->result.maurer_dev) { fputs("null", stdout); } else {
        fputs("{ \"fn\": ", stdout);
        json_num_(f, r->result.maurer_fn);
        fputs(", \"expected\": ", stdout);
        json_num_(f, r->result.maurer_expected);
        fputs(", \"deviation\": ", stdout);
        json_num_(f, r->result.maurer_dev);
        printf(", \"test_blocks\": %" PRIu64, (u64) r->result.maurer_k);
        printf(", \"repetitive\": %s",
               r->result.maurer_degenerate ? "true" : "false");
        fputs(" }", stdout);
      }
    }
    if (o->extended >= 3) {
      fputs(", \"binary_matrix_rank\": ", stdout);
      if (r->result.mrank_dev != r->result.mrank_dev) { fputs("null", stdout); } else {
        fputs("{ \"deviation\": ", stdout);
        json_num_(f, r->result.mrank_dev);
        fputs(", \"chi_square\": ", stdout);
        json_num_(f, r->result.mrank_chi);
        fputs(", \"chi_p\": ", stdout);
        json_num_(f, r->result.mrank_chi_p);
        printf(", \"matrices\": %" PRIu64, (u64) r->result.mrank_matrices);
        printf(", \"rank_32\": %u", r->result.mrank_r32);
        printf(", \"rank_31\": %u", r->result.mrank_r31);
        printf(", \"rank_low\": %u", r->result.mrank_rlo);
        printf(", \"underpowered\": %s",
               r->result.mrank_underpowered ? "true" : "false");
        fputs(" }", stdout);
      }
    }
    fputs(" }", stdout);
    if (k + 1 < n) putchar(',');
    putchar('\n');
  }
  fputs("  ]\n}\n", stdout);
}
