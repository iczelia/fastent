#!/usr/bin/env bash
# Bundle all staged binaries into one archive set per OS family.
#
#   build/ci/bundle-release.sh
#
# Inputs:
#   staged/        every fastent-<id>[.exe|.js] from the build jobs
#   fastent-src/   unpacked source dist (for ver + README/COPYING/...)
#
# Produces, under dist/, for each family that has binaries:
#   fastent-<ver>-<family>.tar  .tar.gz  .tar.bz2  .tar.bz3
#                               .tar.xz  .tar.zst  .zip
#
# Family is the <id> prefix: linux-* windows-* macos-* wasi-* dos-*.
# Every per-arch binary keeps its unique name inside the archive, so
# fastent-1.2-windows.zip holds fastent-windows-x86_64.exe,
# fastent-windows-aarch64.exe, ... alongside the docs.
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
