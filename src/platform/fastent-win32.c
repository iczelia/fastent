/*  fastent: Win32 Unicode-path helpers.  Built only on Windows hosts.
    FASTENT_WIN_LEGACY selects the narrow-API Win95 path.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "fastent-win32.h"

#ifdef _WIN32

#ifndef _WIN32_WINNT
  /*  Vista baseline; configure can override.  */
  #define _WIN32_WINNT 0x0600
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <wchar.h>

#ifndef FASTENT_WIN_LEGACY

/*  UTF-8 / UTF-16 heap conversion helpers.  */

static wchar_t * utf8_to_wide(const char * s) {
  i32 n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0) return NULL;
  wchar_t * w = (wchar_t *) malloc((sz) n * sizeof(wchar_t));
  if (!w) return NULL;
  if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
    free(w); return NULL;
  }
  return w;
}

static char * wide_to_utf8(const wchar_t * w) {
  i32 n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  if (n <= 0) return NULL;
  char * s = (char *) malloc((sz) n);
  if (!s) return NULL;
  if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
    free(s); return NULL;
  }
  return s;
}

/*  MSVCRT command-line parsing: whitespace separates args outside
    quotes; 2N backslashes before `"` become N '\\' and toggle quote
    state, 2N+1 become N '\\' plus a literal '"'; `""` inside quotes
    is a literal '"'.  Two-pass: count argc, then fill.  */

static int grow_buf_w(wchar_t ** pbuf, sz * pbcap) {
  if (*pbcap > (sz) -1 / (2 * sizeof(wchar_t))) return 0;
  sz nc = *pbcap * 2;
  wchar_t * nb = (wchar_t *) realloc(*pbuf, nc * sizeof(wchar_t));
  if (!nb) return 0;
  *pbuf = nb; *pbcap = nc;
  return 1;
}

static int split_cmdline_w(const wchar_t * cmd, wchar_t *** out_wargv) {
  int argc = 0;
  i32 pass;
  wchar_t ** wargv = NULL;
  wchar_t * buf = NULL;
  for (pass = 0; pass < 2; pass++) {
    const wchar_t * p = cmd;
    if (pass == 1) {
      wargv = (wchar_t **) calloc((sz) argc + 1, sizeof(wchar_t *));
      if (!wargv) return -1;
    }
    argc = 0;
    while (*p) {
      while (*p == L' ' || *p == L'\t') p++;
      if (!*p) break;
      sz blen = 0, bcap = 0;
      buf = NULL;
      if (pass == 1) {
        bcap = 64;
        buf = (wchar_t *) malloc(bcap * sizeof(wchar_t));
        if (!buf) goto fail;
      }
      int in_quote = 0;
      while (*p) {
        if (!in_quote && (*p == L' ' || *p == L'\t')) break;
        if (*p == L'\\') {
          i32 nbs = 0;
          while (*p == L'\\') { nbs++; p++; }
          if (*p == L'"') {
            i32 slashes = nbs / 2;
            if (pass == 1) {
              Fi(slashes,
                if (blen + 1 >= bcap && !grow_buf_w(&buf, &bcap)) goto fail;
                buf[blen++] = L'\\');
            }
            if (nbs & 1) {
              if (pass == 1) {
                if (blen + 1 >= bcap && !grow_buf_w(&buf, &bcap)) goto fail;
                buf[blen++] = L'"';
              }
              p++;
            } else {
              in_quote = !in_quote;
              p++;
            }
          } else {
            if (pass == 1) {
              Fi(nbs,
                if (blen + 1 >= bcap && !grow_buf_w(&buf, &bcap)) goto fail;
                buf[blen++] = L'\\');
            }
          }
        } else if (*p == L'"') {
          if (in_quote && p[1] == L'"') {
            if (pass == 1) {
              if (blen + 1 >= bcap && !grow_buf_w(&buf, &bcap)) goto fail;
              buf[blen++] = L'"';
            }
            p += 2;
          } else {
            in_quote = !in_quote;
            p++;
          }
        } else {
          if (pass == 1) {
            if (blen + 1 >= bcap && !grow_buf_w(&buf, &bcap)) goto fail;
            buf[blen++] = *p;
          }
          p++;
        }
      }
      if (pass == 1) {
        buf[blen] = L'\0';
        wargv[argc] = buf;
        buf = NULL;
      }
      argc++;
    }
    if (pass == 1) { *out_wargv = wargv; return argc; }
  }
  return -1;
fail:
  free(buf);
  if (wargv) {
    Fj(argc, free(wargv[j]));
    free(wargv);
  }
  return -1;
}

/*  Public API.  */

int fastent_win32_argv_utf8(int * argc_out, char *** argv_out) {
  wchar_t ** wargv = NULL;
  int wargc = split_cmdline_w(GetCommandLineW(), &wargv);
  char ** argv;
  if (wargc < 0) return -1;

  argv = (char **) calloc((sz) wargc + 1, sizeof(char *));
  if (!argv) {
    Fi(wargc, free(wargv[i]));
    free(wargv);
    return -1;
  }
  Fi(wargc,
     argv[i] = wide_to_utf8(wargv[i]);
     free(wargv[i]);
     if (!argv[i]) {
       Fj(i, free(argv[j]));
       free(argv);
       Fj0(wargc, i + 1, free(wargv[j]));
       free(wargv);
       return -1;
     })
  free(wargv);
  *argc_out = wargc;
  *argv_out = argv;
  return 0;
}

void fastent_win32_init_console(void) {
  /*  No-ops when stdout/stderr is redirected.  */
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
}

int fastent_win32_open_utf8(const char * path, int flags) {
  if (!path) { errno = EINVAL; return -1; }
  wchar_t * w = utf8_to_wide(path);
  if (!w) {
    errno = EINVAL;
    return -1;
  }
  int fd = _wopen(w, flags);
  int saved_errno = errno;
  free(w);
  errno = saved_errno;
  return fd;
}

#else  /*  FASTENT_WIN_LEGACY: Win95 narrow-API path  */

int fastent_win32_argv_utf8(int * argc_out, char *** argv_out) {
  (void) argc_out; (void) argv_out;
  return 0;
}

void fastent_win32_init_console(void) {
  /*  CP 65001 is unsafe pre-OSR2 + IE5.  */
}

int fastent_win32_open_utf8(const char * path, int flags) {
  if (!path) { errno = EINVAL; return -1; }
  return _open(path, flags);
}

#endif  /*  FASTENT_WIN_LEGACY  */

void fastent_win32_set_stdin_binary(void) {
  _setmode(_fileno(stdin), _O_BINARY);
}

int fastent_win32_close(int fd) {
  return _close(fd);
}

i64 fastent_win32_read(int fd, void * buf, sz n) {
  /*  _read takes u32; cap to avoid truncation.  */
  u32 cap = (n > 0x7FFFFFFFu) ? 0x7FFFFFFFu : (u32) n;
  return (i64) _read(fd, buf, cap);
}

i64 fastent_win32_num_cpus(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (i64) si.dwNumberOfProcessors;
}

/*  GetFileSizeEx is Win2000+; FASTENT_WIN_LEGACY uses GetFileSize.
    Returns 0 and sets *out on success (a zero-length file is a
    valid size 0, not an error: callers fall back to STREAM for it,
    matching the POSIX backend); -1 only on a real query failure.  */
static int fastent_win32_filesize_(HANDLE h, u64 * out) {
#ifdef FASTENT_WIN_LEGACY
  DWORD hi = 0;
  DWORD lo = GetFileSize(h, &hi);
  if (lo == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) return -1;
  *out = ((u64) hi << 32) | (u64) lo;
  return 0;
#else
  LARGE_INTEGER li;
  if (!GetFileSizeEx(h, &li) || li.QuadPart < 0) return -1;
  *out = (u64) li.QuadPart;
  return 0;
#endif
}

int fastent_win32_mmap(
    int fd, void ** out_base, u64 * out_size, void ** out_handle) {
  HANDLE h, hm;
  u64 fsz;
  void * p;
  intptr_t raw;
  if (fd < 0) return -1;
  raw = _get_osfhandle(fd);
  if (raw == -1) return -1;
  h = (HANDLE) raw;
  if (GetFileType(h) != FILE_TYPE_DISK) return -1;
  if (fastent_win32_filesize_(h, &fsz) != 0) return -1;
  if (fsz == 0) return 1;   /*  empty: cannot map; caller streams  */
  /*  0,0 = current file size; NULL name = anonymous.  CreateFileMappingW
      is a stub on Win9x; with a NULL name the A entry is equivalent.  */
  hm = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
  if (!hm) return -1;
  p = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
  if (!p) {
    CloseHandle(hm);
    return -1;
  }
  *out_base   = p;
  *out_size   = fsz;
  *out_handle = hm;
  return 0;
}

void fastent_win32_munmap(void * base, void * handle) {
  if (base)   UnmapViewOfFile(base);
  if (handle) CloseHandle((HANDLE) handle);
}

void * fastent_win32_open_overlapped(const char * utf8_path, u64 * out_size) {
  HANDLE h = INVALID_HANDLE_VALUE;
  u64 fsz;
  if (!utf8_path || !out_size) return NULL;
#ifndef FASTENT_WIN_LEGACY
  {
    i32 n = MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t * w = (wchar_t *) malloc((sz) n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, w, n) <= 0) {
      free(w); return NULL;
    }
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN,
                    NULL);
    free(w);
  }
#else
  h = CreateFileA(utf8_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                  OPEN_EXISTING,
                  FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN,
                  NULL);
#endif
  if (h == INVALID_HANDLE_VALUE) return NULL;
  if (GetFileType(h) != FILE_TYPE_DISK
      || fastent_win32_filesize_(h, &fsz) != 0) {
    CloseHandle(h);
    return NULL;
  }
  *out_size = fsz;
  return h;
}

void fastent_win32_set_console_fg(int cls) {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h == NULL || h == INVALID_HANDLE_VALUE) return;
  static WORD initial_attrs = 0;
  static int  initial_saved = 0;
  if (!initial_saved) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(h, &info)) {
      initial_attrs = info.wAttributes;
      initial_saved = 1;
    } else return;
  }
  WORD attr;
  if (cls < 0) {
    attr = initial_attrs;
  } else {
    /*  4th entry keeps the `cls & 3` mask in bounds; classes 0..2.  */
    static const WORD fg[4] = {
      FOREGROUND_INTENSITY,
      FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
      FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
      FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE
    };
    WORD bg_bits = initial_attrs & 0x00F0;
    attr = (WORD)(fg[cls & 3] | bg_bits);
  }
  SetConsoleTextAttribute(h, attr);
}

void fastent_win32_set_console_sev(int sev) {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h == NULL || h == INVALID_HANDLE_VALUE) return;
  static WORD initial_attrs = 0;
  static int  initial_saved = 0;
  if (!initial_saved) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(h, &info)) {
      initial_attrs = info.wAttributes;
      initial_saved = 1;
    } else return;
  }
  WORD attr;
  if (sev < 0) {
    attr = initial_attrs;
  } else {
    static const WORD sv[4] = {
      FOREGROUND_GREEN | FOREGROUND_INTENSITY,
      FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
      FOREGROUND_RED | FOREGROUND_INTENSITY,
      FOREGROUND_INTENSITY
    };
    WORD bg_bits = initial_attrs & 0x00F0;
    attr = (WORD)(sv[sev & 3] | bg_bits);
  }
  SetConsoleTextAttribute(h, attr);
}

void fastent_win32_mmap_prefetch(void * base, u64 size) {
  /*  PrefetchVirtualMemory is Win 8+; resolve at runtime so the
      binary still loads on Vista/7.  */
  typedef struct _FE_MEM_RANGE {
    PVOID VirtualAddress;  SIZE_T NumberOfBytes;
  } FE_MEM_RANGE;
  typedef BOOL (WINAPI * PFN_PVM)(HANDLE, ULONG_PTR, FE_MEM_RANGE *, ULONG);
  static PFN_PVM pfn = NULL;
  static int     resolved = 0;
  FE_MEM_RANGE   r;
  if (!resolved) {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
      void * raw = (void *) GetProcAddress(k32, "PrefetchVirtualMemory");
      pfn = (PFN_PVM) raw;
    }
    resolved = 1;
  }
  if (!pfn || !base || size == 0) return;
  r.VirtualAddress = base;
  r.NumberOfBytes  = (SIZE_T) size;
  pfn(GetCurrentProcess(), 1, &r, 0);
}

#endif
