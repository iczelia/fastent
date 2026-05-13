#!/usr/bin/env bash
#  fastent  --  self-contained sanity test suite.
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

#  ----- fixture generators (idempotent) -----------------------------
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

#  ---- assert helpers ----------------------------------------------
assert_grep() {  #  assert_grep <pattern> <text>
  local pat="$1"; local txt="$2"
  printf '%s\n' "${txt}" | grep -qE -- "${pat}"
}

#  ---- 1: all-zeros: entropy ~0, only value 0 occurs ----------------
check "all-zeros: entropy = 0" bash -c '
  out=$('"${FASTENT}"' "'"${FIX}"'/all-zeros.bin")
  grep -qE "Entropy = 0\.000000 bits per byte\." <<< "$out"
'

#  ---- 2: all-ones: only value 255 occurs, entropy ~0 ---------------
check "all-ones: entropy = 0" bash -c '
  out=$('"${FASTENT}"' "'"${FIX}"'/all-ones.bin")
  grep -qE "Entropy = 0\.000000 bits per byte\." <<< "$out"
'

#  ---- 3: uniform: each byte value 4096x => entropy = 8.0 -----------
check "uniform: entropy = 8.000000" bash -c '
  out=$('"${FASTENT}"' "'"${FIX}"'/uniform.bin")
  grep -qE "Entropy = 8\.000000 bits per byte\." <<< "$out"
'

#  ---- 4: terse mode has exactly two summary lines ------------------
check "terse mode: well-formed" bash -c '
  out=$('"${FASTENT}"' -t "'"${FIX}"'/lcg.bin")
  [ "$(printf "%s\n" "$out" | wc -l)" -eq 2 ] &&
    grep -q "^0,File-bytes," <<< "$out" &&
    grep -q "^1,1048576," <<< "$out"
'

#  ---- 5: --json mode emits a valid JSON object ---------------------
check "JSON mode: parseable" bash -c '
  out=$('"${FASTENT}"' --json "'"${FIX}"'/lcg.bin")
  python3 -c "import json,sys; json.loads(sys.stdin.read())" <<< "$out"
'

#  ---- 6: -c with default mode prints the occurrences table --------
check "counts mode: histogram present" bash -c '
  out=$('"${FASTENT}"' -c "'"${FIX}"'/uniform.bin")
  grep -q "^Value Char Occurrences Fraction$" <<< "$out" &&
    grep -qE "^Total:.*1048576" <<< "$out"
'

#  ---- 7: bit mode entropy on uniform 4096x bytes ~ 1.0 -------------
check "bit mode: entropy ~1.0 on uniform" bash -c '
  out=$('"${FASTENT}"' -b "'"${FIX}"'/uniform.bin")
  grep -qE "Entropy = 1\.000000 bits per bit\." <<< "$out"
'

#  ---- 8: stdin path works the same as file path --------------------
check "stdin matches file path" bash -c '
  a=$('"${FASTENT}"' -t "'"${FIX}"'/lcg.bin")
  b=$('"${FASTENT}"' -t < "'"${FIX}"'/lcg.bin")
  [ "$a" = "$b" ]
'

#  ---- 9: fold flag preserves total byte count ----------------------
check "fold flag: total preserved" bash -c '
  out=$('"${FASTENT}"' -tf "'"${FIX}"'/lcg.bin")
  grep -q "^1,1048576," <<< "$out"
'

#  ---- 10: --no-mmap path agrees with mmap path ---------------------
check "no-mmap path agrees" bash -c '
  a=$('"${FASTENT}"' -t "'"${FIX}"'/lcg.bin")
  b=$('"${FASTENT}"' -t --no-mmap "'"${FIX}"'/lcg.bin")
  [ "$a" = "$b" ]
'

#  ---- 11: -j N path agrees with -j 1 path --------------------------
if "${FASTENT}" --help 2>&1 | grep -q -- "-j"; then
  check "-j 4 agrees with -j 1" bash -c '
    a=$('"${FASTENT}"' -t -j 1 "'"${FIX}"'/lcg.bin")
    b=$('"${FASTENT}"' -t -j 4 "'"${FIX}"'/lcg.bin")
    [ "$a" = "$b" ]
  '
fi

#  ---- 12: empty input does not crash -------------------------------
check "empty input: clean exit" bash -c '
  : > "'"${FIX}"'/empty.bin"
  '"${FASTENT}"' -t "'"${FIX}"'/empty.bin" > /dev/null
'

#  ---- 13: --version exits 0 and mentions fastent -------------------
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
