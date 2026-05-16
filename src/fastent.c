/*  fastent: high-throughput pseudorandom byte-stream entropy tester.

    Copyright (C) 2023-2026 Kamila Szewczyk.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.  */

#include "common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analyze.h"
#include "cli.h"
#include "fastent-options.h"
#include "fips1402.h"
#include "output.h"
#include "port-io.h"
#include "port-os.h"
#include "runner.h"
#include "port-thread.h"

int main(int argc, char ** argv) {
  fastent_os_init_console();
  if (fastent_os_argv_utf8(&argc, &argv) != 0) {
    fprintf(stderr, "fastent: failed to decode command line\n");
    return 2;
  }

  fastent_options o;
  int rc = fastent_parse_args(argc, argv, &o);
  if (rc != 0) return rc < 0 ? 1 : rc;

  if (!o.path) fastent_os_set_stdin_binary();

  if (o.recursive && !o.path) {
    fprintf(stderr, "fastent: -r requires a directory path\n");
    free((void *) o.path);
    return 1;
  }

  /*  rpath needed to open files; recursive mode also needs to walk
      directories so it stays at rpath for the lifetime of the run.  */
  if (fastent_os_pledge(o.path ? "stdio rpath" : "stdio") == -1) {
    perror("pledge");
    free((void *) o.path);
    return 2;
  }

#ifdef FASTENT_HAVE_THREADS
  if (o.threads < 0) {
    long n = fastent_os_num_cpus();
    o.threads = n > 0 && n < 1024 ? (int) n : 1;
  }
  if (o.threads > 1) fastent_set_num_threads(o.threads);
#endif

  fastent_analyze_fn fn_byte      = fastent_pick_variant(NULL);
  fastent_analyze_fn fn_bits      = fastent_pick_bits_variant(NULL);
  fastent_analyze_fn fn_byte_fold = fastent_pick_fold_byte_variant(NULL);
  fastent_analyze_fn fn_bits_fold = fastent_pick_fold_bits_variant(NULL);

  if (o.recursive) {
    fastent_recursive_row * rows = NULL;
    sz n = 0;
    if (fastent_run_recursive(o.path, &o, fn_byte, fn_bits,
                              fn_byte_fold, fn_bits_fold, &rows, &n) != 0) {
      fprintf(stderr, "fastent: %s: %s\n", o.path, strerror(errno));
      free((void *) o.path);
      return 2;
    }
    fastent_rows_sort(rows, n, &o);
    if (o.json) fastent_print_recursive_json(rows, n, &o);
    else        fastent_print_recursive_csv (rows, n, &o);
    fastent_rows_free(rows, n);
    free((void *) o.path);
    return 0;
  }

  fastent_source src;
  fastent_io_mode io_mode = (fastent_io_mode) o.io_mode;
  if (fastent_src_open(&src, o.path, io_mode) != 0) {
    if (o.path) {
      fprintf(stderr, "cannot open file %s: %s\n", o.path, strerror(errno));
    } else {
      perror("stdin");
    }
    free((void *) o.path);
    return 2;
  }

  if (o.path && fastent_os_pledge("stdio") == -1) {
    perror("pledge");
    free((void *) o.path);
    return 2;
  }

  if (o.fips140) {
    const u8 * data;
    sz dlen;
    void * owned = NULL;
    if (src.kind == FASTENT_SRC_MMAP) {
      data = (const u8 *) src.map;  dlen = (sz) src.size;
    } else {
      u8 * acc = NULL;
      sz cap = 0, used = 0;
      for (;;) {
        sz n = fastent_src_read(&src);
        if (n == (sz) -1) {
          perror("read");  free(acc);
          fastent_src_close(&src);  free((void *) o.path);  return 2;
        }
        if (n == 0) break;
        if (used + n > cap) {
          sz nc = (used + n) * 2;
          u8 * g = (u8 *) realloc(acc, nc ? nc : (used + n));
          if (!g) {
            fprintf(stderr, "out of memory\n");  free(acc);
            fastent_src_close(&src);  free((void *) o.path);  return 2;
          }
          acc = g;  cap = nc ? nc : (used + n);
        }
        memcpy(acc + used, src.stream_buf, n);  used += n;
      }
      data = acc;  dlen = used;  owned = acc;
    }
    fastent_fips_report rep;
    fastent_fips140_run(data, dlen, o.threads, &rep);
    int ok = fastent_fips140_print(&rep, stdout);
    free(owned);
    fastent_src_close(&src);
    free((void *) o.path);
    return ok ? 0 : 1;
  }

  fastent_chunk_state st;
  fastent_chunk_state_init(&st);
  if (o.extended >= 2 && !o.binary) {
    st.bigram = fastent_bigram_alloc();
    if (!st.bigram) {
      fprintf(stderr, "out of memory\n");
      free((void *) o.path);
      return 2;
    }
  }

  if (src.kind == FASTENT_SRC_MMAP) {
    fastent_run_mmap(&st, &o, fn_byte, fn_bits, fn_byte_fold, fn_bits_fold,
                     (const u8 *) src.map, src.size);
  } else {
    fastent_run_stream(&st, &o, fn_byte, fn_bits, fn_byte_fold, fn_bits_fold,
                       &src);
  }

  fastent_result result;
  fastent_finalize(&st, o.binary, &result);

  fastent_src_close(&src);
  fastent_bigram_free(st.bigram);

  if      (o.json)     fastent_print_json(&result, &o);
  else if (o.terse)    fastent_print_terse(&result, &o);
  else if (o.annotate) fastent_print_annotated(&result, &o);
  else                 fastent_print_default(&result, &o);

  free((void *) o.path);
  return 0;
}
