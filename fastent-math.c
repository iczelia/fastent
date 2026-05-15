/*  fastent: scalar math shims.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "fastent-math.h"

#include <math.h>          /*  fma (correctly rounded by IEEE 754 mandate)  */
#include <string.h>

#ifdef __DJGPP__
  /*  No fma in DJGPP libm.  Two_prod's error-free product collapses to
      a*b - p (a slightly looser error term).  Acceptable for the
      entropy term where the polynomial gives plenty of headroom.  */
  static inline double fma(double a, double b, double c) {
    return a * b + c;
  }
#endif

#include "log2_tables.h"   /*  fastent_log2_T_dd[], inv_c[], log2_1ps_coeff_dd[]  */

/*  Double-double primitives.  All ops use only basic arithmetic + FMA;
    portable to any IEEE 754 host with a correctly-rounded fma (C99
    mandates it).  Identical in spirit to chisq.c's DD helpers - kept
    private here so fastent-math.c doesn't depend on chisq.h.  */

typedef fastent_dd_t dd_t;

static inline dd_t two_sum_(f64 a, f64 b) {
  f64 s = a + b;
  f64 bb = s - a;
  f64 err = (a - (s - bb)) + (b - bb);
  return (dd_t){ s, err };
}
static inline dd_t fast_two_sum_(f64 a, f64 b) {
  f64 s = a + b;
  f64 err = b - (s - a);
  return (dd_t){ s, err };
}
static inline dd_t two_prod_(f64 a, f64 b) {
  f64 p   = a * b;
  f64 err = fma(a, b, -p);
  return (dd_t){ p, err };
}
static inline dd_t dd_add_(dd_t a, dd_t b) {
  dd_t s = two_sum_(a.hi, b.hi);
  s.lo += a.lo + b.lo;
  return fast_two_sum_(s.hi, s.lo);
}
static inline dd_t dd_add_d_(dd_t a, f64 b) {
  dd_t s = two_sum_(a.hi, b);
  s.lo += a.lo;
  return fast_two_sum_(s.hi, s.lo);
}
static inline dd_t dd_mul_d_(dd_t a, f64 b) {
  dd_t p = two_prod_(a.hi, b);
  p.lo += a.lo * b;
  return fast_two_sum_(p.hi, p.lo);
}

/*  Build a qNaN bit pattern without going through math.h.  */
static inline f64 fastent_qnan_(void) {
  u64 b = 0x7ff8000000000000ULL;
  f64 r; memcpy(&r, &b, 8); return r;
}

/*  log2(1 + r) via the degree-12 Horner over DD coefficients.  The
    same coefficients also drive log2(1 - u): pass r = -u, the
    alternating signs in c_k * (-u)^k collapse to the all-negative
    series for log2(1 - u).  */
static inline dd_t log2_1pr_poly(f64 r) {
  dd_t poly = fastent_log2_1ps_coeff_dd[11];
  int k;
  for (k = 10; k >= 0; k--) {
    poly = dd_mul_d_(poly, r);
    poly = dd_add_(poly, fastent_log2_1ps_coeff_dd[k]);
  }
  return dd_mul_d_(poly, r);
}

f64 fastent_entropy_term(f64 p) {
  /*  Special cases.  */
  u64 pb;
  memcpy(&pb, &p, 8);
  /*  Sign bit set -> negative (or -0).  */
  if (pb >> 63) {
    /*  -0 -> +0 (the limit at 0 is 0), every other negative -> NaN.  */
    if (pb == 0x8000000000000000ULL) return 0.0;
    return fastent_qnan_();
  }
  /*  NaN / +Inf: top exponent all ones, mantissa nonzero -> NaN out;
      +Inf -> NaN out (not a probability).  */
  if ((pb >> 52) == 0x7ff) return fastent_qnan_();
  /*  +0  -> +0  (entropy contribution of an absent symbol is zero).  */
  if (pb == 0) return 0.0;
  /*  >= 1: only p == 1 is valid (contributes 0); anything above is NaN.  */
  if (p >= 1.0) {
    if (p == 1.0) return 0.0;
    return fastent_qnan_();
  }
  /*  p == 0.5 -> 0.5 exactly.  Avoids the i = 128 out-of-table edge
      in the split-domain branch below.  */
  if (p == 0.5) return 0.5;

  if (p > 0.5) {
    /*  Split-domain path.  q = 1 - p is exact (Sterbenz, since
        p in (0.5, 1)).  q in (0, 0.5).  Reduce via a table indexed
        by floor(q * 256): c_i = i/256, log2(1 - q) = log2(1 - c_i)
        + log2(1 - u) with u = (q - c_i) * inv_one_minus_c[i].  No
        cancellation - we never add a large `e` to a near-1 log2(m).  */
    f64 q  = 1.0 - p;
    int i  = (int)(q * 256.0);          /*  in [0, 127] since q < 0.5  */
    f64 ci = (f64) i / 256.0;
    f64 s  = q - ci;                    /*  exact (Sterbenz)            */
    f64 u  = s * fastent_inv_one_minus_c[i];
    dd_t log2_1mu = log2_1pr_poly(-u);  /*  log2(1 - u)                 */
    dd_t log2_p   = dd_add_(fastent_log2_T_minus_dd[i], log2_1mu);
    dd_t neg_log2 = (dd_t){ -log2_p.hi, -log2_p.lo };
    dd_t res_dd   = dd_mul_d_(neg_log2, p);
    return res_dd.hi + res_dd.lo;
  }

  /*  p < 0.5: original path.  Decompose p = m * 2^e with m in [1, 2);
      |e| >= 1 and log2(m) in [0, 1) don't overlap so e + log2(m) has
      no cancellation.  */
  i32 e_biased = (i32)((pb >> 52) & 0x7ff);
  u64 mant     = pb & 0x000fffffffffffffULL;
  i32 e;
  if (e_biased == 0) {
    /*  Subnormal: normalize until the implicit-1 lands at bit 52.  */
    int sh = 0;
    while ((mant & (1ULL << 52)) == 0) {
      mant <<= 1;
      sh++;
      if (sh > 64) break;
    }
    e = -1022 - sh;
  } else {
    e = e_biased - 1023;
  }
  u64 m_bits = ((u64) 1023u << 52) | (mant & 0x000fffffffffffffULL);
  f64 m;
  memcpy(&m, &m_bits, 8);

  int i = (int)((mant >> 45) & 0x7f);
  f64 r = fma(m, fastent_inv_c[i], -1.0);

  dd_t log2_1ps = log2_1pr_poly(r);
  dd_t log2_m   = dd_add_(fastent_log2_T_dd[i], log2_1ps);
  dd_t log2_p   = dd_add_d_(log2_m, (f64) e);

  dd_t neg_log2 = (dd_t){ -log2_p.hi, -log2_p.lo };
  dd_t res_dd   = dd_mul_d_(neg_log2, p);
  return res_dd.hi + res_dd.lo;
}
