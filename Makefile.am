ACLOCAL_AMFLAGS = -I m4

AM_CFLAGS = -std=c99 -Wall -Wextra

bin_PROGRAMS  = fastent
noinst_HEADERS = analyze.h analyze-impl.h chisq.h fastent-io.h \
                 threadpool.h common.h fastent-win32.h

fastent_SOURCES = fastent.c analyze.c analyze-scalar.c chisq.c fastent-io.c
if FASTENT_THREADS
fastent_SOURCES += threadpool.c
endif
if FASTENT_IS_WINDOWS
fastent_SOURCES += fastent-win32.c
endif
fastent_CPPFLAGS = $(FASTENT_WIN_CPPFLAGS)
fastent_LDFLAGS  = $(FASTENT_WIN_LDFLAGS)
fastent_LDADD =

noinst_LIBRARIES =

if HAVE_SSSE3
noinst_LIBRARIES           += libfastent_ssse3.a
libfastent_ssse3_a_SOURCES  = analyze-ssse3.c
libfastent_ssse3_a_CFLAGS   = $(AM_CFLAGS) $(SSSE3_CFLAGS)
fastent_LDADD              += libfastent_ssse3.a
endif

if HAVE_SSE41
noinst_LIBRARIES           += libfastent_sse41.a
libfastent_sse41_a_SOURCES  = analyze-sse41.c
libfastent_sse41_a_CFLAGS   = $(AM_CFLAGS) $(SSE41_CFLAGS)
fastent_LDADD              += libfastent_sse41.a
endif

if HAVE_AVX2
noinst_LIBRARIES           += libfastent_avx2.a
libfastent_avx2_a_SOURCES   = analyze-avx2.c
libfastent_avx2_a_CFLAGS    = $(AM_CFLAGS) $(AVX2_CFLAGS)
fastent_LDADD              += libfastent_avx2.a
endif

if HAVE_AVX512
noinst_LIBRARIES            += libfastent_avx512.a
libfastent_avx512_a_SOURCES  = analyze-avx512.c
libfastent_avx512_a_CFLAGS   = $(AM_CFLAGS) $(AVX512_CFLAGS)
fastent_LDADD               += libfastent_avx512.a
endif

if HAVE_AVX512_BITALG
noinst_LIBRARIES                   += libfastent_avx512_bitalg.a
libfastent_avx512_bitalg_a_SOURCES  = analyze-avx512-bitalg.c
libfastent_avx512_bitalg_a_CFLAGS   = $(AM_CFLAGS) $(AVX512_BITALG_CFLAGS)
fastent_LDADD                      += libfastent_avx512_bitalg.a
endif

if HAVE_NEON
noinst_LIBRARIES           += libfastent_neon.a
libfastent_neon_a_SOURCES   = analyze-neon.c
libfastent_neon_a_CFLAGS    = $(AM_CFLAGS) $(NEON_CFLAGS)
fastent_LDADD              += libfastent_neon.a
endif

dist_man_MANS = fastent.1

EXTRA_DIST = README.md NEWS ChangeLog AUTHORS COPYING bootstrap \
             tests/run-tests.sh tests/bench.sh tests/bench-gen.c \
             doc/bench.png doc/bench.tsv \
             contrib/dos/dosbox-x.conf

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
