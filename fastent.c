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

#include "common.h"   /*  Must be first; defines feature macros.  */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analyze.h"
#include "fastent-options.h"
#include "output.h"
#include "port-io.h"
#include "port-os.h"
#ifdef FASTENT_HAVE_PTHREAD
  #include "threadpool.h"
#endif

/*  Help / version.  */

static void print_version(void) {
  printf("fastent %s\n", FASTENT_VERSION_STRING);
  printf("Copyright (C) 2026 Kamila Szewczyk.\n");
  printf("License GPLv3: GNU GPL version 3 only"
         " <https://gnu.org/licenses/gpl-3.0.html>.\n");
  printf("This is free software: you are free to change and redistribute it.\n");
  printf("There is NO WARRANTY, to the extent permitted by law.\n");
}

static void print_help(void) {
  printf("fastent -- measure randomness of a byte (or bit) stream.\n");
  printf("Usage:     fastent [options] [file]\n");
  printf("\n");
  printf("Options:   -b   Treat input as a stream of bits\n");
  printf("           -c   Print occurrence counts\n");
  printf("           -f   Fold upper- to lower-case letters\n");
  printf("           -t   Terse output in CSV format\n");
  printf("           -H,  --histogram          Render a block bar plot\n");
  printf("                                     of the byte distribution\n");
  printf("                --log                Logarithmic y-axis for --histogram\n");
  printf("                --color=MODE         auto (default), always, never\n");
  printf("           -p,  --full-precision     Render every float at %%.17g\n");
  printf("           -j N --threads=N          Use N worker threads"
         " (default 1)\n");
  printf("                --no-mmap            Alias for --io=stream\n");
  printf("                --io=MODE            mmap (default for regular files),\n");
  printf("                                     stream, uring (Linux 5.1+), auto\n");
  printf("                --json               Emit results as JSON\n");
  printf("           -V,  --version            Print version and exit\n");
  printf("           -h,  --help               Print this message\n");
}

/*  Argument parsing.  */

static int parse_int(const char * s, int * out) {
  if (!s || !*s) return -1;
  if (!strcmp(s, "auto")) {
    *out = -1;   /*  Sentinel; resolved later via sysconf.  */
    return 0;
  }
  char * end = NULL;
  long v = strtol(s, &end, 10);
  if (end == s || (end && *end)) return -1;
  if (v < 0 || v > 1024) return -1;
  *out = (int) v;
  return 0;
}

static int parse_args(int argc, char ** argv, fastent_options * o) {
  int i;
  int saw_path = 0;
  memset(o, 0, sizeof(*o));
  o->threads = 1;
  o->color   = 1;   /*  --color=auto by default  */

  for (i = 1; i < argc; i++) {
    const char * a = argv[i];
    if (a[0] == '-' && a[1] != '\0') {
      if (a[1] == '-') {
        /*  Long option.  */
        const char * name = a + 2;
        const char * eq = strchr(name, '=');
        const char * val = NULL;
        size_t nlen;
        if (eq) {
          nlen = (size_t)(eq - name);
          val = eq + 1;
        } else {
          nlen = strlen(name);
        }
        #define LONG_IS(s) (nlen == (sizeof(s) - 1) && \
                            !strncmp(name, (s), sizeof(s) - 1))
        if      (LONG_IS("bits"))           o->binary = 1;
        else if (LONG_IS("counts"))         o->counts = 1;
        else if (LONG_IS("fold"))           o->fold = 1;
        else if (LONG_IS("terse"))          o->terse = 1;
        else if (LONG_IS("json"))           o->json = 1;
        else if (LONG_IS("full-precision")) o->full_precision = 1;
        else if (LONG_IS("no-mmap"))        o->no_mmap = 1;
        else if (LONG_IS("histogram"))      o->histogram = 1;
        else if (LONG_IS("log"))            o->histogram_log = 1;
        else if (LONG_IS("color")) {
          if (!val) o->color = 2;            /*  --color (no arg) = always  */
          else if (!strcmp(val, "auto"))   o->color = 1;
          else if (!strcmp(val, "always")) o->color = 2;
          else if (!strcmp(val, "never"))  o->color = 0;
          else { fprintf(stderr, "unknown --color mode: %s\n", val); return -1; }
        }
        else if (LONG_IS("io")) {
          if (!val) {
            if (i + 1 >= argc) { fprintf(stderr, "--io requires an argument\n"); return -1; }
            val = argv[++i];
          }
          if      (!strcmp(val, "auto"))   o->io_mode = (int) FASTENT_IO_AUTO;
          else if (!strcmp(val, "mmap"))   o->io_mode = (int) FASTENT_IO_MMAP;
          else if (!strcmp(val, "stream")) o->io_mode = (int) FASTENT_IO_STREAM;
          else if (!strcmp(val, "uring"))  o->io_mode = (int) FASTENT_IO_URING;
          else { fprintf(stderr, "unknown --io mode: %s\n", val); return -1; }
        }
        else if (LONG_IS("help"))         { print_help(); exit(0); }
        else if (LONG_IS("version"))      { print_version(); exit(0); }
        else if (LONG_IS("threads")) {
          if (!val) {
            if (i + 1 >= argc) { fprintf(stderr, "--threads requires an argument\n");  return -1; }
            val = argv[++i];
          }
          if (parse_int(val, &o->threads) != 0) { fprintf(stderr, "--threads: invalid count '%s'\n", val);  return -1; }
          if (o->threads == 0) o->threads = 1;
        } else { fprintf(stderr, "--%.*s: unknown option\n", (int) nlen, name);  return -1; }
        #undef LONG_IS
      } else {
        /*  Short option(s).  */
        int k;
        for (k = 1; a[k]; k++) {
          char c = a[k];
          switch (c) {
            case 'b': o->binary = 1; break;
            case 'c': o->counts = 1; break;
            case 'f': o->fold   = 1; break;
            case 't': o->terse  = 1; break;
            case 'H': o->histogram = 1; break;
            case 'p': o->full_precision = 1; break;
            case '?':
            case 'h': print_help();    exit(0);
            case 'V': print_version(); exit(0);
            case 'j': {
              const char * val = NULL;
              if (a[k + 1] != '\0') {
                val = a + k + 1;
              } else {
                if (i + 1 >= argc) { fprintf(stderr, "-j requires an argument\n");  return -1; }
                val = argv[++i];
              }
              if (parse_int(val, &o->threads) != 0) { fprintf(stderr, "-j: invalid count '%s'\n", val);  return -1; }
              if (o->threads == 0) o->threads = 1;
              k = (int) strlen(a) - 1;   /*  break the for(k) loop  */
              break;
            }
            default: fprintf(stderr, "-%c: unknown option\n", a[k]);  return -1;
          }
        }
      }
    } else {
      /*  Positional.  */
      if (saw_path) {
        fprintf(stderr, "duplicate file name: %s\n", a);
        return -1;
      }
      o->path = a;
      saw_path = 1;
    }
  }
  return 0;
}

#ifdef FASTENT_HAVE_PTHREAD
typedef struct {
  const u8 *     data;
  const u64 *    bounds;     /*  N+1 entries, multiples of 6 except last  */
  fastent_chunk_state * states;
  fastent_analyze_fn fn;
} mt_ctx;

static void mt_worker(sz k, void * vctx) {
  mt_ctx * c = (mt_ctx *) vctx;
  u64 start = c->bounds[k];
  u64 end   = c->bounds[k + 1];
  fastent_chunk_state_init(&c->states[k]);
  c->fn(&c->states[k], c->data + start, (sz)(end - start));
}

static void run_mmap_mt(fastent_chunk_state * out, const fastent_options * o,
                        fastent_analyze_fn fn,
                        const u8 * data, u64 size) {
  int N = o->threads;
  fastent_set_num_threads(N);

  /*  6-byte-aligned slab boundaries so each thread's MC Pi state
      starts at mc_pos == 0; no cross-slab hexads.  */
  u64 * bounds = (u64 *) malloc((sz)(N + 1) * sizeof(u64));
  if (!bounds) { fprintf(stderr, "out of memory\n"); exit(2); }
  bounds[0] = 0;
  bounds[N] = size;
  Fk0(N, 1,
      u64 raw = (u64)((double) size * (double) k / (double) N);
      bounds[k] = (raw / 6ULL) * 6ULL;
      if (bounds[k] < bounds[k - 1]) bounds[k] = bounds[k - 1])

  fastent_chunk_state * states = (fastent_chunk_state *) calloc((sz) N, sizeof(*states));
  if (!states) { fprintf(stderr, "out of memory\n"); exit(2); }

  mt_ctx ctx = { data, bounds, states, fn };
  fastent_parallel_for((sz) N, mt_worker, &ctx);

  /*  Merge slabs: adjacent slab pairs contribute b[end-1] *
      b[start_next] to the SCC; final wrap (last * first) is added in
      fastent_finalize().  In bit mode the carry/first/last fields are
      single bits, so the same expression covers the cross-slab bit
      pair.  */
  Fk(N,
     const fastent_chunk_state * s = &states[k];
     if (s->total_bytes == 0) continue;
     Fi(FASTENT_BANKS, Fj(256, out->bank[i][j] += s->bank[i][j]))
     out->bit_hist[0] += s->bit_hist[0];
     out->bit_hist[1] += s->bit_hist[1];
     if (out->have_carry) {
       out->cross_product += (i64) out->carry_byte * (i64) s->first_byte;
     } else {
       out->first_byte = s->first_byte;
       out->have_first = 1;
     }
     out->cross_product += s->cross_product;
     out->carry_byte    = s->last_byte;
     out->last_byte     = s->last_byte;
     out->have_carry    = 1;
     out->total_bytes  += s->total_bytes;
     out->mc_count     += s->mc_count;
     out->mc_inside    += s->mc_inside;
     /*  Only the last slab can carry trailing MC ring bytes; the
         others end on a 6-aligned boundary.  */
     if (k == N - 1) {
       out->mc_pos = s->mc_pos;
       memcpy(out->mc_buf, s->mc_buf, sizeof(out->mc_buf));
     })

  free(states);
  free(bounds);
}
#endif

static void run_mmap(fastent_chunk_state * st, const fastent_options * o,
                     fastent_analyze_fn fn_byte, fastent_analyze_fn fn_bits,
                     fastent_analyze_fn fn_byte_fold,
                     fastent_analyze_fn fn_bits_fold,
                     const u8 * data, u64 size) {
  fastent_analyze_fn body = o->binary
    ? (o->fold ? fn_bits_fold : fn_bits)
    : (o->fold ? fn_byte_fold : fn_byte);

#ifdef FASTENT_HAVE_PTHREAD
  if (o->threads > 1 && size >= (u64)(o->threads) * 1024u * 1024u) {
    run_mmap_mt(st, o, body, data, size);
    return;
  }
#endif

  body(st, data, (sz) size);
}

static void run_stream(fastent_chunk_state * st, const fastent_options * o,
                       fastent_analyze_fn fn_byte, fastent_analyze_fn fn_bits,
                       fastent_analyze_fn fn_byte_fold,
                       fastent_analyze_fn fn_bits_fold,
                       fastent_source * src) {
  fastent_analyze_fn body = o->binary
    ? (o->fold ? fn_bits_fold : fn_bits)
    : (o->fold ? fn_byte_fold : fn_byte);
  for (;;) {
    sz n = fastent_src_read(src);
    if (n == (sz) -1) {
      perror("read");
      exit(2);
    }
    if (n == 0) break;
    body(st, src->stream_buf, n);
  }
}


/*  Main entry.  */

int main(int argc, char ** argv) {
  fastent_os_init_console();
  if (fastent_os_argv_utf8(&argc, &argv) != 0) {
    fprintf(stderr, "fastent: failed to decode command line\n");
    return 2;
  }

  fastent_options o;
  int rc = parse_args(argc, argv, &o);
  if (rc != 0) return rc < 0 ? 1 : rc;

  if (!o.path) fastent_os_set_stdin_binary();

  /*  rpath only needed to open the positional argument; the second
      pledge after src_open() drops it.  */
  if (fastent_os_pledge(o.path ? "stdio rpath" : "stdio") == -1) {
    perror("pledge");
    return 2;
  }

#ifdef FASTENT_HAVE_PTHREAD
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

  fastent_variant var = FASTENT_VAR_SCALAR;
  fastent_analyze_fn fn_byte      = fastent_pick_variant(&var);
  fastent_analyze_fn fn_bits      = fastent_pick_bits_variant(NULL);
  fastent_analyze_fn fn_byte_fold = fastent_pick_fold_byte_variant(NULL);
  fastent_analyze_fn fn_bits_fold = fastent_pick_fold_bits_variant(NULL);

  fastent_chunk_state st;
  fastent_chunk_state_init(&st);

  if (src.kind == FASTENT_SRC_MMAP) {
    run_mmap(&st, &o, fn_byte, fn_bits, fn_byte_fold, fn_bits_fold,
             (const u8 *) src.map, src.size);
  } else {
    run_stream(&st, &o, fn_byte, fn_bits, fn_byte_fold, fn_bits_fold, &src);
  }

  fastent_src_close(&src);

  fastent_result result;
  fastent_finalize(&st, o.binary, &result);

  if      (o.json)  fastent_print_json(&result, &o);
  else if (o.terse) fastent_print_terse(&result, &o);
  else              fastent_print_default(&result, &o);

  return 0;
}
