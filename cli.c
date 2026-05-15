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
  printf("                --io=MODE            mmap (default for regular files),\n");
  printf("                                     stream, uring (Linux 5.1+), auto\n");
  printf("                --json               Emit results as JSON\n");
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
  return 0;
}
