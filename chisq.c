/*  fastent: chi-square upper tail probability.

    Copyright (C) 2026 Kamila Szewczyk.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.

    Portable double-double implementation, audited bit-for-bit against
    a __float128 reference: 0 ULP max error across 5 M log-uniform
    random samples spanning chisq in [10^-10, 1490] for df=1 (a=0.5)
    and chisq in [10^-3, 1490] for df=255 (a=127.5), including the
    deep tail down to Q ~ 3*10^-173.

    Why double-double:

      The two domains we care about (df = 1 and df = 255) both have
      half-integer shape parameter a, which collapses the regularised
      incomplete gamma to a closed form requiring only erfc and a
      finite polynomial sum.  We avoid every "wobbly" libm function
      (no erfc, no erfcl, no lgamma, no lgammal) by computing those
      values ourselves via series / continued fraction in double-
      double arithmetic.

      Libm calls in the deployed code are restricted to: fma, sqrt,
      round, ldexp: all four are either correctly-rounded by IEEE
      754 (sqrt, fma) or trivial bit operations (round, ldexp).

    Algorithms:

      df = 1   :  Q(0.5, z) computed directly as the regularised
                  incomplete gamma via the lower-tail Taylor series
                  (z < 1.5) or the Steed/Lentz continued fraction
                  (z >= 1.5), all in DD.  We never call libm erfc.

      df = 255 :  Half-integer closed form
                    Q(127.5, z) = Q(0.5, z) + 2 * e^-z * U(z)
                    U(z)        = sum_{k=0..126} T_k
                    T_0         = sqrt(z/pi)
                    T_{k+1}     = T_k * 2z/(2k+3)
                  derived by integrating Gamma(a, z) by parts 127
                  times down to Gamma(1/2, z) = sqrt(pi)*erfc(sqrt(z)).
                  Finite 127-term sum, no convergence test, no regime
                  split.  e^-z is factored out of the recurrence so
                  the loop runs in normal range; the single final
                  multiplication carries any subnormal precision loss
                  (limited to the inherent precision of subnormal
                  doubles).  */

#include <math.h>

#include "common.h"
#include "chisq.h"

/*  Double-double (DD) primitives.  hi + lo with |lo| <= ulp(hi)/2.
    All ops use only basic arithmetic + fma; portable to any IEEE 754
    host with a correctly-rounded fma (C99 mandates it).  */

typedef struct { f64 hi, lo; } dd_t;

static inline dd_t dd_of(f64 a) { return (dd_t){ a, 0.0 }; }

/*  Knuth TwoSum: exact a + b decomposed into (hi, lo).  6 flops.  */
static inline dd_t two_sum(f64 a, f64 b) {
  f64 s  = a + b;
  f64 bb = s - a;
  f64 err = (a - (s - bb)) + (b - bb);
  return (dd_t){ s, err };
}

/*  Dekker FastTwoSum: requires |a| >= |b|.  3 flops.  */
static inline dd_t fast_two_sum(f64 a, f64 b) {
  f64 s   = a + b;
  f64 err = b - (s - a);
  return (dd_t){ s, err };
}

/*  TwoProd via FMA: exact a * b decomposed into (hi, lo).  2 flops.  */
static inline dd_t two_prod(f64 a, f64 b) {
  f64 p  = a * b;
  f64 er = fma(a, b, -p);
  return (dd_t){ p, er };
}

/*  DD + DD (Hida/Li/Bailey sloppy add).  */
static inline dd_t dd_add(dd_t a, dd_t b) {
  dd_t s = two_sum(a.hi, b.hi);
  s.lo += a.lo + b.lo;
  return fast_two_sum(s.hi, s.lo);
}
static inline dd_t dd_sub(dd_t a, dd_t b) {
  dd_t s = two_sum(a.hi, -b.hi);
  s.lo += a.lo - b.lo;
  return fast_two_sum(s.hi, s.lo);
}
static inline dd_t dd_add_d(dd_t a, f64 b) {
  dd_t s = two_sum(a.hi, b);
  s.lo += a.lo;
  return fast_two_sum(s.hi, s.lo);
}

/*  DD * DD.  ~4 * 2^-106 relative error.  */
static inline dd_t dd_mul(dd_t a, dd_t b) {
  dd_t p = two_prod(a.hi, b.hi);
  p.lo += a.hi * b.lo + a.lo * b.hi;
  return fast_two_sum(p.hi, p.lo);
}
static inline dd_t dd_mul_d(dd_t a, f64 b) {
  dd_t p = two_prod(a.hi, b);
  p.lo += a.lo * b;
  return fast_two_sum(p.hi, p.lo);
}

/*  DD / DD via two-step Newton: q1 + (a - q1 b)/b.  */
static inline dd_t dd_div(dd_t a, dd_t b) {
  f64 q1 = a.hi / b.hi;
  dd_t p = dd_mul_d(b, q1);
  dd_t r = dd_sub(a, p);
  f64 q2 = r.hi / b.hi;
  return fast_two_sum(q1, q2);
}
static inline dd_t dd_div_d(dd_t a, f64 b) {
  f64 q  = a.hi / b;
  dd_t p = two_prod(q, b);
  f64 r1 = (a.hi - p.hi) - p.lo;
  f64 r  = (r1 + a.lo) / b;
  return fast_two_sum(q, r);
}

/*  DD sqrt of a double, refined by one Newton step over the libm
    double sqrt seed.  Final error bounded by ~2 ULPs of DD.  */
static inline dd_t dd_sqrt_d(f64 x) {
  if (x <= 0.0) return dd_of(0.0);
  f64 y = sqrt(x);
  f64 y_sq    = y * y;
  f64 err_ysq = fma(y, y, -y_sq);
  dd_t r      = two_sum(x, -y_sq);
  r.lo -= err_ysq;
  f64 delta = (r.hi + r.lo) / (2.0 * y);
  return fast_two_sum(y, delta);
}

/*  DD exp.  Reduce x = k*ln(2) + r with |r| <= ln(2)/2, then Taylor
    on r, then 2^k via ldexp.  The _scaled variant defers the ldexp
    so callers that subsequently multiply e^x by a large factor see
    the product land in normal range with full precision preserved.
    Crucial for the byte-mode tail near z = 745, where e^-z is in
    the subnormal regime but Q itself is in normal range.  */

static const dd_t LN2_DD = {
  0.6931471805599453,        /*  ln(2) high  */
  2.3190468138462996e-17     /*  ln(2) - high  */
};
static const f64 INV_LN2 = 1.4426950408889634;

static dd_t dd_exp_d_scaled(f64 x, int * k_out) {
  if (x < -1500.0) { *k_out = -1500; return dd_of(0.0); }
  if (x >  1500.0) { *k_out =  1500; return dd_of((f64) INFINITY); }
  f64 k_d = round(x * INV_LN2);
  int k   = (int) k_d;
  dd_t kln2 = dd_mul_d(LN2_DD, k_d);
  dd_t r    = two_sum(x, -kln2.hi);
  r.lo -= kln2.lo;
  r = fast_two_sum(r.hi, r.lo);
  dd_t res  = dd_of(1.0);
  dd_t term = dd_of(1.0);
  Fi0(19, 1,
      term = dd_div_d(dd_mul(term, r), (f64) i);
      res  = dd_add(res, term))
  *k_out = k;
  return res;
}

static dd_t dd_exp_d(f64 x) {
  int k;
  dd_t r = dd_exp_d_scaled(x, &k);
  r.hi = ldexp(r.hi, k);
  r.lo = ldexp(r.lo, k);
  return r;
}

/*  Q(0.5, z): bit-mode tail.  Computed as the regularised
    incomplete gamma via series for z < 1.5 and Lentz CF for
    z >= 1.5.  No call to libm erfc.  */

static const dd_t PI_DD = {
  3.141592653589793, 1.2246467991473532e-16
};

/*  pref_05(z) = e^-z * sqrt(z/pi), in DD.  Used by both the series
    and CF branches of Q(0.5, z).  Eager ldexp inside dd_exp_d is OK
    here because Q(0.5, z) is only ever the FINAL result for binary
    mode: if z is so large that e^-z underflows, Q itself is
    underflowing too and we just round to 0.  */
static dd_t pref_05_dd(f64 z) {
  dd_t e   = dd_exp_d(-z);
  dd_t zp  = dd_div(dd_of(z), PI_DD);
  dd_t s0  = dd_sqrt_d(zp.hi);
  dd_t s02 = dd_mul(s0, s0);
  dd_t r   = dd_sub(zp, s02);
  dd_t two_s0 = dd_mul_d(s0, 2.0);
  dd_t s   = dd_add(s0, dd_div(r, two_s0));
  return dd_mul(e, s);
}

/*  Series for P(0.5, z), Q = 1 - P, in DD.  Valid for z up to a + 1.
    For a = 0.5, that means z < 1.5.  P is small or moderate in this
    range, so the 1 - P subtraction does not cancel.  */
static dd_t Q_05_series_dd(f64 z) {
  dd_t pref = pref_05_dd(z);
  /*  term_0 = 1 / 0.5 = 2; term_{n+1} = term_n * z / (n + 1.5).  */
  dd_t term = dd_of(2.0);
  dd_t sum  = term;
  Fi0(256, 0,
      term = dd_div_d(dd_mul_d(term, z), (f64) i + 1.5);
      sum  = dd_add(sum, term);
      if (i >= 4 && fabs(term.hi) < fabs(sum.hi) * 1e-33) break)
  dd_t P = dd_mul(pref, sum);
  return dd_sub(dd_of(1.0), P);
}

/*  Lentz modified CF for Gamma(a, z), in DD.  Returns h such that
    Gamma(a, z) = z^a * e^-z * h, equivalently Q(a, z) = pref * h.  */
static dd_t gamma_cf_dd(f64 z, f64 a) {
  dd_t b = dd_add_d(dd_of(z), 1.0 - a);
  dd_t d = dd_div(dd_of(1.0), b);
  dd_t h = d;
  dd_t c = dd_of(1.0 / 1e-300);
  Fi0(4096, 1,
      f64 an = -(f64) i * ((f64) i - a);
      b = dd_add_d(b, 2.0);
      d = dd_add(dd_mul_d(d, an), b);
      if (fabs(d.hi) < 1e-300) d = dd_of(1e-300);
      dd_t an_over_c = dd_div(dd_of(an), c);
      c = dd_add(b, an_over_c);
      if (fabs(c.hi) < 1e-300) c = dd_of(1e-300);
      d = dd_div(dd_of(1.0), d);
      dd_t delta = dd_mul(d, c);
      h = dd_mul(h, delta);
      dd_t dm1 = dd_sub(delta, dd_of(1.0));
      if (fabs(dm1.hi) < 1e-32) break)
  return h;
}

static dd_t Q_05_dd(f64 z) {
  if (z <= 0.0)   return dd_of(1.0);
  if (z >= 750.0) return dd_of(0.0);
  if (z < 1.5) return Q_05_series_dd(z);
  dd_t pref = pref_05_dd(z);
  dd_t h    = gamma_cf_dd(z, 0.5);
  return dd_mul(pref, h);
}

/*  Q(127.5, z): byte-mode tail via half-integer closed form.

      Q(127.5, z) = Q(0.5, z) + 2 * e^-z * U(z)
      U(z)        = sum_{k=0}^{126} T_k
      T_0         = sqrt(z/pi)
      T_{k+1}     = T_k * 2z/(2k+3)

    The 127-step recurrence on T_k runs in normal range for all z
    in [0, 750].  e^-z is computed scaled (mantissa + 2^k exponent)
    and applied via a single ldexp AFTER the product U * mantissa
    is formed, so the deep tail (z near 745, e^-z subnormal) does
    not destroy the recurrence's accumulated precision.  */

static dd_t Q_1275_dd(f64 z) {
  if (z <= 0.0)   return dd_of(1.0);
  if (z >= 750.0) return dd_of(0.0);

  /*  T_0 = sqrt(z/pi) in DD.  Sqrt-of-DD via one Newton step over
      the dd_sqrt_d seed.  */
  dd_t zp     = dd_div(dd_of(z), PI_DD);
  dd_t s0     = dd_sqrt_d(zp.hi);
  dd_t s02    = dd_mul(s0, s0);
  dd_t resid  = dd_sub(zp, s02);
  dd_t two_s0 = dd_mul_d(s0, 2.0);
  dd_t T      = dd_add(s0, dd_div(resid, two_s0));

  /*  U = sum_{k=0..126} T_k.  All values normal-range.  */
  dd_t U = T;
  Fi0(126, 0,
      T = dd_mul_d(T, 2.0 * z);
      T = dd_div_d(T, (f64)(2 * i + 3));
      U = dd_add(U, T))

  /*  Multiply by 2 * e^-z, keeping the 2^k scale separate until
      after the U * mantissa product lands in normal range.  */
  int e_scale;
  dd_t e_mant     = dd_exp_d_scaled(-z, &e_scale);
  dd_t S_unscaled = dd_mul_d(dd_mul(U, e_mant), 2.0);
  dd_t S          = (dd_t){ ldexp(S_unscaled.hi, e_scale),
                            ldexp(S_unscaled.lo, e_scale) };

  /*  Add Q(0.5, z) (the erfc(sqrt(z)) term in the half-integer
      expansion).  */
  dd_t q05 = Q_05_dd(z);
  return dd_add(S, q05);
}

/*  Public entry point.  */

double fastent_chisq_tail(double chisq, int binary) {
  if (chisq <= 0.0)     return 1.0;
  if (!isfinite(chisq)) return 0.0;
  const f64 z = 0.5 * chisq;
  dd_t q = binary ? Q_05_dd(z) : Q_1275_dd(z);
  f64 qd = q.hi + q.lo;
  if (qd < 0.0) qd = 0.0;
  if (qd > 1.0) qd = 1.0;
  return qd;
}
