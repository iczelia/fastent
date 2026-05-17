/*  fastent: Win32 recursive directory walker (FindFirstFileW).
    Operates on UTF-8 paths by transcoding to UTF-16 for the
    underlying API and back for the callback.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "port-walk.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifndef FASTENT_WIN_LEGACY

static wchar_t * utf8_to_wide_(const char * s) {
  i32 n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0) return NULL;
  wchar_t * w = (wchar_t *) malloc((sz) n * sizeof(wchar_t));
  if (!w) return NULL;
  if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
    free(w); return NULL;
  }
  return w;
}

static char * wide_to_utf8_(const wchar_t * w) {
  i32 n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  if (n <= 0) return NULL;
  char * s = (char *) malloc((sz) n);
  if (!s) return NULL;
  if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
    free(s); return NULL;
  }
  return s;
}

static wchar_t * join_path_w_(const wchar_t * a, const wchar_t * b) {
  sz la = wcslen(a);
  sz lb = wcslen(b);
  int sep  = (la > 0 && a[la - 1] != L'\\' && a[la - 1] != L'/');
  wchar_t * out = (wchar_t *) malloc((la + (sep ? 1 : 0) + lb + 1)
                                      * sizeof(wchar_t));
  if (!out) return NULL;
  memcpy(out, a, la * sizeof(wchar_t));
  if (sep) out[la++] = L'\\';
  memcpy(out + la, b, (lb + 1) * sizeof(wchar_t));
  return out;
}

static wchar_t * make_pattern_(const wchar_t * dir) {
  sz ld = wcslen(dir);
  int sep  = (ld > 0 && dir[ld - 1] != L'\\' && dir[ld - 1] != L'/');
  wchar_t * out = (wchar_t *) malloc((ld + (sep ? 1 : 0) + 2)
                                      * sizeof(wchar_t));
  if (!out) return NULL;
  memcpy(out, dir, ld * sizeof(wchar_t));
  if (sep) out[ld++] = L'\\';
  out[ld++] = L'*';
  out[ld]   = L'\0';
  return out;
}

/*  Recursion-depth backstop, matching the POSIX walker.  */
#define FASTENT_WALK_MAX_DEPTH 4096

static int walk_dir_w_(const wchar_t * dir, fastent_walk_fn fn, void * ctx,
                       int depth) {
  if (depth > FASTENT_WALK_MAX_DEPTH) return 0;
  wchar_t * pat = make_pattern_(dir);
  if (!pat) return -1;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(pat, &fd);
  free(pat);
  if (h == INVALID_HANDLE_VALUE) return 0;
  int rc = 0;
  do {
    const wchar_t * name = fd.cFileName;
    if (name[0] == L'.' && (name[1] == L'\0'
                       || (name[1] == L'.' && name[2] == L'\0'))) continue;
    wchar_t * full = join_path_w_(dir, name);
    if (!full) { rc = -1; break; }
    /*  Skip reparse-point directories (junctions / symlinks): the
        POSIX walker drops dir symlinks via lstat, and following them
        risks an unbounded self-referential loop.  */
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      rc = walk_dir_w_(full, fn, ctx, depth + 1);
      free(full);
    } else if (!(fd.dwFileAttributes
                 & (FILE_ATTRIBUTE_DIRECTORY
                    | FILE_ATTRIBUTE_REPARSE_POINT))) {
      char * u8 = wide_to_utf8_(full);
      free(full);
      if (!u8) { rc = -1; break; }
      rc = fn(u8, ctx);
      free(u8);
    } else {
      free(full);
    }
    if (rc != 0) break;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return rc;
}

int fastent_walk(const char * root, fastent_walk_fn fn, void * ctx) {
  if (!root || !fn) { errno = EINVAL; return -1; }
  wchar_t * wroot = utf8_to_wide_(root);
  if (!wroot) { errno = EINVAL; return -1; }
  DWORD attr = GetFileAttributesW(wroot);
  int rc;
  if (attr == INVALID_FILE_ATTRIBUTES) {
    free(wroot);
    errno = ENOENT;
    return -1;
  }
  if (attr & FILE_ATTRIBUTE_DIRECTORY) {
    rc = walk_dir_w_(wroot, fn, ctx, 0);
  } else {
    rc = fn(root, ctx);
  }
  free(wroot);
  return rc;
}

#else  /*  FASTENT_WIN_LEGACY: Win95 narrow API.  */

int fastent_walk(const char * root, fastent_walk_fn fn, void * ctx) {
  /*  Win95 stub: no recursive walk.  */
  (void) root; (void) fn; (void) ctx;
  errno = ENOSYS;
  return -1;
}

#endif

#endif  /*  _WIN32  */
