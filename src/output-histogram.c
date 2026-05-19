/*  fastent: terminal histogram renderer.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

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
  i32 sub;
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
     const f64 yf = (f64)(height - i) / (f64) height;
     const u64 yval = histogram_log
                      ? (u64)(exp(yf * log_denom) - 1.0 + 0.5)
                      : (u64)(yf * (f64) max + 0.5);
     printf("%*" PRIu64 " |", gw, (u64) yval);
     Fj(cols,
        f64 frac;
        if (histogram_log) {
          frac = grouped[j] == 0 ? 0.0
               : log((f64) grouped[j] + 1.0) / log_denom;
        } else {
          frac = (f64) grouped[j] / (f64) max;
        }
        i32 hh = (i32)(frac * (f64) levels + 0.5);
        if (hh > levels) hh = levels;
        const char * glyph;
        if (hh >= row_top)      glyph = blocks[sub];
        else if (hh <= row_bot) glyph = blocks[0];
        else                    glyph = blocks[hh - row_bot];
        if (use_color && col_class) {
          i32 cls = col_class(j, cc_ctx);
          if (cls != last_class) { fastent_term_set_fg(cls);  last_class = cls; }
        }
        fastent_term_write(glyph))
     if (use_color && col_class) fastent_term_set_fg(-1);
     putchar('\n'))
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
  const i32 bins = o->binary ? 2 : 256;

  i32 group = 1;
  if (!o->binary) {
    i32 w = fastent_term_width();
    if (w < 1) w = 80;
    while (bins / group > w && group < bins) group <<= 1;
  }
  const i32 cols = bins / group;

  u64 grouped[256];
  Fi(cols,
     u64 sum = 0;
     Fj(group, sum += r->hist[i * group + j]);
     grouped[i] = sum)

  u64 max = 0;
  Fi(cols, if (grouped[i] > max) max = grouped[i])
  if (max == 0) {
    printf("(histogram: no samples)\n\n");
    return;
  }

  const int use_color = color_active_(o->color);
  byte_cc_ cc = { group };
  i32 gw = hist_bars_(grouped, cols, max, o->histogram_log, use_color,
                      byte_col_class_, &cc);

  if (bins == 256) {
    i32 tick_every = cols / 8;
    if (tick_every < 1) tick_every = 1;
    Fi(gw + 1, putchar(' '))
    putchar('+');
    Fi(cols, putchar((i % tick_every == 0) ? '|' : '-'))
    putchar('\n');
    Fi(gw + 2, putchar(' '))
    i32 c;
    for (c = 0; c < cols; c += tick_every) {
      char buf[8];
      i32 n = snprintf(buf, sizeof(buf), "%d", c * group);
      if (n > tick_every) n = tick_every;
      if (c + tick_every >= cols) {
        printf("%.*s", n, buf);
      } else {
        printf("%-*.*s", tick_every, n, buf);
      }
    }
    putchar('\n');
  } else {
    Fi(gw + 2, putchar(' '))
    printf("0 1\n");
  }
  u64 raw_peak = 0;
  i32 peak_v   = 0;
  Fi(bins, if (r->hist[i] > raw_peak) { raw_peak = r->hist[i]; peak_v = i; })
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
  if (nb < 1) nb = 1;
  if (nb > 24) nb = 24;
  Fi(nb, bkt[i] = 0)
  u64 t = 0;
  Fi0(hi + 1, lo,
      const u64 c = src[i];
      if (!c) continue;
      i32 b = log2_bucket_((u64) i);
      if (b >= nb) b = nb - 1;
      bkt[b] += c;  t += c)
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
  const f64 yf = (f64)(height - rr) / (f64) height;
  const u64 yv = hlog ? (u64)(exp(yf * ld) - 1.0 + 0.5)
                      : (u64)(yf * (f64) max + 0.5);
  printf("%*" PRIu64 " |", gw, yv);
  Fi(nb,
     f64 frac = hlog ? (bkt[i] == 0 ? 0.0
                        : log((f64) bkt[i] + 1.0) / ld)
                     : (f64) bkt[i] / (f64) max;
     i32 hh = (i32)(frac * (f64) levels + 0.5);
     if (hh > levels) hh = levels;
     fputs(hh >= row_top ? blk[sub]
         : hh <= row_bot ? blk[0] : blk[hh - row_bot], stdout))
}

/*  The two -eee log2 plots SIDE BY SIDE: offsets left, match lengths
    right.  X-axis labels run vertically (one digit per row, MSD on
    top) so each 2^k label is a single column under its tick.  */
static void hist_log2_pair_(
    const char * tA, const u64 * sA, i32 loA, i32 hiA,
    const char * tB, const u64 * sB, i32 loB, i32 hiB,
    const fastent_options * o) {
  u64 bA[24], bB[24], totA, totB;
  i32 nbA = log2_collect_(sA, loA, hiA, bA, &totA);
  i32 nbB = log2_collect_(sB, loB, hiB, bB, &totB);
  u64 mA = 1, mB = 1;
  Fi(nbA, if (bA[i] > mA) mA = bA[i])
  Fi(nbB, if (bB[i] > mB) mB = bB[i])
  char g[24];
  i32 gwA = snprintf(g, sizeof g, "%" PRIu64, mA);  if (gwA < 1) gwA = 1;
  i32 gwB = snprintf(g, sizeof g, "%" PRIu64, mB);  if (gwB < 1) gwB = 1;
  i32 sub;
  const char * const * blk = hist_glyphs_(&sub);
  const int hlog = o->histogram_log;
  const i32 wA = gwA + 2 + nbA;            /*  width of one A piece  */
  i32 tAlen = 0;  while (tA[tAlen]) tAlen++;
  const i32 bw = wA > tAlen ? wA : tAlen;  /*  A column block width  */
  const i32 padA = bw - wA;
  const char * SEP = "   ";
  char fb[48];
  snprintf(fb, sizeof fb, "(log2 buckets [2^k, 2^k+1)%s)",
           hlog ? ", log y" : "");

  printf("%s", tA);  Fi(bw - tAlen, putchar(' '));
  fputs(SEP, stdout);  printf("%s\n", tB);
  Fi(8,
     log2_row_(bA, nbA, mA, hlog, blk, sub, gwA, i);
     Fj(padA, putchar(' '));  fputs(SEP, stdout);
     log2_row_(bB, nbB, mB, hlog, blk, sub, gwB, i);
     putchar('\n'))

  Fi(gwA + 1, putchar(' '))  putchar('+');  Fi(nbA, putchar('|'))
  Fj(padA, putchar(' '));  fputs(SEP, stdout);
  Fi(gwB + 1, putchar(' '))  putchar('+');  Fi(nbB, putchar('|'))
  putchar('\n');

  /*  Vertical x-labels under each tick: digit of 2^k, MSD on top,
      bottom-justified within each plot's own width.  The footer
      rides the last (units) row of the offsets block.  */
  i32 mlA = (i32) snprintf(g, sizeof g, "%d", 1 << (nbA - 1));
  i32 mlB = (i32) snprintf(g, sizeof g, "%d", 1 << (nbB - 1));
  i32 rows = mlA > mlB ? mlA : mlB;
  Fi(rows,
     i32 r = i;
     Fj(gwA + 2, putchar(' '))
     Fj(nbA,
        char lb[16];
        i32 ln = snprintf(lb, sizeof lb, "%d", 1 << j);
        i32 pos = r - (mlA - ln);
        putchar(r < mlA && pos >= 0 && pos < ln ? lb[pos] : ' '))
     if (r < mlB) {
       Fj(padA, putchar(' '));  fputs(SEP, stdout);
       Fj(gwB + 2, putchar(' '))
       Fj(nbB,
          char lb[16];
          i32 ln = snprintf(lb, sizeof lb, "%d", 1 << j);
          i32 pos = r - (mlB - ln);
          putchar(pos >= 0 && pos < ln ? lb[pos] : ' '))
     }
     if (r == rows - 1) printf(" %s", fb);
     putchar('\n'))
  putchar('\n');
}

/*  256-bin literal byte-value plot: same downsample / colour / tick
    machinery as the order-0 byte plot, fed by lit_byte.  */
static void hist_lit256_(
    const char * title, const u64 * lit, const fastent_options * o) {
  printf("%s\n", title);
  i32 group = 1;
  i32 w = fastent_term_width();
  if (w < 1) w = 80;
  while (256 / group > w && group < 256) group <<= 1;
  const i32 cols = 256 / group;
  u64 grouped[256];
  Fi(cols,
     u64 sum = 0;
     Fj(group, sum += lit[i * group + j]);
     grouped[i] = sum)
  u64 max = 0;
  Fi(cols, if (grouped[i] > max) max = grouped[i])
  if (max == 0) { printf("(no samples)\n\n");  return; }

  const int use_color = color_active_(o->color);
  byte_cc_ cc = { group };
  i32 gw = hist_bars_(grouped, cols, max, o->histogram_log, use_color,
                      byte_col_class_, &cc);
  i32 tick_every = cols / 8;
  if (tick_every < 1) tick_every = 1;
  Fi(gw + 1, putchar(' '))
  putchar('+');
  Fi(cols, putchar((i % tick_every == 0) ? '|' : '-'))
  putchar('\n');
  Fi(gw + 2, putchar(' '))
  i32 c;
  for (c = 0; c < cols; c += tick_every) {
    char buf[8];
    i32 n = snprintf(buf, sizeof(buf), "%d", c * group);
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

void fastent_print_histogram(
    const fastent_result * r, const fastent_options * o) {
  hist_byte_(r, o);

  /*  -eee + -H: the offset and length log2 plots side by side, then
      a 256-bin literal histogram titled with H_lit.  Composed with
      -t/--json like the byte plot.  */
  if (o->extended >= 3 && r->lz) {
    char t[96];
    hist_log2_pair_("LZ77F match offsets (1..65535)",
                    r->lz->off_par, 1, 65535,
                    "LZ77F match lengths (4..255+)",
                    r->lz->mlen_full, 4, 255, o);
    snprintf(t, sizeof t,
             "LZ77F literal byte values (H_lit=%.4g bits)", r->lz_lit_h);
    hist_lit256_(t, r->lz->lit_byte, o);
  }
}
