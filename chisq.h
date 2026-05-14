/*  fastent: chi-square upper tail probability.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_CHISQ_H
#define FASTENT_CHISQ_H

/*  Upper-tail probability Pr(X^2 > chisq) for the chi-square statistic
    against either the byte (df=255) or bit (df=1) null distribution.
    The result is clamped to [0, 1].  */
double fastent_chisq_tail(double chisq, int binary);

#endif
