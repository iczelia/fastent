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
#include "output.h"
#include "port-term.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int color_active_(int color_opt) {
  if (color_opt == 0) return 0;
  if (color_opt == 2) return 1;
  if (getenv("NO_COLOR")) return 0;
  return fastent_term_isatty();
}

/*  Glyph set + vertical sub-steps for the active terminal.  */
static const char * const * hist_glyphs_(i32 * sub) {
  static const char * const glyphs_unicode[9] = {
    " ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83",
         "\xe2\x96\x84", "\xe2\x96\x85", "\xe2\x96\x86",
         "\xe2\x96\x87", "\xe2\x96\x88"
  };
  static const char * const glyphs_cp437[3] = { " ", "\xdc", "\xdb" };
  static const char * const glyphs_ascii[2] = { " ", "#" };
  switch (fastent_term_glyphs()) {
    case FASTENT_GLYPHS_CP437: *sub = 2;  return glyphs_cp437;
    case FASTENT_GLYPHS_ASCII: *sub = 1;  return glyphs_ascii;
    case FASTENT_GLYPHS_UNICODE:
    default:                   *sub = 8;  return glyphs_unicode;
  }
}

/*  Shared 8-row bar grid with the count gutter (byte/bit plot and the
    -eee log2 plots).  col_class(j) -> 0/1/2 colours a column, -1
    disables.  Returns the gutter width.  */
static i32 hist_bars_(
    const u64 * grouped, i32 cols, u64 max, int histogram_log,
    int use_color, i32 (*col_class)(i32, void *), void * cc_ctx) {
  i32 i, j, sub;
  const char * const * blocks = hist_glyphs_(&sub);
  const i32 height = 8;
  const i32 levels = height * sub;
  const f64 log_denom = histogram_log ? log((f64) max + 1.0) : 0.0;

  char gbuf[24];
  i32  gw = snprintf(gbuf, sizeof gbuf, "%" PRIu64, (u64) max);
  if (gw < 1) gw = 1;

  Fi(height,
    const i32 row_bot = (height - 1 - i) * sub;
    const i32 row_top = row_bot + sub;
    i32 last_class = -1;
    const f64 yf = (f64) (height - i) / (f64) height;
    const u64 yval = histogram_log
                     ? (u64) (exp(yf * log_denom) - 1.0 + 0.5)
                     : (u64) (yf * (f64) max + 0.5);
    printf("%*" PRIu64 " |", gw, (u64) yval);
    Fj(cols,
      f64 frac;
      i32 hh, cls;
      const char * glyph;
      if (histogram_log) {
        frac = grouped[j] == 0 ? 0.0
             : log((f64) grouped[j] + 1.0) / log_denom;
      } else {
        frac = (f64) grouped[j] / (f64) max;
      }
      hh = (i32) (frac * (f64) levels + 0.5);
      if (hh > levels) hh = levels;
      if (hh >= row_top)      glyph = blocks[sub];
      else if (hh <= row_bot) glyph = blocks[0];
      else                    glyph = blocks[hh - row_bot];
      if (use_color && col_class) {
        cls = col_class(j, cc_ctx);
        if (cls != last_class) { fastent_term_set_fg(cls);  last_class = cls; }
      }
      fastent_term_write(glyph));
    if (use_color && col_class) fastent_term_set_fg(-1);
    putchar('\n'));
  return gw;
}

typedef struct { i32 group; } byte_cc_;
static i32 byte_col_class_(i32 j, void * ctx) {
  i32 first_byte = j * ((byte_cc_ *) ctx)->group;
  return (first_byte < 32 || first_byte == 127) ? 0
       : (first_byte < 128 ? 1 : 2);
}

/*  Byte / bit order-0 distribution plot.  Behaviour unchanged.  */
static void hist_byte_(const fastent_result * r, const fastent_options * o) {
  i32 i, j, c, w, cols, tick_every, gw, peak_v = 0;
  const i32 bins = o->binary ? 2 : 256;
  i32 group = 1;
  u64 grouped[256], max = 0, raw_peak = 0;
  int use_color;
  byte_cc_ cc;
  if (!o->binary) {
    w = fastent_term_width();
    if (w < 1) w = 80;
    while (bins / group > w && group < bins) group <<= 1;
  }
  cols = bins / group;

  Fi(cols,
    u64 sum = 0;
    Fj(group, sum += r->hist[i * group + j]);
    grouped[i] = sum);

  Fi(cols, if (grouped[i] > max) max = grouped[i]);
  if (max == 0) {
    printf("(histogram: no samples)\n\n");
    return;
  }

  use_color = color_active_(o->color);  cc.group = group;
  gw = hist_bars_(grouped, cols, max, o->histogram_log, use_color,
                  byte_col_class_, &cc);

  if (bins == 256) {
    tick_every = cols / 8;
    if (tick_every < 1) tick_every = 1;
    Fi(gw + 1, putchar(' '));
    putchar('+');
    Fi(cols, putchar((i % tick_every == 0) ? '|' : '-'));
    putchar('\n');
    Fi(gw + 2, putchar(' '));
    for (c = 0; c < cols; c += tick_every) {
      char buf[8];
      i32 n = snprintf(buf, sizeof (buf), "%d", c * group);
      if (n > tick_every) n = tick_every;
      if (c + tick_every >= cols) { printf("%.*s", n, buf); } else {
        printf("%-*.*s", tick_every, n, buf);
      }
    }
    putchar('\n');
  } else {
    Fi(gw + 2, putchar(' '));
    printf("0 1\n");
  }
  Fi(bins, if (r->hist[i] > raw_peak) { raw_peak = r->hist[i]; peak_v = i; });
  printf("(peak %" PRIu64 " sample%s at byte %d",
         (u64) raw_peak,
         raw_peak == 1 ? "" : "s",
         peak_v);
  if (!o->binary && fastent_is_displayable((u32) peak_v))
    printf(" '%c'", (char) peak_v);
  if (group > 1)         printf(", %d bytes/col", group);
  if (o->histogram_log)  printf(", log y");
  printf(")\n\n");
}

/*  Integer log2 bucket of x >= 1: 0 for [1,2), 1 for [2,4), ...
    (libm-free; the bucketing itself is integer).  */
static i32 log2_bucket_(u64 x) {
  i32 b = 0;
  while (x > 1) { x >>= 1;  b++; }
  return b;
}

/*  Collapse src[lo..hi] into floor(log2(index)) buckets; fills bkt[]
    and *tot, returns the bucket count.  */
static i32 log2_collect_(
    const u64 * src, i32 lo, i32 hi, u64 * bkt, u64 * tot) {
  i32 nb = log2_bucket_((u64) hi) + 1;
  i32 i;
  u64 t = 0;
  if (nb < 1) nb = 1;
  if (nb > 24) nb = 24;
  Fi(nb, bkt[i] = 0);
  for (i = lo; i < hi + 1; i++) {
    i32 b;
    const u64 c = src[i];
    if (!c) continue;
    b = log2_bucket_((u64) i);
    if (b >= nb) b = nb - 1;
    bkt[b] += c;  t += c;
  }
  *tot = t;
  return nb;
}

/*  Print one 8-row glyph plot's row `rr` (0 = top), gutter width gw,
    no trailing newline, no colour.  Same bar math as hist_bars_.  */
static void log2_row_(
    const u64 * bkt, i32 nb, u64 max, int hlog,
    const char * const * blk, i32 sub, i32 gw, i32 rr) {
  const i32 height = 8, levels = height * sub;
  const f64 ld = hlog ? log((f64) max + 1.0) : 0.0;
  const i32 row_bot = (height - 1 - rr) * sub, row_top = row_bot + sub;
  const f64 yf = (f64) (height - rr) / (f64) height;
  const u64 yv = hlog ? (u64) (exp(yf * ld) - 1.0 + 0.5)
                      : (u64) (yf * (f64) max + 0.5);
  i32 i;
  printf("%*" PRIu64 " |", gw, yv);
  Fi(nb,
    f64 frac = hlog ? (bkt[i] == 0 ? 0.0
                       : log((f64) bkt[i] + 1.0) / ld)
                    : (f64) bkt[i] / (f64) max;
    i32 hh = (i32) (frac * (f64) levels + 0.5);
    if (hh > levels) hh = levels;
    fputs(hh >= row_top ? blk[sub]
        : hh <= row_bot ? blk[0] : blk[hh - row_bot], stdout));
}

/*  The two -eee log2 plots SIDE BY SIDE: offsets left, match lengths
    right.  X-axis labels run vertically (one digit per row, MSD on
    top) so each 2^k label is a single column under its tick.  */
static void hist_log2_pair_(
    const char * tA, const u64 * sA, i32 loA, i32 hiA,
    const char * tB, const u64 * sB, i32 loB, i32 hiB,
    const fastent_options * o) {
  u64 bA[24], bB[24], totA, totB, mA = 1, mB = 1;
  i32 i, j, sub, gwA, gwB, tAlen = 0, mlA, mlB, rows;
  i32 nbA, nbB, wA, bw, padA;
  const char * const * blk;
  const char * SEP = "   ";
  int hlog = o->histogram_log;
  char g[24], fb[48];
  nbA = log2_collect_(sA, loA, hiA, bA, &totA);
  nbB = log2_collect_(sB, loB, hiB, bB, &totB);
  Fi(nbA, if (bA[i] > mA) mA = bA[i]);
  Fi(nbB, if (bB[i] > mB) mB = bB[i]);
  gwA = snprintf(g, sizeof g, "%" PRIu64, mA);  if (gwA < 1) gwA = 1;
  gwB = snprintf(g, sizeof g, "%" PRIu64, mB);  if (gwB < 1) gwB = 1;
  blk = hist_glyphs_(&sub);
  wA = gwA + 2 + nbA;
  while (tA[tAlen]) tAlen++;
  bw = wA > tAlen ? wA : tAlen;  padA = bw - wA;
  snprintf(fb, sizeof fb, "(log2 buckets [2^k, 2^k+1)%s)",
           hlog ? ", log y" : "");

  printf("%s", tA);  Fi(bw - tAlen, putchar(' '));
  fputs(SEP, stdout);  printf("%s\n", tB);
  Fi(8,
    log2_row_(bA, nbA, mA, hlog, blk, sub, gwA, i);
    Fj(padA, putchar(' '));  fputs(SEP, stdout);
    log2_row_(bB, nbB, mB, hlog, blk, sub, gwB, i);
    putchar('\n'));

  Fi(gwA + 1, putchar(' '));  putchar('+');  Fi(nbA, putchar('|'));
  Fj(padA, putchar(' '));  fputs(SEP, stdout);
  Fi(gwB + 1, putchar(' '));  putchar('+');  Fi(nbB, putchar('|'));
  putchar('\n');

  /*  Vertical x-labels under each tick: digit of 2^k, MSD on top,
      bottom-justified within each plot's own width.  The footer
      rides the last (units) row of the offsets block.  */
  mlA = (i32) snprintf(g, sizeof g, "%d", 1 << (nbA - 1));
  mlB = (i32) snprintf(g, sizeof g, "%d", 1 << (nbB - 1));
  rows = mlA > mlB ? mlA : mlB;
  Fi(rows,
    i32 r = i;
    Fj(gwA + 2, putchar(' '));
    Fj(nbA,
      char lb[16];
      i32 ln = snprintf(lb, sizeof lb, "%d", 1 << j);
      i32 pos = r - (mlA - ln);
      putchar(r < mlA && pos >= 0 && pos < ln ? lb[pos] : ' '));
    if (r < mlB) {
      Fj(padA, putchar(' '));  fputs(SEP, stdout);
      Fj(gwB + 2, putchar(' '));
      Fj(nbB,
        char lb[16];
        i32 ln = snprintf(lb, sizeof lb, "%d", 1 << j);
        i32 pos = r - (mlB - ln);
        putchar(pos >= 0 && pos < ln ? lb[pos] : ' '));
    }
    if (r == rows - 1) printf(" %s", fb);
    putchar('\n'));
  putchar('\n');
}

/*  256-bin literal byte-value plot: same downsample / colour / tick
    machinery as the order-0 byte plot, fed by lit_byte.  */
static void hist_lit256_(
    const char * title, const u64 * lit, const fastent_options * o) {
  i32 i, j, c, group = 1, w, cols, gw, tick_every;
  u64 grouped[256], max = 0;
  int use_color;
  byte_cc_ cc;
  printf("%s\n", title);
  w = fastent_term_width();
  if (w < 1) w = 80;
  while (256 / group > w && group < 256) group <<= 1;
  cols = 256 / group;
  Fi(cols,
    u64 sum = 0;
    Fj(group, sum += lit[i * group + j]);
    grouped[i] = sum);
  Fi(cols, if (grouped[i] > max) max = grouped[i]);
  if (max == 0) { printf("(no samples)\n\n");  return; }

  use_color = color_active_(o->color);  cc.group = group;
  gw = hist_bars_(grouped, cols, max, o->histogram_log, use_color,
                  byte_col_class_, &cc);
  tick_every = cols / 8;
  if (tick_every < 1) tick_every = 1;
  Fi(gw + 1, putchar(' '));
  putchar('+');
  Fi(cols, putchar((i % tick_every == 0) ? '|' : '-'));
  putchar('\n');
  Fi(gw + 2, putchar(' '));
  for (c = 0; c < cols; c += tick_every) {
    char buf[8];
    i32 n = snprintf(buf, sizeof (buf), "%d", c * group);
    if (n > tick_every) n = tick_every;
    if (c + tick_every >= cols) printf("%.*s", n, buf);
    else                        printf("%-*.*s", tick_every, n, buf);
  }
  putchar('\n');
  if (group > 1)        printf("(%d bytes/col", group);
  else                  printf("(byte value");
  if (o->histogram_log) printf(", log y");
  printf(")\n\n");
}

/*  Linear-complexity L_i histogram: 64 bins over [0, M], one column
    per bin (<= 79 cols).  Same 8-row glyph grid as the byte plot; no
    downsample needed (64 fits any sane terminal).  */
static void hist_lc_(const fastent_result * r, const fastent_options * o) {
  u64 g[64], max = 0;
  i32 i, c, gw, tick_every;
  int use_color;
  printf("Linear complexity L per %d-bit window (mean L=%.6g, mu=%.6g)\n",
         512, r->bm_mean_lc, r->bm_mu);
  Fi(64, g[i] = r->bm_lhist[i]);
  Fi(64, if (g[i] > max) max = g[i]);
  if (max == 0) { printf("(no full windows)\n\n");  return; }
  use_color = color_active_(o->color);
  gw = hist_bars_(g, 64, max, o->histogram_log, use_color, NULL, NULL);
  tick_every = 64 / 8;
  Fi(gw + 1, putchar(' '));
  putchar('+');
  Fi(64, putchar((i % tick_every == 0) ? '|' : '-'));
  putchar('\n');
  Fi(gw + 2, putchar(' '));
  for (c = 0; c < 64; c += tick_every) {
    char buf[8];
    i32 v = (i32) ((u64) c * (512u + 1u) / 64u);
    i32 ln = snprintf(buf, sizeof (buf), "%d", v);
    if (ln > tick_every) ln = tick_every;
    if (c + tick_every >= 64) printf("%.*s", ln, buf);
    else                      printf("%-*.*s", tick_every, ln, buf);
  }
  putchar('\n');
  printf("(L bucket");
  if (o->histogram_log) printf(", log y");
  printf(")\n\n");
}

/*  Maurer log2-distance histogram: 64 bins (bin = floor log2 of the
    recurrence distance), one column per bin (<= 79 cols).  Same 8-row
    glyph grid as the byte plot.  */
static void hist_maurer_(
    const fastent_result * r, const fastent_options * o) {
  u64 g[64], max = 0;
  i32 i, c, gw, tick_every;
  int use_color;
  printf("Maurer log2(recurrence distance) (fn=%.6g, expected=%.6g)\n",
         r->maurer_fn, r->maurer_expected);
  Fi(64, g[i] = r->maurer_lhist[i]);
  Fi(64, if (g[i] > max) max = g[i]);
  if (max == 0) { printf("(no test blocks)\n\n");  return; }
  use_color = color_active_(o->color);
  gw = hist_bars_(g, 64, max, o->histogram_log, use_color, NULL, NULL);
  tick_every = 64 / 8;
  Fi(gw + 1, putchar(' '));
  putchar('+');
  Fi(64, putchar((i % tick_every == 0) ? '|' : '-'));
  putchar('\n');
  Fi(gw + 2, putchar(' '));
  for (c = 0; c < 64; c += tick_every) {
    char buf[8];
    i32 ln = snprintf(buf, sizeof (buf), "%d", c);
    if (ln > tick_every) ln = tick_every;
    if (c + tick_every >= 64) printf("%.*s", ln, buf);
    else                      printf("%-*.*s", tick_every, ln, buf);
  }
  putchar('\n');
  printf("(log2 distance");
  if (o->histogram_log) printf(", log y");
  printf(")\n\n");
}

/*  Binary matrix-rank 3-bar histogram: counts in the r=32 / r=31 /
    r<=30 pooled bins, labelled under each bar.  Tiny plot, fits in any
    terminal; the labels carry context the bar widths cannot.  */
static void hist_mrank_(
    const fastent_result * r, const fastent_options * o) {
  u64 g[3], max = 0;
  i32 i, gw;
  int use_color;
  printf("Matrix rank (32x32, %" PRIu64 " matrices)\n",
         (u64) r->mrank_matrices);
  g[0] = r->mrank_r32;  g[1] = r->mrank_r31;  g[2] = r->mrank_rlo;
  Fi(3, if (g[i] > max) max = g[i]);
  if (max == 0) { printf("(no matrices)\n\n");  return; }
  use_color = color_active_(o->color);
  gw = hist_bars_(g, 3, max, o->histogram_log, use_color, NULL, NULL);
  Fi(gw + 1, putchar(' '));
  putchar('+');  Fi(3, putchar('|'));  putchar('\n');
  Fi(gw + 2, putchar(' '));
  fputs("r=32 r=31 r<=30", stdout);  putchar('\n');
  printf("(rank bin");
  if (o->histogram_log) printf(", log y");
  printf(")\n\n");
}

/*  Bandt-Pompe permutation entropy: 24-bin pattern (Lehmer-code id)
    histogram, one column per bin.  Same 8-row glyph grid as the byte
    plot; tick every 4 bins so the axis stays readable.  */
static void hist_perment_(
    const fastent_result * r, const fastent_options * o) {
  u64 g[24], max = 0;
  i32 i, c, gw, tick_every = 4;
  int use_color;
  printf("Permutation entropy patterns (m=4, %" PRIu64 " windows, "
         "H_norm=%.6g)\n",
         (u64) r->perment_windows, r->perment_h_norm);
  Fi(24, g[i] = r->perment_hist[i]);
  Fi(24, if (g[i] > max) max = g[i]);
  if (max == 0) { printf("(no windows)\n\n");  return; }
  use_color = color_active_(o->color);
  gw = hist_bars_(g, 24, max, o->histogram_log, use_color, NULL, NULL);
  Fi(gw + 1, putchar(' '));
  putchar('+');
  Fi(24, putchar((i % tick_every == 0) ? '|' : '-'));
  putchar('\n');
  Fi(gw + 2, putchar(' '));
  for (c = 0; c < 24; c += tick_every) {
    char buf[8];
    i32 ln = snprintf(buf, sizeof (buf), "%d", c);
    if (ln > tick_every) ln = tick_every;
    if (c + tick_every >= 24) printf("%.*s", ln, buf);
    else                      printf("%-*.*s", tick_every, ln, buf);
  }
  putchar('\n');
  printf("(pattern id");
  if (o->histogram_log) printf(", log y");
  printf(")\n\n");
}

void fastent_print_histogram(
    const fastent_result * r, const fastent_options * o) {
  hist_byte_(r, o);

  /*  -eee + -H: the offset and length log2 plots side by side, then
      a 256-bin literal histogram titled with H_lit.  Composed with
      -t/--json like the byte plot.  */
  if (o->extended >= 1 && r->lz) {
    char t[96];
    hist_log2_pair_("LZ77F match offsets (1..65535)",
                    r->lz->off_par, 1, 65535,
                    "LZ77F match lengths (4..255+)",
                    r->lz->mlen_full, 4, 255, o);
    snprintf(t, sizeof t,
             "LZ77F literal byte values (H_lit=%.4g bits)", r->lz_lit_h);
    hist_lit256_(t, r->lz->lit_byte, o);
  }
  if (o->extended >= 3 && r->bm_deviation == r->bm_deviation)
    hist_lc_(r, o);
  if (o->extended >= 3 && r->maurer_dev == r->maurer_dev)
    hist_maurer_(r, o);
  if (o->extended >= 3 && r->mrank_dev == r->mrank_dev)
    hist_mrank_(r, o);
  if (o->extended >= 1 && r->perment_deviation == r->perment_deviation)
    hist_perment_(r, o);
}
