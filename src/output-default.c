/*  fastent: default human-readable output.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "output.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

/*  Emit literal "nan" regardless of the NaN sign bit: glibc x86 prints
    a sign-set qNaN as "-nan" while aarch64 / musl print "nan", which
    would otherwise vary the byte-exact output across hosts.  Same
    convention as output-terse.c's tnum_.  Finite values use fmt.  */
static void dnum_(const char * fmt, f64 x) {
  if (x != x) fputs("nan", stdout);
  else        printf(fmt, x);
}

static void print_counts_(const fastent_result * r, int binary) {
  const i32 bins = binary ? 2 : 256;
  printf("Value Char Occurrences Fraction\n");
  Fi(bins,
     if (r->hist[i] == 0) continue;
     char ch = fastent_is_displayable((u32) i) ? (char) i : ' ';
     printf("%3d   %c   %10" PRIu64 "   %f\n", i, ch,
            (u64) r->hist[i],
            (f64) r->hist[i] / (f64) r->total_samples))
  printf("\nTotal:    %10" PRIu64 "   %f\n\n",
         (u64) r->total_samples, 1.0);
}

void fastent_print_default(
    const fastent_result * r, const fastent_options * o) {
  const char * samp = o->binary ? "bit" : "byte";
  const int fp = o->full_precision;

  if (o->counts)    print_counts_(r, o->binary);
  if (o->histogram) fastent_print_histogram(r, o);

  if (fp) printf("Entropy = %.17g bits per %s.\n", r->entropy, samp);
  else    printf("Entropy = %f bits per %s.\n",    r->entropy, samp);

  printf("\nOptimum compression would reduce the size\n");
  const f64 per = o->binary ? 1.0 : 8.0;
  /*  Clamp to [0, 100]; non-finite -> 0 (int cast of NaN is UB).  */
  const f64 comp_raw = 100.0 * (per - r->entropy) / per;
  const i32 comp_pct = !isfinite(comp_raw) ? 0
                     : comp_raw < 0.0      ? 0
                     : comp_raw > 100.0    ? 100
                                           : (i32) comp_raw;
  printf("of this %" PRIu64 " %s file by %d percent.\n\n",
         (u64) r->total_samples, samp, comp_pct);

  printf("Chi square distribution for %" PRIu64 " samples is ",
         (u64) r->total_samples);
  dnum_(fp ? "%.17g" : "%1.2f", r->chi_square);
  printf(", and randomly\n");
  if      (r->chi_probability < 0.0001)
    printf("would exceed this value less than 0.01 percent of the times.\n\n");
  else if (r->chi_probability > 0.9999)
    printf("would exceed this value more than 99.99 percent of the times.\n\n");
  else {
    printf("would exceed this value ");
    dnum_(fp ? "%.17g" : "%1.2f", r->chi_probability * 100);
    printf(" percent of the times.\n\n");
  }

  printf("Arithmetic mean value of data %ss is ", samp);
  dnum_(fp ? "%.17g" : "%1.4f", r->mean);
  if (fp) printf(" (%.17g = random).\n", o->binary ? 0.5 : 127.5);
  else    printf(" (%.1f = random).\n",  o->binary ? 0.5 : 127.5);

  printf("Monte Carlo value for Pi is ");
  dnum_(fp ? "%.17g" : "%1.9f", r->monte_pi);
  printf(" (error ");
  dnum_(fp ? "%.17g" : "%1.2f", 100.0 * (fabs(M_PI - r->monte_pi) / M_PI));
  printf(" percent).\n");
  printf("Serial correlation coefficient is ");
  if (FASTENT_SCC_DEFINED(r->scc)) {
    if (fp) printf("%.17g (totally uncorrelated = 0.0).\n", r->scc);
    else    printf("%1.6f (totally uncorrelated = 0.0).\n", r->scc);
  } else {
    printf("undefined (all values equal!).\n");
  }

  if (!o->extended) return;

  const i32 bins = o->binary ? 2 : 256;
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
    printf("Most common symbol is %d (%" PRIu64 " times); "
           "rarest is %d (%" PRIu64 " times).\n",
           r->mode_value, (u64) r->mode_count,
           r->rarest_value, (u64) r->rarest_count);

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

  if (r->lz_deviation != r->lz_deviation) {
    printf("LZ77F estimator: pass -e to compute it.\n");
  } else {
    printf("\nLZ77F compressibility excess is ");
    dnum_(fp ? "%.17g" : "%f", r->lz_cr_excess);
    printf(" (0 = incompressible).\n");
    printf("LZ77F literal entropy H_lit is ");
    dnum_(fp ? "%.17g" : "%f", r->lz_lit_h);
    printf(" bits (KL to uniform ");
    dnum_(fp ? "%.17g" : "%f", r->lz_lit_kl);
    printf(").\n");
    printf("LZ77F match coverage is ");
    dnum_(fp ? "%.17g" : "%f", r->lz_match_cov);
    printf(" (fraction of matched bytes).\n");
    printf("LZ77F offset concentration is ");
    dnum_(fp ? "%.17g" : "%f", r->lz_off_conc);
    printf(", mean match-length excess ");
    dnum_(fp ? "%.17g" : "%f", r->lz_mlen_excess);
    printf(".\n");
    printf("LZ77F literal chi square (df=255) is ");
    dnum_(fp ? "%.17g" : "%1.2f", r->lz_lit_chi);
    printf(" (p ");
    dnum_(fp ? "%.17g" : "%f", r->lz_lit_chi_p);
    printf(", advisory).\n");
    if (r->lz_megamatch)
      printf("LZ77F note: exact-repeat / single dominant match "
             "(offset/length concentration at limit).\n");
    printf("LZ77F deviation z is ");
    dnum_(fp ? "%.17g" : "%f", r->lz_deviation);
    printf(" (%" PRIu64 " matches).\n", (u64) r->lz_nmatch);
  }

  if (r->bm_deviation != r->bm_deviation) {
    printf("Linear complexity: pass -eee to compute it.\n");
  } else {
    printf("\nLinear complexity mean L is ");
    dnum_(fp ? "%.17g" : "%f", r->bm_mean_lc);
    printf(" (mu ");
    dnum_(fp ? "%.17g" : "%f", r->bm_mu);
    printf(", %" PRIu64 " windows).\n", (u64) r->bm_windows);
    if (r->bm_degenerate)
      printf("Linear complexity note: near-constant stream "
             "(mean L below 2).\n");
    printf("Linear complexity class chi square (df=5) is ");
    dnum_(fp ? "%.17g" : "%1.2f", r->bm_chi);
    printf(" (p ");
    dnum_(fp ? "%.17g" : "%f", r->bm_chi_p);
    printf(", advisory).\n");
    printf("Linear complexity deviation z is ");
    dnum_(fp ? "%.17g" : "%f", r->bm_deviation);
    printf(".\n");
  }

  if (r->maurer_dev != r->maurer_dev) {
    printf("Maurer universal: pass -eee to compute it.\n");
  } else {
    printf("\nMaurer universal fn is ");
    dnum_(fp ? "%.17g" : "%f", r->maurer_fn);
    printf(" (expected ");
    dnum_(fp ? "%.17g" : "%f", r->maurer_expected);
    printf(", %" PRIu64 " test blocks).\n", (u64) r->maurer_k);
    if (r->maurer_degenerate)
      printf("Maurer universal note: highly repetitive stream "
             "(fn far below expected).\n");
    printf("Maurer universal deviation z is ");
    dnum_(fp ? "%.17g" : "%f", r->maurer_dev);
    printf(".\n");
  }

  if (r->perment_deviation != r->perment_deviation) {
    printf("Permutation entropy: pass -e to compute it.\n");
  } else {
    printf("\nPermutation entropy H_norm is ");
    dnum_(fp ? "%.17g" : "%f", r->perment_h_norm);
    printf(" (m=4, %" PRIu64 " windows).\n", (u64) r->perment_windows);
    printf("Permutation entropy chi square (df=23) is ");
    dnum_(fp ? "%.17g" : "%1.2f", r->perment_chi);
    printf(" (p ");
    dnum_(fp ? "%.17g" : "%f", r->perment_chi_p);
    printf(", advisory).\n");
    printf("Permutation entropy deviation z is ");
    dnum_(fp ? "%.17g" : "%f", r->perment_deviation);
    printf(".\n");
  }

  if (r->mrank_dev != r->mrank_dev) {
    printf("Binary matrix rank: pass -eee to compute it.\n");
  } else {
    printf("\nBinary matrix rank (32x32, %" PRIu64 " matrices): "
           "r=32 %u, r=31 %u, r<=30 %u.\n",
           (u64) r->mrank_matrices,
           r->mrank_r32, r->mrank_r31, r->mrank_rlo);
    if (r->mrank_underpowered)
      printf("Binary matrix rank note: underpowered "
             "(fewer than 16 matrices).\n");
    printf("Binary matrix rank chi square (df=2) is ");
    dnum_(fp ? "%.17g" : "%1.2f", r->mrank_chi);
    printf(".\n");
    printf("Binary matrix rank deviation z is ");
    dnum_(fp ? "%.17g" : "%f", r->mrank_dev);
    printf(".\n");
  }

}
