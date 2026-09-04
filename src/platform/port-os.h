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

#ifndef FASTENT_PORT_OS_H
#define FASTENT_PORT_OS_H

#include "common.h"

/*  Return online CPU count (>=1), 1 if the OS can't tell us.  */
i64 fastent_os_num_cpus(void);

/*  Make stdin a binary stream.  No-op on POSIX; _setmode on Win32.  */
void fastent_os_set_stdin_binary(void);

/*  Initialise the console for the lifetime of the process.  POSIX:
    no-op.  Win32: SetConsoleOutputCP(CP_UTF8), SetConsoleCP(CP_UTF8).  */
void fastent_os_init_console(void);

/*  Replace argv with a heap-allocated UTF-8 copy reconstructed from
    GetCommandLineW.  POSIX: no-op (returns 0).  Returns 0 on success,
    -1 on OOM.  */
int  fastent_os_argv_utf8(int * argc_out, char *** argv_out);

/*  OpenBSD pledge() wrapper.  POSIX without pledge: no-op (returns 0).
    `promises` is a space-separated list per pledge(2).  Returns 0 on
    success, -1 with errno set on failure.  */
int  fastent_os_pledge(const char * promises);

#endif
