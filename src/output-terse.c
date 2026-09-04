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
  i32 i;
  printf("2,Value,Occurrences,Fraction\n");
  Fi(bins,
    if (r->hist[i] == 0) continue;
    printf("3,%d,%" PRIu64 ",%f\n", i,
           (u64) r->hist[i],
           (f64) r->hist[i] / (f64) r->total_samples));
}

void fastent_print_terse(const fastent_result * r, const fastent_options * o) {
  const char * f = o->full_precision ? "%.17g" : "%f";
  i32 i;
  printf("0,File-%ss,Entropy,Chi-square,P-Exceed,Mean,Monte-Carlo-Pi,"
         "Serial-Correlation", o->binary ? "bit" : "byte");
  if (o->extended)
    printf(",Min-Entropy,Collision-Entropy,IC,Poker,Poker-p,Variance,Stddev,"
           "Redundancy,Distinct,Mode,Mode-Count,Rarest,Rarest-Count,"
           "Bit0,Bit1,Bit2,Bit3,Bit4,Bit5,Bit6,Bit7,"
           "Bit-Bias-Max,Bit-Bias-Worst,"
           "Conditional-Entropy,Mutual-Information,"
           "Runs,Longest-Run,Cusum-Max");
  if (o->extended >= 1)
    printf(",LZ-CR-Excess,LZ-Lit-H,LZ-Lit-KL,LZ-Match-Cov,LZ-Off-Conc,"
           "LZ-Mlen-Excess,LZ-Lit-Chi,LZ-Lit-Chi-p,LZ-Deviation,"
           "LZ-Matches,LZ-Megamatch,"
           "Perment-Hnorm,Perment-Deviation,Perment-Windows");
  if (o->extended >= 3)
    printf(",BM-Mean-LC,BM-Mu,BM-Chi,BM-Chi-p,BM-Deviation,"
           "BM-Windows,BM-Degenerate,"
           "Maurer-Fn,Maurer-Expected,Maurer-Deviation,"
           "Maurer-K,Maurer-Degenerate,"
           "MRank-Dev,MRank-Chi,MRank-Matrices,MRank-Underpowered");
  putchar('\n');

  printf("1,%" PRIu64 ",", (u64) r->total_samples);
  tnum_(f, r->entropy);          putchar(',');
  tnum_(f, r->chi_square);       putchar(',');
  tnum_(f, r->chi_probability);  putchar(',');
  tnum_(f, r->mean);             putchar(',');
  tnum_(f, r->monte_pi);         putchar(',');
  if (FASTENT_SCC_DEFINED(r->scc)) tnum_(f, r->scc);
  else                             fputs("nan", stdout);
  if (o->extended) {
    putchar(','); tnum_(f, r->min_entropy);
    putchar(','); tnum_(f, r->collision_entropy);
    putchar(','); tnum_(f, r->ic);
    putchar(','); tnum_(f, r->poker_chisq);
    putchar(','); tnum_(f, r->poker_p);
    putchar(','); tnum_(f, r->variance);
    putchar(','); tnum_(f, r->stddev);
    putchar(','); tnum_(f, r->redundancy);
    printf(",%u,%d,%" PRIu64 ",%d,%" PRIu64,
           r->distinct, r->mode_value, (u64) r->mode_count,
           r->rarest_value, (u64) r->rarest_count);
    Fi(8, putchar(','); tnum_(f, r->bit_freq[i]));
    putchar(','); tnum_(f, r->bit_bias_max);
    printf(",%d", r->bit_bias_worst);
    putchar(','); tnum_(f, r->conditional_entropy);
    putchar(','); tnum_(f, r->mutual_information);
    putchar(','); tint_(r->runs);
    putchar(','); tint_(r->longest_run);
    putchar(','); tint_(r->cusum_max);
  }
  if (o->extended >= 1) {
    putchar(','); tnum_(f, r->lz_cr_excess);
    putchar(','); tnum_(f, r->lz_lit_h);
    putchar(','); tnum_(f, r->lz_lit_kl);
    putchar(','); tnum_(f, r->lz_match_cov);
    putchar(','); tnum_(f, r->lz_off_conc);
    putchar(','); tnum_(f, r->lz_mlen_excess);
    putchar(','); tnum_(f, r->lz_lit_chi);
    putchar(','); tnum_(f, r->lz_lit_chi_p);
    putchar(','); tnum_(f, r->lz_deviation);
    printf(",%" PRIu64 ",%d", (u64) r->lz_nmatch, r->lz_megamatch);
    putchar(','); tnum_(f, r->perment_h_norm);
    putchar(','); tnum_(f, r->perment_deviation);
    printf(",%" PRIu64, (u64) r->perment_windows);
  }
  if (o->extended >= 3) {
    putchar(','); tnum_(f, r->bm_mean_lc);
    putchar(','); tnum_(f, r->bm_mu);
    putchar(','); tnum_(f, r->bm_chi);
    putchar(','); tnum_(f, r->bm_chi_p);
    putchar(','); tnum_(f, r->bm_deviation);
    printf(",%" PRIu64 ",%d", (u64) r->bm_windows, r->bm_degenerate);
    putchar(','); tnum_(f, r->maurer_fn);
    putchar(','); tnum_(f, r->maurer_expected);
    putchar(','); tnum_(f, r->maurer_dev);
    printf(",%" PRIu64 ",%d", (u64) r->maurer_k, r->maurer_degenerate);
    putchar(','); tnum_(f, r->mrank_dev);
    putchar(','); tnum_(f, r->mrank_chi);
    printf(",%" PRIu64 ",%d",
           (u64) r->mrank_matrices, r->mrank_underpowered);
  }
  putchar('\n');
  if (o->counts) print_counts_(r, o->binary);
}
