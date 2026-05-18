#!/usr/bin/env bash
# Stage one built binary for the family bundle job.
#
#   build/ci/stage-binary.sh <built-binary-path> <platform-id>
#
# Copies the binary into staged/ under its platform-unique name
# (fastent-<id>[.exe|.js]).  bundle-release.sh later groups every
# staged binary by OS family into one archive set per family.
set -euo pipefail

bin=$1
id=$2

case "$id" in
  windows-*|dos-*) name="fastent-${id}.exe" ;;
  wasi-*)          name="fastent-${id}.js"  ;;
  *)               name="fastent-${id}"     ;;
esac

mkdir -p staged
cp "$bin" "staged/$name"
ls -l "staged/$name"
