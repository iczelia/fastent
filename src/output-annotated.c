/*  fastent: annotated interpretive report.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "output.h"
#include "port-term.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

enum { B_PASS = 0, B_WEAK = 1, B_FAIL = 2, B_NA = 3, B_INFO = 4 };

static const char * badge_txt_(int b) {
  switch (b) {
    case B_PASS: return "[PASS]";
    case B_WEAK: return "[WEAK]";
    case B_FAIL: return "[FAIL]";
    case B_INFO: return "[info]";
    default:     return "[ N/A]";
  }
}

static const char * badge_name_(int b) {
  switch (b) {
    case B_PASS: return "PASS";
    case B_WEAK: return "WEAK";
    case B_FAIL: return "FAIL";
    case B_INFO: return "INFO";
    default:     return "N/A";
  }
}

static int color_active_(int color_opt) {
  if (color_opt == 0) return 0;
  if (color_opt == 2) return 1;
  if (getenv("NO_COLOR")) return 0;
  return fastent_term_isatty();
}

/*  Ratio of an entropy-like measure to its maximum.  */
static int ratio_badge_(f64 r) {
  if (!(r == r))   return B_NA;
  if (r >= 0.999)  return B_PASS;
  if (r >= 0.99)   return B_WEAK;
  return B_FAIL;
}

/*  Upper-tail p-value: random data sits mid-range.  */
static int p_badge_(f64 p) {
  if (!(p == p)) return B_NA;
  if (p >= 0.05 && p <= 0.95) return B_PASS;
  if ((p >= 0.01 && p < 0.05) || (p > 0.95 && p <= 0.99)) return B_WEAK;
  return B_FAIL;
}

static int z_badge_(f64 z) {
  z = fabs(z);
  if (!(z == z)) return B_NA;
  if (z < 2.0)   return B_PASS;
  if (z < 3.0)   return B_WEAK;
  return B_FAIL;
}

typedef struct {
  int  worst;        /*  highest non-N/A severity seen so far  */
  int  seen;         /*  any core metric scored yet            */
  char who[24];      /*  copy of the metric carrying `worst`   */
} verdict_acc;

static void row_(
    const fastent_options * o, int color, verdict_acc * v, int core,
    const char * label, const char * aux, int badge, const char * expl) {
  printf("  %-19s%-25s ", label, aux);
  if (color) fastent_term_set_sev(badge == B_INFO ? B_NA : badge);
  fastent_term_write(badge_txt_(badge));
  if (color) fastent_term_set_sev(-1);
  if (expl && *expl) printf("  %s", expl);
  putchar('\n');
  if (core && badge != B_NA) {
    v->seen = 1;
    if (badge > v->worst) {
      v->worst = badge;
      snprintf(v->who, sizeof v->who, "%s", label);
    }
  }
}

void fastent_print_annotated(
    const fastent_result * r, const fastent_options * o) {
  if (o->histogram) fastent_print_histogram(r, o);

  const char * samp = o->binary ? "bit" : "byte";
  const i32    bins = o->binary ? 2 : 256;
  const f64    hmax = o->binary ? 1.0 : 8.0;
  const f64    totalc = (f64) r->total_samples;
  const int    color  = color_active_(o->color);
  const char * nf = o->full_precision ? "%.17g" : "%.6g";
  char aux[96];

  printf("fastent  %" PRIu64 " %s%s\n\n",
         (u64) r->total_samples, samp,
         r->total_samples == 1 ? "" : "s");

  verdict_acc v = { B_PASS, 0, "" };

  /*  Shannon entropy.  */
  {
    const f64 ratio = r->entropy / hmax;
    int b = ratio_badge_(ratio);
    snprintf(aux, sizeof aux, o->full_precision
             ? "%.17g/%g  %.2f%% max" : "%.6g/%g  %.2f%% max",
             r->entropy, hmax, 100.0 * ratio);
    row_(o, color, &v, 1, "Shannon entropy", aux, b,
         b == B_PASS ? "looks random"
       : b == B_WEAK ? "slightly compressible"
       :               "compressible");
  }

  /*  Min-entropy (worst-case, the crypto-relevant measure).  */
  {
    const f64 ratio = r->min_entropy / hmax;
    int b = ratio_badge_(ratio);
    snprintf(aux, sizeof aux, o->full_precision
             ? "%.17g/%g  %.2f%% max" : "%.6g/%g  %.2f%% max",
             r->min_entropy, hmax, 100.0 * ratio);
    row_(o, color, &v, 1, "Min-entropy", aux, b,
         b == B_PASS ? "full strength"
       : b == B_WEAK ? "some worst-case bias"
       :               "weak worst-case");
  }

  /*  Chi-square.  */
  {
    int b = p_badge_(r->chi_probability);
    snprintf(aux, sizeof aux, o->full_precision
             ? "%.17g  p=%.17g" : "%.4g  p=%.4f",
             r->chi_square, r->chi_probability);
    char lbl[24];
    snprintf(lbl, sizeof lbl, "Chi-square d=%d", o->binary ? 1 : 255);
    row_(o, color, &v, 1, lbl, aux, b,
         b == B_PASS ? "within random range"
       : b == B_WEAK ? "suspect"
       : b == B_NA   ? ""
       :               "far from random");
  }

  /*  Poker (16 nibble bins, df=15); byte mode only.  */
  if (o->binary) {
    row_(o, color, &v, 0, "Poker d=15", "not applicable to bits", B_NA, "");
  } else {
    int b = p_badge_(r->poker_p);
    snprintf(aux, sizeof aux, o->full_precision
             ? "%.17g  p=%.17g" : "%.4g  p=%.4f",
             r->poker_chisq, r->poker_p);
    row_(o, color, &v, 1, "Poker d=15", aux, b,
         b == B_PASS ? "nibble freqs uniform"
       : b == B_WEAK ? "suspect nibble bias"
       :               "nibble freqs skewed");
  }

  /*  Collision entropy.  */
  {
    const f64 ratio = r->collision_entropy / hmax;
    int b = ratio_badge_(ratio);
    snprintf(aux, sizeof aux, o->full_precision
             ? "%.17g/%g  %.2f%% max" : "%.6g/%g  %.2f%% max",
             r->collision_entropy, hmax, 100.0 * ratio);
    row_(o, color, &v, 0, "Collision entropy", aux, b, "");
  }

  /*  Index of coincidence (verdict follows collision entropy).  */
  {
    int b = ratio_badge_(r->collision_entropy / hmax);
    snprintf(aux, sizeof aux, "%.6g  (uniform %.6g)", r->ic, 1.0 / (f64) bins);
    if (o->full_precision)
      snprintf(aux, sizeof aux, "%.17g  (uniform %.17g)",
               r->ic, 1.0 / (f64) bins);
    row_(o, color, &v, 0, "Coincidence", aux, b, "");
  }

  /*  Arithmetic mean: z against the uniform sampling distribution.  */
  {
    const f64 exp_m = o->binary ? 0.5 : 127.5;
    const f64 sd    = sqrt(((f64) bins * (f64) bins - 1.0) / 12.0);
    const f64 z     = (totalc > 0.0)
                      ? (r->mean - exp_m) / (sd / sqrt(totalc)) : NAN;
    int b = z_badge_(z);
    snprintf(aux, sizeof aux, o->full_precision
             ? "%.17g  (expect %.1f)" : "%.6g  (expect %.1f)",
             r->mean, exp_m);
    row_(o, color, &v, 1, "Mean", aux, b,
         b == B_PASS ? "centered"
       : b == B_WEAK ? "slightly off-center"
       : b == B_NA   ? ""
       :               "off-center");
  }

  /*  Monte Carlo pi.  */
  {
    const f64 err = 100.0 * (fabs(M_PI - r->monte_pi) / M_PI);
    int b = (!(err == err)) ? B_NA
          : (err < 0.5) ? B_PASS : (err < 2.0) ? B_WEAK : B_FAIL;
    snprintf(aux, sizeof aux, o->full_precision
             ? "%.17g  err %.4g%%" : "%.6g  err %.3f%%", r->monte_pi, err);
    row_(o, color, &v, 0, "Monte Carlo pi", aux, b, "");
  }

  /*  Serial correlation: z = |scc| * sqrt(N).  */
  {
    int b;
    if (!FASTENT_SCC_DEFINED(r->scc)) {
      snprintf(aux, sizeof aux, "undefined");
      b = B_NA;
    } else {
      const f64 z = fabs(r->scc) * sqrt(totalc);
      b = z_badge_(z);
      snprintf(aux, sizeof aux, o->full_precision
               ? "%.17g  (expect 0)" : "%.6g  (expect 0)", r->scc);
    }
    row_(o, color, &v, 1, "Serial correlation", aux, b,
         b == B_PASS ? "uncorrelated"
       : b == B_WEAK ? "weak correlation"
       : b == B_NA   ? "all values equal"
       :               "correlated");
  }

  /*  Per-bit-position bias: worst bit vs the uniform Binomial(N,1/2),
      z = 2*|f-0.5|*sqrt(N).  Byte mode only.  */
  if (r->bit_bias_worst < 0) {
    row_(o, color, &v, 0, "Bit balance", "not applicable to bits",
         B_NA, "");
  } else {
    const f64 z = 2.0 * r->bit_bias_max * sqrt(totalc);
    int b = z_badge_(z);
    snprintf(aux, sizeof aux, o->full_precision
             ? "bit %d  P(1)=%.17g" : "bit %d  P(1)=%.4f",
             r->bit_bias_worst, r->bit_freq[r->bit_bias_worst]);
    row_(o, color, &v, 1, "Bit balance", aux, b,
         b == B_PASS ? "bit lanes balanced"
       : b == B_WEAK ? "mild bit-lane bias"
       :               "structured bit lane");
  }

  /*  Informational only (core=0).  redux = fraction of order-0
      entropy the previous byte explains; NaN unless -ee ran.  */
  if (r->conditional_entropy != r->conditional_entropy) {
    row_(o, color, &v, 0, "Cond. entropy", "pass -ee (byte only)",
         B_NA, "");
    row_(o, color, &v, 0, "Mutual info", "pass -ee (byte only)",
         B_NA, "");
  } else {
    const f64 h0 = r->entropy;
    const f64 redux = h0 > 0.0 ? 1.0 - r->conditional_entropy / h0 : 0.0;
    int b = (h0 <= 0.0)    ? B_NA
          : redux < 0.05   ? B_PASS
          : redux < 0.20   ? B_WEAK : B_FAIL;
    snprintf(aux, sizeof aux, o->full_precision
             ? "%.17g b/%s" : "%.4g bits/%s",
             r->conditional_entropy, samp);
    row_(o, color, &v, 0, "Cond. entropy", aux, b,
         b == B_NA   ? ""
       : b == B_PASS ? "no order-1 structure"
       : b == B_WEAK ? "mild order-1 correlation"
       :               "strong order-1 structure");
    snprintf(aux, sizeof aux, o->full_precision
             ? "%.17g bits (%.0f%% H0)" : "%.4g bits (%.0f%% H0)",
             r->mutual_information,
             h0 > 0.0 ? 100.0 * r->mutual_information / h0 : 0.0);
    row_(o, color, &v, 0, "Mutual info", aux, b,
         b == B_NA   ? ""
       : b == B_PASS ? "prev symbol uninformative"
       : b == B_WEAK ? "prev mildly informative"
       :               "prev highly informative");
  }

  /*  LZ77F (-eee): lz_deviation is the headline verdict; the two
      z-component rows (S1/S3 and S2) carry their own per-component
      badge so the headline is exactly their worst.  Off/len conc. is
      a structure descriptor (no pass/fail), literal chi advisory.  */
  if (r->lz_deviation != r->lz_deviation) {
    row_(o, color, &v, 0, "LZ77F", "pass -eee", B_NA, "");
  } else {
    int b = z_badge_(r->lz_deviation);
    /*  z_i = component / sigma0 ; sigma0 = cmax / lz_deviation, so
        z_i = comp * dev / cmax (no n needed; reproduces the fuse).  */
    f64 s1 = r->lz_cr_excess, s2 = r->lz_lit_kl / 8.0;
    f64 s3 = r->lz_match_cov;
    f64 cmax = s1 > s2 ? s1 : s2;  if (s3 > cmax) cmax = s3;
    f64 dev = r->lz_deviation;
    int ok = (dev > 0.0 && cmax > 0.0);
    f64 s13 = s1 > s3 ? s1 : s3;
    int bcov = ok ? z_badge_(s13 * dev / cmax) : B_PASS;
    int blit = ok ? z_badge_(s2  * dev / cmax) : B_PASS;
    snprintf(aux, sizeof aux, o->full_precision
             ? "z=%.17g" : "z=%.4g", r->lz_deviation);
    row_(o, color, &v, 1, "LZ77F deviation", aux, b,
         b == B_PASS ? "incompressible"
       : b == B_WEAK ? "slightly compressible"
       :               "compressible / structured");
    snprintf(aux, sizeof aux, o->full_precision
             ? "%.17g  cov %.17g" : "%.4g  cov %.4g",
             r->lz_cr_excess, r->lz_match_cov);
    row_(o, color, &v, 0, "  CR excess / cov", aux, bcov,
         bcov == B_PASS ? "incompressible" : "compressible");
    snprintf(aux, sizeof aux, o->full_precision
             ? "H_lit=%.17g  KL=%.17g" : "H_lit=%.4g  KL=%.4g",
             r->lz_lit_h, r->lz_lit_kl);
    row_(o, color, &v, 0, "  Literal skew", aux, blit,
         blit == B_PASS ? "literals unbiased" : "literal byte skew");
    snprintf(aux, sizeof aux, o->full_precision
             ? "conc=%.17g  mlen+%.17g" : "conc=%.4g  mlen+%.4g",
             r->lz_off_conc, r->lz_mlen_excess);
    row_(o, color, &v, 0, "  Off/len conc.", aux, B_INFO,
         "structure descriptor");
    {
      int cb = p_badge_(r->lz_lit_chi_p);
      snprintf(aux, sizeof aux, o->full_precision
               ? "%.17g  p=%.17g" : "%.4g  p=%.4f",
               r->lz_lit_chi, r->lz_lit_chi_p);
      row_(o, color, &v, 0, "  Literal chi", aux, cb,
           cb == B_PASS ? "literals uniform (advisory)"
         : cb == B_WEAK ? "suspect literal bias (advisory)"
         : cb == B_NA   ? ""
         :                "literal byte bias (advisory)");
    }
    if (r->lz_megamatch)
      printf("  %-19s%-25s        exact-repeat / single dominant "
             "match (concentration at limit)\n", "  Note", "");
  }

  /*  Linear complexity (-eee, alongside LZ77F): bm_deviation is the
      headline verdict; meanL vs mu(M) is a structure descriptor
      (B_INFO, no pass/fail); the NIST class chi-square is advisory.  */
  if (r->bm_deviation != r->bm_deviation) {
    row_(o, color, &v, 0, "Linear complexity", "pass -eee", B_NA, "");
  } else {
    int b = z_badge_(r->bm_deviation);
    snprintf(aux, sizeof aux, o->full_precision
             ? "z=%.17g" : "z=%.4g", r->bm_deviation);
    row_(o, color, &v, 1, "Linear complexity", aux, b,
         b == B_PASS ? "no linear recurrence"
       : b == B_WEAK ? "weak linear structure"
       :               "low linear complexity (LFSR-like)");
    snprintf(aux, sizeof aux, o->full_precision
             ? "L=%.17g  mu=%.17g" : "L=%.6g  mu=%.6g",
             r->bm_mean_lc, r->bm_mu);
    row_(o, color, &v, 0, "  Mean L / mu", aux, B_INFO,
         "structure descriptor");
    {
      int cb = p_badge_(r->bm_chi_p);
      snprintf(aux, sizeof aux, o->full_precision
               ? "%.17g  p=%.17g" : "%.4g  p=%.4f",
               r->bm_chi, r->bm_chi_p);
      row_(o, color, &v, 0, "  Class chi", aux, cb,
           cb == B_PASS ? "classes as expected (advisory)"
         : cb == B_WEAK ? "suspect class skew (advisory)"
         : cb == B_NA   ? ""
         :                "linear-complexity class skew (advisory)");
    }
    if (r->bm_degenerate)
      printf("  %-19s%-25s        near-constant stream "
             "(mean L below 2)\n", "  Note", "");
  }

  /*  Maurer universal (-eee, alongside LZ77F / BM): maurer_dev is the
      headline verdict; fn vs expected(L) is a compressibility
      descriptor (B_INFO, no pass/fail).  */
  if (r->maurer_dev != r->maurer_dev) {
    row_(o, color, &v, 0, "Maurer universal", "pass -eee", B_NA, "");
  } else {
    int b = z_badge_(r->maurer_dev);
    snprintf(aux, sizeof aux, o->full_precision
             ? "z=%.17g" : "z=%.4g", r->maurer_dev);
    row_(o, color, &v, 1, "Maurer universal", aux, b,
         b == B_PASS ? "incompressible (universal)"
       : b == B_WEAK ? "weak compressible structure"
       :               "compressible / repetitive");
    snprintf(aux, sizeof aux, o->full_precision
             ? "fn=%.17g  exp=%.17g" : "fn=%.6g  exp=%.6g",
             r->maurer_fn, r->maurer_expected);
    row_(o, color, &v, 0, "  Fn / expected", aux, B_INFO,
         "compressibility descriptor");
    if (r->maurer_degenerate)
      printf("  %-19s%-25s        highly repetitive stream "
             "(fn far below expected)\n", "  Note", "");
  }

  /*  Informational footer (no verdict).  */
  char d1[48], d2[48], d3[48];
  snprintf(d1, sizeof d1, "Distinct %u/%d", r->distinct, bins);
  snprintf(d2, sizeof d2, "Redundancy %.4g%%", r->redundancy * 100.0);
  snprintf(d3, sizeof d3, nf, r->stddev);
  printf("\n  %s   %s   Stddev %s\n", d1, d2, d3);

  /*  Runs / longest run / cusum: descriptive, no verdict.  Shown
      only when -ee ran.  */
  if (r->longest_run == r->longest_run) {
    printf("  Longest run %.0f %s", r->longest_run, samp);
    if (r->runs == r->runs)      printf("s   Runs %.0f", r->runs);
    else                         printf("s");
    if (r->cusum_max == r->cusum_max) printf("   Cusum %.0f", r->cusum_max);
    putchar('\n');
  }

  /*  Headline.  */
  const char * head = v.worst == B_PASS ? "PASSES AS RANDOM"
                    : v.worst == B_WEAK ? "PASSES AS ALMOST RANDOM"
                    :                     "DOES NOT PASS AS RANDOM";
  const int hsev = v.worst == B_PASS ? B_PASS
                 : v.worst == B_WEAK ? B_WEAK : B_FAIL;
  printf("\n  VERDICT: ");
  if (color) fastent_term_set_sev(hsev);
  fastent_term_write(head);
  if (color) fastent_term_set_sev(-1);
  if (v.worst == B_PASS || !v.seen)
    printf("   (all checks pass)\n");
  else
    printf("   (weakest: %s, %s)\n", v.who, badge_name_(v.worst));
}
