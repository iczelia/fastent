ACLOCAL_AMFLAGS = -I m4

AM_CFLAGS = -std=c99 -Wall -Wextra

bin_PROGRAMS  = fastent
noinst_HEADERS = analyze.h analyze-impl.h chisq.h io.h \
                 threadpool.h common.h

fastent_SOURCES = fastent.c analyze.c analyze-scalar.c chisq.c io.c
if FASTENT_THREADS
fastent_SOURCES += threadpool.c
endif
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

dist_man_MANS = fastent.1

EXTRA_DIST = README.md NEWS ChangeLog AUTHORS COPYING bootstrap \
             tests/run-tests.sh

check-local:
	$(srcdir)/tests/run-tests.sh "$(builddir)/fastent$(EXEEXT)" \
	  "$(builddir)/tests/fixtures"

clean-local:
	-rm -rf "$(builddir)/tests/fixtures"

.PHONY: bench
bench: fastent
	@printf '== stdin (10 GiB of /dev/zero) ==\n'
	@dd if=/dev/zero bs=1M count=10240 status=none | \
	  /usr/bin/time -f '  %e s, %M kB' ./fastent > /dev/null
	@printf '== mmap (4 GiB random) ==\n'
	@if [ ! -f /tmp/fastent-bench.bin ]; then \
	   dd if=/dev/urandom of=/tmp/fastent-bench.bin bs=1M count=4096 status=none; \
	 fi
	@/usr/bin/time -f '  %e s, %M kB' ./fastent /tmp/fastent-bench.bin > /dev/null
