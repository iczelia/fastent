/*  fastent: Win32 console port.  Uses SetConsoleTextAttribute so we
    work on Win95+ without ANSI.SYS.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "port-term.h"

#if defined(_WIN32) && !defined(__DJGPP__)

#include "fastent-win32.h"

#include <stdio.h>
#include <io.h>

int fastent_term_isatty(void) {
  return _isatty(_fileno(stdout)) ? 1 : 0;
}

int fastent_term_width(void) {
  return 80;
}

fastent_glyph_mode fastent_term_glyphs(void) {
#if defined(FASTENT_WIN_LEGACY)
  return fastent_term_isatty() ? FASTENT_GLYPHS_CP437 : FASTENT_GLYPHS_ASCII;
#else
  return FASTENT_GLYPHS_UNICODE;
#endif
}

void fastent_term_set_fg(int cls) {
  fastent_win32_set_console_fg(cls);
}

void fastent_term_set_sev(int sev) {
  fastent_win32_set_console_sev(sev);
}

void fastent_term_write(const char * glyph) {
  fputs(glyph, stdout);
}

#endif
