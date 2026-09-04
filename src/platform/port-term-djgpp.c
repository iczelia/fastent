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
#include "port-term.h"

#ifdef __DJGPP__

#include <stdio.h>
#include <unistd.h>
#include <conio.h>

static int color_active_ = 0;

int fastent_term_isatty(void) {
  return isatty(1);
}

int fastent_term_width(void) {
  return 80;
}

fastent_glyph_mode fastent_term_glyphs(void) {
  return isatty(1) ? FASTENT_GLYPHS_CP437 : FASTENT_GLYPHS_ASCII;
}

void fastent_term_set_fg(int cls) {
  /*  4th entry keeps the `cls & 3` mask in bounds; classes are 0..2.  */
  static const int pal[4] = { DARKGRAY, LIGHTGRAY, LIGHTCYAN, LIGHTGRAY };
  fflush(stdout);
  if (cls < 0) {
    textcolor(LIGHTGRAY);
    color_active_ = 0;
  } else {
    textcolor(pal[cls & 3]);
    color_active_ = 1;
  }
}

void fastent_term_set_sev(int sev) {
  static const int pal[4] = { LIGHTGREEN, YELLOW, LIGHTRED, DARKGRAY };
  fflush(stdout);
  if (sev < 0) {
    textcolor(LIGHTGRAY);
    color_active_ = 0;
  } else {
    textcolor(pal[sev & 3]);
    color_active_ = 1;
  }
}

void fastent_term_write(const char * glyph) {
  if (color_active_) cputs(glyph);
  else               fputs(glyph, stdout);
}

#endif
