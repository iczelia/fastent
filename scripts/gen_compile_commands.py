#  fastent: regenerate compile_commands.json for clangd / IDE tooling.
#  Usage:  make clean && make -n V=1 | python3 scripts/gen_compile_commands.py "$PWD" > compile_commands.json
#  (the autotools build has no native compdb export and bear is not vendored.)
import sys, re, json, shlex
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
    out.append({"directory": root, "file": src, "arguments": args})
json.dump(out, sys.stdout, indent=1)
sys.stdout.write("\n")
