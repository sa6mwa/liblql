#!/bin/sh
set -eu

usage() {
  printf 'usage: clean.sh [--dist|--fixtures]\n' >&2
}

safe_remove_path() {
  root=$1
  path=$2
  case "$root" in
    ""|/|"$HOME")
      printf 'clean: refusing unsafe root: %s\n' "$root" >&2
      exit 1
      ;;
  esac
  case "$path" in
    ""|/|"$root"|"$HOME"|..|../*|*/..|*/../*)
      printf 'clean: refusing unsafe generated path: %s\n' "$path" >&2
      exit 1
      ;;
  esac
  case "$path" in
    "$root"/build|"$root"/dist|"$root"/.cache|"$root"/lql|"$root"/lua/*.o)
      rm -rf "$path"
      ;;
    *)
      printf 'clean: refusing path outside generated state: %s\n' "$path" >&2
      exit 1
      ;;
  esac
}

clean_root() {
  root=$1
  safe_remove_path "$root" "${root}/build"
  safe_remove_path "$root" "${root}/dist"
  safe_remove_path "$root" "${root}/.cache"
  safe_remove_path "$root" "${root}/lql"
  for obj in "${root}"/lua/*.o; do
    [ -e "$obj" ] || continue
    safe_remove_path "$root" "$obj"
  done
}

clean_dist() {
  root=$1
  safe_remove_path "$root" "${root}/dist"
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
  if (safe_remove_path "$tmp" "$tmp") >/dev/null 2>&1; then
    printf 'clean fixture: unsafe root removal was accepted\n' >&2
    exit 1
  fi
  if (safe_remove_path "$tmp" "$tmp/src") >/dev/null 2>&1; then
    printf 'clean fixture: non-generated path removal was accepted\n' >&2
    exit 1
  fi
  mkdir -p "$tmp/dist"
  printf 'generated\n' >"$tmp/dist/state"
  clean_dist "$tmp"
  if [ -e "$tmp/dist" ]; then
    printf 'clean fixture: clean-dist left dist behind\n' >&2
    exit 1
  fi
  exit 0
fi

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
case "${1:-}" in
  "")
    clean_root "$root"
    ;;
  --dist)
    clean_dist "$root"
    ;;
  *)
    usage
    exit 2
    ;;
esac
