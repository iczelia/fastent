/*  fastent: command-line parsing + help/version.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "cli.h"
#include "port-io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fastent_print_version(void) {
  printf("fastent %s\n", FASTENT_VERSION_STRING);
  printf("Copyright (C) 2026 Kamila Szewczyk.\n");
  printf("License GPLv3: GNU GPL version 3 only"
         " <https://gnu.org/licenses/gpl-3.0.html>.\n");
  printf("This is free software: you are free to change and redistribute it.\n");
  printf("There is NO WARRANTY, to the extent permitted by law.\n");
}

void fastent_print_help(void) {
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
  printf("                --io=MODE            auto (default), mmap, stream,\n");
  printf("                                     uring (Linux io_uring / Win32 IOCP)\n");
  printf("                --json               Emit results as JSON\n");
  printf("           -e,  --extended           Also report min-entropy,\n");
  printf("                                     collision entropy / IC, poker\n");
  printf("                                     test, variance, distinct, etc.\n");
  printf("                --annotate            Interpretive pass/fail report\n");
  printf("                                     (implies --extended)\n");
  printf("           -r,  --recursive          Treat the positional arg as a\n");
  printf("                                     directory; emit one row per file\n");
  printf("                --sort-by=COL[:dir]  Sort recursive output by COL:\n");
  printf("                                     path, samples, entropy, chisq,\n");
  printf("                                     mean, pi, scc, min-entropy,\n");
  printf("                                     collision, ic, poker, variance,\n");
  printf("                                     redundancy, distinct.\n");
  printf("                                     dir = asc | desc\n");
  printf("           -V,  --version            Print version and exit\n");
  printf("           -h,  --help               Print this message\n");
}

static int parse_int(const char * s, int * out) {
  if (!s || !*s) return -1;
  if (!strcmp(s, "auto")) {
    *out = -1;
    return 0;
  }
  char * end = NULL;
  long v = strtol(s, &end, 10);
  if (end == s || (end && *end)) return -1;
  if (v < 0 || v > 1024) return -1;
  *out = (int) v;
  return 0;
}

static int parse_sort_by_(const char * arg, fastent_options * o) {
  char buf[64];
  size_t n = strlen(arg);
  if (n >= sizeof buf) return -1;
  memcpy(buf, arg, n + 1);
  char * colon = strchr(buf, ':');
  if (colon) {
    *colon = '\0';
    const char * dir = colon + 1;
    if      (!strcmp(dir, "asc"))  o->sort_desc = 0;
    else if (!strcmp(dir, "desc")) o->sort_desc = 1;
    else { fprintf(stderr, "--sort-by direction must be asc or desc\n"); return -1; }
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
  else {
    fprintf(stderr, "--sort-by column must be one of: path samples entropy "
                    "chisq mean pi scc min-entropy collision ic poker "
                    "variance redundancy distinct\n");
    return -1;
  }
  o->sort_by = (int) col;
  /*  Sorting by an extended column implies the columns are emitted.  */
  if (col >= FASTENT_SORT_MIN_ENTROPY) o->extended = 1;
  if (o->sort_desc == -1) o->sort_desc = (col == FASTENT_SORT_PATH) ? 0 : 1;
  return 0;
}

int fastent_parse_args(int argc, char ** argv, fastent_options * o) {
  int i;
  int saw_path = 0;
  memset(o, 0, sizeof(*o));
  o->threads = 1;
  o->color   = 1;

  for (i = 1; i < argc; i++) {
    const char * a = argv[i];
    if (a[0] == '-' && a[1] != '\0') {
      if (a[1] == '-') {
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
        else if (LONG_IS("recursive"))      o->recursive = 1;
        else if (LONG_IS("extended"))       o->extended = 1;
        else if (LONG_IS("annotate"))     { o->annotate = 1; o->extended = 1; }
        else if (LONG_IS("sort-by")) {
          if (!val) {
            if (i + 1 >= argc) { fprintf(stderr, "--sort-by requires an argument\n"); return -1; }
            val = argv[++i];
          }
          if (parse_sort_by_(val, o) != 0) return -1;
        }
        else if (LONG_IS("color")) {
          if (!val) o->color = 2;
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
        else if (LONG_IS("help"))         { fastent_print_help(); exit(0); }
        else if (LONG_IS("version"))      { fastent_print_version(); exit(0); }
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
            case 'r': o->recursive = 1; break;
            case 'e': o->extended  = 1; break;
            case '?':
            case 'h': fastent_print_help();    exit(0);
            case 'V': fastent_print_version(); exit(0);
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
              k = (int) strlen(a) - 1;
              break;
            }
            default: fprintf(stderr, "-%c: unknown option\n", a[k]);  return -1;
          }
        }
      }
    } else {
      if (saw_path) {
        fprintf(stderr, "duplicate file name: %s\n", a);
        return -1;
      }
      o->path = a;
      saw_path = 1;
    }
  }
  if (o->annotate && o->recursive) {
    fprintf(stderr, "fastent: --annotate is not supported with -r "
                    "(recursive output is CSV/JSON)\n");
    return -1;
  }
  return 0;
}
