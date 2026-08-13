/*  fastent: command-line parsing + help/version.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "cli.h"
#include "port-io.h"

/*  Vendored verbatim from github.com/iczelia/yarg; keep it pristine
    and silence its GCC-only calloc-arg-order note here instead.  */
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpragmas"
#  pragma GCC diagnostic ignored "-Wcalloc-transposed-args"
#endif
#include "yarg.h"
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fastent_print_version(void) {
  printf("fastent %s\n", FASTENT_VERSION_STRING);
  fputs(
    "Copyright (C) 2023-2026 Kamila Szewczyk.\n"
    "License GPLv3: GNU GPL version 3 only"
    " <https://gnu.org/licenses/gpl-3.0.html>.\n"
    "This is free software: you are free to change and redistribute it.\n"
    "There is NO WARRANTY, to the extent permitted by law.\n",
    stdout);
}

void fastent_print_help(void) {
  fputs(
    "fastent: measure randomness of a byte (or bit) stream.\n"
    "Usage: fastent [options] [file]   (full detail in fastent(1))\n"
    "\n"
    "  -b, --bits            Treat input as a bit stream\n"
    "  -c, --counts          Print per-value occurrence counts\n"
    "  -f, --fold            Fold upper- to lower-case before counting\n"
    "  -t, --terse           CSV output\n"
    "  -J, --json            JSON output\n"
    "  -H, --histogram       Block bar plot of the distribution\n"
    "  -l, --log             Logarithmic y-axis for -H\n"
    "  -C, --color=MODE      Colour: auto, always, never\n"
    "  -p, --full-precision  Render every float at %.17g\n"
    "  -e, --extended        Extended stats: min-entropy, collision/IC,"
                                                            " poker,\n"
    "                        variance, distinct, per-bit bias, LZ77F"
                                                       " estimator,\n"
    "                        Bandt-Pompe permutation entropy. Repeat:"
                                                          " -ee adds\n"
    "                        order-1 bigram H(cur|prev)+I(prev;cur);"
                                                       " -eee adds the\n"
    "                        linear-complexity, Maurer universal and"
                                                       "\n"
    "                        binary matrix-rank estimators (with -H,"
                                                       " the matching\n"
    "                        plots)\n"
    "  -a, --annotate        Interpretive pass/fail report (implies -e)\n"
    "  -i, --io=MODE         Input: auto (default), mmap, stream, uring\n",
    stdout);
#ifdef FASTENT_HAVE_THREADS
  fputs(
    "  -j N, --threads=N     Use N worker threads (default 1)\n", stdout);
#endif
  fputs(
    "  -r, --recursive       Treat arg as a directory; one row per file\n"
    "  --sort-by=COL[:dir]   Sort -r output (dir = asc|desc). COL: path"
                                                           " samples\n"
    "                        entropy chisq mean pi scc min-entropy"
                                                       " collision ic\n"
    "                        poker variance redundancy distinct bitbias"
                                                       " cond-entropy\n"
    "                        mutual-info lz-deviation lz-cr lz-match-cov"
                                                       " bm-deviation\n"
    "                        bm-mean-lc maurer-deviation mrank-dev"
                                                       " perment-dev\n"
    "  --fips-140-2          FIPS 140-2 RNG power-up self-tests"
                                                  " (exit 1 on fail)\n"
    "  -V, --version         Print version and exit\n"
    "  -h, --help            Print this message\n",
    stdout);
}

static int parse_int(const char * s, int * out) {
  if (!s || !*s) return -1;
  if (!strcmp(s, "auto")) {
    *out = -1;
    return 0;
  }
  char * end = NULL;
  i64 v = strtol(s, &end, 10);
  if (end == s || (end && *end)) return -1;
  if (v < 0 || v > 1024) return -1;
  *out = (int) v;
  return 0;
}

static int parse_sort_by_(const char * arg, fastent_options * o) {
  char buf[64];
  sz n = strlen(arg);
  if (n >= sizeof buf) return -1;
  memcpy(buf, arg, n + 1);
  char * colon = strchr(buf, ':');
  if (colon) {
    *colon = '\0';
    const char * dir = colon + 1;
    if      (!strcmp(dir, "asc"))  o->sort_desc = 0;
    else if (!strcmp(dir, "desc")) o->sort_desc = 1;
    else {
      fprintf(stderr, "--sort-by direction must be asc or desc\n");
      return -1;
    }
  } else {
    o->sort_desc = -1;  /*  fill in column default below  */
  }
  fastent_sort_by col;
  if      (!strcmp(buf, "path"))    col = FASTENT_SORT_PATH;
  else if (!strcmp(buf, "samples")) col = FASTENT_SORT_SAMPLES;
  else if (!strcmp(buf, "entropy")) col = FASTENT_SORT_ENTROPY;
  else if (!strcmp(buf, "chisq"))   col = FASTENT_SORT_CHI_SQUARE;
  else if (!strcmp(buf, "mean"))    col = FASTENT_SORT_MEAN;
  else if (!strcmp(buf, "pi"))      col = FASTENT_SORT_MONTE_PI;
  else if (!strcmp(buf, "scc"))     col = FASTENT_SORT_SCC;
  else if (!strcmp(buf, "min-entropy")) col = FASTENT_SORT_MIN_ENTROPY;
  else if (!strcmp(buf, "collision"))   col = FASTENT_SORT_COLLISION;
  else if (!strcmp(buf, "ic"))          col = FASTENT_SORT_IC;
  else if (!strcmp(buf, "poker"))       col = FASTENT_SORT_POKER;
  else if (!strcmp(buf, "variance"))    col = FASTENT_SORT_VARIANCE;
  else if (!strcmp(buf, "redundancy"))  col = FASTENT_SORT_REDUNDANCY;
  else if (!strcmp(buf, "distinct"))    col = FASTENT_SORT_DISTINCT;
  else if (!strcmp(buf, "bitbias"))     col = FASTENT_SORT_BIT_BIAS;
  else if (!strcmp(buf, "cond-entropy")) col = FASTENT_SORT_COND_ENTROPY;
  else if (!strcmp(buf, "mutual-info"))  col = FASTENT_SORT_MUTUAL_INFO;
  else if (!strcmp(buf, "lz-deviation")) col = FASTENT_SORT_LZ_DEVIATION;
  else if (!strcmp(buf, "lz-cr"))        col = FASTENT_SORT_LZ_CR;
  else if (!strcmp(buf, "lz-match-cov")) col = FASTENT_SORT_LZ_MATCH_COV;
  else if (!strcmp(buf, "bm-deviation")) col = FASTENT_SORT_BM_DEVIATION;
  else if (!strcmp(buf, "bm-mean-lc"))   col = FASTENT_SORT_BM_MEAN_LC;
  else if (!strcmp(buf, "maurer-deviation"))
                                         col = FASTENT_SORT_MAURER_DEVIATION;
  else if (!strcmp(buf, "mrank-dev"))    col = FASTENT_SORT_MRANK_DEV;
  else if (!strcmp(buf, "perment-dev"))  col = FASTENT_SORT_PERMENT_DEV;
  else {
    fprintf(stderr, "--sort-by column must be one of: path samples entropy "
                    "chisq mean pi scc min-entropy collision ic poker "
                    "variance redundancy distinct bitbias cond-entropy "
                    "mutual-info lz-deviation lz-cr lz-match-cov "
                    "bm-deviation bm-mean-lc maurer-deviation "
                    "mrank-dev perment-dev\n");
    return -1;
  }
  o->sort_by = (int) col;
  /*  Sorting by an extended column implies it is emitted: LZ77F / BM /
      Maurer / mrank / perment columns need -eee (level 3), the bigram
      columns need -ee (level 2), the basic extended columns -e.  */
  if (col >= FASTENT_SORT_LZ_DEVIATION) {
    if (o->extended < 3) o->extended = 3;
  } else if (col >= FASTENT_SORT_COND_ENTROPY) {
    if (o->extended < 2) o->extended = 2;
  } else if (col >= FASTENT_SORT_MIN_ENTROPY) {
    if (o->extended < 1) o->extended = 1;
  }
  if (o->sort_desc == -1) o->sort_desc = (col == FASTENT_SORT_PATH) ? 0 : 1;
  return 0;
}

/*  Long-only options get a synthetic opt code (> any short letter).  */
enum { OPT_SORT_BY = 256, OPT_FIPS140 };

int fastent_parse_args(int argc, char ** argv, fastent_options * o) {
  static yarg_options opts[] = {
    { 'b',         no_argument,       "bits"           },
    { 'c',         no_argument,       "counts"         },
    { 'f',         no_argument,       "fold"           },
    { 't',         no_argument,       "terse"          },
    { 'J',         no_argument,       "json"           },
    { 'p',         no_argument,       "full-precision" },
    { 'H',         no_argument,       "histogram"      },
    { 'l',         no_argument,       "log"            },
    { 'C',         required_argument, "color"          },
    { 'e',         no_argument,       "extended"       },
    { 'a',         no_argument,       "annotate"       },
    { 'r',         no_argument,       "recursive"      },
    { 'i',         required_argument, "io"             },
#ifdef FASTENT_HAVE_THREADS
    { 'j',         required_argument, "threads"        },
#endif
    { OPT_SORT_BY,  required_argument, "sort-by"        },
    { OPT_FIPS140,  no_argument,       "fips-140-2"     },
    { 'h',         no_argument,       "help"           },
    { 'V',         no_argument,       "version"        },
    { 0,           no_argument,       NULL             }
  };

  memset(o, 0, sizeof(*o));
  o->threads = 1;
  o->color   = 1;

  yarg_settings st;
  st.dash_dash = true;
  st.style     = YARG_STYLE_UNIX;
  yarg_result * r = yarg_parse(argc, argv, opts, st);
  if (!r) {
    fprintf(stderr, "fastent: out of memory parsing arguments\n");
    return -1;
  }
  if (r->error) {
    fprintf(stderr, "fastent: %s", r->error);   /*  error ends in \n  */
    yarg_destroy(r);
    return -1;
  }

  /*  Help / version win over everything and exit immediately.  */
  Fk(r->argc,
     if (r->args[k].opt == 'h') {
       fastent_print_help();  yarg_destroy(r);  exit(0);
     }
     if (r->args[k].opt == 'V') {
       fastent_print_version();  yarg_destroy(r);  exit(0);
     })

  int rc = 0;
  for (i32 k = 0; k < r->argc && rc == 0; k++) {
    const yarg_option * a = &r->args[k];
    const char * v = a->arg;
    switch (a->opt) {
      case 'b': o->binary = 1; break;
      case 'c': o->counts = 1; break;
      case 'f': o->fold = 1; break;
      case 't': o->terse = 1; break;
      case 'J': o->json = 1; break;
      case 'p': o->full_precision = 1; break;
      case 'H': o->histogram = 1; break;
      case 'l': o->histogram_log = 1; break;
      /*  -e level (repeatable; -ee adds the order-1 bigram).  */
      case 'e': o->extended++; break;
      case 'a': o->annotate = 1; if (o->extended < 1) o->extended = 1; break;
      case 'r': o->recursive = 1; break;
      case 'C':
        if      (v && !strcmp(v, "auto"))   o->color = 1;
        else if (v && !strcmp(v, "always")) o->color = 2;
        else if (v && !strcmp(v, "never"))  o->color = 0;
        else { fprintf(stderr,
                 "fastent: --color must be auto, always or never\n");
               rc = -1; }
        break;
      case 'i':
        if      (v && !strcmp(v, "auto"))   o->io_mode = (int) FASTENT_IO_AUTO;
        else if (v && !strcmp(v, "mmap"))   o->io_mode = (int) FASTENT_IO_MMAP;
        else if (v && !strcmp(v, "stream"))
          o->io_mode = (int) FASTENT_IO_STREAM;
        else if (v && !strcmp(v, "uring"))  o->io_mode = (int) FASTENT_IO_URING;
        else { fprintf(stderr,
                 "fastent: --io must be auto, mmap, stream or uring\n");
               rc = -1; }
        break;
#ifdef FASTENT_HAVE_THREADS
      case 'j':
        if (!v || parse_int(v, &o->threads) != 0) {
          fprintf(stderr, "fastent: --threads: invalid count '%s'\n",
                  v ? v : "");
          rc = -1;
        } else if (o->threads == 0) {
          o->threads = 1;
        }
        break;
#endif
      case OPT_SORT_BY:
        if (!v || parse_sort_by_(v, o) != 0) rc = -1;
        break;
      case OPT_FIPS140: o->fips140 = 1; break;
      default: break;  /*  unreachable: yarg only yields known opts  */
    }
  }

  if (rc == 0 && r->pos_argc > 1) {
    fprintf(stderr, "fastent: duplicate file name: %s\n", r->pos_args[1]);
    rc = -1;
  }
  if (rc == 0 && r->pos_argc == 1 && strcmp(r->pos_args[0], "-")) {
    /*  Copy out of the result; owned for the process lifetime.
        A lone "-" is left as a NULL path, i.e. read stdin.  */
    char * p = yarg_strdup(r->pos_args[0]);
    if (!p) { fprintf(stderr, "fastent: out of memory\n"); rc = -1; }
    else o->path = p;
  }

  yarg_destroy(r);
  if (rc != 0) return -1;

  if (o->annotate && o->recursive) {
    fprintf(stderr, "fastent: --annotate is not supported with -r "
                    "(recursive output is CSV/JSON)\n");
    return -1;
  }
  if (o->fips140 && o->recursive) {
    fprintf(stderr, "fastent: --fips-140-2 is not supported with -r\n");
    return -1;
  }
  return 0;
}
