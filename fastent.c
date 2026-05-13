/*  fastent  --  high-throughput pseudorandom byte-stream entropy tester.

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
#include <unistd.h>  /*  sysconf  */

#ifdef _WIN32
  #include <fcntl.h>
  #include <io.h>
#endif

#include "analyze.h"
#include "io.h"
#ifdef FASTENT_HAVE_PTHREAD
  #include "threadpool.h"
#endif

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

/*  Glyph-bearing characters in the C / Latin-1 supplement: printable
    and not whitespace.  Used to decide which byte values get a
    printed glyph in the -c histogram.  */
static inline int fastent_is_displayable(unsigned c) {
  return (c >= 0x21u && c <= 0x7Eu) || c >= 0xA1u;
}

/*  ----------------------------------------------------------------------
    Options.  */

typedef struct {
  int binary;         /*  -b  */
  int counts;         /*  -c  */
  int fold;           /*  -f  */
  int terse;          /*  -t  */
  int json;           /*  --json  */
  int full_precision; /*  -p / --full-precision  */
  int no_mmap;        /*  --no-mmap  */
  int threads;        /*  -j / --threads  (0 = auto, 1 = default)  */
  const char * path;  /*  positional (NULL = stdin)  */
} fastent_options;

/*  ----------------------------------------------------------------------
    Help / version.  */

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
  printf("           -p,  --full-precision     Render every float at %%.17g\n");
  printf("           -j N --threads=N          Use N worker threads"
         " (default 1)\n");
  printf("                --no-mmap            Read via read(2), not mmap(2)\n");
  printf("                --json               Emit results as JSON\n");
  printf("           -V,  --version            Print version and exit\n");
  printf("           -h,  --help               Print this message\n");
}

/*  ----------------------------------------------------------------------
    Argument parsing.  */

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
  memset(o, 0, sizeof(*o));
  o->threads = 1;

  int saw_path = 0;

  for (int i = 1; i < argc; i++) {
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
        else if (LONG_IS("help"))         { print_help(); exit(0); }
        else if (LONG_IS("version"))      { print_version(); exit(0); }
        else if (LONG_IS("threads")) {
          if (!val) {
            if (i + 1 >= argc) {
              fprintf(stderr, "--threads requires an argument\n");
              return -1;
            }
            val = argv[++i];
          }
          if (parse_int(val, &o->threads) != 0) {
            fprintf(stderr, "--threads: invalid count '%s'\n", val);
            return -1;
          }
          if (o->threads == 0) o->threads = 1;
        } else {
          fprintf(stderr, "--%.*s: unknown option\n", (int) nlen, name);
          return -1;
        }
        #undef LONG_IS
      } else {
        /*  Short option(s).  */
        for (int k = 1; a[k]; k++) {
          char c = a[k];
          switch (c) {
            case 'b': o->binary = 1; break;
            case 'c': o->counts = 1; break;
            case 'f': o->fold   = 1; break;
            case 't': o->terse  = 1; break;
            case 'p': o->full_precision = 1; break;
            case '?':
            case 'h': print_help();    exit(0);
            case 'V': print_version(); exit(0);
            case 'j': {
              const char * val = NULL;
              if (a[k + 1] != '\0') {
                val = a + k + 1;
                if (parse_int(val, &o->threads) != 0) {
                  fprintf(stderr, "-j: invalid count '%s'\n", val);
                  return -1;
                }
                if (o->threads == 0) o->threads = 1;
                k = (int) strlen(a) - 1; /*  break the for(k) loop  */
              } else {
                if (i + 1 >= argc) {
                  fprintf(stderr, "-j requires an argument\n");
                  return -1;
                }
                val = argv[++i];
                if (parse_int(val, &o->threads) != 0) {
                  fprintf(stderr, "-j: invalid count '%s'\n", val);
                  return -1;
                }
                if (o->threads == 0) o->threads = 1;
                k = (int) strlen(a) - 1;
              }
              break;
            }
            default:
              fprintf(stderr, "-%c: unknown option\n", a[k]);
              return -1;
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

/*  ----------------------------------------------------------------------
    Analysis drivers.  The fused fold + analyse entry points
    (analyze_fold_<variant> / analyze_bits_fold_<variant>) case-fold
    each loaded SIMD vector in-register, so the threaded mmap path no
    longer needs a per-worker 32 KiB staging buffer or a separate
    memcpy + fold pre-pass.  */

#ifdef FASTENT_HAVE_PTHREAD
typedef struct {
  const u8 *     data;
  const u64 *    bounds;     /*  N+1 entries, multiples of 6 except last  */
  fastent_chunk_state * states;
  fastent_analyze_fn fn;     /*  Already specialised: fused or plain.  */
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

  /*  Partition into N slabs at 6-byte-aligned offsets so each slab's
      MC Pi state machine starts at mc_pos == 0 and there are no
      cross-slab hexads.  */
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

  mt_ctx ctx;
  ctx.data    = data;
  ctx.bounds  = bounds;
  ctx.states  = states;
  ctx.fn      = fn;
  fastent_parallel_for((sz) N, mt_worker, &ctx);

  /*  Merge per-thread states into `out`.  Slab 0 supplies first_byte;
      adjacent slab boundaries contribute b[end-1] * b[start_next] to
      the SCC cross-product.  The final wrap-around (last_byte *
      first_byte) is added in fastent_finalize().  In bit mode, the
      stored carry_byte / first_byte / last_byte are bit values (0 or
      1), so the same merge expression gives the cross-slab bit-pair
      contribution.  */
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
     /*  Stash trailing MC ring bytes from the LAST slab (others have
         none because they're 6-aligned).  */
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

/*  ----------------------------------------------------------------------
    Output formatters.  */

static void print_counts_default(const fastent_result * r, int binary) {
  const int bins = binary ? 2 : 256;
  printf("Value Char Occurrences Fraction\n");
  Fi(bins,
     if (r->hist[i] == 0) continue;
     char ch = fastent_is_displayable((unsigned) i) ? (char) i : ' ';
     printf("%3d   %c   %10llu   %f\n", i, ch,
            (unsigned long long) r->hist[i],
            (f64) r->hist[i] / (f64) r->total_samples))
  printf("\nTotal:    %10llu   %f\n\n",
         (unsigned long long) r->total_samples, 1.0);
}

static void print_counts_terse(const fastent_result * r, int binary) {
  const int bins = binary ? 2 : 256;
  printf("2,Value,Occurrences,Fraction\n");
  Fi(bins,
     printf("3,%d,%llu,%f\n", i,
            (unsigned long long) r->hist[i],
            (f64) r->hist[i] / (f64) r->total_samples))
}

static void print_default(const fastent_result * r, const fastent_options * o) {
  const char * samp = o->binary ? "bit" : "byte";
  const int fp = o->full_precision;

  if (o->counts) print_counts_default(r, o->binary);

  if (fp) printf("Entropy = %.17g bits per %s.\n", r->entropy, samp);
  else    printf("Entropy = %f bits per %s.\n",    r->entropy, samp);

  printf("\nOptimum compression would reduce the size\n");
  const f64 per = o->binary ? 1.0 : 8.0;
  const int comp_pct = (int)(short)(100.0 * (per - r->entropy) / per);
  printf("of this %llu %s file by %d percent.\n\n",
         (unsigned long long) r->total_samples, samp, comp_pct);

  if (fp) {
    printf("Chi square distribution for %llu samples is %.17g, and randomly\n",
           (unsigned long long) r->total_samples, r->chi_square);
  } else {
    printf("Chi square distribution for %llu samples is %1.2f, and randomly\n",
           (unsigned long long) r->total_samples, r->chi_square);
  }
  if      (r->chi_probability < 0.0001)
    printf("would exceed this value less than 0.01 percent of the times.\n\n");
  else if (r->chi_probability > 0.9999)
    printf("would exceed this value more than 99.99 percent of the times.\n\n");
  else if (fp)
    printf("would exceed this value %.17g percent of the times.\n\n",
           r->chi_probability * 100);
  else
    printf("would exceed this value %1.2f percent of the times.\n\n",
           r->chi_probability * 100);

  if (fp) {
    printf("Arithmetic mean value of data %ss is %.17g (%.17g = random).\n",
           samp, r->mean, o->binary ? 0.5 : 127.5);
  } else {
    printf("Arithmetic mean value of data %ss is %1.4f (%.1f = random).\n",
           samp, r->mean, o->binary ? 0.5 : 127.5);
  }
  if (fp) {
    printf("Monte Carlo value for Pi is %.17g (error %.17g percent).\n",
           r->monte_pi, 100.0 * (fabs(M_PI - r->monte_pi) / M_PI));
  } else {
    printf("Monte Carlo value for Pi is %1.9f (error %1.2f percent).\n",
           r->monte_pi, 100.0 * (fabs(M_PI - r->monte_pi) / M_PI));
  }
  printf("Serial correlation coefficient is ");
  if (r->scc >= -99999) {
    if (fp) printf("%.17g (totally uncorrelated = 0.0).\n", r->scc);
    else    printf("%1.6f (totally uncorrelated = 0.0).\n", r->scc);
  } else {
    printf("undefined (all values equal!).\n");
  }
}

static void print_terse(const fastent_result * r, const fastent_options * o) {
  printf("0,File-%ss,Entropy,Chi-square,Mean,Monte-Carlo-Pi,Serial-Correlation\n",
         o->binary ? "bit" : "byte");
  if (o->full_precision) {
    printf("1,%llu,%.17g,%.17g,%.17g,%.17g,%.17g\n",
           (unsigned long long) r->total_samples,
           r->entropy, r->chi_square, r->mean, r->monte_pi, r->scc);
  } else {
    printf("1,%llu,%f,%f,%f,%f,%f\n",
           (unsigned long long) r->total_samples,
           r->entropy, r->chi_square, r->mean, r->monte_pi, r->scc);
  }
  if (o->counts) print_counts_terse(r, o->binary);
}

static void print_json(const fastent_result * r, const fastent_options * o) {
  const char * samp = o->binary ? "bit" : "byte";
  const int fp = o->full_precision;
  const char * fmt_fp = fp ? "%.17g" : "%g";
  const f64 per = o->binary ? 1.0 : 8.0;
  const int comp_pct = (int)(short)(100.0 * (per - r->entropy) / per);

  printf("{\n");
  printf("  \"unit\": \"%s\",\n", samp);
  printf("  \"samples\": %llu,\n", (unsigned long long) r->total_samples);
  printf("  \"entropy\": "); printf(fmt_fp, r->entropy); printf(",\n");
  printf("  \"optimum_compression_percent\": %d,\n", comp_pct);
  printf("  \"chi_square\": {\n");
  printf("    \"statistic\": "); printf(fmt_fp, r->chi_square); printf(",\n");
  printf("    \"df\": %d,\n", o->binary ? 1 : 255);
  printf("    \"p_exceed\": "); printf(fmt_fp, r->chi_probability); printf("\n");
  printf("  },\n");
  printf("  \"arithmetic_mean\": "); printf(fmt_fp, r->mean); printf(",\n");
  printf("  \"monte_carlo_pi\": {\n");
  printf("    \"value\": "); printf(fmt_fp, r->monte_pi); printf(",\n");
  printf("    \"error_percent\": ");
  printf(fmt_fp, 100.0 * (fabs(M_PI - r->monte_pi) / M_PI));
  printf("\n  },\n");
  printf("  \"serial_correlation\": ");
  if (r->scc < -99999) printf("null");
  else                 printf(fmt_fp, r->scc);
  if (o->counts) {
    printf(",\n  \"occurrences\": [\n");
    const int bins = o->binary ? 2 : 256;
    int first = 1;
    Fi(bins,
       if (r->hist[i] == 0) continue;
       if (!first) printf(",\n");
       first = 0;
       printf("    { \"value\": %d, \"count\": %llu, \"fraction\": ",
              i, (unsigned long long) r->hist[i]);
       printf(fmt_fp, (f64) r->hist[i] / (f64) r->total_samples);
       printf(" }"))
    printf("\n  ]");
  }
  printf("\n}\n");
}

/*  ----------------------------------------------------------------------
    Main entry.  */

int main(int argc, char ** argv) {
  fastent_options o;
  int rc = parse_args(argc, argv, &o);
  if (rc != 0) return rc < 0 ? 1 : rc;

#ifdef _WIN32
  if (!o.path) _setmode(_fileno(stdin), _O_BINARY);
#endif

#ifdef FASTENT_HAVE_PTHREAD
  if (o.threads < 0) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    o.threads = n > 0 && n < 1024 ? (int) n : 1;
  }
  if (o.threads > 1) fastent_set_num_threads(o.threads);
#endif

  fastent_source src;
  if (fastent_src_open(&src, o.path, o.no_mmap) != 0) {
    if (o.path) fprintf(stderr, "cannot open file %s\n", o.path);
    else        perror("stdin");
    return 2;
  }

  /*  Pick best variant for each kernel: byte-mode analyse, bit-mode
      analyse, and the fused fold + analyse pair used when -f is set.
      Each picker confirms ISA support at runtime via
      __builtin_cpu_supports.  */
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

  if      (o.json)  print_json(&result, &o);
  else if (o.terse) print_terse(&result, &o);
  else              print_default(&result, &o);

  return 0;
}
