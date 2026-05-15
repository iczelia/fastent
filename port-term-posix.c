/*  fastent: POSIX/ANSI terminal port.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

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
  static const char * const ansi[3] = { "\x1b[2m", "\x1b[0m", "\x1b[36m" };
  if (cls < 0) { fputs("\x1b[0m", stdout); return; }
  fputs(ansi[cls & 3], stdout);
}

void fastent_term_write(const char * glyph) {
  fputs(glyph, stdout);
}

#endif
