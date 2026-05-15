/*  fastent: high-throughput pseudorandom byte-stream entropy tester.

    Copyright (C) 2026 Kamila Szewczyk.

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
#include <string.h>

#include "analyze.h"
#include "cli.h"
#include "fastent-options.h"
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

  /*  rpath only needed to open the positional argument; the second
      pledge after src_open() drops it.  */
  if (fastent_os_pledge(o.path ? "stdio rpath" : "stdio") == -1) {
    perror("pledge");
    return 2;
  }

#ifdef FASTENT_HAVE_THREADS
  if (o.threads < 0) {
    long n = fastent_os_num_cpus();
    o.threads = n > 0 && n < 1024 ? (int) n : 1;
  }
  if (o.threads > 1) fastent_set_num_threads(o.threads);
#endif

  fastent_source src;
  fastent_io_mode io_mode = (fastent_io_mode) o.io_mode;
  if (io_mode == FASTENT_IO_AUTO && o.no_mmap) io_mode = FASTENT_IO_STREAM;
  if (fastent_src_open(&src, o.path, io_mode) != 0) {
    if (o.path) {
      fprintf(stderr, "cannot open file %s: %s\n", o.path, strerror(errno));
    } else {
      perror("stdin");
    }
    return 2;
  }

  if (o.path && fastent_os_pledge("stdio") == -1) {
    perror("pledge");
    return 2;
  }

  fastent_analyze_fn fn_byte      = fastent_pick_variant(NULL);
  fastent_analyze_fn fn_bits      = fastent_pick_bits_variant(NULL);
  fastent_analyze_fn fn_byte_fold = fastent_pick_fold_byte_variant(NULL);
  fastent_analyze_fn fn_bits_fold = fastent_pick_fold_bits_variant(NULL);

  fastent_chunk_state st;
  fastent_chunk_state_init(&st);

  if (src.kind == FASTENT_SRC_MMAP) {
    fastent_run_mmap(&st, &o, fn_byte, fn_bits, fn_byte_fold, fn_bits_fold,
                     (const u8 *) src.map, src.size);
  } else {
    fastent_run_stream(&st, &o, fn_byte, fn_bits, fn_byte_fold, fn_bits_fold,
                       &src);
  }

  fastent_src_close(&src);

  fastent_result result;
  fastent_finalize(&st, o.binary, &result);

  if      (o.json)  fastent_print_json(&result, &o);
  else if (o.terse) fastent_print_terse(&result, &o);
  else              fastent_print_default(&result, &o);

  return 0;
}
