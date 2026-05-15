/*  fastent: terminal output abstractions.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_PORT_TERM_H
#define FASTENT_PORT_TERM_H

#include "common.h"

typedef enum {
  FASTENT_GLYPHS_UNICODE = 0,
  FASTENT_GLYPHS_CP437   = 1,
  FASTENT_GLYPHS_ASCII   = 2
} fastent_glyph_mode;

/*  Effective output width in columns; 80 if it can't be queried.  */
int  fastent_term_width(void);

/*  Whether stdout is attached to an interactive terminal.  */
int  fastent_term_isatty(void);

/*  Native glyph repertoire of the platform's console.  */
fastent_glyph_mode fastent_term_glyphs(void);

/*  Set foreground color class: 0=dim, 1=default, 2=accent; cls=-1
    restores initial attributes.  No-op when stdout isn't a console.  */
void fastent_term_set_fg(int cls);

/*  Set a severity color: 0=ok (green), 1=warn (yellow), 2=bad (red),
    3=muted (dim); sev=-1 restores initial attributes.  Used by the
    annotated report's pass/warn/fail badges.  No-op off-console.  */
void fastent_term_set_sev(int sev);

/*  Write `glyph` to stdout honoring any platform-specific color
    write path (e.g. DJGPP cputs to the BIOS attribute slot).  Caller
    chose color via fastent_term_set_fg() beforehand.  */
void fastent_term_write(const char * glyph);

#endif
