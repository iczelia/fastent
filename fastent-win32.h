/*  fastent: Win32 helpers.  MSVCRT's narrow open() re-narrows path
    arguments through CP_ACP, so we route paths via _wopen.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_WIN32_H
#define FASTENT_WIN32_H

#ifdef _WIN32

#include <stddef.h>

/*  argv -> UTF-8 from GetCommandLineW.  Heap-allocated; lives until
    process exit.  Returns 0 on success, -1 on OOM.  */
int fastent_win32_argv_utf8(int * argc_out, char *** argv_out);

/*  SetConsoleOutputCP(CP_UTF8).  No-op on redirected output.  */
void fastent_win32_init_console(void);

/*  _setmode(_fileno(stdin), _O_BINARY).  Kept here because the
    underscore-prefixed names are hidden under -std=c99 in MinGW.  */
void fastent_win32_set_stdin_binary(void);

/*  open(2)/close/read pass-throughs that route through _wopen so
    the path doesn't get narrowed through CP_ACP.  */
int  fastent_win32_open_utf8(const char * path, int flags);
int  fastent_win32_close(int fd);
long fastent_win32_read(int fd, void * buf, size_t n);

long fastent_win32_num_cpus(void);

/*  CreateFileMapping + MapViewOfFile.  Caller frees via the
    matching munmap.  Returns -1 on non-disk fd / empty file /
    address-space exhaustion.  */
int  fastent_win32_mmap(int fd, void ** out_base,
                        unsigned long long * out_size,
                        void ** out_handle);
void fastent_win32_munmap(void * base, void * handle);

/*  Open a file for async reads (FILE_FLAG_OVERLAPPED +
    FILE_FLAG_SEQUENTIAL_SCAN).  Returns a HANDLE that the caller
    binds to an IOCP, or NULL on failure.  *out_size is the file
    size in bytes on success.  */
void * fastent_win32_open_overlapped(const char * utf8_path,
                                     unsigned long long * out_size);

/*  PrefetchVirtualMemory (Win 8+); no-op below.  */
void fastent_win32_mmap_prefetch(void * base, unsigned long long size);

/*  SetConsoleTextAttribute wrapper.  cls in {0,1,2} = dim, default,
    bright accent; cls = -1 restores initial attrs.  Background bits
    preserved.  No-op when stdout isn't a console.  */
void fastent_win32_set_console_fg(int cls);

/*  Severity palette: sev in {0,1,2,3} = green, yellow, red, gray;
    sev = -1 restores initial attrs.  Background preserved.  */
void fastent_win32_set_console_sev(int sev);

#endif

#endif
