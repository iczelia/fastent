/*  fastent: Win32 Unicode-path helpers.

    Without these, fastent on pre-Windows-10-1903 (or any Windows with
    the legacy CP_ACP active code page) can't open files whose names
    don't round-trip through the ACP.  argv is pre-narrowed by MSVCRT
    and open(2) re-narrows again on its way to NtCreateFile.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_WIN32_H
#define FASTENT_WIN32_H

#ifdef _WIN32

#include <stddef.h>

/*  Replace argv with UTF-8 strings derived from GetCommandLineW.  The
    returned argv array and its strings are malloc'd; they're intended
    to live for the entire process lifetime and are not freed (process
    exit reclaims them).  Returns 0 on success, -1 on OOM or invalid
    command line.  */
int fastent_win32_argv_utf8(int * argc_out, char *** argv_out);

/*  Set both the console input and output code pages to UTF-8 so that
    bytes written to stdout/stderr via printf and friends are
    interpreted by the console host as UTF-8.  Best effort; silently
    no-ops when there's no console attached (e.g. redirected output).
    Available since Windows 2000.  */
void fastent_win32_init_console(void);

/*  Switch stdin to binary mode so that read(2) returns raw bytes
    (no \r\n translation).  Wraps _setmode(_fileno(stdin), _O_BINARY).
    Lives here because the underlying MSVCRT names aren't visible
    under -std=c99 (which sets __STRICT_ANSI__ and hides the legacy
    underscore-prefixed identifiers in MinGW's headers).  */
void fastent_win32_set_stdin_binary(void);

/*  UTF-8-aware open(2) replacement.  Converts the path argument from
    UTF-8 to UTF-16 via MultiByteToWideChar(CP_UTF8) and calls _wopen,
    which talks to NtCreateFile with the wide name directly (no
    ACP round-trip).  Returns a POSIX-style fd usable with read,
    close, fstat or -1 on failure (errno set).  `flags` accepts the
    usual O_RDONLY | O_BINARY from <fcntl.h>; we never need the
    optional mode argument because fastent only opens for read.  */
int fastent_win32_open_utf8(const char * path, int flags);

/*  Thin wrappers around _close and _read.  Needed for the same
    -std=c99 hiding reason as fastent_win32_set_stdin_binary.  */
int fastent_win32_close(int fd);
long fastent_win32_read(int fd, void * buf, size_t n);

/*  GetSystemInfo-backed sysconf(_SC_NPROCESSORS_ONLN) replacement.  */
long fastent_win32_num_cpus(void);

/*  CreateFileMapping + MapViewOfFile backed read-only mmap.  `fd` is
    a POSIX-style descriptor as returned by fastent_win32_open_utf8;
    the underlying HANDLE is recovered via _get_osfhandle.  Returns 0
    and fills *out_base / *out_size / *out_handle on success.  Returns
    -1 if the fd isn't disk-backed, the file is empty, or the mapping
    can't be created (eg. address space too tight for the file size on
    32-bit Windows).  The caller passes *out_handle back to
    fastent_win32_munmap to release the kernel mapping object.  */
int  fastent_win32_mmap(int fd, void ** out_base,
                        unsigned long long * out_size,
                        void ** out_handle);
void fastent_win32_munmap(void * base, void * handle);

/*  Best-effort sequential read-ahead hint for a mapped region.  Calls
    PrefetchVirtualMemory on Win 8+ (resolved at runtime so the binary
    still loads on Vista/7); silent no-op on older hosts.  */
void fastent_win32_mmap_prefetch(void * base, unsigned long long size);

#endif

#endif
