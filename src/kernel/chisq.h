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

#ifndef FASTENT_CHISQ_H
#define FASTENT_CHISQ_H

/*  Upper-tail probability Pr(X^2 > chisq) for the chi-square statistic
    against either the byte (df=255) or bit (df=1) null distribution.
    The result is clamped to [0, 1].  */
double fastent_chisq_tail(double chisq, int binary);

/*  Same upper tail for an arbitrary ODD degrees of freedom df >= 1 (shape
    parameter a = df/2 is half-integer, which the closed form requires).  */
double fastent_chisq_tail_df(double chisq, int df);

#endif
