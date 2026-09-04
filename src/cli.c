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
#include "cli.h"
#include "port-io.h"

/*  Vendored verbatim from github.com/iczelia/yarg; keep it pristine
    and silence its GCC-only calloc-arg-order note here instead.  */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wcalloc-transposed-args"
#endif
#include "yarg.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void banner(FILE * to) {
  fprintf(to,
    "fastent " FASTENT_VERSION_STRING " -- fast entropy and randomness tester\n"
    "Written by Kamila Szewczyk <k@iczelia.net>.\n"
    "License GNU GPL version 3.\n");
}

void fastent_print_version(void) {
  banner(stdout);
  printf("Copyright (C) 2023-2026 Kamila Szewczyk.\n"
         "This is free software: you are free to change and redistribute it.\n"
         "There is NO WARRANTY, to the extent permitted by law.\n");
}

void fastent_print_help(void) {
  banner(stdout);
  fprintf(stdout,
    "\n"
    "usage: fastent [OPTIONS] [FILE]\n"
    "       fastent -r [OPTIONS] DIRECTORY\n"
    "       fastent -h | -V\n"
    "\n"
    "  -b, --bits            analyze bits\n"
    "  -c, --counts          include occurrence counts\n"
    "  -f, --fold            fold uppercase before analysis\n"
    "  -t, --terse           write CSV\n"
    "  -J, --json            write JSON\n"
    "  -e, --extended        add tests; repeat through -eee\n"
    "  -a, --annotate        explain verdicts; implies -e\n"
    "  -p, --full-precision  print round-trippable floats\n"
    "  -H, --histogram       draw distributions\n"
    "  -l, --log             use a logarithmic histogram\n"
    "  -C, --color=MODE      auto, always, or never\n"
    "  -i, --io=MODE         auto, mmap, stream, or uring\n");
#ifdef FASTENT_HAVE_THREADS
  fprintf(stdout, "  -j N, --threads=N     use N workers, or auto\n");
#endif
  fprintf(stdout,
    "  -r, --recursive       analyze each file below a directory\n"
    "  --sort-by=COL[:DIR]   sort recursive output\n"
    "  --fips-140-2          run FIPS 140-2 power-up tests\n"
    "  -h, --help            show help\n"
    "  -V, --version         show version\n"
    "\n"
    "Exit codes  0 success, 1 usage or failed test, 2 system error.\n");
}

static int parse_int(const char * s, int * out) {
  char * end;
  i64 v;
  if (!s || !*s) return -1;
  if (!strcmp(s, "auto")) { *out = -1;  return 0; }
  end = NULL;
  v = strtol(s, &end, 10);
  if (end == s || (end && *end)) return -1;
  if (v < 0 || v > 1024) return -1;
  *out = (int) v;
  return 0;
}

static int parse_sort_by_(const char * arg, fastent_options * o) {
  char buf[64];
  char * colon;
  const char * dir;
  fastent_sort_by col;
  sz n;
  n = strlen(arg);
  if (n >= sizeof buf) return -1;
  memcpy(buf, arg, n + 1);
  colon = strchr(buf, ':');
  if (colon) {
    *colon = '\0';
    dir = colon + 1;
    if      (!strcmp(dir, "asc"))  o->sort_desc = 0;
    else if (!strcmp(dir, "desc")) o->sort_desc = 1;
    else {
      fastent_message("--sort-by direction must be asc or desc");
      return -1;
    }
  } else o->sort_desc = -1;
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
    fastent_message("unknown --sort-by column '%s'", buf);
    return -1;
  }
  o->sort_by = (int) col;
  /*  Sorting enables the selected field.  */
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
  yarg_settings st;
  yarg_result * r;
  int rc;
  i32 k;

  memset(o, 0, sizeof (*o));
  o->threads = 1;
  o->color   = 1;

  st.dash_dash = true;
  st.style     = YARG_STYLE_UNIX;
  r = yarg_parse(argc, argv, opts, st);
  if (!r) {
    fastent_message("out of memory");
    return -1;
  }
  if (r->error) {
    fprintf(stderr, "fastent: %s", r->error);   /*  error ends in \n  */
    yarg_destroy(r);
    return -1;
  }

  /*  Help and version take precedence.  */
  Fk(r->argc,
    if (r->args[k].opt == 'h') { fastent_print_help();  yarg_destroy(r);  exit(0); }
    if (r->args[k].opt == 'V') { fastent_print_version();  yarg_destroy(r);  exit(0); });

  rc = 0;
  for (k = 0; k < r->argc && rc == 0; k++) {
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
      case 'e': o->extended++; break;
      case 'a': o->annotate = 1; if (o->extended < 1) o->extended = 1; break;
      case 'r': o->recursive = 1; break;
      case 'C':
        if      (v && !strcmp(v, "auto"))   o->color = 1;
        else if (v && !strcmp(v, "always")) o->color = 2;
        else if (v && !strcmp(v, "never"))  o->color = 0;
        else { fastent_message("--color must be auto, always or never");
               rc = -1; }
        break;
      case 'i':
        if      (v && !strcmp(v, "auto"))   o->io_mode = (int) FASTENT_IO_AUTO;
        else if (v && !strcmp(v, "mmap"))   o->io_mode = (int) FASTENT_IO_MMAP;
        else if (v && !strcmp(v, "stream"))
          o->io_mode = (int) FASTENT_IO_STREAM;
        else if (v && !strcmp(v, "uring"))  o->io_mode = (int) FASTENT_IO_URING;
        else { fastent_message("--io must be auto, mmap, stream or uring");
               rc = -1; }
        break;
#ifdef FASTENT_HAVE_THREADS
      case 'j':
        if (!v || parse_int(v, &o->threads) != 0) {
          fastent_message("invalid thread count '%s'", v ? v : "");
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
      default: break;
    }
  }

  if (rc == 0 && r->pos_argc > 1) {
    fastent_message("too many input paths: %s", r->pos_args[1]);
    rc = -1;
  }
  if (rc == 0 && r->pos_argc == 1 && strcmp(r->pos_args[0], "-")) {
    /*  A lone dash means standard input.  */
    char * p = yarg_strdup(r->pos_args[0]);
    if (!p) { fastent_message("out of memory");  rc = -1; }
    else o->path = p;
  }

  yarg_destroy(r);
  if (rc != 0) return -1;

  if (o->annotate && o->recursive) {
    fastent_message("--annotate cannot be used with -r");
    return -1;
  }
  if (o->fips140 && o->recursive) {
    fastent_message("--fips-140-2 cannot be used with -r");
    return -1;
  }
  return 0;
}
