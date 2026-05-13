/*  fastent  --  chi-square upper tail probability.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_CHISQ_H
#define FASTENT_CHISQ_H

/*  Upper-tail probability for a chi-square statistic ax with df
    degrees of freedom: returns Pr(X^2 > ax) under the assumption
    that the test statistic is chi-square distributed.  */
double pochisq(double ax, int df);

#endif
