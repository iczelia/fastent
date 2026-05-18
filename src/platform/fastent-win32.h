/*  fastent: Win32 helpers.  MSVCRT's narrow open() re-narrows path
    arguments through CP_ACP, so we route paths via _wopen.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_WIN32_H
#define FASTENT_WIN32_H

#ifdef _WIN32

#include "common.h"

/*  argv -> UTF-8 from GetCommandLineW.  Heap-allocated, lives until
    exit.  Returns 0 on success, -1 on OOM.  */
int fastent_win32_argv_utf8(int * argc_out, char *** argv_out);

/*  SetConsoleOutputCP(CP_UTF8).  No-op on redirected output.  */
void fastent_win32_init_console(void);

/*  _setmode(_fileno(stdin), _O_BINARY).  Wrapped because the
    underscore names are hidden under -std=c99 in MinGW.  */
void fastent_win32_set_stdin_binary(void);

/*  open/close/read pass-throughs; open routes via _wopen so the
    path isn't narrowed through CP_ACP.  */
int  fastent_win32_open_utf8(const char * path, int flags);
int  fastent_win32_close(int fd);
i64  fastent_win32_read(int fd, void * buf, sz n);

i64  fastent_win32_num_cpus(void);

/*  CreateFileMapping + MapViewOfFile.  Caller frees via munmap.
    Returns -1 on non-disk fd, empty file, or AS exhaustion.  */
int fastent_win32_mmap(
    int fd, void ** out_base, u64 * out_size, void ** out_handle);
void fastent_win32_munmap(void * base, void * handle);

/*  Open for async reads (FILE_FLAG_OVERLAPPED +
    FILE_FLAG_SEQUENTIAL_SCAN).  Returns a HANDLE to bind to an IOCP,
    or NULL on failure.  *out_size set to file size on success.  */
void * fastent_win32_open_overlapped(const char * utf8_path, u64 * out_size);

/*  PrefetchVirtualMemory (Win 8+); no-op below.  */
void fastent_win32_mmap_prefetch(void * base, u64 size);

/*  SetConsoleTextAttribute wrapper.  cls in {0,1,2} = dim, default,
    bright accent; cls = -1 restores initial attrs.  Background bits
    preserved.  No-op when stdout isn't a console.  */
void fastent_win32_set_console_fg(int cls);

/*  Severity palette: sev in {0,1,2,3} = green, yellow, red, gray;
    sev = -1 restores initial attrs.  Background preserved.  */
void fastent_win32_set_console_sev(int sev);

#endif

#endif
