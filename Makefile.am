ACLOCAL_AMFLAGS = -I m4

AM_CFLAGS = -std=c99 -Wall -Wextra

# config.h lives in the build root; the sources include each other
# across src/, src/kernel/ and src/platform/ with quoted includes.
AM_CPPFLAGS = -I$(top_builddir) \
              -I$(top_srcdir)/src \
              -I$(top_srcdir)/src/kernel \
              -I$(top_srcdir)/src/platform

bin_PROGRAMS  = fastent
noinst_HEADERS = src/kernel/analyze.h src/kernel/analyze-impl.h \
                 src/kernel/chisq.h \
                 src/kernel/analyze-vec-x86.h src/kernel/analyze-vec-neon.h \
                 src/kernel/analyze-vec-wasm.h \
                 src/kernel/fastent-math.h src/kernel/log2_tables.h \
                 src/kernel/fips1402.h \
                 src/common.h src/fastent-options.h src/output.h \
                 src/cli.h src/runner.h src/yarg.h \
                 src/platform/fastent-win32.h \
                 src/platform/port-cpu.h src/platform/port-io.h \
                 src/platform/port-term.h src/platform/port-os.h \
                 src/platform/port-thread.h src/platform/port-walk.h

fastent_SOURCES = src/fastent.c src/cli.c src/runner.c \
                  src/kernel/analyze.c src/kernel/analyze-scalar.c \
                  src/kernel/digram.c src/kernel/chisq.c \
                  src/kernel/fips1402.c src/kernel/fastent-math.c \
                  src/output-default.c src/output-terse.c src/output-json.c \
                  src/output-histogram.c src/output-recursive.c \
                  src/output-annotated.c \
                  src/platform/port-cpu-x86.c src/platform/port-cpu-arm.c \
                  src/platform/port-cpu-other.c \
                  src/platform/port-io.c src/platform/port-io-posix.c \
                  src/platform/port-io-win32.c \
                  src/platform/port-term-posix.c \
                  src/platform/port-term-win32.c \
                  src/platform/port-term-djgpp.c \
                  src/platform/port-os-posix.c src/platform/port-os-win32.c \
                  src/platform/port-walk-posix.c \
                  src/platform/port-walk-win32.c
if FASTENT_THREADS
fastent_SOURCES += src/platform/port-thread-posix.c \
                   src/platform/port-thread-win32.c
endif
if FASTENT_IS_WINDOWS
fastent_SOURCES += src/platform/fastent-win32.c
endif
fastent_CPPFLAGS = $(AM_CPPFLAGS) $(FASTENT_WIN_CPPFLAGS)
fastent_LDFLAGS  = $(FASTENT_WIN_LDFLAGS)
fastent_LDADD =

noinst_LIBRARIES =

if HAVE_SSSE3
noinst_LIBRARIES           += libfastent_ssse3.a
libfastent_ssse3_a_SOURCES  = src/kernel/analyze-ssse3.c
libfastent_ssse3_a_CFLAGS   = $(AM_CFLAGS) $(SSSE3_CFLAGS)
fastent_LDADD              += libfastent_ssse3.a
endif

if HAVE_SSE41
noinst_LIBRARIES           += libfastent_sse41.a
libfastent_sse41_a_SOURCES  = src/kernel/analyze-sse41.c
libfastent_sse41_a_CFLAGS   = $(AM_CFLAGS) $(SSE41_CFLAGS)
fastent_LDADD              += libfastent_sse41.a
endif

if HAVE_AVX2
noinst_LIBRARIES           += libfastent_avx2.a
libfastent_avx2_a_SOURCES   = src/kernel/analyze-avx2.c
libfastent_avx2_a_CFLAGS    = $(AM_CFLAGS) $(AVX2_CFLAGS)
fastent_LDADD              += libfastent_avx2.a
endif

if HAVE_AVX512
noinst_LIBRARIES            += libfastent_avx512.a
libfastent_avx512_a_SOURCES  = src/kernel/analyze-avx512.c
libfastent_avx512_a_CFLAGS   = $(AM_CFLAGS) $(AVX512_CFLAGS)
fastent_LDADD               += libfastent_avx512.a
endif

if HAVE_AVX512_BITALG
noinst_LIBRARIES                   += libfastent_avx512_bitalg.a
libfastent_avx512_bitalg_a_SOURCES  = src/kernel/analyze-avx512-bitalg.c
libfastent_avx512_bitalg_a_CFLAGS   = $(AM_CFLAGS) $(AVX512_BITALG_CFLAGS)
fastent_LDADD                      += libfastent_avx512_bitalg.a
endif

if HAVE_NEON
noinst_LIBRARIES           += libfastent_neon.a
libfastent_neon_a_SOURCES   = src/kernel/analyze-neon.c
libfastent_neon_a_CFLAGS    = $(AM_CFLAGS) $(NEON_CFLAGS)
fastent_LDADD              += libfastent_neon.a
endif

if HAVE_SVE2
noinst_LIBRARIES           += libfastent_sve2.a
libfastent_sve2_a_SOURCES   = src/kernel/analyze-sve2.c
libfastent_sve2_a_CFLAGS    = $(AM_CFLAGS) $(SVE2_CFLAGS)
fastent_LDADD              += libfastent_sve2.a
endif

if HAVE_WASM128
noinst_LIBRARIES               += libfastent_wasm128.a
libfastent_wasm128_a_SOURCES    = src/kernel/analyze-wasm128simd.c
libfastent_wasm128_a_CFLAGS     = $(AM_CFLAGS) $(WASM128_CFLAGS)
fastent_LDADD                  += libfastent_wasm128.a
endif

dist_man_MANS = fastent.1

EXTRA_DIST = README.md NEWS ChangeLog AUTHORS COPYING bootstrap \
             build/msvc/Makefile.nmake build/ci/package.sh \
             tests/run-tests.sh tests/bench.sh tests/bench-gen.c \
             doc/bench.png doc/bench.tsv \
             contrib/dos/dosbox-x.conf contrib/gplv3.png

check-local:
	$(srcdir)/tests/run-tests.sh "$(builddir)/fastent$(EXEEXT)" \
	  "$(builddir)/tests/fixtures"

clean-local:
	-rm -rf "$(builddir)/tests/fixtures"
	-rm -f "$(builddir)/fastent-dos-stub-stamp" "$(builddir)/fastent"

.PHONY: bench bench-quick bench-clean
bench: fastent
	@BUILDDIR="$(builddir)" SRCDIR="$(srcdir)" CC="$(CC)" \
	  $(SHELL) "$(srcdir)/tests/bench.sh"

bench-quick: fastent
	@BUILDDIR="$(builddir)" SRCDIR="$(srcdir)" CC="$(CC)" BENCH_QUICK=1 \
	  $(SHELL) "$(srcdir)/tests/bench.sh"

bench-clean:
	-rm -rf "$${CACHE_DIR:-/tmp/fastent-bench}"
	-rm -f "$(builddir)/tests/bench-gen"

# Bundle CWSDPMI into the .exe; self-contained on bare DOS.
if FASTENT_DOS
all-local: fastent-dos-stub-stamp
fastent-dos-stub-stamp: fastent$(EXEEXT)
	$(EXE2COFF) fastent$(EXEEXT)
	cat $(CWSDSTUB) fastent > fastent.new
	rm -f fastent
	mv fastent.new fastent$(EXEEXT)
	chmod +x fastent$(EXEEXT)
	touch $@
endif
