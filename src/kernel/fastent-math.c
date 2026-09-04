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

#include "fastent-math.h"

#include <math.h>
#include <string.h>

#ifdef __DJGPP__
  /*  No fma in DJGPP libm; the error term in two_prod degrades.  */
  static inline double fma(double a, double b, double c) { return a * b + c; }
#endif

#include "log2_tables.h"

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

static inline f64 fastent_qnan_(void) {
  u64 b = 0x7ff8000000000000ULL;
  f64 r; memcpy(&r, &b, 8); return r;
}

/*  log2(1 + r) via degree-12 Horner.  Passing r = -u yields log2(1 - u)
    because the c_k alternate in sign.  */
static inline dd_t log2_1pr_poly(f64 r) {
  dd_t poly = fastent_log2_1ps_coeff_dd[11];
  i32 k;
  for (k = 10; k >= 0; k--) {
    poly = dd_mul_d_(poly, r);
    poly = dd_add_(poly, fastent_log2_1ps_coeff_dd[k]);
  }
  return dd_mul_d_(poly, r);
}

/*  log2(x) in DD by generic exponent / mantissa decomposition.  */
static dd_t log2_generic_dd_(f64 x) {
  u64 pb;
  memcpy(&pb, &x, 8);
  i32 e_biased = (i32) ((pb >> 52) & 0x7ff);
  u64 mant     = pb & 0x000fffffffffffffULL;
  i32 e;
  if (e_biased == 0) {
    i32 sh = 0;
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

  i32 i = (i32) ((mant >> 45) & 0x7f);
  f64 r = fma(m, fastent_inv_c[i], -1.0);

  dd_t log2_1ps = log2_1pr_poly(r);
  dd_t log2_m   = dd_add_(fastent_log2_T_dd[i], log2_1ps);
  return dd_add_d_(log2_m, (f64) e);
}

/*  log2(x) in DD for 0.5 < x < 1, via q = 1 - x (Sterbenz-exact), so
    the result avoids the e + log2(m) cancellation near 1.  */
static dd_t log2_near1_dd_(f64 x) {
  f64 q  = 1.0 - x;
  i32 i  = (i32) (q * 256.0);
  f64 ci = (f64) i / 256.0;
  f64 s  = q - ci;
  f64 u  = s * fastent_inv_one_minus_c[i];
  dd_t log2_1mu = log2_1pr_poly(-u);
  return dd_add_(fastent_log2_T_minus_dd[i], log2_1mu);
}

f64 fastent_entropy_term(f64 p) {
  u64 pb;
  memcpy(&pb, &p, 8);
  if (pb >> 63) {
    if (pb == 0x8000000000000000ULL) return 0.0;  /*  -0  */
    return fastent_qnan_();
  }
  if ((pb >> 52) == 0x7ff) return fastent_qnan_();
  if (pb == 0)             return 0.0;
  if (p >= 1.0)            return p == 1.0 ? 0.0 : fastent_qnan_();
  if (p == 0.5)            return 0.5;

  /*  Op order chosen so finalised entropy stays bit-identical.  */
  dd_t log2_p   = (p > 0.5) ? log2_near1_dd_(p) : log2_generic_dd_(p);
  dd_t neg_log2 = (dd_t){ -log2_p.hi, -log2_p.lo };
  dd_t res_dd   = dd_mul_d_(neg_log2, p);
  return res_dd.hi + res_dd.lo;
}

f64 fastent_log2(f64 x) {
  u64 b;
  memcpy(&b, &x, 8);
  if ((b >> 52 & 0x7ff) == 0x7ff)            /*  NaN or +-inf  */
    return (b << 1) == 0xffe0000000000000ULL ? x : fastent_qnan_();
  if (b >> 63 || b == 0) return fastent_qnan_();   /*  x <= 0  */
  if (x == 1.0)          return 0.0;
  dd_t lp = (x > 0.5 && x < 1.0) ? log2_near1_dd_(x) : log2_generic_dd_(x);
  return lp.hi + lp.lo;
}

f64 fastent_log2_ge1(f64 x) {
  u64 b;
  memcpy(&b, &x, 8);
  if ((b >> 52 & 0x7ff) == 0x7ff)
    return (b << 1) == 0xffe0000000000000ULL ? x : fastent_qnan_();
  if (b >> 63 || b == 0) return fastent_qnan_();
  if (x == 1.0)          return 0.0;
  dd_t lp = log2_generic_dd_(x);   /*  x >= 1: no split, no denormal  */
  return lp.hi + lp.lo;
}

f64 fastent_log2_ratio(u64 a, u64 b) {
  if (b == 0 || a == 0) return fastent_qnan_();
  if (a == b)           return 0.0;
  return fastent_log2_ge1((f64) a / (f64) b);
}

f64 fastent_log2_fast(f64 x) {
  /*  Caller guarantees finite normal x > 0.  ~1e-9, plain double.  */
  u64 b;
  memcpy(&b, &x, 8);
  i32 e    = (i32) ((b >> 52) & 0x7ff) - 1023;
  u64 mant = b & 0x000fffffffffffffULL;
  u64 mb   = ((u64) 1023u << 52) | mant;
  f64 m;
  memcpy(&m, &mb, 8);
  i32 i  = (i32) ((mant >> 45) & 0x7f);
  f64 r  = fma(m, fastent_inv_c[i], -1.0);
  /*  log2(1+r) ~ (r - r^2/2 + r^3/3) / ln2, |r| < 2^-7.  */
  f64 poly = r * (1.0 + r * (-0.5 + r * (1.0 / 3.0)));
  return (f64) e + fastent_log2_T_dd[i].hi + poly * 1.4426950408889634;
}
