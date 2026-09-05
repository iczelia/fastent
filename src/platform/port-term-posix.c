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

#if !defined(_WIN32) && !defined(__DJGPP__)

#include <stdio.h>
#include <unistd.h>

#ifdef TIOCGWINSZ
#include <sys/ioctl.h>
#endif

int fastent_term_isatty(void) {
  return isatty(1);
}

int fastent_term_width(void) {
#ifdef TIOCGWINSZ
  struct winsize w;
  if (isatty(1) && ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
    return (int) w.ws_col;
#endif
  return 80;
}

fastent_glyph_mode fastent_term_glyphs(void) {
  return FASTENT_GLYPHS_UNICODE;
}

void fastent_term_set_fg(int cls) {
  /*  4th entry (reset) keeps the `cls & 3` mask in bounds for an
      out-of-contract class; valid classes are 0..2.  */
  static const char * const ansi[4] =
    { "\x1b[2m", "\x1b[0m", "\x1b[36m", "\x1b[0m" };
  if (cls < 0) { fputs("\x1b[0m", stdout);  return; }
  fputs(ansi[cls & 3], stdout);
}

void fastent_term_set_sev(int sev) {
  static const char * const ansi[4] = {
    "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[2m"
  };
  if (sev < 0) { fputs("\x1b[0m", stdout);  return; }
  fputs(ansi[sev & 3], stdout);
}

void fastent_term_write(const char * glyph) {
  fputs(glyph, stdout);
}

#endif
