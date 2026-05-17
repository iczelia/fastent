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

void fastent_print_histogram(const fastent_result * r,
                             const fastent_options * o) {
  const i32 bins = o->binary ? 2 : 256;
  static const char * const glyphs_unicode[9] = {
    " ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83",
         "\xe2\x96\x84", "\xe2\x96\x85", "\xe2\x96\x86",
         "\xe2\x96\x87", "\xe2\x96\x88"
  };
  static const char * const glyphs_cp437[3] = { " ", "\xdc", "\xdb" };
  static const char * const glyphs_ascii[2] = { " ", "#" };
  const char * const * blocks;
  i32 sub;
  switch (fastent_term_glyphs()) {
    case FASTENT_GLYPHS_CP437:   blocks = glyphs_cp437;   sub = 2; break;
    case FASTENT_GLYPHS_ASCII:   blocks = glyphs_ascii;   sub = 1; break;
    case FASTENT_GLYPHS_UNICODE:
    default:                     blocks = glyphs_unicode; sub = 8; break;
  }
  const i32 height = 8;
  const i32 levels = height * sub;

  /*  Downsample 256 bins to the smallest power-of-2 group that fits.  */
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

  const f64 log_denom = o->histogram_log
                        ? log((f64) max + 1.0) : 0.0;

  /*  Left gutter: sample count at each row's top edge,
      right-justified to the peak's width.  */
  char gbuf[24];
  i32  gw = snprintf(gbuf, sizeof gbuf, "%" PRIu64, (u64) max);
  if (gw < 1) gw = 1;

  Fi(height,
     const i32 row_bot = (height - 1 - i) * sub;
     const i32 row_top = row_bot + sub;
     i32 last_class = -1;
     const f64 yf = (f64)(height - i) / (f64) height;
     const u64 yval = o->histogram_log
                      ? (u64)(exp(yf * log_denom) - 1.0 + 0.5)
                      : (u64)(yf * (f64) max + 0.5);
     printf("%*" PRIu64 " |", gw, (u64) yval);
     Fj(cols,
        f64 frac;
        if (o->histogram_log) {
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
        i32 first_byte = j * group;
        i32 cls = (first_byte < 32 || first_byte == 127) ? 0
                : (first_byte < 128 ? 1 : 2);
        if (use_color && cls != last_class) {
          fastent_term_set_fg(cls);
          last_class = cls;
        }
        fastent_term_write(glyph))
     if (use_color) fastent_term_set_fg(-1);
     putchar('\n'))
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
        /*  Last tick: no trailing padding.  */
        printf("%.*s", n, buf);
      } else {
        /*  Left-justify in the cell so the first digit sits under
            the '|' tick mark.  */
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
