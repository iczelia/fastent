/*  fastent: recursive directory walker port.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#ifndef FASTENT_PORT_WALK_H
#define FASTENT_PORT_WALK_H

#include "common.h"

/*  Called once per regular file encountered.  Returns 0 to continue,
    non-zero to abort the walk (fastent_walk returns that value).  */
typedef int (*fastent_walk_fn)(const char * path, void * ctx);

/*  Recursively walk `root`.  If `root` itself is a regular file, fn is
    called once on it and the walk returns.  Symlinks are followed only
    for the root; subdirectory traversal does not follow symlinks.
    Returns 0 on success, -1 with errno set on OS error, or fn's
    abort-code if non-zero.  Individual unreadable entries are skipped
    silently.  */
int  fastent_walk(const char * root, fastent_walk_fn fn, void * ctx);

#endif
