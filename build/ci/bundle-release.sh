#!/usr/bin/env bash
#  Copyright (C) 2023-2026 Kamila Szewczyk
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, version 3.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program. If not, see <http://www.gnu.org/licenses/>.

#  Bundle staged binaries by operating-system family.
set -euo pipefail

# Ubuntu CI runners: pull any missing compressor.
need=""
for t in tar gzip bzip2 xz zstd bzip3 zip; do
  command -v "$t" >/dev/null 2>&1 || need="$need $t"
done
if [ -n "$need" ]; then
  pkgs=$(printf '%s\n' $need | sed 's/^xz$/xz-utils/' | tr '\n' ' ')
  sudo apt-get update -qq
  sudo apt-get install -y --no-install-recommends $pkgs
fi

ver=
for d in fastent-*/; do ver=${d#fastent-}; ver=${ver%/}; break; done
[ -n "$ver" ] || { echo "no unpacked fastent-*/ source tree" >&2; exit 1; }

mkdir -p dist
for family in linux windows macos wasi dos; do
  set -- staged/fastent-"${family}"-*
  [ -e "$1" ] || continue

  base="fastent-${ver}-${family}"
  stage="dist/${base}"
  mkdir -p "$stage"
  cp "$@" "$stage/"
  cp fastent-src/README.md fastent-src/COPYING fastent-src/NEWS \
     fastent-src/fastent.1 "$stage/"

  ( cd dist
    tar  -cf  "${base}.tar"         "${base}"
    gzip  -9  -c "${base}.tar"   >  "${base}.tar.gz"
    bzip2 -9  -c "${base}.tar"   >  "${base}.tar.bz2"
    xz    -9  -c "${base}.tar"   >  "${base}.tar.xz"
    zstd  -19 -q -c "${base}.tar" > "${base}.tar.zst"
    bzip3     -c "${base}.tar"   >  "${base}.tar.bz3"
    zip   -9 -q -r "${base}.zip"    "${base}"
    rm -rf "${base}"
    ls -l "${base}".tar "${base}".tar.* "${base}".zip )
done
