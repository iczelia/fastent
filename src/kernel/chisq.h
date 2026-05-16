/*  fastent: chi-square upper tail probability.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_CHISQ_H
#define FASTENT_CHISQ_H

/*  Upper-tail probability Pr(X^2 > chisq) for the chi-square statistic
    against either the byte (df=255) or bit (df=1) null distribution.
    The result is clamped to [0, 1].  */
double fastent_chisq_tail(double chisq, int binary);

/*  Same upper tail for an arbitrary ODD degrees of freedom df >= 1
    (shape parameter a = df/2 is half-integer, which the closed form
    requires).  Used by the poker test (16 nibble bins, df = 15).
    Result clamped to [0, 1].  */
double fastent_chisq_tail_df(double chisq, int df);

#endif
