# fastent

fastent measures entropy and runs statistical tests on byte or bit streams.

fastent is licensed under GNU GPL version 3. See [COPYING](COPYING). Report
issues to Kamila Szewczyk <k@iczelia.net>. The project is hosted at
<https://github.com/iczelia/fastent>.

[![CI](https://github.com/iczelia/fastent/actions/workflows/ci.yml/badge.svg?branch=trunk)](https://github.com/iczelia/fastent/actions/workflows/ci.yml)

## Quick start

```sh
fastent sample.bin
fastent -a -ee sample.bin
fastent -b -H random.bin
fastent -r --sort-by=entropy data/
```

With no path, fastent reads standard input. Byte mode is the default; `-b`
selects bits. `-a` explains the result, `-H` draws a histogram, and `-r` emits
one CSV row per file below a directory.

## Tests

The default report contains Shannon entropy, chi-square and its upper-tail
probability, arithmetic mean, a Monte Carlo estimate of pi, and serial
correlation. Repeat `-e` to add slower tests.

| Level | Added tests |
| --- | --- |
| `-e` | Min-entropy, collision entropy, index of coincidence, poker, variance, redundancy, symbol and bit bias, LZ77F, and Bandt-Pompe permutation entropy |
| `-ee` | Order-1 conditional entropy and mutual information, runs, longest run, and bit cusum |
| `-eee` | 512-bit Berlekamp-Massey complexity, Maurer universal, and NIST 32x32 binary matrix rank |

`--fips-140-2` replaces the normal report with the FIPS 140-2 monobit, poker,
runs, and long-run power-up tests over each complete 20,000-bit block. It exits
with status 1 when a block fails.

Passing every test does not prove that the input is random. Some tests need
more data than others.

## Output

Plain text is the default. `-t` writes CSV, `-J` writes JSON, and `-a` writes a
PASS, WEAK, or FAIL report. `-c` includes occurrence counts. `-p` prints binary64
values with enough digits to round-trip.

`-H` draws the selected distribution. Extended mode adds plots for LZ77F,
permutation entropy, linear complexity, Maurer distance, and matrix rank when
their tests are active. `--log` selects a logarithmic Y axis and
`--color=auto|always|never` controls color.

Recursive mode accepts `--sort-by=COL[:asc|desc]`. Common columns are `path`,
`samples`, `entropy`, `chisq`, `mean`, `pi`, and `scc`; `fastent --help` lists
the extended columns. Sorting by one enables the required `-e` level.

## Installation

Use a package or download a binary from GitHub Releases. To build a release
tarball, run

```sh
./configure
make
make check
sudo make install
```

Run `./bootstrap` first in a Git checkout. It requires autoconf and automake.
Releases regenerate `ChangeLog` from git history with the vendored
`contrib/gitlog-to-changelog`. The program requires C99 and libm. Threads are
optional.

| Configure option | Effect |
| --- | --- |
| `--enable-native` | Tune for the build host |
| `--enable-lto` | Enable link-time optimization |
| `--disable-threads` | Build without a worker pool |
| `--disable-wasm128` | Omit the WebAssembly SIMD128 kernels |
| `--with-windows-target=win95` | Use the narrow Windows 95 API and PE baseline |

## Input and concurrency

`--io=auto` maps regular files and reads other input as a stream. `mmap` rejects
input that cannot be mapped. `stream` uses ordinary reads. `uring` uses io_uring
on Linux or IOCP on Windows. Selecting an unavailable backend is an error.

`-j N` uses `N` workers; `-j auto` uses the online CPU count. Mapped files are
split into aligned slabs. Stream and asynchronous input use a bounded shared
pipeline.

fastent includes scalar, SSSE3, SSE4.1, AVX2, AVX-512, NEON, SVE2, and
WebAssembly SIMD128 kernels. It selects one at run time.

## Portability

fastent builds on Linux, Windows, macOS, OpenBSD, and FreeBSD on x86 and ARM.
Release archives also include statically linked Linux binaries, WebAssembly,
Windows 95, and DJGPP/MS-DOS. Cross-build recipes are in
[the release workflow](.github/workflows/release.yml).

Build for 64-bit Windows with

```sh
./configure --host=x86_64-w64-mingw32 \
            CC=x86_64-w64-mingw32-gcc LDFLAGS=-static
make
```

Emscripten builds use `-msimd128` and produce a single-file Node launcher. DOS
builds include CWSDPMI in the executable.

## Performance

Byte analysis uses banked histograms. SIMD kernels compute correlation and bit
counts. Grid tests process fixed 4 MiB blocks.

`make bench` generates ten deterministic 512 MiB inputs and compares all modes
and thread counts with `ent(1)`. `make bench-quick` runs a smaller sanity check.

![Throughput scaling](doc/bench.png)

## TODO

- Major readability-focused code reorganisation.
- NIST SP 800-90B battery.
- Order-2/3 conditional entropy.
- LZ76 / LZ78 / LZW dictionary complexity -- need to determine actual
  practical gains to justify addition over the LZ77F estimator.
- Spectral / DFT test, Hurst exponent / DFA / autocorrelation(lag k) tests.
- NIST SP 800-22 STS battery.

## See also

`ent(1)`, `rngtest(1)`, `dieharder(1)`, `od(1)`
