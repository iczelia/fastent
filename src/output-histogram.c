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

/*  One -eee log2 plot: collapse src[lo..hi] into floor(log2(index))
    buckets, render via the shared grid, [2^k,2^(k+1)) axis.  */
static void hist_log2_(
    const char * title, const u64 * src, i32 lo, i32 hi,
    const fastent_options * o) {
  i32 nb = log2_bucket_((u64) hi) + 1;
  if (nb < 1) nb = 1;
  if (nb > 24) nb = 24;
  u64 bkt[24];
  Fi(nb, bkt[i] = 0)
  u64 tot = 0;
  Fi0(hi + 1, lo,
      const u64 c = src[i];
      if (!c) continue;
      i32 b = log2_bucket_((u64) i);
      if (b >= nb) b = nb - 1;
      bkt[b] += c;  tot += c)

  printf("%s\n", title);
  if (tot == 0) { printf("(no samples)\n\n");  return; }

  u64 max = 0;
  Fi(nb, if (bkt[i] > max) max = bkt[i])
  const int use_color = color_active_(o->color);
  i32 gw = hist_bars_(bkt, nb, max, o->histogram_log, use_color, NULL, NULL);

  Fi(gw + 1, putchar(' '))
  putchar('+');
  Fi(nb, putchar('|'))
  putchar('\n');
  Fi(gw + 2, putchar(' '))
  Fi(nb, printf("%d ", 1 << i))
  putchar('\n');
  printf("(log2 buckets [2^k, 2^k+1)");
  if (o->histogram_log) printf(", log y");
  printf(")\n\n");
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

  /*  -eee + -H: three extra plots after the byte/bit plot (offset and
      length log2 buckets, plus a 256-bin literal histogram titled
      with H_lit).  Composed with -t/--json like the byte plot.  */
  if (o->extended >= 3 && r->lz) {
    char t[96];
    hist_log2_("LZ77F match offsets (1..65535)",
               r->lz->off_par, 1, 65535, o);
    hist_log2_("LZ77F match lengths (4..255+)",
               r->lz->mlen_full, 4, 255, o);
    snprintf(t, sizeof t,
             "LZ77F literal byte values (H_lit=%.4g bits)", r->lz_lit_h);
    hist_lit256_(t, r->lz->lit_byte, o);
  }
}
