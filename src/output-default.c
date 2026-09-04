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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*  Spell NaN alike on every libc.  */
static void num(const char * fmt, f64 x) {
  if (x != x) fputs("nan", stdout);
  else        printf(fmt, x);
}

static void counts(const fastent_result * r, int binary) {
  i32 i, bins = binary ? 2 : 256;
  printf("value char occurrences fraction\n");
  Fi(bins,
    if (r->hist[i]) {
      char ch = fastent_is_displayable((u32) i) ? (char) i : ' ';
      printf("%3d   %c   %10" PRIu64 " %.6f\n", i, ch, (u64) r->hist[i],
             (f64) r->hist[i] / (f64) r->total_samples);
    });
  printf("total     %10" PRIu64 " %.6f\n\n", (u64) r->total_samples, 1.0);
}

void fastent_print_default(
    const fastent_result * r, const fastent_options * o) {
  const char * unit = o->binary ? "bit" : "byte";
  const char * nf = o->full_precision ? "%.17g" : "%.6g";
  f64 width = o->binary ? 1.0 : 8.0;
  f64 raw = 100.0 * (width - r->entropy) / width;
  i32 saved = !isfinite(raw) ? 0 : raw < 0.0 ? 0 : raw > 100.0 ? 100
                                                           : (i32) raw;
  i32 bins = o->binary ? 2 : 256;
  i32 i;

  if (o->counts) counts(r, o->binary);
  if (o->histogram) fastent_print_histogram(r, o);

  printf("fastent: %" PRIu64 " %s%s\n", (u64) r->total_samples, unit,
         r->total_samples == 1 ? "" : "s");
  printf("  entropy             ");  num(nf, r->entropy);
  printf(" bits/%s  compression %d%%\n", unit, saved);
  printf("  chi-square          ");  num(nf, r->chi_square);
  printf("  p ");  num(nf, r->chi_probability);  putchar('\n');
  printf("  mean                ");  num(nf, r->mean);
  printf("  expected %.1f\n", o->binary ? 0.5 : 127.5);
  printf("  Monte Carlo pi      ");  num(nf, r->monte_pi);
  printf("  error ");
  num(nf, 100.0 * fabs(M_PI - r->monte_pi) / M_PI);  printf("%%\n");
  printf("  serial correlation  ");
  if (FASTENT_SCC_DEFINED(r->scc)) num(nf, r->scc);
  else fputs("undefined", stdout);
  putchar('\n');

  if (!o->extended) return;

  printf("  min entropy         ");  num(nf, r->min_entropy);
  printf(" bits/%s\n", unit);
  printf("  collision entropy   ");  num(nf, r->collision_entropy);
  printf(" bits/%s\n", unit);
  printf("  coincidence         ");  num(nf, r->ic);
  printf("  uniform ");  num(nf, 1.0 / (f64) bins);  putchar('\n');
  if (!o->binary) {
    printf("  poker d=15          ");  num(nf, r->poker_chisq);
    printf("  p ");  num(nf, r->poker_p);  putchar('\n');
  }
  printf("  variance            ");  num(nf, r->variance);
  printf("  stddev ");  num(nf, r->stddev);  putchar('\n');
  printf("  redundancy          ");  num(nf, r->redundancy * 100.0);
  printf("%%\n");
  printf("  symbols             %u/%d", r->distinct, bins);
  if (r->mode_value >= 0)
    printf("  mode %d (%" PRIu64 ")  rarest %d (%" PRIu64 ")",
           r->mode_value, (u64) r->mode_count,
           r->rarest_value, (u64) r->rarest_count);
  putchar('\n');

  if (r->bit_bias_worst >= 0) {
    printf("  bit P(1)            ");
    Fi(8, if (i) putchar(' ');  num(nf, r->bit_freq[i]));
    printf("\n  worst bit           %d  bias ", r->bit_bias_worst);
    num(nf, r->bit_bias_max);  putchar('\n');
  }
  if (r->conditional_entropy == r->conditional_entropy) {
    printf("  conditional entropy ");  num(nf, r->conditional_entropy);
    printf(" bits/%s\n", unit);
    printf("  mutual information  ");  num(nf, r->mutual_information);
    printf(" bits\n");
  }
  if (r->longest_run == r->longest_run) {
    printf("  longest run         ");  num(nf, r->longest_run);
    if (r->runs == r->runs) { printf("  runs ");  num(nf, r->runs); }
    if (r->cusum_max == r->cusum_max) { printf("  cusum ");  num(nf, r->cusum_max); }
    putchar('\n');
  }

  if (r->lz_deviation == r->lz_deviation) {
    printf("  LZ77F               z ");  num(nf, r->lz_deviation);
    printf("  CR ");  num(nf, r->lz_cr_excess);
    printf("  coverage ");  num(nf, r->lz_match_cov);
    printf("  matches %" PRIu64 "\n", (u64) r->lz_nmatch);
    printf("    literals          H ");  num(nf, r->lz_lit_h);
    printf("  KL ");  num(nf, r->lz_lit_kl);
    printf("  chi ");  num(nf, r->lz_lit_chi);
    printf("  p ");  num(nf, r->lz_lit_chi_p);  putchar('\n');
    printf("    matches           offset concentration ");
    num(nf, r->lz_off_conc);  printf("  length excess ");
    num(nf, r->lz_mlen_excess);
    if (r->lz_megamatch) printf("  dominant match");
    putchar('\n');
  }
  if (r->perment_deviation == r->perment_deviation) {
    printf("  permutation H       ");  num(nf, r->perment_h_norm);
    printf("  z ");  num(nf, r->perment_deviation);
    printf("  chi ");  num(nf, r->perment_chi);
    printf("  p ");  num(nf, r->perment_chi_p);
    printf("  windows %" PRIu64 "\n", (u64) r->perment_windows);
  }
  if (r->bm_deviation == r->bm_deviation) {
    printf("  linear complexity   L ");  num(nf, r->bm_mean_lc);
    printf("  mu ");  num(nf, r->bm_mu);
    printf("  z ");  num(nf, r->bm_deviation);
    printf("  chi ");  num(nf, r->bm_chi);
    printf("  p ");  num(nf, r->bm_chi_p);
    if (r->bm_degenerate) printf("  degenerate");
    putchar('\n');
  }
  if (r->maurer_dev == r->maurer_dev) {
    printf("  Maurer              fn ");  num(nf, r->maurer_fn);
    printf("  expected ");  num(nf, r->maurer_expected);
    printf("  z ");  num(nf, r->maurer_dev);
    printf("  blocks %" PRIu64, (u64) r->maurer_k);
    if (r->maurer_degenerate) printf("  degenerate");
    putchar('\n');
  }
  if (r->mrank_dev == r->mrank_dev) {
    printf("  matrix rank         z ");  num(nf, r->mrank_dev);
    printf("  chi ");  num(nf, r->mrank_chi);
    printf("  32 %u  31 %u  <=30 %u  matrices %" PRIu64,
           r->mrank_r32, r->mrank_r31, r->mrank_rlo,
           (u64) r->mrank_matrices);
    if (r->mrank_underpowered) printf("  underpowered");
    putchar('\n');
  }
}
