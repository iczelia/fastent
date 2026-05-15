/*  fastent: recursive-mode output (CSV + JSON).

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "output.h"

#include <stdio.h>

static void csv_escape_(const char * s) {
  /*  Quote only if necessary, doubling embedded quotes per RFC 4180.  */
  int need_quote = 0;
  const char * p;
  for (p = s; *p; p++) {
    if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
      need_quote = 1;
      break;
    }
  }
  if (!need_quote) { fputs(s, stdout); return; }
  putchar('"');
  for (p = s; *p; p++) {
    if (*p == '"') putchar('"');
    putchar(*p);
  }
  putchar('"');
}

void fastent_print_recursive_csv(const fastent_recursive_row * rows, sz n,
                                 const fastent_options * o) {
  const int fp = o->full_precision;
  const char * f = fp ? "%.17g" : "%g";
  fputs("path,unit,samples,entropy,chi_square,p_exceed,"
        "mean,monte_carlo_pi,serial_correlation\n", stdout);
  for (sz i = 0; i < n; i++) {
    const fastent_recursive_row * r = &rows[i];
    csv_escape_(r->path);
    printf(",%s,%llu,", o->binary ? "bit" : "byte",
           (unsigned long long) r->result.total_samples);
    printf(f, r->result.entropy);     putchar(',');
    printf(f, r->result.chi_square);  putchar(',');
    printf(f, r->result.chi_probability); putchar(',');
    printf(f, r->result.mean);        putchar(',');
    printf(f, r->result.monte_pi);    putchar(',');
    if (r->result.scc < -99999) fputs("nan", stdout);
    else                        printf(f, r->result.scc);
    putchar('\n');
  }
}

static void json_escape_(const char * s) {
  putchar('"');
  for (; *s; s++) {
    unsigned char c = (unsigned char) *s;
    switch (c) {
      case '"':  fputs("\\\"", stdout); break;
      case '\\': fputs("\\\\", stdout); break;
      case '\b': fputs("\\b",  stdout); break;
      case '\f': fputs("\\f",  stdout); break;
      case '\n': fputs("\\n",  stdout); break;
      case '\r': fputs("\\r",  stdout); break;
      case '\t': fputs("\\t",  stdout); break;
      default:
        if (c < 0x20) printf("\\u%04x", c);
        else          putchar((int) c);
    }
  }
  putchar('"');
}

static void json_num_(const char * fmt, double v) {
  if (!(v == v)) { fputs("null", stdout); return; }  /*  NaN -> null  */
  printf(fmt, v);
}

void fastent_print_recursive_json(const fastent_recursive_row * rows, sz n,
                                  const fastent_options * o) {
  const int fp = o->full_precision;
  const char * f = fp ? "%.17g" : "%g";
  printf("{\n  \"unit\": \"%s\",\n  \"files\": [\n",
         o->binary ? "bit" : "byte");
  for (sz i = 0; i < n; i++) {
    const fastent_recursive_row * r = &rows[i];
    fputs("    { \"path\": ", stdout);
    json_escape_(r->path);
    printf(", \"samples\": %llu",
           (unsigned long long) r->result.total_samples);
    fputs(", \"entropy\": ", stdout);        json_num_(f, r->result.entropy);
    fputs(", \"chi_square\": ", stdout);     json_num_(f, r->result.chi_square);
    fputs(", \"p_exceed\": ", stdout);       json_num_(f, r->result.chi_probability);
    fputs(", \"mean\": ", stdout);           json_num_(f, r->result.mean);
    fputs(", \"monte_carlo_pi\": ", stdout); json_num_(f, r->result.monte_pi);
    fputs(", \"serial_correlation\": ", stdout);
    if (r->result.scc < -99999) fputs("null", stdout);
    else                        json_num_(f, r->result.scc);
    fputs(" }", stdout);
    if (i + 1 < n) putchar(',');
    putchar('\n');
  }
  fputs("  ]\n}\n", stdout);
}
