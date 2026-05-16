#!/usr/bin/env python3
"""Regenerate src/kernel/log2_tables.h.  Requires mpmath."""
import mpmath as mp
mp.mp.prec = 300

def dd_split(x):
    hi = float(x); lo = float(x - mp.mpf(hi))
    return hi, lo

def hex_lit(x):
    if x == 0.0: return "0x0.0p+0"
    return x.hex().replace('0X', '0x')

out = []
out.append("/*  AUTO-GENERATED.  do not edit.  See scripts/gen_log2_tables.py.")
out.append("    Source: mpmath at 300-bit precision, rounded to doubles for hi+lo. */")
out.append("")
out.append("#ifndef FASTENT_LOG2_TABLES_H")
out.append("#define FASTENT_LOG2_TABLES_H")
out.append("")
out.append("typedef struct { double hi, lo; } fastent_dd_t;")
out.append("")
out.append("static const fastent_dd_t fastent_log2_T_dd[128] = {")
for i in range(128):
    c = mp.mpf(1) + mp.mpf(i)/128
    L = mp.log(c, 2)
    hi, lo = dd_split(L)
    out.append(f"  {{ {hex_lit(hi):>22s}, {hex_lit(lo):>22s} }},  /* log2(1 + {i}/128) */")
out.append("};")
out.append("")
out.append("static const double fastent_inv_c[128] = {")
for i in range(128):
    v = mp.mpf(1) / (mp.mpf(1) + mp.mpf(i)/128)
    out.append(f"  {hex_lit(float(v)):>22s},  /* 1 / (1 + {i}/128) */")
out.append("};")
out.append("")
N = 12
out.append(f"static const fastent_dd_t fastent_log2_1ps_coeff_dd[{N}] = {{")
ln2 = mp.log(2)
for k in range(1, N+1):
    sign = 1 if (k%2==1) else -1
    c = mp.mpf(sign) / (mp.mpf(k) * ln2)
    hi, lo = dd_split(c)
    out.append(f"  {{ {hex_lit(hi):>22s}, {hex_lit(lo):>22s} }},  /* (-1)^({k}+1) / ({k} * ln 2) */")
out.append("};")
out.append("")

# Split-domain tables for p >= 0.5.  q = 1 - p (exact via Sterbenz),
# i = floor(q * 256), c_i = i / 256.  log2(1 - q) = log2(1 - i/256)
# + log2(1 - u) where u = (q - i/256) * inv_one_minus_c[i].  Same
# polynomial as the p < 0.5 path: pass r = -u to log2(1+r).
out.append("static const fastent_dd_t fastent_log2_T_minus_dd[128] = {")
for i in range(128):
    c = mp.mpf(1) - mp.mpf(i)/256
    L = mp.log(c, 2)
    hi, lo = dd_split(L)
    out.append(f"  {{ {hex_lit(hi):>22s}, {hex_lit(lo):>22s} }},  /* log2(1 - {i}/256) */")
out.append("};")
out.append("")

out.append("static const double fastent_inv_one_minus_c[128] = {")
for i in range(128):
    v = mp.mpf(1) / (mp.mpf(1) - mp.mpf(i)/256)
    out.append(f"  {hex_lit(float(v)):>22s},  /* 1 / (1 - {i}/256) */")
out.append("};")
out.append("")

out.append("#endif")
print("\n".join(out))
