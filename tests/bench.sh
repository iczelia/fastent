#!/bin/sh
#  Copyright (C) 2023-2026 Kamila Szewczyk
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, version 3.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program. If not, see <http://www.gnu.org/licenses/>.

#  Benchmark deterministic data sets.
#
#  SIZE_MB, RUNS, WARMUP, JOBS, DATASETS, MODES and LEVELS tune the run.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRCDIR="${SRCDIR:-$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)}"
BUILDDIR="${BUILDDIR:-$PWD}"
FASTENT="${FASTENT:-$BUILDDIR/fastent}"
GEN_SRC="$SRCDIR/tests/bench-gen.c"
GEN_BIN="${GEN_BIN:-$BUILDDIR/tests/bench-gen}"
CACHE_DIR="${CACHE_DIR:-/tmp/fastent-bench}"

if [ ! -x "$FASTENT" ]; then
  printf 'bench: %s: not found or not executable\n' "$FASTENT" >&2
  exit 1
fi

# Locate ent(1) if installed.  Empty ENT disables the side-by-side row.
# Override via ENT=/path/to/ent or ENT=skip to force-disable.
if [ "${ENT:-}" = "skip" ]; then
  ENT=""
elif [ -z "${ENT:-}" ]; then
  ENT=$(command -v ent 2>/dev/null || true)
fi

if command -v nproc >/dev/null 2>&1; then
  NPROC=$(nproc)
else
  NPROC=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
fi

# Power-of-two job sweep up to nproc (capped at 64 to keep matrix tidy).
gen_default_jobs() {
  j=1; out=""
  while [ "$j" -le "$NPROC" ] && [ "$j" -le 64 ]; do
    out="${out} ${j}"
    j=$(( j * 2 ))
  done
  # Include nproc itself if it's not already a power of two on the list.
  case " $out " in
    *" $NPROC "*) ;;
    *) [ "$NPROC" -le 64 ] && out="$out $NPROC" ;;
  esac
  printf '%s\n' "$out"
}

SIZE_MB="${SIZE_MB:-1024}"
RUNS="${RUNS:-5}"
WARMUP="${WARMUP:-1}"
DATASETS="${DATASETS:-random zeros counter dna ascii biased sparse-bits lcg walk stripes}"
MODES="${MODES:-byte bit byte-fold}"
LEVELS="${LEVELS:-plain e ee}"
BENCH_FIPS="${BENCH_FIPS:-1}"
JOBS_DEFAULT=$(gen_default_jobs)
JOBS="${JOBS:-$JOBS_DEFAULT}"

if [ -n "${BENCH_QUICK:-}" ]; then
  SIZE_MB=64
  RUNS=3
  WARMUP=0
  JOBS="${JOBS_QUICK:-1 $NPROC}"
fi

# Build the generator helper if needed.
mkdir -p "$BUILDDIR/tests"
if [ ! -x "$GEN_BIN" ] || [ "$GEN_SRC" -nt "$GEN_BIN" ]; then
  : "${CC:=cc}"
  $CC -O2 -std=c99 -o "$GEN_BIN" "$GEN_SRC"
fi

mkdir -p "$CACHE_DIR"

printf '== fastent benchmark ==\n'
printf '  binary    : %s\n' "$FASTENT"
if [ -n "$ENT" ] && [ -x "$ENT" ]; then
  printf '  ent       : %s (compared at jobs=ent, single-threaded)\n' "$ENT"
else
  printf '  ent       : not found in PATH (set ENT=path or ENT=skip)\n'
fi
printf '  size/run  : %d MiB\n' "$SIZE_MB"
printf '  runs/cell : %d (after %d warmup)\n' "$RUNS" "$WARMUP"
printf '  jobs      :%s\n' "$JOBS"
case "$BENCH_FIPS" in ''|0|no|off) FIPS_ON=0 ;; *) FIPS_ON=1 ;; esac

printf '  modes     : %s\n' "$MODES"
printf '  levels    : %s\n' "$LEVELS"
printf '  fips      : %s\n' "$( [ "$FIPS_ON" -eq 1 ] && echo on || echo off )"
printf '  datasets  : %s\n' "$DATASETS"
printf '  cache     : %s\n' "$CACHE_DIR"
printf '\n'

expected_size=$(( SIZE_MB * 1024 * 1024 ))
for ds in $DATASETS; do
  path="$CACHE_DIR/${ds}-${SIZE_MB}M.bin"
  actual=0
  if [ -f "$path" ]; then
    actual=$(wc -c < "$path" | tr -d ' \t')
  fi
  if [ "$actual" != "$expected_size" ]; then
    printf '  generating %-12s (%d MiB) ...\n' "$ds" "$SIZE_MB"
    "$GEN_BIN" "$ds" "$SIZE_MB" "$path"
  fi
done
printf '\n'

modeflags_for() {
  case "$1" in
    byte)       printf '%s' ''        ;;
    bit)        printf '%s' '-b'      ;;
    byte-fold)  printf '%s' '-f'      ;;
    bit-fold)   printf '%s' '-b -f'   ;;
    *)          printf '%s' ''        ;;
  esac
}

# Extended-analysis depth: plain (default), -e (level 1), -ee (level 2).
levelflags_for() {
  case "$1" in
    plain)  printf '%s' ''     ;;
    e)      printf '%s' '-e'   ;;
    ee)     printf '%s' '-ee'  ;;
    *)      printf '%s' ''     ;;
  esac
}

# Bash/zsh have $EPOCHREALTIME, but POSIX sh doesn't. `date +%s.%N` works on
# Linux/glibc (which the rest of this project already assumes via
# /usr/bin/time -f). Fall back to whole-second `date +%s` if %N isn't a digit.
now() {
  t=$(date +%s.%N 2>/dev/null || true)
  case "$t" in
    *[!0-9.]*|''|'%N'*|*.%N) date +%s ;;
    *) printf '%s\n' "$t" ;;
  esac
}

run_cell() {
  ds=$1; modeflag=$2; levelflag=$3; jobs=$4
  path="$CACHE_DIR/${ds}-${SIZE_MB}M.bin"

  # shellcheck disable=SC2086
  # --fips-140-2 exits 1 when a block fails (expected on most data);
  # this is a timing harness, so tolerate any non-zero exit.
  i=0
  while [ "$i" -lt "$WARMUP" ]; do
    "$FASTENT" -j "$jobs" $modeflag $levelflag "$path" >/dev/null || :
    i=$(( i + 1 ))
  done

  i=0
  while [ "$i" -lt "$RUNS" ]; do
    t0=$(now)
    "$FASTENT" -j "$jobs" $modeflag $levelflag "$path" >/dev/null || :
    t1=$(now)
    awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.6f\n", b - a }'
    i=$(( i + 1 ))
  done
}

# Same timing harness, but for ent(1).  Single-threaded; modeflags
# happen to match fastent for byte / bit / byte-fold / bit-fold.
run_cell_ent() {
  ds=$1; modeflag=$2
  path="$CACHE_DIR/${ds}-${SIZE_MB}M.bin"

  # shellcheck disable=SC2086
  i=0
  while [ "$i" -lt "$WARMUP" ]; do
    "$ENT" $modeflag "$path" >/dev/null
    i=$(( i + 1 ))
  done

  i=0
  while [ "$i" -lt "$RUNS" ]; do
    t0=$(now)
    "$ENT" $modeflag "$path" >/dev/null
    t1=$(now)
    awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.6f\n", b - a }'
    i=$(( i + 1 ))
  done
}

reduce() {
  awk -v size_mb="$SIZE_MB" '
    {
      x = $1 + 0.0
      n++
      sum += x
      sumsq += x * x
      if (n == 1 || x < min) min = x
      if (n == 1 || x > max) max = x
    }
    END {
      if (n == 0) { print "nan nan nan nan nan"; exit }
      mean = sum / n
      var = (n > 1) ? (sumsq - n * mean * mean) / (n - 1) : 0
      if (var < 0) var = 0
      sd = sqrt(var)
      mibs = (mean > 0) ? size_mb / mean : 0
      printf "%.4f %.4f %.4f %.4f %.1f\n", mean, sd, min, max, mibs
    }'
}

# Header: The jobs column is right-aligned text so it can hold
# both an integer (fastent) and the literal "ent" (reference run).
printf '  %-12s %-9s %-5s %4s   %9s   %9s   %9s   %9s   %9s\n' \
  dataset mode level jobs mean stddev min max 'MiB/s'
printf '  %-12s %-9s %-5s %4s   %9s   %9s   %9s   %9s   %9s\n' \
  ------------ --------- ----- ---- --------- --------- --------- --------- ---------

for ds in $DATASETS; do
  for mode in $MODES; do
    mf=$(modeflags_for "$mode")
    for level in $LEVELS; do
      lf=$(levelflags_for "$level")
      # ent(1) has no -e/-ee equivalent; compare only at the plain level.
      if [ "$level" = plain ] && [ -n "$ENT" ] && [ -x "$ENT" ]; then
        stats=$(run_cell_ent "$ds" "$mf" | reduce)
        set -- $stats
        printf '  %-12s %-9s %-5s %4s   %8ss   %8ss   %8ss   %8ss   %9s\n' \
          "$ds" "$mode" "$level" ent "$1" "$2" "$3" "$4" "$5"
      fi
      for j in $JOBS; do
        stats=$(run_cell "$ds" "$mf" "$lf" "$j" | reduce)
        # stats = "mean stddev min max mibs"
        set -- $stats
        printf '  %-12s %-9s %-5s %4d   %8ss   %8ss   %8ss   %8ss   %9s\n' \
          "$ds" "$mode" "$level" "$j" "$1" "$2" "$3" "$4" "$5"
      done
    done
  done

  # FIPS 140-2 self-tests: byte-stream only, ignores mode/level, so
  # it is one extra pass per dataset over the jobs sweep (not
  # multiplied by modes or levels).
  if [ "$FIPS_ON" -eq 1 ]; then
    for j in $JOBS; do
      stats=$(run_cell "$ds" "--fips-140-2" "" "$j" | reduce)
      set -- $stats
      printf '  %-12s %-9s %-5s %4d   %8ss   %8ss   %8ss   %8ss   %9s\n' \
        "$ds" fips - "$j" "$1" "$2" "$3" "$4" "$5"
    done
  fi
done
