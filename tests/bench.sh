#!/bin/sh
# fastent benchmark harness.
# Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).
#
# Knobs (override via environment):
#   FASTENT     path to the fastent binary  (default: $BUILDDIR/fastent)
#   SIZE_MB     per-dataset file size in MiB (default: 512)
#   RUNS        timed runs per cell          (default: 5)
#   WARMUP      untimed warmup runs / cell   (default: 1)
#   JOBS        space-separated -j sweep     (default: 1 2 4 8 ... nproc, capped)
#   DATASETS    space-separated kinds        (default: random zeros counter dna
#                                                      ascii biased sparse-bits
#                                                      lcg walk stripes)
#   MODES      'byte' / 'bit' / 'byte-fold' / 'bit-fold' subset
#                                            (default: byte bit byte-fold)
#   CACHE_DIR   where generated files live   (default: /tmp/fastent-bench)
#   BENCH_QUICK if non-empty: SIZE_MB=64 RUNS=3 WARMUP=0, small JOBS sweep
#
# All datasets are written once and reused across runs; subsequent runs are
# served from page cache, which is what we want for CPU-bound measurement.
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

SIZE_MB="${SIZE_MB:-512}"
RUNS="${RUNS:-5}"
WARMUP="${WARMUP:-1}"
DATASETS="${DATASETS:-random zeros counter dna ascii biased sparse-bits lcg walk stripes}"
MODES="${MODES:-byte bit byte-fold}"
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
printf '  modes     : %s\n' "$MODES"
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
  ds=$1; modeflag=$2; jobs=$3
  path="$CACHE_DIR/${ds}-${SIZE_MB}M.bin"

  # shellcheck disable=SC2086
  i=0
  while [ "$i" -lt "$WARMUP" ]; do
    "$FASTENT" -j "$jobs" $modeflag "$path" >/dev/null
    i=$(( i + 1 ))
  done

  i=0
  while [ "$i" -lt "$RUNS" ]; do
    t0=$(now)
    "$FASTENT" -j "$jobs" $modeflag "$path" >/dev/null
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
printf '  %-12s %-9s %4s   %9s   %9s   %9s   %9s   %9s\n' \
  dataset mode jobs mean stddev min max 'MiB/s'
printf '  %-12s %-9s %4s   %9s   %9s   %9s   %9s   %9s\n' \
  ------------ --------- ---- --------- --------- --------- --------- ---------

for ds in $DATASETS; do
  for mode in $MODES; do
    mf=$(modeflags_for "$mode")
    if [ -n "$ENT" ] && [ -x "$ENT" ]; then
      stats=$(run_cell_ent "$ds" "$mf" | reduce)
      set -- $stats
      printf '  %-12s %-9s %4s   %8ss   %8ss   %8ss   %8ss   %9s\n' \
        "$ds" "$mode" ent "$1" "$2" "$3" "$4" "$5"
    fi
    for j in $JOBS; do
      stats=$(run_cell "$ds" "$mf" "$j" | reduce)
      # stats = "mean stddev min max mibs"
      set -- $stats
      printf '  %-12s %-9s %4d   %8ss   %8ss   %8ss   %8ss   %9s\n' \
        "$ds" "$mode" "$j" "$1" "$2" "$3" "$4" "$5"
    done
  done
done
