/*  Copyright (C) 2023-2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.  */

#ifndef FASTENT_PORT_WALK_H
#define FASTENT_PORT_WALK_H

#include "common.h"

/*  Called once per regular file encountered.  Returns 0 to continue,
    non-zero to abort the walk (fastent_walk returns that value).  */
typedef int (*fastent_walk_fn)(const char * path, void * ctx);

/*  Recursively walk `root`.  Symlinks followed only for the root, not
    in subdirectory traversal.  Returns 0 on success, -1 with errno set
    on OS error, or fn's abort-code.  Unreadable entries skipped.  */
int  fastent_walk(const char * root, fastent_walk_fn fn, void * ctx);

#endif
