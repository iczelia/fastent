/*  fastent: POSIX process helpers.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "port-os.h"

#ifndef _WIN32

#include <unistd.h>

long fastent_os_num_cpus(void) {
#ifdef _SC_NPROCESSORS_ONLN
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? n : 1;
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
