#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
for path in "$@"; do
  [ -n "$path" ] || { printf '%s\n' 'output path: empty path' >&2; exit 1; }
  absolute=$(realpath -m -- "$path")
  case "$absolute" in
    "$root/build"|"$root/build/"*|"$root/dist"|"$root/dist/"*|"$root/.cache"|"$root/.cache/"*) ;;
    *) printf 'output path: outside this checkout generated directories: %s\n' "$path" >&2; exit 1 ;;
  esac
done
