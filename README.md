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
With `-e` it additionally reports min-entropy, collision entropy and
index of coincidence, a poker test (16-bin nibble chi-square, df=15),
variance and standard deviation, redundancy, the distinct-symbol count,
the most/least common symbol, and the per-bit-position bias.  Byte mode by default; bit mode under
`-b`.  Output formats: human-readable, CSV (`-t`), JSON (`-J`), and
an interpretive pass/fail report (`-a`/`--annotate`); add a per-value
occurrence table with `-c` or a terminal block-plot of the histogram
with `-H`.

## Highlights

- 4-way banked SIMD histogram in the inner loop, breaking the
  read-modify-write hazard chain on the increment-memory path
- serial-correlation cross-product via `VPMADDUBSW` with an on-the-fly
  sign-correction derived from `PSADBW`; AArch64 SVE2 builds use
  `svdot_u32` at the host's native vector length
- bit-mode population count via `VPOPCNTB` (AVX-512 BITALG), with a
  PSHUFB nibble lookup fallback on AVX2 / SSE, `vcntq_u8` on NEON,
  and `wasm_i8x16_popcnt` on WebAssembly SIMD128
- runtime CPU dispatch across scalar / SSSE3 / SSE4.1 / AVX2 /
  AVX-512 (F + BW + BITALG) / AArch64 NEON / AArch64 SVE2 / ARMv7-A
  NEON / WebAssembly SIMD128
- input strategy selectable via `-i`/`--io={auto,mmap,stream,uring}`:
  `mmap` with `MADV_SEQUENTIAL`/`POSIX_FADV_SEQUENTIAL` for regular
  files; `uring` for a four-deep async pipeline (io_uring on Linux
  5.1+, IOCP on Windows Vista+) to hide latency on cold-cache NVMe;
  `stream` for a 2 MiB aligned `read(2)` loop everywhere
- extended statistics (`-e`) derived at finalisation from the histogram
  and running sums (min-entropy, collision entropy / IC, poker test,
  variance, redundancy, distinct/mode/rarest, per-bit-position bias);
  no per-byte cost and bit-identical across hosts and thread counts.
  Per-bit-position bias catches structured binary (dead high bits,
  ASCII bit 7) that order-0 entropy and chi-square miss.  `--annotate` turns
  them into a per-metric PASS / WEAK / FAIL report with a headline
  verdict
- recursive mode (`-r DIR`) walks a directory and emits one CSV / JSON
  row per file, with `--sort-by={path,samples,entropy,chisq,mean,pi,scc,`
  `min-entropy,collision,ic,poker,variance,redundancy,distinct,bitbias}`
- terminal histogram visualisation (`-H`) with Unicode block glyphs,
  optional log Y axis (`--log`), and platform-native colouring
  (ANSI / `SetConsoleTextAttribute` / DJGPP `<conio.h>`)
- optional worker pool (pthread on POSIX, CreateThread + SRWLOCK +
  CONDITION_VARIABLE on Windows Vista+) partitioning the mmap region
  into 6-aligned slabs so the Monte Carlo Pi state machine never
  crosses a thread; merge order is fixed and the multi-threaded
  output is byte-identical to `-j 1`
- entropy is computed via a double-double Horner expansion over a
  128-entry log-table with a split-domain Sterbenz branch at p=0.5;
  faithfully rounded and bit-identical across libc / FPU
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
- `--disable-wasm128` - skip the WebAssembly SIMD128 analyse body
  (target older wasm runtimes without the SIMD128 proposal; scalar
  variant dispatches in its place)
- `--with-windows-target=vista|win95` - Windows target version
  - `vista` (default): wide-API path, modern PE subsystem
  - `win95`: narrow-API path, PE subsystem 4.0, kernel32 + msvcrt imports only

## Supported platforms

Verified to compile and pass `make check`:

Primary platforms:
- Linux: x86_64, i386, aarch64 (NEON + SVE2), armhf (NEON) (gcc, clang, tcc)
- Windows: x86_64, i686, aarch64 (mingw-w64, zig cc) - threads + IOCP on Vista+
- macOS: x86_64, aarch64 (NEON + SVE2) (clang)
- OpenBSD / FreeBSD: pledge(2) on OpenBSD; SVE2 hwcap query on FreeBSD/macOS

Exotic Linux (musl, fully static; smoke-tested under qemu-user):
- riscv64, ppc64le, ppc64, powerpc, s390x, loongarch64,
  mips, mipsel, mips64, mips64el

Exotic Linux (glibc, fully static; build-validated):
- alpha, sparc64, sparc, m68k, hppa, arc, sh4

WebAssembly (emscripten, single-file node launcher; `-msimd128`):
- wasm32: `node fastent.js ...`
- wasm64: `node --experimental-wasm-memory64 fastent.js ...` (-sMEMORY64)

Self-contained `.js` per target (the wasm is base64-embedded via
`-sSINGLE_FILE`); `-sNODERAWFS` routes the libc through node's `fs`.

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
