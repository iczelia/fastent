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
#include "port-os.h"

#ifndef _WIN32

#include <unistd.h>

i64 fastent_os_num_cpus(void) {
#ifdef _SC_NPROCESSORS_ONLN
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (i64) n : 1;
#else
  return 1;
#endif
}

void fastent_os_set_stdin_binary(void) {
  /*  POSIX stdin is already a byte stream.  */
}

void fastent_os_init_console(void) {
}

int fastent_os_argv_utf8(int * argc_out, char *** argv_out) {
  (void) argc_out; (void) argv_out;
  return 0;
}

int fastent_os_pledge(const char * promises) {
#ifdef HAVE_PLEDGE
  return pledge(promises, NULL);
#else
  (void) promises;
  return 0;
#endif
}

#endif
