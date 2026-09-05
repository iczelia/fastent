/*  Copyright (C) 2023-2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

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
  fastent_options o;
  fastent_source src;
  fastent_chunk_state st;
  fastent_result result;
  fastent_io_mode io_mode;
  fastent_analyze_fn fn_byte, fn_bits, fn_byte_fold, fn_bits_fold;
  fastent_lz_acc * lz = NULL;
  fastent_bm_acc * bm = NULL;
  fastent_maurer_acc * ma = NULL;
  fastent_mrank_acc * mr = NULL;
  fastent_perment_acc * pe = NULL;
  int lzp_active, bmm_active;

  /*  Machine output always uses decimal points.  */
  setlocale(LC_NUMERIC, "C");
  fastent_os_init_console();
  if (fastent_os_argv_utf8(&argc, &argv) != 0) {
    fastent_message("cannot decode the command line");
    return 2;
  }

  if (fastent_parse_args(argc, argv, &o) != 0) return 1;

  if (!o.path) fastent_os_set_stdin_binary();

  if (o.recursive && !o.path) {
    fastent_message("-r needs a directory");
    free((void *) o.path);
    return 1;
  }

  /*  Recursive mode retains directory access.  */
  if (fastent_os_pledge(o.path ? "stdio rpath" : "stdio") == -1) {
    fastent_message("pledge: %s", strerror(errno));
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

  fn_byte      = fastent_pick_variant(NULL);
  fn_bits      = fastent_pick_bits_variant(NULL);
  fn_byte_fold = fastent_pick_fold_byte_variant(NULL);
  fn_bits_fold = fastent_pick_fold_bits_variant(NULL);

  if (o.recursive) {
    fastent_recursive_row * rows = NULL;
    sz n = 0;
    if (fastent_run_recursive(o.path, &o, fn_byte, fn_bits,
                              fn_byte_fold, fn_bits_fold, &rows, &n) != 0) {
      fastent_message("cannot walk %s: %s", o.path, strerror(errno));
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

  io_mode = (fastent_io_mode) o.io_mode;
  if (fastent_src_open(&src, o.path, io_mode) != 0) {
    if (o.path) fastent_message("cannot open %s: %s", o.path, strerror(errno));
    else fastent_message("cannot open stdin: %s", strerror(errno));
    free((void *) o.path);
    return 2;
  }

  if (o.path && fastent_os_pledge("stdio") == -1) {
    fastent_message("pledge: %s", strerror(errno));
    free((void *) o.path);
    return 2;
  }

  if (o.fips140) {
    fastent_fips_report rep;
    int ok;
    if (src.kind == FASTENT_SRC_MMAP) {
      fastent_fips140_run((const u8 *) src.map, (sz) src.size,
                          o.threads, &rep);
    } else {
      fastent_fips_stream fs;
      fastent_fips140_stream_init(&fs, &rep);
      for (;;) {
        sz n = fastent_src_read(&src);
        if (n == (sz) -1) {
          fastent_message("read error: %s", strerror(errno));
          fastent_fips140_stream_finish(&fs);
          fastent_src_close(&src);  free((void *) o.path);  return 2;
        }
        if (n == 0) break;
        fastent_fips140_stream_push(&fs, src.stream_buf, n);
      }
      fastent_fips140_stream_finish(&fs);
    }
    ok = fastent_fips140_print(&rep, stdout);
    fastent_src_close(&src);
    free((void *) o.path);
    return ok ? 0 : 1;
  }

  fastent_chunk_state_init(&st);
  if (o.extended >= 2 && !o.binary) {
    st.bigram = fastent_bigram_alloc();
    st.dg_u32 = fastent_dg_u32_alloc();
    if (!st.bigram || !st.dg_u32) {
      fastent_message("out of memory");
      free((void *) o.path);
      return 2;
    }
  }

  /*  Extended estimators use the shared absolute grid.  */
  lzp_active = o.extended >= 1;
  bmm_active = o.extended >= 3;
  if (lzp_active) {
    lz = (fastent_lz_acc *) malloc(sizeof *lz);
    pe = (fastent_perment_acc *) malloc(sizeof *pe);
    if (!lz || !pe) {
      fastent_message("out of memory");
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
      fastent_message("out of memory");
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

  if (src.kind == FASTENT_SRC_MMAP) {
    fastent_run_mmap(&st, &o, fn_byte, fn_bits, fn_byte_fold, fn_bits_fold,
                     (const u8 *) src.map, src.size);
    if (lzp_active) {
      fastent_run_lz(lz, &o, &src);
      fastent_run_perment(pe, &o, &src);
      if (bmm_active) {
        fastent_run_bm(bm, &o, &src);
        fastent_run_maurer(ma, &o, &src);
        fastent_run_mrank(mr, &o, &src);
      }
    }
  } else if (lzp_active) {
    fastent_run_stream_lz_tee(&st, lz, bm, ma, mr, pe, &o, fn_byte, fn_bits,
                              fn_byte_fold, fn_bits_fold, &src);
  } else {
    fastent_run_stream(&st, &o, fn_byte, fn_bits, fn_byte_fold, fn_bits_fold,
                       &src);
  }

  fastent_finalize(&st, o.binary, &result);

  if (lzp_active) {
    result.lz = fastent_lz77f_tables_alloc();
    if (lz->oom || pe->oom || !result.lz
        || (bmm_active && (bm->oom || ma->oom || mr->oom))) {
      fastent_message("out of memory");
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
