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
    sh "$root/scripts/remove_path.sh" "$root/build" "$root/dist"
    ;;
  dist)
    sh "$root/scripts/remove_path.sh" "$root/dist"
    ;;
  *)
    printf '%s\n' "clean: unsupported scope: $1" >&2
    exit 2
    ;;
esac
