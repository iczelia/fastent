# fastent

fastent - high-throughput entropy and randomness tester for byte and bit streams,
with deterministic multi-threaded output and runtime SIMD dispatch.
Licensed under the terms of GNU GPL version 3 only - see [`COPYING`](COPYING).
Report issues to Kamila Szewczyk &lt;k@iczelia.net&gt;.
Project homepage: https://github.com/iczelia/fastent

[![CI](https://github.com/iczelia/fastent/actions/workflows/ci.yml/badge.svg?branch=trunk)](https://github.com/iczelia/fastent/actions/workflows/ci.yml)

![GPLv3](contrib/gplv3.png)

## What it reports

Shannon entropy, chi-square statistic with tail probability, arithmetic
mean, Monte Carlo value of pi, and serial correlation coefficient.
Byte mode by default; bit mode under `-b`.  Output formats: human-readable,
CSV (`-t`), JSON (`--json`); add a per-value occurrence table with `-c`.

## Highlights

- 4-way banked SIMD histogram in the inner loop, breaking the
  read-modify-write hazard chain on the increment-memory path
- serial-correlation cross-product via `VPMADDUBSW` with an on-the-fly
  sign-correction derived from `PSADBW`
- bit-mode population count via `VPOPCNTB` (AVX-512 BITALG), with a
  PSHUFB nibble lookup fallback on AVX2 / SSE
- regular-file inputs memory-mapped with `MADV_SEQUENTIAL` and
  `POSIX_FADV_SEQUENTIAL`; stream sources go through a 2 MiB aligned
  `read(2)` loop
- runtime CPU dispatch across scalar / SSSE3 / SSE4.1 / AVX2 /
  AVX-512 (F + BW + CD + DQ + VL + VPOPCNTDQ + BITALG) via a CPUID
  query
- optional pthread worker pool partitioning the mmap region into
  6-aligned slabs so the Monte Carlo Pi state machine never crosses
  a thread; merge order is fixed and the multi-threaded output is
  byte-identical to `-j 1`
- portable C99 sources: builds under gcc, clang, mingw-w64, DJGPP,
  and TinyCC; only the SIMD bodies need a vendor-extended compiler

## Requirements

- Any C99 compiler with a working libc (gcc, clang, tcc all known to work)
- autotools (`autoconf`, `automake`), `make`
- `pthreads` (optional; build without via `--disable-threads`)

## Build from a release tarball

```sh
./configure --enable-native --enable-lto
make -j"$(nproc)"
make check
```

## Build from git

```sh
./bootstrap
./configure --enable-native --enable-lto
make -j"$(nproc)"
make check
```

## Install

```sh
sudo make install
```

## Configure options

- `--enable-native` - `-march=native -mtune=native`
- `--enable-lto` - link-time optimisation
- `--disable-threads` - single-threaded build, no pthread dependency
- `--with-windows-target=vista|win95` - Windows target version
  - `vista` (default): wide-API path, modern PE subsystem
  - `win95`: narrow-API path, PE subsystem 4.0, kernel32 + msvcrt imports only

## Supported platforms

Verified to compile and pass `make check`:

Primary platforms:
- Linux: x86_64, i386, aarch64, armhf (gcc, clang, tcc)
- Windows: x86_64, i686, aarch64 (mingw-w64, zig cc)
- macOS: x86_64, aarch64 (clang)

Exotic Linux (musl, fully static; smoke-tested under qemu-user):
- riscv64, ppc64le, ppc64, powerpc, s390x, loongarch64,
  mips, mipsel, mips64, mips64el

Exotic Linux (glibc, fully static; build-validated):
- alpha, sparc64, sparc, m68k, hppa, arc, sh4

Legacy targets:
- Windows 95 (i686, mingw-w64 + `--with-windows-target=win95`)
- MS-DOS (i386, DJGPP with CWSDPMI baked into the executable)

Pre-built binaries for every target are published with each tagged
release.  Cross-compile recipes for every platform live in
[`.github/workflows/release.yml`](.github/workflows/release.yml).

## Throughput

`make bench` sweeps 10 deterministic datasets (random, zeros, counter,
dna, ascii, biased, sparse-bits, lcg, walk, stripes) at 512 MiB each
through three modes and a power-of-two `-j` ladder, with `ent(1)`
timed alongside as a single-threaded baseline.

![throughput scaling](doc/bench.png)

Median throughput on a Ryzen 9 5950X (16C/32T, dual-channel DDR4):

| jobs    |  byte  |  bit   | byte + `-f` |
|:--------|-------:|-------:|------------:|
| `ent`   |    112 |     52 |          94 |
| `-j 1`  |  2 540 |  4 721 |       1 943 |
| `-j 8`  | 10 772 | 16 497 |       9 337 |
| `-j 16` | 13 244 | **17 308** |    11 826 |
| `-j 32` | **15 707** | 17 070 |  **14 475** |

Numbers in MiB/s.  A single fastent worker is 22 to 90x faster than
`ent(1)`; saturated multi-threaded `fastent` reaches 140 to 327x
before DDR4 bandwidth caps it.

### Why the per-core ceiling

The byte-histogram inner loop does 64 indexed read-modify-write
stores per 64-byte stride into four banked u32 counters.  The
AVX-512 path (activated when the host advertises AVX-512F + BW + CD +
VPOPCNTDQ + BITALG) doubles the stride to 128 bytes and runs SCC,
fold, and Monte Carlo Pi at 64-byte vector width.  The histogram
itself stays banked-scalar: `VPSCATTERDD` is roughly 16 c reciprocal
throughput for a 16-element zmm scatter on current x86 (Zen 3 / Zen
4 alike), losing to the 4-banked scalar inc-mem chain which hits
~0.5 c/B at the ROB-rate limit.

Past the per-core compute ceiling, headroom comes from
multi-threading.  Slabs are 6-aligned so the Monte Carlo Pi state
machine never crosses threads, and adjacent slab boundary products
are stitched into the serial-correlation sum at merge time.  `-j
auto` resolves to `sysconf(_SC_NPROCESSORS_ONLN)` (GetSystemInfo on
Windows).

`make bench-quick` is a sub-minute smoke variant (64 MiB, 3 trials,
`-j 1` and `-j nproc` only).  Override `SIZE_MB`, `RUNS`, `WARMUP`,
`JOBS`, `MODES`, `DATASETS`, `ENT` on the make command line; see
[`tests/bench.sh`](tests/bench.sh).

## See also

`rngtest(1)`, `dieharder(1)`, `od(1)`, `ent(1)`
