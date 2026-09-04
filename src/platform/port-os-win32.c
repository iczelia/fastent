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

#ifdef _WIN32

#include "fastent-win32.h"

i64 fastent_os_num_cpus(void) {
  return fastent_win32_num_cpus();
}

void fastent_os_set_stdin_binary(void) {
  fastent_win32_set_stdin_binary();
}

void fastent_os_init_console(void) {
  fastent_win32_init_console();
}

int fastent_os_argv_utf8(int * argc_out, char *** argv_out) {
  return fastent_win32_argv_utf8(argc_out, argv_out);
}

int fastent_os_pledge(const char * promises) {
  (void) promises;
  return 0;
}

#endif
