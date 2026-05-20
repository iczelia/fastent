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
#include <locale.h>
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
  /*  Machine-readable output (terse/recursive CSV, JSON) must use
      C-locale decimal points regardless of the environment locale.  */
  setlocale(LC_NUMERIC, "C");
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
    i64 n = fastent_os_num_cpus();
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
    fastent_fips_report rep;
    if (src.kind == FASTENT_SRC_MMAP) {
      /*  mmap: pass the map directly (block-parallel under -j).  */
      fastent_fips140_run((const u8 *) src.map, (sz) src.size,
                          o.threads, &rep);
    } else {
      /*  Stream/pipe/uring: a bounded read loop feeds the streaming
          FIPS driver, O(1) memory.  Verdicts and leftover are
          byte-identical to the mmap/slurp path.  */
      fastent_fips_stream fs;
      fastent_fips140_stream_init(&fs, &rep);
      for (;;) {
        sz n = fastent_src_read(&src);
        if (n == (sz) -1) {
          perror("read");  fastent_fips140_stream_finish(&fs);
          fastent_src_close(&src);  free((void *) o.path);  return 2;
        }
        if (n == 0) break;
        fastent_fips140_stream_push(&fs, src.stream_buf, n);
      }
      fastent_fips140_stream_finish(&fs);
    }
    int ok = fastent_fips140_print(&rep, stdout);
    fastent_src_close(&src);
    free((void *) o.path);
    return ok ? 0 : 1;
  }

  fastent_chunk_state st;
  fastent_chunk_state_init(&st);
  if (o.extended >= 2 && !o.binary) {
    st.bigram = fastent_bigram_alloc();
    st.dg_u32 = fastent_dg_u32_alloc();
    if (!st.bigram || !st.dg_u32) {
      fprintf(stderr, "out of memory\n");
      free((void *) o.path);
      return 2;
    }
  }

  /*  -e reads the bytes twice (SIMD order-0/-ee scan + absolute-grid
      LZ77F parse), and also runs perment on the same grid.  mmap feeds
      every acc directly; a stream tees each chunk, O(chunk + grid
      block) and bit-identical to mmap/-j1.  -eee additionally drives
      the BM / Maurer / matrix-rank accs.  Heap, not stack: each acc
      carries a 4 MiB grid buffer (small wasm / DJGPP stacks).  */
  fastent_lz_acc * lz = NULL;
  fastent_bm_acc * bm = NULL;
  fastent_maurer_acc * ma = NULL;
  fastent_mrank_acc * mr = NULL;
  fastent_perment_acc * pe = NULL;
  /*  Grid estimators are bucketed by measured single-thread throughput.
      LZ77F (~2 GiB/s) and Bandt-Pompe permutation entropy (~1 GiB/s)
      ride -e; Berlekamp-Massey, Maurer and matrix-rank (~50-350 MiB/s)
      stay at -eee.  Allocations split accordingly.  */
  int lzp_active = (o.extended >= 1);
  int bmm_active = (o.extended >= 3);
  if (lzp_active) {
    lz = (fastent_lz_acc *) malloc(sizeof *lz);
    pe = (fastent_perment_acc *) malloc(sizeof *pe);
    if (!lz || !pe) {
      fprintf(stderr, "out of memory\n");
      free(lz);  free(pe);
      fastent_src_close(&src);  fastent_bigram_free(st.bigram);
      fastent_dg_u32_free(st.dg_u32);  free((void *) o.path);
      return 2;
    }
    fastent_lz_acc_init(lz, 0);
    fastent_perment_acc_init(pe, 0);
  }
  if (bmm_active) {
    bm = (fastent_bm_acc *) malloc(sizeof *bm);
    ma = (fastent_maurer_acc *) malloc(sizeof *ma);
    mr = (fastent_mrank_acc *) malloc(sizeof *mr);
    if (!bm || !ma || !mr) {
      fprintf(stderr, "out of memory\n");
      free(bm);  free(ma);  free(mr);
      if (lzp_active) {
        fastent_lz_acc_free(lz);  free(lz);
        fastent_perment_acc_free(pe);  free(pe);
      }
      fastent_src_close(&src);  fastent_bigram_free(st.bigram);
      fastent_dg_u32_free(st.dg_u32);  free((void *) o.path);
      return 2;
    }
    fastent_bm_acc_init(bm, 0);
    fastent_maurer_acc_init(ma, 0);
    fastent_mrank_acc_init(mr, 0);
  }

  if (lzp_active && src.kind == FASTENT_SRC_MMAP) {
    fastent_run_mmap(&st, &o, fn_byte, fn_bits, fn_byte_fold, fn_bits_fold,
                     (const u8 *) src.map, src.size);
    fastent_run_lz(lz, &o, &src);
    fastent_run_perment(pe, &o, &src);
    if (bmm_active) {
      fastent_run_bm(bm, &o, &src);
      fastent_run_maurer(ma, &o, &src);
      fastent_run_mrank(mr, &o, &src);
    }
  } else if (lzp_active) {
    fastent_run_stream_lz_tee(&st, lz,
                              bmm_active ? bm : NULL,
                              bmm_active ? ma : NULL,
                              bmm_active ? mr : NULL,
                              pe, &o, fn_byte, fn_bits,
                              fn_byte_fold, fn_bits_fold, &src);
  } else if (src.kind == FASTENT_SRC_MMAP) {
    fastent_run_mmap(&st, &o, fn_byte, fn_bits, fn_byte_fold, fn_bits_fold,
                     (const u8 *) src.map, src.size);
  } else {
    fastent_run_stream(&st, &o, fn_byte, fn_bits, fn_byte_fold, fn_bits_fold,
                       &src);
  }

  fastent_result result;
  fastent_finalize(&st, o.binary, &result);

  /*  Grid finalize.  LZ77F + perment finalise at -e; BM, Maurer, mrank
      finalise at -eee.  Sentinel fields stay NaN/0 when each gate is
      off.  result.lz (the 528 KiB LZ77F raw tables) is allocated under
      -e since LZ77F itself runs there.  */
  if (lzp_active) {
    result.lz = fastent_lz77f_tables_alloc();
    if (lz->oom || pe->oom || !result.lz
        || (bmm_active && (bm->oom || ma->oom || mr->oom))) {
      fprintf(stderr, "out of memory\n");
      fastent_lz_acc_free(lz);  free(lz);
      fastent_perment_acc_free(pe);  free(pe);
      if (bmm_active) {
        fastent_bm_acc_free(bm);  free(bm);
        fastent_maurer_acc_free(ma);  free(ma);
        fastent_mrank_acc_free(mr);  free(mr);
      }
      fastent_lz77f_tables_free(result.lz);
      fastent_src_close(&src);
      fastent_bigram_free(st.bigram);
      fastent_dg_u32_free(st.dg_u32);
      free((void *) o.path);
      return 2;
    }
    fastent_lz_finalize(lz, st.total_bytes, &result);
    fastent_perment_finalize(pe, st.total_bytes, &result);
    fastent_lz_acc_free(lz);  free(lz);
    fastent_perment_acc_free(pe);  free(pe);
    if (bmm_active) {
      fastent_bm_finalize(bm, st.total_bytes, &result);
      fastent_maurer_finalize(ma, st.total_bytes, &result);
      fastent_mrank_finalize(mr, st.total_bytes, &result);
      fastent_bm_acc_free(bm);  free(bm);
      fastent_maurer_acc_free(ma);  free(ma);
      fastent_mrank_acc_free(mr);  free(mr);
    }
  }

  fastent_src_close(&src);
  fastent_bigram_free(st.bigram);
  fastent_dg_u32_free(st.dg_u32);

  if      (o.json)     fastent_print_json(&result, &o);
  else if (o.terse)    fastent_print_terse(&result, &o);
  else if (o.annotate) fastent_print_annotated(&result, &o);
  else                 fastent_print_default(&result, &o);

  fastent_lz77f_tables_free(result.lz);
  free((void *) o.path);
  return 0;
}
