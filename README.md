# fastent

A high-throughput entropy and randomness tester for byte streams.

Licensed under the GNU General Public License, version 3 only;
see [`COPYING`](COPYING).

## What it does

`fastent` applies five tests of randomness to a byte (or, with `-b`,
bit) stream and reports:

- Shannon entropy in bits per sample
- chi-square statistic and tail probability
- arithmetic mean
- Monte Carlo value of pi
- serial correlation coefficient

Results are emitted in human-readable, CSV (`-t`), or JSON (`--json`)
form.

## Highlights

- 4-way banked SIMD histogram in the inner loop;
- serial-correlation cross-product via `VPMADDUBSW` with an on-the-fly
  sign-correction derived from `PSADBW`;
- bit-mode population count via `VPOPCNTB` (AVX-512 BITALG), falling
  back to a PSHUFB nibble LUT on AVX2 / SSE;
- memory-maps regular-file inputs with `MADV_SEQUENTIAL`;
- runtime dispatch to the best available variant
  (scalar / SSSE3 / SSE4.1 / AVX2 / AVX-512) via
  `__builtin_cpu_supports`;
- optional pthread worker pool partitioning the mmap region into
  6-aligned slabs so the Monte-Carlo Pi state machine never crosses
  threads; merge order is deterministic and yields byte-identical
  output to the single-threaded run.

### Ryzen 9 5950X desktop (16C/32T, dual-channel DDR4, Zen 3)

`make bench` sweeps 10 deterministic datasets (random, zeros, counter,
dna, ascii, biased, sparse-bits, lcg, walk, stripes) at 512 MiB each
through three modes and a power-of-two `-j` ladder, with `ent(1)`
timed alongside as a single-threaded baseline.  Five timed runs per
cell after a warmup; page cache hot.

![throughput scaling](doc/bench.png)

Log-log axes, so ideal linear scaling reads as a straight line of
unit slope and the gap to ent reads as a constant log-vertical
offset.  Mode is encoded by colour; each colour has three layers: ten
thin per-dataset curves (the band thickness is the dataset spread),
the bold cross-dataset median on top, and the dashed horizontal
ent(1) baseline at the bottom.

Headline numbers (median across the 10 datasets, MiB/s):

| jobs    |  byte  |  bit   | byte + `-f` |
|:--------|-------:|-------:|------------:|
| `ent`   |    112 |     52 |          94 |
| `-j 1`  |  2 540 |  4 721 |       1 943 |
| `-j 8`  | 10 772 | 16 497 |       9 337 |
| `-j 16` | 13 244 | **17 308** |    11 826 |
| `-j 32` | **15 707** | 17 070 |  **14 475** |

vs `ent(1)`: a single fastent worker is 22 to 90x faster than ent
in the same mode; the full pthread sweep stretches that to 140 to
327x before DDR4 bandwidth caps it (bit mode flattens by `-j 8`,
byte and byte + `-f` keep climbing through `-j 32` thanks to the
cheaper per-byte inner loop being more compute-bound).

### Why the per-core ceiling

The byte-histogram inner loop does 64 indexed read-modify-write stores
per 64-byte stride into four banked u32 counters.  The AVX-512 path
(activated when the host advertises AVX-512F + BW + CD + VPOPCNTDQ +
BITALG) doubles the stride to 128 bytes and runs SCC, fold, and MC Pi
at 64-byte vector width; the histogram itself stays banked-scalar
because `VPSCATTERDD` is roughly 16 c reciprocal throughput for a
16-element zmm scatter on current x86 (Zen 3 / Zen 4 alike), losing
to the 4-banked scalar inc-mem chain which hits ~0.5 c/B at the
ROB-rate limit.

Past the per-core compute ceiling, headroom comes from multi-threading.
Pass `-j N` to spread the mmap region across N pthread workers; slabs
are 6-aligned so the Monte-Carlo Pi state machine never crosses
threads, and adjacent slab boundary products are stitched into the
serial-correlation sum at merge time.  `-j auto` resolves to
`sysconf(_SC_NPROCESSORS_ONLN)`.

## Building

From a git clone (autotools artefacts not committed):

```
$ ./bootstrap
$ ./configure --enable-native --enable-lto
$ make
$ sudo make install
```

From a release tarball:

```
$ ./configure --enable-native --enable-lto
$ make
$ sudo make install
```

Generic build (no host-specific tuning):

```
$ ./configure
$ make
```

Single-threaded build (drops the pthread dependency):

```
$ ./configure --disable-threads
```

Host-specific tuning and LTO improve performance by another 5-25% on top
of the baseline presented in the benchmark section.

## Testing

`make check` runs a self-contained sanity suite over deterministic
fixtures.  See [`tests/run-tests.sh`](tests/run-tests.sh).

`make bench` runs the full matrix (10 deterministic datasets, three
modes, power-of-two `-j` sweep, with `ent(1)` timed alongside if it
is on `PATH`).  `make bench-quick` cuts file size to 64 MiB, runs 3
trials per cell, and only times `-j 1` and `-j nproc` for a sub-minute
smoke run.  Override `SIZE_MB`, `RUNS`, `WARMUP`, `JOBS`, `MODES`,
`DATASETS`, or `ENT` on the make command line; see
[`tests/bench.sh`](tests/bench.sh).

## See also

- `rngtest(1)`, `dieharder(1)`, `od(1)`
