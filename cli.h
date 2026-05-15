/*  fastent: command-line interface.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_CLI_H
#define FASTENT_CLI_H

#include "fastent-options.h"

void fastent_print_version(void);
void fastent_print_help(void);

/*  Returns 0 on success.  Negative on parse error (caller exits 1);
    positive return value should be used as the process exit code.  */
int  fastent_parse_args(int argc, char ** argv, fastent_options * o);

#endif
