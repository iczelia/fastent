#!/usr/bin/env bash
#  fastent: self-contained sanity test suite.
#
#  Generates a small set of deterministic fixtures and asserts that
#  fastent reports the expected statistics on each.  Locale is pinned
#  to C so float formatting is stable.
#
#  Usage: run-tests.sh path/to/fastent.bin path/to/fixturedir
#
#  The fixture dir lives in builddir, NOT srcdir, so the source tree
#  stays read-only-clean across out-of-tree builds and dist tarballs.
#
#  Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only.

set -eu
export LC_ALL=C

FASTENT="${1:-./fastent}"
FIX="${2:-./tests/fixtures}"
mkdir -p "${FIX}"

if [ ! -x "${FASTENT}" ]; then
  echo "fastent binary not executable: ${FASTENT}" >&2
  exit 2
fi

gen_zero() {
  [ -f "${FIX}/$1" ] || dd if=/dev/zero bs=1 count="$2" of="${FIX}/$1" status=none
}
#  Compile $2 (a C program writing fixture bytes to stdout) to a temp
#  binary and stream it into fixture $1; remaining args go to the binary.
cc_gen() {
  local fixname="$1" body="$2"; shift 2
  local src bin
  src=$(mktemp "${TMPDIR:-/tmp}/ccgenXXXXXX.c")
  bin=$(mktemp "${TMPDIR:-/tmp}/ccgenXXXXXX")
  printf '%s\n' "${body}" > "${src}"
  "${CC:-cc}" -O2 -o "${bin}" "${src}" && "${bin}" "$@" > "${FIX}/${fixname}"
  rm -f "${src}" "${bin}"
}
gen_byte() {
  #  count copies of a single byte value.
  if [ ! -f "${FIX}/$1" ]; then
    cc_gen "$1" '#include <stdio.h>
#include <stdlib.h>
int main(int argc, char ** argv) {
  int val = atoi(argv[1]);
  long count = atol(argv[2]);
  for (long i = 0; i < count; i++) putchar(val);
  return 0;
}' "$2" "$3"
  fi
}
gen_uniform() {
  #  256*N bytes, each value exactly Nx, Fisher-Yates permuted by a
  #  fixed-seed 64-bit LCG so it does not trivially correlate.
  if [ ! -f "${FIX}/$1" ]; then
    cc_gen "$1" '#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int main(int argc, char ** argv) {
  long copies = atol(argv[1]);
  long n = 256L * copies;
  unsigned char * buf = malloc((size_t)n);
  for (long i = 0; i < n; i++) buf[i] = (unsigned char)(i & 255);
  uint64_t s = 0xDEADBEEFCAFEBABEull;
  for (long i = n - 1; i > 0; i--) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    long j = (long)((s >> 17) % (uint64_t)(i + 1));
    unsigned char t = buf[i]; buf[i] = buf[j]; buf[j] = t;
  }
  fwrite(buf, 1, (size_t)n, stdout);
  return 0;
}' "$2"
  fi
}
gen_lcg() {
  #  64-bit LCG, high byte (>>24) per output byte, fixed seed.
  if [ ! -f "${FIX}/$1" ]; then
    cc_gen "$1" '#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int main(int argc, char ** argv) {
  long count = atol(argv[1]);
  uint64_t s = 0xC0FFEEull;
  for (long i = 0; i < count; i++) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    putchar((int)((s >> 24) & 0xFFu));
  }
  return 0;
}' "$2"
  fi
}

gen_lfsr16() {
  #  Maximal 16-bit LFSR (x^16+x^14+x^13+x^11+1), MSB-first per byte.
  #  Linear complexity per window collapses to ~16, far below mu=256.
  if [ ! -f "${FIX}/$1" ]; then
    src=$(mktemp "${TMPDIR:-/tmp}/lfsrXXXXXX.c")
    bin=$(mktemp "${TMPDIR:-/tmp}/lfsrXXXXXX")
    cat > "${src}" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char ** argv) {
  long n = atol(argv[1]);
  unsigned s = 0xACE1u;
  for (long i = 0; i < n; i++) {
    int v = 0;
    for (int k = 0; k < 8; k++) {
      v = (v << 1) | (int)(s & 1u);
      unsigned nb = (s ^ (s >> 2) ^ (s >> 3) ^ (s >> 5)) & 1u;
      s = (s >> 1) | (nb << 15);
    }
    putchar(v);
  }
  return 0;
}
EOF
    "${CC:-cc}" -O2 -o "${bin}" "${src}" && "${bin}" "$2" > "${FIX}/$1"
    rm -f "${src}" "${bin}"
  fi
}

gen_zero    all-zeros.bin    1048576
gen_byte    all-ones.bin     255 1048576
gen_uniform uniform.bin      4096          # 1 MiB, each value 4096x
gen_lcg     lcg.bin          1048576
gen_lfsr16  lfsr16.bin       1048576

passes=0
fails=0
failed_cases=()

check() {
  local name="$1"; shift
  local rc=0
  "$@" || rc=$?
  if [ $rc -eq 0 ]; then
    passes=$((passes + 1))
  else
    fails=$((fails + 1))
    failed_cases+=( "${name}" )
  fi
}

assert_grep() {  #  assert_grep <pattern> <text>
  local pat="$1"; local txt="$2"
  printf '%s\n' "${txt}" | grep -qE -- "${pat}"
}

check "all-zeros: entropy = 0" bash -c '
  out=$('"${FASTENT}"' "'"${FIX}"'/all-zeros.bin")
  grep -qE "Entropy = 0\.000000 bits per byte\." <<< "$out"
'

check "all-ones: entropy = 0" bash -c '
  out=$('"${FASTENT}"' "'"${FIX}"'/all-ones.bin")
  grep -qE "Entropy = 0\.000000 bits per byte\." <<< "$out"
'

check "uniform: entropy = 8.000000" bash -c '
  out=$('"${FASTENT}"' "'"${FIX}"'/uniform.bin")
  grep -qE "Entropy = 8\.000000 bits per byte\." <<< "$out"
'

check "terse mode: well-formed" bash -c '
  out=$('"${FASTENT}"' -t "'"${FIX}"'/lcg.bin")
  [ "$(printf "%s\n" "$out" | wc -l)" -eq 2 ] &&
    grep -q "^0,File-bytes," <<< "$out" &&
    grep -q "^1,1048576," <<< "$out"
'

#  JSON-validity scanner: nonzero exit on unbalanced braces/brackets,
#  unterminated string, control byte outside strings, trailing garbage
#  or empty input.
JSONCHK=$(mktemp "${TMPDIR:-/tmp}/jsonchkXXXXXX")
JSONCHK_SRC=$(mktemp "${TMPDIR:-/tmp}/jsonchkXXXXXX.c")
cat > "${JSONCHK_SRC}" <<'EOF'
#include <stdio.h>
#include <ctype.h>
int main(void) {
  int c, depth = 0, instr = 0, esc = 0, seen = 0, done = 0;
  while ((c = getchar()) != EOF) {
    if (instr) {
      if (esc) { esc = 0; continue; }
      if (c == '\\') { esc = 1; continue; }
      if (c == '"') instr = 0;
      continue;
    }
    if (c == '"') { instr = 1; seen = 1; continue; }
    if (c == '{' || c == '[') { depth++; seen = 1; continue; }
    if (c == '}' || c == ']') {
      if (--depth < 0) return 1;
      if (depth == 0) done = 1;
      continue;
    }
    if (isspace(c)) continue;
    if (done) return 1;            /* trailing garbage after top-level close */
    if (!isprint(c)) return 1;     /* control byte outside a string */
  }
  if (instr || depth != 0 || !seen) return 1;
  return 0;
}
EOF
"${CC:-cc}" -O2 -o "${JSONCHK}" "${JSONCHK_SRC}"
rm -f "${JSONCHK_SRC}"
trap 'rm -f "${JSONCHK}"' EXIT

check "JSON mode: parseable" bash -c '
  out=$('"${FASTENT}"' --json "'"${FIX}"'/lcg.bin")
  printf "%s" "$out" | "'"${JSONCHK}"'"
'

check "counts mode: histogram present" bash -c '
  out=$('"${FASTENT}"' -c "'"${FIX}"'/uniform.bin")
  grep -q "^Value Char Occurrences Fraction$" <<< "$out" &&
    grep -qE "^Total:.*1048576" <<< "$out"
'

check "bit mode: entropy ~1.0 on uniform" bash -c '
  out=$('"${FASTENT}"' -b "'"${FIX}"'/uniform.bin")
  grep -qE "Entropy = 1\.000000 bits per bit\." <<< "$out"
'

check "stdin matches file path" bash -c '
  a=$('"${FASTENT}"' -t "'"${FIX}"'/lcg.bin")
  b=$('"${FASTENT}"' -t < "'"${FIX}"'/lcg.bin")
  [ "$a" = "$b" ]
'

check "fold flag: total preserved" bash -c '
  out=$('"${FASTENT}"' -tf "'"${FIX}"'/lcg.bin")
  grep -q "^1,1048576," <<< "$out"
'

check "io=stream path agrees" bash -c '
  a=$('"${FASTENT}"' -t "'"${FIX}"'/lcg.bin")
  b=$('"${FASTENT}"' -t --io=stream "'"${FIX}"'/lcg.bin")
  c=$('"${FASTENT}"' -t -i stream "'"${FIX}"'/lcg.bin")
  [ "$a" = "$b" ] && [ "$b" = "$c" ]
'

check "--no-mmap is rejected" bash -c '
  '"${FASTENT}"' -t --no-mmap "'"${FIX}"'/lcg.bin" 2>/dev/null && exit 1
  exit 0
'

check "short aliases match long forms" bash -c '
  a=$('"${FASTENT}"' --json --extended "'"${FIX}"'/lcg.bin")
  b=$('"${FASTENT}"' -J -e "'"${FIX}"'/lcg.bin")
  [ "$a" = "$b" ]
'

if "${FASTENT}" --help 2>&1 | grep -q -- "-j"; then
  check "-j 4 agrees with -j 1" bash -c '
    a=$('"${FASTENT}"' -t -j 1 "'"${FIX}"'/lcg.bin")
    b=$('"${FASTENT}"' -t -j 4 "'"${FIX}"'/lcg.bin")
    [ "$a" = "$b" ]
  '
fi

check "no -e: extended stats hidden" bash -c '
  out=$('"${FASTENT}"' "'"${FIX}"'/lcg.bin")
  ! grep -q "Min-entropy" <<< "$out"
'

check "uniform -e: min-entropy = 8.000000" bash -c '
  out=$('"${FASTENT}"' -e "'"${FIX}"'/uniform.bin")
  grep -qE "Min-entropy is 8\.000000 bits per byte\." <<< "$out" &&
    grep -qE "Collision entropy is 8\.000000 bits per byte\." <<< "$out"
'

check "terse -e: 36 columns" bash -c '
  out=$('"${FASTENT}"' -e -t "'"${FIX}"'/lcg.bin")
  grep -q "Chi-square,P-Exceed,Mean," <<< "$out" &&
    grep -q "Min-Entropy,Collision-Entropy,IC,Poker,Poker-p," <<< "$out" &&
    grep -q "Conditional-Entropy,Mutual-Information,Runs,Longest-Run,Cusum-Max" <<< "$out" &&
    [ "$(printf "%s\n" "$out" | sed -n 2p | tr , "\n" | wc -l)" -eq 36 ]
'

check "no -ee: runs/longrun/cusum nan" bash -c '
  out=$('"${FASTENT}"' -e -t "'"${FIX}"'/lcg.bin")
  printf "%s\n" "$out" | sed -n 2p | awk -F, "{ exit !(\$34==\"nan\" && \$35==\"nan\" && \$36==\"nan\") }"
'

#  Runs=col34, Longest-Run=col35, Cusum-Max=col36; absent fields read "nan".
check "-ee byte: longest-run set, cusum nan, runs set (mmap median)" bash -c '
  r=$('"${FASTENT}"' -ee -t "'"${FIX}"'/lcg.bin" | sed -n 2p)
  runs=$(printf %s "$r" | cut -d, -f34)
  lr=$(printf %s "$r" | cut -d, -f35)
  cm=$(printf %s "$r" | cut -d, -f36)
  [ "$cm" = "nan" ] && [ "$runs" != "nan" ] && [ "$lr" != "nan" ] &&
    awk -v lr="$lr" "BEGIN{exit !(lr>=1)}"
'

check "-ee bit: runs/longest/cusum all set" bash -c '
  r=$('"${FASTENT}"' -ee -b -t "'"${FIX}"'/lcg.bin" | sed -n 2p)
  runs=$(printf %s "$r" | cut -d, -f34)
  lr=$(printf %s "$r" | cut -d, -f35)
  cm=$(printf %s "$r" | cut -d, -f36)
  [ "$runs" != "nan" ] && [ "$lr" != "nan" ] && [ "$cm" != "nan" ]
'

#  samples=File-bytes col2 (byte mode), Longest-Run=col35.
check "-ee all-zeros: 1 run, longest = N" bash -c '
  r=$('"${FASTENT}"' -ee -t "'"${FIX}"'/all-zeros.bin" | sed -n 2p)
  n=$(printf %s "$r" | cut -d, -f2)
  lr=$(printf %s "$r" | cut -d, -f35)
  awk -v n="$n" -v lr="$lr" "BEGIN{exit !(lr==n)}"
'

check "-ee runs determinism j1 vs j4" bash -c '
  a=$('"${FASTENT}"' -ee -t -j 1 "'"${FIX}"'/lcg.bin" | sed -n 2p | cut -d, -f34,35,36)
  b=$('"${FASTENT}"' -ee -t -j 4 "'"${FIX}"'/lcg.bin" | sed -n 2p | cut -d, -f34,35,36)
  [ "$a" = "$b" ]
'

check "no -ee: bigram fields are nan" bash -c '
  out=$('"${FASTENT}"' -e -t "'"${FIX}"'/lcg.bin")
  printf "%s\n" "$out" | sed -n 2p | awk -F, "{ exit !(\$32==\"nan\" && \$33==\"nan\") }"
'

#  Entropy=col3, Conditional-Entropy=col32, Mutual-Information=col33.
check "-ee text: low cond-entropy, high MI" bash -c '
  r=$('"${FASTENT}"' -ee -t "'"${FIX}"'/lcg.bin" | sed -n 2p)
  h0=$(printf %s "$r" | cut -d, -f3)
  ce=$(printf %s "$r" | cut -d, -f32)
  mi=$(printf %s "$r" | cut -d, -f33)
  [ "$ce" != "nan" ] && [ "$mi" != "nan" ] &&
    awk -v ce="$ce" -v mi="$mi" -v h0="$h0" "BEGIN{exit !(ce<=h0+1e-9 && mi>=-1e-9)}"
'

check "-ee uniform: cond ~ H0, MI ~ 0" bash -c '
  r=$('"${FASTENT}"' -ee -t "'"${FIX}"'/uniform.bin" | sed -n 2p)
  h0=$(printf %s "$r" | cut -d, -f3)
  ce=$(printf %s "$r" | cut -d, -f32)
  mi=$(printf %s "$r" | cut -d, -f33)
  awk -v ce="$ce" -v mi="$mi" -v h0="$h0" "BEGIN{d=ce-h0; if(d<0)d=-d; m=mi; if(m<0)m=-m; exit !(d<0.05 && m<0.05)}"
'

if "${FASTENT}" --help 2>&1 | grep -q -- "-j"; then
  check "-ee determinism j1 == j4" bash -c '
    a=$('"${FASTENT}"' -ee -t -j 1 "'"${FIX}"'/lcg.bin")
    b=$('"${FASTENT}"' -ee -t -j 4 "'"${FIX}"'/lcg.bin")
    [ "$a" = "$b" ]
  '
fi

check "no -eee: LZ77F fields are nan" bash -c '
  out=$('"${FASTENT}"' -ee -t "'"${FIX}"'/lcg.bin")
  printf "%s\n" "$out" | sed -n 1p | grep -qv "LZ-Deviation"
'

#  lz77f: cr_excess=col37, match_coverage=col40, deviation=col45, single_dominant_match=col47.
check "-eee zeros: mega-match FAIL" bash -c '
  r=$('"${FASTENT}"' -eee -t "'"${FIX}"'/all-zeros.bin" | sed -n 2p)
  cr=$(printf %s "$r" | cut -d, -f37)
  mc=$(printf %s "$r" | cut -d, -f40)
  dv=$(printf %s "$r" | cut -d, -f45)
  sd=$(printf %s "$r" | cut -d, -f47)
  awk -v cr="$cr" -v mc="$mc" -v dv="$dv" -v sd="$sd" "BEGIN{exit !(sd==1 && cr>0.9 && mc>0.9 && dv>=3.0)}"
'

check "-eee uniform: LZ77F ~ random PASS" bash -c '
  r=$('"${FASTENT}"' -eee -t "'"${FIX}"'/uniform.bin" | sed -n 2p)
  cr=$(printf %s "$r" | cut -d, -f37)
  mc=$(printf %s "$r" | cut -d, -f40)
  dv=$(printf %s "$r" | cut -d, -f45)
  awk -v cr="$cr" -v mc="$mc" -v dv="$dv" "BEGIN{exit !(mc<0.05 && cr<0.05 && dv<2.0)}"
'

if "${FASTENT}" --help 2>&1 | grep -q -- "-j"; then
  check "-eee LZ77F determinism j1 == j4" bash -c '
    a=$('"${FASTENT}"' -eee -t -j 1 "'"${FIX}"'/lcg.bin" | sed -n 2p)
    b=$('"${FASTENT}"' -eee -t -j 4 "'"${FIX}"'/lcg.bin" | sed -n 2p)
    [ "$a" = "$b" ]
  '
  check "-eee LZ77F determinism mmap == stream" bash -c '
    a=$('"${FASTENT}"' -eee -t --io=mmap   "'"${FIX}"'/lcg.bin" | sed -n 2p)
    b=$('"${FASTENT}"' -eee -t --io=stream "'"${FIX}"'/lcg.bin" | sed -n 2p)
    [ "$a" = "$b" ]
  '
fi

check "no -eee: linear-complexity fields absent" bash -c '
  out=$('"${FASTENT}"' -ee -t "'"${FIX}"'/lcg.bin")
  printf "%s\n" "$out" | sed -n 1p | grep -qv "BM-Deviation"
'

#  Terse row 2 fields: BM-Mean-LC=48, BM-Deviation=52, BM-Degenerate=54.
check "no -eee: BM columns absent" bash -c '
  '"${FASTENT}"' -ee -t "'"${FIX}"'/lcg.bin" | sed -n 1p | grep -qv "BM-Deviation"
'

check "-eee LFSR16: bm_deviation >= 3 FAIL, mean L ~ 16" bash -c '
  r=$('"${FASTENT}"' -eee -t "'"${FIX}"'/lfsr16.bin" | sed -n 2p)
  d=$(printf %s "$r" | cut -d, -f52)
  m=$(printf %s "$r" | cut -d, -f48)
  awk -v d="$d" -v m="$m" "BEGIN{exit !(d>=3.0 && m>=14 && m<=18)}"
'

check "-eee uniform: bm_deviation < 2 PASS, mean L ~ 256" bash -c '
  r=$('"${FASTENT}"' -eee -t "'"${FIX}"'/uniform.bin" | sed -n 2p)
  d=$(printf %s "$r" | cut -d, -f52)
  m=$(printf %s "$r" | cut -d, -f48)
  awk -v d="$d" -v m="$m" "BEGIN{exit !(d<2.0 && m>=254 && m<=258)}"
'

check "-eee lcg.bin (MSB-byte): bm PASS (documented boundary)" bash -c '
  r=$('"${FASTENT}"' -eee -t "'"${FIX}"'/lcg.bin" | sed -n 2p)
  d=$(printf %s "$r" | cut -d, -f52)
  awk -v d="$d" "BEGIN{exit !(d<2.0)}"
'

check "-eee zeros: near-constant flagged, FAIL" bash -c '
  r=$('"${FASTENT}"' -eee -t "'"${FIX}"'/all-zeros.bin" | sed -n 2p)
  g=$(printf %s "$r" | cut -d, -f54)
  m=$(printf %s "$r" | cut -d, -f48)
  d=$(printf %s "$r" | cut -d, -f52)
  awk -v g="$g" -v m="$m" -v d="$d" "BEGIN{exit !(g==1 && m==0 && d>=3.0)}"
'

if "${FASTENT}" --help 2>&1 | grep -q -- "-j"; then
  check "-eee linear-complexity determinism j1 == j4" bash -c '
    a=$('"${FASTENT}"' -eee -t -j 1 "'"${FIX}"'/lfsr16.bin" | sed -n 2p)
    b=$('"${FASTENT}"' -eee -t -j 4 "'"${FIX}"'/lfsr16.bin" | sed -n 2p)
    [ "$a" = "$b" ]
  '
  check "-eee linear-complexity determinism mmap == stream" bash -c '
    a=$('"${FASTENT}"' -eee -t --io=mmap   "'"${FIX}"'/lfsr16.bin" | sed -n 2p)
    b=$('"${FASTENT}"' -eee -t --io=stream "'"${FIX}"'/lfsr16.bin" | sed -n 2p)
    [ "$a" = "$b" ]
  '
fi

#  Terse row 2 Maurer fields: Maurer-Deviation=57, Maurer-K=58,
#  Maurer-Degenerate=59 (header field names: Maurer-*).
check "no -eee: Maurer columns absent" bash -c '
  '"${FASTENT}"' -ee -t "'"${FIX}"'/lcg.bin" | sed -n 1p | grep -qv "Maurer-Deviation"
'

check "-eee zeros: Maurer FAIL, repetitive flagged" bash -c '
  r=$('"${FASTENT}"' -eee -t "'"${FIX}"'/all-zeros.bin" | sed -n 2p)
  d=$(printf %s "$r" | cut -d, -f57)
  g=$(printf %s "$r" | cut -d, -f59)
  awk -v d="$d" -v g="$g" "BEGIN{exit !(d>=3.0 && g==1)}"
'

check "-eee lcg.bin: Maurer < 2 PASS" bash -c '
  r=$('"${FASTENT}"' -eee -t "'"${FIX}"'/lcg.bin" | sed -n 2p)
  d=$(printf %s "$r" | cut -d, -f57)
  k=$(printf %s "$r" | cut -d, -f58)
  awk -v d="$d" -v k="$k" "BEGIN{exit !(d<2.0 && k>0)}"
'

if "${FASTENT}" --help 2>&1 | grep -q -- "-j"; then
  check "-eee Maurer determinism j1 == j4" bash -c '
    a=$('"${FASTENT}"' -eee -t -j 1 "'"${FIX}"'/lcg.bin" | sed -n 2p)
    b=$('"${FASTENT}"' -eee -t -j 4 "'"${FIX}"'/lcg.bin" | sed -n 2p)
    [ "$a" = "$b" ]
  '
  check "-eee Maurer determinism mmap == stream" bash -c '
    a=$('"${FASTENT}"' -eee -t --io=mmap   "'"${FIX}"'/lcg.bin" | sed -n 2p)
    b=$('"${FASTENT}"' -eee -t --io=stream "'"${FIX}"'/lcg.bin" | sed -n 2p)
    [ "$a" = "$b" ]
  '
fi

check "-ee bit mode: 2x2 computed" bash -c '
  r=$('"${FASTENT}"' -ee -b -t "'"${FIX}"'/lcg.bin" | sed -n 2p)
  ce=$(printf %s "$r" | cut -d, -f32)
  mi=$(printf %s "$r" | cut -d, -f33)
  [ "$ce" != "nan" ] && [ "$mi" != "nan" ]
'

check "-ee all-zeros: cond 0, MI 0" bash -c '
  r=$('"${FASTENT}"' -ee -t "'"${FIX}"'/all-zeros.bin" | sed -n 2p)
  ce=$(printf %s "$r" | cut -d, -f32)
  mi=$(printf %s "$r" | cut -d, -f33)
  awk -v ce="$ce" -v mi="$mi" "BEGIN{exit !(ce==0.0 && mi==0.0)}"
'

#  Bit0..Bit7=cols22..29, Bit-Bias-Max=col30.
check "uniform -e: bit balance perfect" bash -c '
  hdr=$('"${FASTENT}"' -e -t "'"${FIX}"'/uniform.bin" | sed -n 1p)
  r=$('"${FASTENT}"' -e -t "'"${FIX}"'/uniform.bin" | sed -n 2p)
  printf %s "$hdr" | grep -q "Bit0,Bit1,Bit2,Bit3,Bit4,Bit5,Bit6,Bit7," || exit 1
  bm=$(printf %s "$r" | cut -d, -f30)
  [ "$bm" = "0.000000" ] || exit 1
  i=22
  while [ "$i" -le 29 ]; do
    v=$(printf %s "$r" | cut -d, -f$i)
    [ "$v" = "0.500000" ] || exit 1
    i=$((i + 1))
  done
'

check "all-zeros -e: bit7=0, max bias 0.5" bash -c '
  r=$('"${FASTENT}"' -e -t "'"${FIX}"'/all-zeros.bin" | sed -n 2p)
  bm=$(printf %s "$r" | cut -d, -f30)
  [ "$bm" = "0.500000" ] || exit 1
  i=22
  while [ "$i" -le 29 ]; do
    v=$(printf %s "$r" | cut -d, -f$i)
    [ "$v" = "0.000000" ] || exit 1
    i=$((i + 1))
  done
'

#  Bit mode: Bit0..Bit7 (cols22..29) and Bit-Bias-Max (col30) read "nan", Bit-Bias-Worst (col31) -1.
check "bit mode: bit_frequencies null" bash -c '
  r=$('"${FASTENT}"' -e -b -t "'"${FIX}"'/lcg.bin" | sed -n 2p)
  bm=$(printf %s "$r" | cut -d, -f30)
  bw=$(printf %s "$r" | cut -d, -f31)
  [ "$bm" = "nan" ] && [ "$bw" = "-1" ] || exit 1
  i=22
  while [ "$i" -le 29 ]; do
    v=$(printf %s "$r" | cut -d, -f$i)
    [ "$v" = "nan" ] || exit 1
    i=$((i + 1))
  done
'

#  Min-Entropy=col9, Poker=col12, Poker-p=col13; byte-mode poker is always the 16-class test (df 15).
check "JSON -e: poker + min_entropy keys" bash -c '
  hdr=$('"${FASTENT}"' -e -t "'"${FIX}"'/lcg.bin" | sed -n 1p)
  r=$('"${FASTENT}"' -e -t "'"${FIX}"'/lcg.bin" | sed -n 2p)
  printf %s "$hdr" | grep -q "Min-Entropy" || exit 1
  printf %s "$hdr" | grep -q "Poker,Poker-p," || exit 1
  me=$(printf %s "$r" | cut -d, -f9)
  pk=$(printf %s "$r" | cut -d, -f12)
  pp=$(printf %s "$r" | cut -d, -f13)
  [ "$me" != "nan" ] && [ "$pk" != "nan" ] && [ "$pp" != "nan" ]
'

#  Bit mode: Poker (col12) and Poker-p (col13) read "nan".
check "JSON -e bit mode: poker null" bash -c '
  r=$('"${FASTENT}"' -e -b -t "'"${FIX}"'/lcg.bin" | sed -n 2p)
  pk=$(printf %s "$r" | cut -d, -f12)
  pp=$(printf %s "$r" | cut -d, -f13)
  [ "$pk" = "nan" ] && [ "$pp" = "nan" ]
'

check "annotate: verdict line present" bash -c '
  out=$('"${FASTENT}"' --annotate --color=never "'"${FIX}"'/lcg.bin")
  grep -qE "^  VERDICT: " <<< "$out"
'

check "annotate all-zeros: not random" bash -c '
  out=$('"${FASTENT}"' --annotate --color=never "'"${FIX}"'/all-zeros.bin")
  grep -qE "VERDICT: DOES NOT PASS AS RANDOM" <<< "$out"
'

check "annotate rejected with -r" bash -c '
  '"${FASTENT}"' --annotate -r "'"${FIX}"'" 2>/dev/null && exit 1
  exit 0
'

if "${FASTENT}" --help 2>&1 | grep -q -- "-j"; then
  check "extended -j4 agrees with -j1" bash -c '
    a=$('"${FASTENT}"' -e -t -j 1 "'"${FIX}"'/lcg.bin")
    b=$('"${FASTENT}"' -e -t -j 4 "'"${FIX}"'/lcg.bin")
    [ "$a" = "$b" ]
  '
fi

check "empty input: clean exit" bash -c '
  : > "'"${FIX}"'/empty.bin"
  '"${FASTENT}"' -t "'"${FIX}"'/empty.bin" > /dev/null
'

check "fips-140-2: all-zeros fails, exit 1" bash -c '
  out=$('"${FASTENT}"' --fips-140-2 "'"${FIX}"'/all-zeros.bin") && exit 1
  grep -qE "overall +: FAIL" <<< "$out"
'

check "fips-140-2: uniform passes, exit 0" bash -c '
  out=$('"${FASTENT}"' --fips-140-2 "'"${FIX}"'/uniform.bin")
  grep -qE "overall +: PASS" <<< "$out"
'

check "fips-140-2: insufficient data, exit 1" bash -c '
  : > "'"${FIX}"'/empty.bin"
  out=$('"${FASTENT}"' --fips-140-2 < "'"${FIX}"'/empty.bin") && exit 1
  grep -qE "insufficient data" <<< "$out"
'

check "fips-140-2: rejected with -r" bash -c '
  '"${FASTENT}"' --fips-140-2 -r "'"${FIX}"'" 2>/dev/null && exit 1
  exit 0
'

if "${FASTENT}" --help 2>&1 | grep -q -- "-j"; then
  check "fips-140-2 determinism j1 == j4" bash -c '
    a=$('"${FASTENT}"' --fips-140-2 -j 1 "'"${FIX}"'/uniform.bin")
    b=$('"${FASTENT}"' --fips-140-2 -j 4 "'"${FIX}"'/uniform.bin")
    [ "$a" = "$b" ]
  '
fi

check "version banner" bash -c '
  out=$('"${FASTENT}"' --version)
  grep -q "^fastent " <<< "$out"
'

echo "passes: ${passes}"
echo "fails:  ${fails}"
if [ ${fails} -gt 0 ]; then
  echo "failed cases:"
  for c in "${failed_cases[@]}"; do
    echo "  ${c}"
  done
  exit 1
fi
exit 0
