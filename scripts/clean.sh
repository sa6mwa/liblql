#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
case "$root" in
  ""|/|"$HOME")
    printf '%s\n' "clean: refusing unsafe root: $root" >&2
    exit 1
    ;;
esac
case "${1:-all}" in
  all)
    rm -rf "$root/build" "$root/dist"
    ;;
  dist)
    rm -rf "$root/dist"
    ;;
  *)
    printf '%s\n' "clean: unsupported scope: $1" >&2
    exit 2
    ;;
esac
