/*  fastent: Win32 process helpers.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

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
