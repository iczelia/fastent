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

#ifndef FASTENT_CLI_H
#define FASTENT_CLI_H

#include "fastent-options.h"

/*  Returns 0 on success, -1 on parse error; help and version exit.  */
int  fastent_parse_args(int argc, char ** argv, fastent_options * o);

#endif
