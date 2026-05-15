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
#include <unistd.h>  /*  sysconf  */

#if !defined(_WIN32) && !defined(__DJGPP__)
  #include <sys/ioctl.h>  /*  TIOCGWINSZ  */
#endif

#ifdef __DJGPP__
  #include <conio.h>     /*  textcolor / cputs for BIOS-attribute output  */
#endif

#ifdef _WIN32
  #include "fastent-win32.h"
#endif

#include "analyze.h"
#include "fastent-io.h"
#ifdef FASTENT_HAVE_PTHREAD
  #include "threadpool.h"
#endif

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

/*  Printable, non-whitespace bytes in C0/Latin-1.  */
static inline int fastent_is_displayable(unsigned c) {
  return (c >= 0x21u && c <= 0x7Eu) || c >= 0xA1u;
}

/*  Options.  */

typedef struct {
  int binary;         /*  -b  */
  int counts;         /*  -c  */
  int fold;           /*  -f  */
  int terse;          /*  -t  */
  int json;           /*  --json  */
  int full_precision; /*  -p / --full-precision  */
  int no_mmap;        /*  --no-mmap (deprecated alias for --io=stream)  */
  int io_mode;        /*  --io={auto,mmap,stream,uring}  */
  int histogram;      /*  -H / --histogram  */
  int histogram_log;  /*  --log  (log-y for the histogram)  */
  int color;          /*  --color={auto,always,never}, 0=never 1=auto 2=always  */
  int threads;        /*  -j / --threads  (0 = auto, 1 = default)  */
  const char * path;  /*  positional (NULL = stdin)  */
} fastent_options;

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

static int color_active(int color_opt) {
  if (color_opt == 0) return 0;
  if (color_opt == 2) return 1;
  if (getenv("NO_COLOR")) return 0;
#if defined(FASTENT_WIN_LEGACY)
  return 0;
#else
  return isatty(1);
#endif
}

typedef enum { GLYPHS_UNICODE = 0, GLYPHS_CP437 = 1, GLYPHS_ASCII = 2 } glyph_mode;

static glyph_mode pick_glyphs(void) {
#if defined(__DJGPP__) || defined(FASTENT_WIN_LEGACY)
  return isatty(1) ? GLYPHS_CP437 : GLYPHS_ASCII;
#else
  return GLYPHS_UNICODE;
#endif
}

static int term_width(void) {
#if defined(__DJGPP__) || defined(FASTENT_WIN_LEGACY) || defined(_WIN32)
  return 80;
#elif defined(TIOCGWINSZ)
  struct winsize w;
  if (isatty(1) && ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
    return (int) w.ws_col;
  return 80;
#else
  return 80;
#endif
}

static void print_histogram(const fastent_result * r, const fastent_options * o) {
  const int bins = o->binary ? 2 : 256;
  static const char * const glyphs_unicode[9] = {
    " ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83",
         "\xe2\x96\x84", "\xe2\x96\x85", "\xe2\x96\x86",
         "\xe2\x96\x87", "\xe2\x96\x88"
  };
  static const char * const glyphs_cp437[3] = { " ", "\xdc", "\xdb" };
  static const char * const glyphs_ascii[2] = { " ", "#" };
  const glyph_mode gmode = pick_glyphs();
  const char * const * blocks;
  int sub;
  switch (gmode) {
    case GLYPHS_CP437:   blocks = glyphs_cp437;   sub = 2; break;
    case GLYPHS_ASCII:   blocks = glyphs_ascii;   sub = 1; break;
    case GLYPHS_UNICODE:
    default:             blocks = glyphs_unicode; sub = 8; break;
  }
  const int height = 8;
  const int levels = height * sub;

  /*  Downsample 256 bins to the smallest power-of-2 group that fits.  */
  int group = 1;
  if (!o->binary) {
    int w = term_width();
    if (w < 1) w = 80;
    while (bins / group > w && group < bins) group <<= 1;
  }
  const int cols = bins / group;

  u64 grouped[256];
  int c;
  for (c = 0; c < cols; c++) {
    u64 sum = 0;
    int j;
    for (j = 0; j < group; j++) sum += r->hist[c * group + j];
    grouped[c] = sum;
  }

  u64 max = 0;
  for (c = 0; c < cols; c++) if (grouped[c] > max) max = grouped[c];
  if (max == 0) {
    printf("(histogram: no samples)\n\n");
    return;
  }

  const int use_color = color_active(o->color);

  int row;
  for (row = 0; row < height; row++) {
    const int row_bot = (height - 1 - row) * sub;
    const int row_top = row_bot + sub;
    int last_class = -1;
    const double log_denom = o->histogram_log
                             ? log((double) max + 1.0) : 0.0;
    for (c = 0; c < cols; c++) {
      double frac;
      if (o->histogram_log) {
        frac = grouped[c] == 0 ? 0.0
             : log((double) grouped[c] + 1.0) / log_denom;
      } else {
        frac = (double) grouped[c] / (double) max;
      }
      int hh = (int)(frac * (double) levels + 0.5);
      if (hh > levels) hh = levels;
      const char * glyph;
      if (hh >= row_top)      glyph = blocks[sub];
      else if (hh <= row_bot) glyph = blocks[0];
      else                    glyph = blocks[hh - row_bot];
      int first_byte = c * group;
      int cls = (first_byte < 32 || first_byte == 127) ? 0
              : (first_byte < 128 ? 1 : 2);
      if (use_color && cls != last_class) {
#if defined(__DJGPP__)
        static const int dos_pal[3] = { DARKGRAY, LIGHTGRAY, LIGHTCYAN };
        fflush(stdout);
        textcolor(dos_pal[cls]);
#elif defined(_WIN32)
        fastent_win32_set_console_fg(cls);
#else
        static const char * const ansi[3] = {
          "\x1b[2m", "\x1b[0m", "\x1b[36m"
        };
        fputs(ansi[cls], stdout);
#endif
        last_class = cls;
      }
#if defined(__DJGPP__)
      if (use_color) cputs(glyph);
      else           fputs(glyph, stdout);
#else
      fputs(glyph, stdout);
#endif
    }
    if (use_color) {
#if defined(__DJGPP__)
      fflush(stdout);
      textcolor(LIGHTGRAY);
#elif defined(_WIN32)
      fastent_win32_set_console_fg(-1);
#else
      fputs("\x1b[0m", stdout);
#endif
    }
    putchar('\n');
  }
  if (bins == 256) {
    int tick_every = cols / 8;
    if (tick_every < 1) tick_every = 1;
    for (c = 0; c < cols; c++) putchar((c % tick_every == 0) ? '|' : '-');
    putchar('\n');
    for (c = 0; c < cols; c++) {
      if (c % tick_every == 0) {
        int label = c * group;
        char buf[8];
        int n = snprintf(buf, sizeof(buf), "%d", label);
        if (n > tick_every) n = tick_every;
        printf("%.*s", n, buf);
        int rest = tick_every - n;
        while (rest-- > 0 && (c + 1) % tick_every != 0) {
          putchar(' ');
          break;
        }
        c += tick_every - 1;
      }
    }
    putchar('\n');
  } else {
    printf("0 1\n");
  }
  u64 raw_peak = 0;
  int peak_v   = 0;
  Fi(bins, if (r->hist[i] > raw_peak) { raw_peak = r->hist[i]; peak_v = i; })
  printf("(peak %llu sample%s at byte %d",
         (unsigned long long) raw_peak,
         raw_peak == 1 ? "" : "s",
         peak_v);
  if (!o->binary && fastent_is_displayable((unsigned) peak_v))
    printf(" '%c'", (char) peak_v);
  if (group > 1)         printf(", %d bytes/col", group);
  if (o->histogram_log)  printf(", log y");
  printf(")\n\n");
}

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

  if (o->counts)    print_counts_default(r, o->binary);
  if (o->histogram) print_histogram(r, o);

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

/*  Main entry.  */

int main(int argc, char ** argv) {
#ifdef _WIN32
  /*  Replace MSVCRT's CP_ACP-narrowed argv with UTF-8 from
      GetCommandLineW() and set the console to CP_UTF8.  */
  fastent_win32_init_console();
  if (fastent_win32_argv_utf8(&argc, &argv) != 0) {
    fprintf(stderr, "fastent: failed to decode Windows command line\n");
    return 2;
  }
#endif

  fastent_options o;
  int rc = parse_args(argc, argv, &o);
  if (rc != 0) return rc < 0 ? 1 : rc;

#ifdef _WIN32
  if (!o.path) fastent_win32_set_stdin_binary();
#endif

#ifdef HAVE_PLEDGE
  /*  rpath only needed to open the positional argument; the second
      pledge after src_open() drops it.  */
  if (pledge(o.path ? "stdio rpath" : "stdio", NULL) == -1) {
    perror("pledge");
    return 2;
  }
#endif

#ifdef FASTENT_HAVE_PTHREAD
  if (o.threads < 0) {
#ifdef _WIN32
    long n = fastent_win32_num_cpus();
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
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

#ifdef HAVE_PLEDGE
  if (o.path && pledge("stdio", NULL) == -1) {
    perror("pledge");
    return 2;
  }
#endif

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
