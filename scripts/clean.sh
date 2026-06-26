#!/bin/sh
set -eu

clean_root() {
  root=$1
  rm -rf "${root}/build" "${root}/dist" "${root}/.cache" "${root}/lql"
  rm -f "${root}"/lua/*.o
}

if [ "${1:-}" = "--fixtures" ]; then
  tmp=${TMPDIR:-/tmp}/liblql-clean-fixtures.$$
  trap 'rm -rf "$tmp"' EXIT HUP INT TERM
  mkdir -p "$tmp/build" "$tmp/dist" "$tmp/.cache" "$tmp/lql" "$tmp/lua"
  printf 'generated\n' >"$tmp/build/state"
  printf 'generated\n' >"$tmp/dist/state"
  printf 'generated\n' >"$tmp/.cache/state"
  printf 'generated\n' >"$tmp/lql/core.so"
  printf 'generated\n' >"$tmp/lua/lql_core.o"
  printf 'source\n' >"$tmp/lua/lql_core.c"

  clean_root "$tmp"

  for path in "$tmp/build" "$tmp/dist" "$tmp/.cache" "$tmp/lql" \
    "$tmp/lua/lql_core.o"; do
    if [ -e "$path" ]; then
      printf 'clean fixture: generated path survived: %s\n' "$path" >&2
      exit 1
    fi
  done
  if [ ! -f "$tmp/lua/lql_core.c" ]; then
    printf 'clean fixture: source file was removed\n' >&2
    exit 1
  fi
  exit 0
fi

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
clean_root "$root"
