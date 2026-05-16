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
#  Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only.

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
gen_byte() {
  if [ ! -f "${FIX}/$1" ]; then
    python3 - "${FIX}/$1" "$2" "$3" <<'PY'
import sys
name, val, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(name, "wb") as f:
    f.write(bytes([val]) * count)
PY
  fi
}
gen_uniform() {
  #  256*N bytes: each byte value appears exactly N times.  Stable
  #  permutation built from an LCG so it doesn't trivially correlate.
  if [ ! -f "${FIX}/$1" ]; then
    python3 - "${FIX}/$1" "$2" <<'PY'
import sys
name, copies = sys.argv[1], int(sys.argv[2])
buf = list(range(256)) * copies
#  Fisher-Yates shuffle with a fixed seeded LCG, no stdlib RNG state.
s = 0xDEADBEEFCAFEBABE
n = len(buf)
for i in range(n - 1, 0, -1):
    s = (s * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
    j = (s >> 17) % (i + 1)
    buf[i], buf[j] = buf[j], buf[i]
with open(name, "wb") as f:
    f.write(bytes(buf))
PY
  fi
}
gen_lcg() {
  if [ ! -f "${FIX}/$1" ]; then
    python3 - "${FIX}/$1" "$2" <<'PY'
import sys
name, count = sys.argv[1], int(sys.argv[2])
seed = 0xC0FFEE
out = bytearray(count)
s = seed
for i in range(count):
    s = (s * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
    out[i] = (s >> 24) & 0xFF
with open(name, "wb") as f:
    f.write(out)
PY
  fi
}

gen_zero    all-zeros.bin    1048576
gen_byte    all-ones.bin     255 1048576
gen_uniform uniform.bin      4096          # 1 MiB, each value 4096x
gen_lcg     lcg.bin          1048576

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

check "JSON mode: parseable" bash -c '
  out=$('"${FASTENT}"' --json "'"${FIX}"'/lcg.bin")
  python3 -c "import json,sys; json.loads(sys.stdin.read())" <<< "$out"
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

check "terse -e: 32 columns" bash -c '
  out=$('"${FASTENT}"' -e -t "'"${FIX}"'/lcg.bin")
  grep -q "Min-Entropy,Collision-Entropy,IC,Poker,Poker-p," <<< "$out" &&
    grep -q "Bit-Bias-Max,Bit-Bias-Worst,Conditional-Entropy,Mutual-Information" <<< "$out" &&
    [ "$(printf "%s\n" "$out" | sed -n 2p | tr , "\n" | wc -l)" -eq 32 ]
'

check "no -ee: bigram fields are nan" bash -c '
  out=$('"${FASTENT}"' -e -t "'"${FIX}"'/lcg.bin")
  printf "%s\n" "$out" | sed -n 2p | awk -F, "{ exit !(\$31==\"nan\" && \$32==\"nan\") }"
'

check "-ee text: low cond-entropy, high MI" bash -c '
  out=$('"${FASTENT}"' -ee --json "'"${FIX}"'/lcg.bin")
  python3 -c "import json,sys
d=json.load(sys.stdin)
ce=d[\"conditional_entropy\"]; mi=d[\"mutual_information\"]; h0=d[\"entropy\"]
assert ce is not None and mi is not None
assert ce <= h0 + 1e-9 and mi >= -1e-9" <<< "$out"
'

check "-ee uniform: cond ~ H0, MI ~ 0" bash -c '
  out=$('"${FASTENT}"' -ee --json "'"${FIX}"'/uniform.bin")
  python3 -c "import json,sys
d=json.load(sys.stdin)
assert abs(d[\"conditional_entropy\"]-d[\"entropy\"]) < 0.05
assert abs(d[\"mutual_information\"]) < 0.05" <<< "$out"
'

if "${FASTENT}" --help 2>&1 | grep -q -- "-j"; then
  check "-ee determinism j1 == j4" bash -c '
    a=$('"${FASTENT}"' -ee -t -j 1 "'"${FIX}"'/lcg.bin")
    b=$('"${FASTENT}"' -ee -t -j 4 "'"${FIX}"'/lcg.bin")
    [ "$a" = "$b" ]
  '
fi

check "-ee bit mode: 2x2 computed" bash -c '
  out=$('"${FASTENT}"' -ee -b --json "'"${FIX}"'/lcg.bin")
  python3 -c "import json,sys
d=json.load(sys.stdin)
assert d[\"conditional_entropy\"] is not None and d[\"mutual_information\"] is not None" <<< "$out"
'

check "-ee all-zeros: cond 0, MI 0" bash -c '
  out=$('"${FASTENT}"' -ee --json "'"${FIX}"'/all-zeros.bin")
  python3 -c "import json,sys
d=json.load(sys.stdin)
assert d[\"conditional_entropy\"]==0.0 and d[\"mutual_information\"]==0.0" <<< "$out"
'

check "uniform -e: bit balance perfect" bash -c '
  out=$('"${FASTENT}"' --json -e "'"${FIX}"'/uniform.bin")
  python3 -c "import json,sys; d=json.load(sys.stdin); assert d[\"bit_bias\"][\"max\"]==0.0 and len(d[\"bit_frequencies\"])==8 and all(f==0.5 for f in d[\"bit_frequencies\"])" <<< "$out"
'

check "all-zeros -e: bit7=0, max bias 0.5" bash -c '
  out=$('"${FASTENT}"' --json -e "'"${FIX}"'/all-zeros.bin")
  python3 -c "import json,sys; d=json.load(sys.stdin); assert d[\"bit_bias\"][\"max\"]==0.5 and d[\"bit_frequencies\"]==[0.0]*8" <<< "$out"
'

check "bit mode: bit_frequencies null" bash -c '
  out=$('"${FASTENT}"' --json -e -b "'"${FIX}"'/lcg.bin")
  python3 -c "import json,sys; d=json.load(sys.stdin); assert d[\"bit_frequencies\"] is None and d[\"bit_bias\"] is None" <<< "$out"
'

check "JSON -e: poker + min_entropy keys" bash -c '
  out=$('"${FASTENT}"' --json -e "'"${FIX}"'/lcg.bin")
  python3 -c "import json,sys; d=json.load(sys.stdin); assert \"min_entropy\" in d and d[\"poker\"][\"df\"]==15" <<< "$out"
'

check "JSON -e bit mode: poker null" bash -c '
  out=$('"${FASTENT}"' --json -e -b "'"${FIX}"'/lcg.bin")
  python3 -c "import json,sys; d=json.load(sys.stdin); assert d[\"poker\"] is None" <<< "$out"
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
