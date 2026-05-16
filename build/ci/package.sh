#!/usr/bin/env bash
# Multi-format release packaging.
#
#   build/ci/package.sh <built-binary-path> <platform-id>
#
# Produces, under dist/:
#   fastent-<ver>-<id>.tar  .tar.gz  .tar.bz2  .tar.bz3  .tar.xz
#                           .tar.zst .zip
#
# The binary is renamed to fastent-<id>[.exe|.js] inside every archive
# so downloads of different platforms never collide. Archives sort by
# os-arch on the release page, grouping each platform together.
set -euo pipefail

bin=$1
id=$2

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

ver=$(ls -d fastent-*/ | head -1 | sed 's,/,,;s/^fastent-//')
base="fastent-${ver}-${id}"
case "$id" in
  windows-*|dos-*) exe="fastent-${id}.exe" ;;
  wasi-*)          exe="fastent-${id}.js"  ;;
  *)               exe="fastent-${id}"     ;;
esac

stage="dist/${base}"
mkdir -p "$stage"
cp "$bin" "$stage/$exe"
cp fastent-src/README.md fastent-src/COPYING fastent-src/NEWS \
   fastent-src/fastent.1 "$stage/"

cd dist
tar  -cf  "${base}.tar"        "${base}"
gzip  -9  -c "${base}.tar" >   "${base}.tar.gz"
bzip2 -9  -c "${base}.tar" >   "${base}.tar.bz2"
xz    -9  -c "${base}.tar" >   "${base}.tar.xz"
zstd  -19 -q -c "${base}.tar" > "${base}.tar.zst"
bzip3     -c "${base}.tar" >   "${base}.tar.bz3"
zip   -9 -q -r "${base}.zip"   "${base}"
ls -l "${base}".tar "${base}".tar.* "${base}".zip
