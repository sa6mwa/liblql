#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
case "$root" in
  ""|/|"$HOME")
    printf '%s\n' "clean: refusing unsafe root: $root" >&2
    exit 1
    ;;
esac
rm -rf "$root/build"
