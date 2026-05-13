/*  fastent  --  chi-square upper tail probability.

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

    ----------------------------------------------------------------------

    Implementation notes.  The chi-square cumulative distribution with
    k degrees of freedom at x is the regularised lower incomplete gamma
    function P(k/2, x/2); its tail Q(a, z) = 1 - P(a, z) is what we
    report.  We compute Q via the textbook split: a series expansion
    for the lower incomplete gamma when z < a + 1, and a continued
    fraction for the upper incomplete gamma when z >= a + 1, evaluated
    via Lentz's modified algorithm.  log gamma is taken from the C99
    library (lgamma).  */

#include <math.h>
#include <float.h>

#include "common.h"
#include "chisq.h"

#define FASTENT_GAMMA_EPS  3e-16
#define FASTENT_GAMMA_ITER 1024
#define FASTENT_GAMMA_TINY 1e-300

static double gamma_series_p(double a, double z) {
  /*  sum_{n>=0} z^n / prod_{k=0..n}(a + k)  times  z^a e^{-z} / Gamma(a).
      Iterates until the relative tail is below FASTENT_GAMMA_EPS.  */
  double term = 1.0 / a;
  double sum  = term;
  double ap   = a;
  Fi(FASTENT_GAMMA_ITER,
     ap   += 1.0;
     term *= z / ap;
     sum  += term;
     if (fabs(term) < fabs(sum) * FASTENT_GAMMA_EPS) break)
  return sum * exp(-z + a * log(z) - lgamma(a));
}

static double gamma_cfrac_q(double a, double z) {
  /*  Q(a, z) for z >= a + 1 via Lentz's modified continued fraction
      for the upper incomplete gamma:
          Gamma(a, z) = z^a e^{-z} [ 1/(z + 1 - a -
                                       1*(1-a)/(z + 3 - a -
                                       2*(2-a)/(z + 5 - a - ...))) ].
      Returned value already incorporates the z^a e^{-z} / Gamma(a)
      normalisation.  */
  double b = z + 1.0 - a;
  double c = 1.0 / FASTENT_GAMMA_TINY;
  double d = 1.0 / b;
  double h = d;
  Fi0(FASTENT_GAMMA_ITER, 1,
      double an = -(double) i * ((double) i - a);
      b += 2.0;
      d  = an * d + b;
      if (fabs(d) < FASTENT_GAMMA_TINY) d = FASTENT_GAMMA_TINY;
      c  = b + an / c;
      if (fabs(c) < FASTENT_GAMMA_TINY) c = FASTENT_GAMMA_TINY;
      d  = 1.0 / d;
      double delta = d * c;
      h *= delta;
      if (fabs(delta - 1.0) < FASTENT_GAMMA_EPS) break)
  return h * exp(-z + a * log(z) - lgamma(a));
}

double pochisq(double ax, int df) {
  if (df < 1 || ax <= 0.0) return 1.0;
  if (!isfinite(ax))       return 0.0;

  const double a = 0.5 * (double) df;
  const double z = 0.5 * ax;

  double q;
  if (z < a + 1.0) {
    /*  P via series, then complement.  */
    double p = gamma_series_p(a, z);
    q = 1.0 - p;
  } else {
    q = gamma_cfrac_q(a, z);
  }

  /*  Numerical guard: tiny negative values from cancellation become 0.  */
  if (q < 0.0) q = 0.0;
  if (q > 1.0) q = 1.0;
  return q;
}
