#!/usr/bin/env python3
"""Generate the fixed-point logarithm tables."""

import mpmath as mp


mp.mp.prec = 300


def dd_split(x):
    hi = float(x)
    lo = float(x - mp.mpf(hi))
    return hi, lo


def hex_lit(x):
    if x == 0.0:
        return "0x0.0p+0"
    return x.hex().replace("0X", "0x")


out = []
out.append("/*  Copyright (C) 2023-2026 Kamila Szewczyk")
out.append("")
out.append("    This program is free software; you can redistribute it and/or modify")
out.append("    it under the terms of the GNU General Public License as published by")
out.append("    the Free Software Foundation, version 3.")
out.append("")
out.append("    This program is distributed in the hope that it will be useful,")
out.append("    but WITHOUT ANY WARRANTY; without even the implied warranty of")
out.append("    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the")
out.append("    GNU General Public License for more details.")
out.append("")
out.append("    You should have received a copy of the GNU General Public License")
out.append("    along with this program. If not, see <http://www.gnu.org/licenses/>.  */")
out.append("")
out.append("/*  Generated at 300-bit precision; do not edit.  */")
out.append("")
out.append("#ifndef FASTENT_LOG2_TABLES_H")
out.append("#define FASTENT_LOG2_TABLES_H")
out.append("")
out.append("typedef struct { f64 hi, lo; } fastent_dd_t;")
out.append("")
out.append("static const fastent_dd_t fastent_log2_T_dd[128] = {")
for i in range(128):
    c = mp.mpf(1) + mp.mpf(i) / 128
    value = mp.log(c, 2)
    hi, lo = dd_split(value)
    out.append(f"  {{ {hex_lit(hi):>22s}, {hex_lit(lo):>22s} }},  /* log2(1 + {i}/128) */")
out.append("};")
out.append("")
out.append("static const f64 fastent_inv_c[128] = {")
for i in range(128):
    v = mp.mpf(1) / (mp.mpf(1) + mp.mpf(i) / 128)
    out.append(f"  {hex_lit(float(v)):>22s},  /* 1 / (1 + {i}/128) */")
out.append("};")
out.append("")
N = 12
out.append(f"static const fastent_dd_t fastent_log2_1ps_coeff_dd[{N}] = {{")
ln2 = mp.log(2)
for k in range(1, N + 1):
    sign = 1 if k % 2 == 1 else -1
    c = mp.mpf(sign) / (mp.mpf(k) * ln2)
    hi, lo = dd_split(c)
    out.append(f"  {{ {hex_lit(hi):>22s}, {hex_lit(lo):>22s} }},  /* (-1)^({k}+1) / ({k} * ln 2) */")
out.append("};")
out.append("")

# Tables for the cancellation-free p >= 0.5 branch.
out.append("static const fastent_dd_t fastent_log2_T_minus_dd[128] = {")
for i in range(128):
    c = mp.mpf(1) - mp.mpf(i) / 256
    value = mp.log(c, 2)
    hi, lo = dd_split(value)
    out.append(f"  {{ {hex_lit(hi):>22s}, {hex_lit(lo):>22s} }},  /* log2(1 - {i}/256) */")
out.append("};")
out.append("")

out.append("static const f64 fastent_inv_one_minus_c[128] = {")
for i in range(128):
    v = mp.mpf(1) / (mp.mpf(1) - mp.mpf(i) / 256)
    out.append(f"  {hex_lit(float(v)):>22s},  /* 1 / (1 - {i}/256) */")
out.append("};")
out.append("")

out.append("#endif")
print("\n".join(out))
