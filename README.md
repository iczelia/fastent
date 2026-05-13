# fastent

A high-throughput entropy and randomness tester for byte streams.

Licensed under the GNU General Public License, version 3 only --
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

### Ryzen 9 5900X desktop (12C/24T, dual-channel DDR4)

4 GiB pre-warmed pseudo-random mmap'd file:

| Run                  | Wall    | Throughput       |
|----------------------|--------:|-----------------:|
| `fastent -j 1`       |  1.41 s | 2 905 MiB/s      |
| `fastent -j 2`       |  0.84 s | 4 876 MiB/s      |
| `fastent -j 4`       |  0.57 s | 7 186 MiB/s      |
| `fastent -j 8`       |  0.42 s | 9 752 MiB/s      |
| `fastent -j 16`      |  0.38 s | **10 779 MiB/s** |
| `fastent -j auto`    |  0.39 s | 10 503 MiB/s     |

At `-j 16` the kernel saturates dual-channel DDR4 bandwidth on this
host; adding SMT siblings (`-j auto` = 24) yields no further gain.

### Ryzen 7 PRO 7840U laptop (8C/16T, single-channel DDR5)

Same 4 GiB fixture, indicative numbers:

| Run                  | Wall    | Throughput     |
|----------------------|--------:|---------------:|
| `fastent -j 1`       |  2.02 s | ~2 000 MiB/s   |
| `fastent -j 8`       |  0.94 s | ~4 400 MiB/s   |
| `fastent -j auto`    |  0.89 s | ~4 600 MiB/s   |

Single-channel memory caps multi-threaded throughput at ~5 GB/s here;
the per-thread compute is the same as on the 5900X.

### Why the single-thread ceiling

The byte-histogram inner loop does 64 indexed read-modify-write stores
per 64-byte stride into four banked u32 counters.  The AVX-512 path
(activated when the host advertises AVX-512F + BW + CD + VPOPCNTDQ +
BITALG) doubles the stride to 128 bytes and runs the SCC/fold/MC Pi
SIMD compute at 64-byte width; the histogram itself stays banked-scalar
because `VPSCATTERDD` on AMD Zen 4 is roughly 16 c reciprocal
throughput for a 16-element zmm scatter, so any scatter-based variant
loses to the 4-banked scalar inc-mem chain (which hits ~0.5 c/B at
the ROB-rate limit).

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

## Testing

`make check` runs a self-contained sanity suite over deterministic
fixtures.  See [`tests/run-tests.sh`](tests/run-tests.sh).

`make bench` runs a stdin (10 GiB of `/dev/zero`) benchmark and an
mmap (4 GiB of `/dev/urandom`) benchmark and reports wall-clock time.

## See also

- `rngtest(1)`, `dieharder(1)`, `od(1)`
