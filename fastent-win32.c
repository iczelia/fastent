/*  fastent: Win32 Unicode-path helpers.  Built only on Windows hosts.

    Copyright (C) 2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "fastent-win32.h"

#ifdef _WIN32

#ifndef _WIN32_WINNT
  /*  Vista baseline by default; the few calls we use here
      (Set*ConsoleCP, GetCommandLineW, MultiByteToWideChar /
      WideCharToMultiByte) all pre-date Vista.  The user can override
      via -D_WIN32_WINNT=0x0500 to retarget Windows 2000.  */
  #define _WIN32_WINNT 0x0600
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <io.h>
#include <wchar.h>

/*  UTF-8 / UTF-16 heap conversion helpers.  */

static wchar_t * utf8_to_wide(const char * s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0) return NULL;
  wchar_t * w = (wchar_t *) malloc((size_t) n * sizeof(wchar_t));
  if (!w) return NULL;
  if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
    free(w); return NULL;
  }
  return w;
}

static char * wide_to_utf8(const wchar_t * w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  if (n <= 0) return NULL;
  char * s = (char *) malloc((size_t) n);
  if (!s) return NULL;
  if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
    free(s); return NULL;
  }
  return s;
}

/*  Manual UTF-16 command-line splitter implementing the MSVCRT C
    command-line parsing rules so the user sees the same argv they
    would on a UTF-8 CRT:

      Outside quotes, runs of spaces/tabs separate args.
      `"..."` quotes; whitespace inside is kept verbatim.
      Backslashes before `"`: 2N backslashes resolve to N literal
        backslashes and the `"` toggles quote state; 2N+1 backslashes
        resolve to N literal backslashes and the `"` becomes a
        literal `"`.
      Inside a quoted string, `""` is a literal `"`.

    Two-pass: first pass counts argc, second pass fills the array.  */

static int grow_buf_w(wchar_t ** pbuf, size_t * pbcap) {
  if (*pbcap > (size_t) -1 / (2 * sizeof(wchar_t))) return 0;
  size_t nc = *pbcap * 2;
  wchar_t * nb = (wchar_t *) realloc(*pbuf, nc * sizeof(wchar_t));
  if (!nb) return 0;
  *pbuf = nb; *pbcap = nc;
  return 1;
}

static int split_cmdline_w(const wchar_t * cmd, wchar_t *** out_wargv) {
  int argc = 0;
  wchar_t ** wargv = NULL;
  wchar_t * buf = NULL;
  for (int pass = 0; pass < 2; pass++) {
    const wchar_t * p = cmd;
    if (pass == 1) {
      wargv = (wchar_t **) calloc((size_t) argc + 1, sizeof(wchar_t *));
      if (!wargv) return -1;
    }
    argc = 0;
    while (*p) {
      while (*p == L' ' || *p == L'\t') p++;
      if (!*p) break;
      size_t blen = 0, bcap = 0;
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
          int nbs = 0;
          while (*p == L'\\') { nbs++; p++; }
          if (*p == L'"') {
            int slashes = nbs / 2;
            if (pass == 1) {
              for (int i = 0; i < slashes; i++) {
                if (blen + 1 >= bcap && !grow_buf_w(&buf, &bcap)) goto fail;
                buf[blen++] = L'\\';
              }
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
              for (int i = 0; i < nbs; i++) {
                if (blen + 1 >= bcap && !grow_buf_w(&buf, &bcap)) goto fail;
                buf[blen++] = L'\\';
              }
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
    for (int j = 0; j < argc; j++) free(wargv[j]);
    free(wargv);
  }
  return -1;
}

/*  Public API.  */

int fastent_win32_argv_utf8(int * argc_out, char *** argv_out) {
  wchar_t ** wargv = NULL;
  int wargc = split_cmdline_w(GetCommandLineW(), &wargv);
  if (wargc < 0) return -1;

  char ** argv = (char **) calloc((size_t) wargc + 1, sizeof(char *));
  if (!argv) {
    for (int i = 0; i < wargc; i++) free(wargv[i]);
    free(wargv);
    return -1;
  }
  for (int i = 0; i < wargc; i++) {
    argv[i] = wide_to_utf8(wargv[i]);
    free(wargv[i]);
    if (!argv[i]) {
      for (int j = 0; j < i; j++) free(argv[j]);
      free(argv);
      for (int j = i + 1; j < wargc; j++) free(wargv[j]);
      free(wargv);
      return -1;
    }
  }
  free(wargv);
  *argc_out = wargc;
  *argv_out = argv;
  return 0;
}

void fastent_win32_init_console(void) {
  /*  Both succeed on Windows 2000+ when a console is attached; on
      redirected stdout/stderr the calls are silent no-ops.  The
      output CP affects how WriteFile bytes are rendered by the
      console host; the input CP is set for symmetry but doesn't
      matter to fastent (stdin is _setmode'd to _O_BINARY and bytes
      pass through ReadFile, not ReadConsoleA).  */
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
}

void fastent_win32_set_stdin_binary(void) {
  _setmode(_fileno(stdin), _O_BINARY);
}

int fastent_win32_open_utf8(const char * path, int flags) {
  if (!path) { errno = EINVAL; return -1; }
  wchar_t * w = utf8_to_wide(path);
  if (!w) {
    /*  MultiByteToWideChar already set GetLastError; map roughly.  */
    errno = EINVAL;
    return -1;
  }
  int fd = _wopen(w, flags);
  int saved_errno = errno;
  free(w);
  errno = saved_errno;
  return fd;
}

int fastent_win32_close(int fd) {
  return _close(fd);
}

long fastent_win32_read(int fd, void * buf, size_t n) {
  /*  _read's count argument is unsigned int; cap to INT_MAX-ish so
      we don't accidentally truncate to zero on very large n.  Callers
      in fastent never request more than FASTENT_STREAM_BUF (2 MiB)
      so this is safety, not a likely path.  */
  unsigned int cap = (n > 0x7FFFFFFFu) ? 0x7FFFFFFFu : (unsigned int) n;
  return (long) _read(fd, buf, cap);
}

#endif
