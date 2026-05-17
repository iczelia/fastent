/*  fastent: POSIX recursive directory walker (opendir + readdir).

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "port-walk.h"

#ifndef _WIN32

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static char * join_path_(const char * a, const char * b) {
  size_t la = strlen(a);
  size_t lb = strlen(b);
  int sep  = (la > 0 && a[la - 1] != '/');
  char * out = (char *) malloc(la + (sep ? 1 : 0) + lb + 1);
  if (!out) return NULL;
  memcpy(out, a, la);
  if (sep) out[la++] = '/';
  memcpy(out + la, b, lb + 1);
  return out;
}

/*  Recursion-depth backstop against pathologically deep trees
    (symlinked dirs are already excluded by the lstat below).  */
#define FASTENT_WALK_MAX_DEPTH 4096

static int walk_dir_(const char * path, fastent_walk_fn fn, void * ctx,
                     int depth) {
  if (depth > FASTENT_WALK_MAX_DEPTH) return 0;
  DIR * d = opendir(path);
  if (!d) return 0;  /*  Skip unreadable directories.  */
  int rc = 0;
  struct dirent * ent;
  while ((ent = readdir(d)) != NULL) {
    const char * name = ent->d_name;
    if (name[0] == '.' && (name[1] == '\0'
                       || (name[1] == '.' && name[2] == '\0'))) continue;
    char * full = join_path_(path, name);
    if (!full) { rc = -1; break; }
    struct stat st;
    if (lstat(full, &st) != 0) { free(full); continue; }
    if (S_ISREG(st.st_mode)) {
      rc = fn(full, ctx);
      free(full);
      if (rc != 0) break;
    } else if (S_ISDIR(st.st_mode)) {
      rc = walk_dir_(full, fn, ctx, depth + 1);
      free(full);
      if (rc != 0) break;
    } else {
      free(full);
    }
  }
  closedir(d);
  return rc;
}

int fastent_walk(const char * root, fastent_walk_fn fn, void * ctx) {
  if (!root || !fn) { errno = EINVAL; return -1; }
  struct stat st;
  if (stat(root, &st) != 0) return -1;
  if (S_ISREG(st.st_mode)) return fn(root, ctx);
  if (S_ISDIR(st.st_mode)) return walk_dir_(root, fn, ctx, 0);
  errno = EINVAL;
  return -1;
}

#endif
