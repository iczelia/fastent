#!/usr/bin/env python3
"""Convert verbose make output to a clang compilation database."""

import json
import re
import shlex
import sys


root = sys.argv[1]
out = []
for line in sys.stdin:
    line = line.strip()
    if not re.search(r'(^|/| )(gcc|cc|clang)\b', line) or ' -c ' not in line:
        continue
    line = re.sub(r"`test -f '[^']+' \|\| echo '\./'`", "", line)
    m = re.search(r'(\S+\.c)\s*$', line)
    if not m:
        continue
    src = m.group(1)
    try:
        args = shlex.split(line)
    except ValueError:
        continue
    out.append({"arguments": args, "directory": root, "file": src})
json.dump(out, sys.stdout, indent=1)
sys.stdout.write("\n")
