#!/bin/sh
set -eu

root=${1:?usage: check_repository_boundaries.sh ROOT}

if [ "$root" = "--fixtures" ]; then
  tmp=${TMPDIR:-/tmp}/liblql-repository-boundary.$$
  trap 'rm -rf "$tmp"' EXIT HUP INT TERM
  mkdir -p "$tmp"
  printf '%s\n' 'clean reference to pkt.systems/lql is allowed' \
    >"$tmp/clean.txt"
  if ! sh "$0" "$tmp"; then
    printf 'repository boundary fixture: clean tree failed\n' >&2
    exit 1
  fi
  printf '%s%s\n' 'forbidden adjacent checkout ../' 'lql' >"$tmp/bad.txt"
  if sh "$0" "$tmp" >/dev/null 2>&1; then
    printf 'repository boundary fixture: adjacent checkout reference passed\n' >&2
    exit 1
  fi
  rm -f "$tmp/bad.txt"
  printf '%s%s%s\n' 'forbidden local checkout /home/dev/g/' 'lql' '/' \
    >"$tmp/bad.txt"
  if sh "$0" "$tmp" >/dev/null 2>&1; then
    printf 'repository boundary fixture: local checkout reference passed\n' >&2
    exit 1
  fi
  exit 0
fi

if [ ! -d "$root" ]; then
  printf 'repository boundary check: missing root: %s\n' "$root" >&2
  exit 1
fi

hits=$(
  (
    cd "$root"
    find . \
      \( -path './.git' -o -path './build' -o -path './dist' \
      -o -path './deps' -o -path './.cache' -o -path './lql' \) -prune \
      -o -type f \
      ! -name '*.o' ! -name '*.a' ! -name '*.so' ! -name '*.so.*' \
      -exec grep -HEn \
        '(^|[^[:alnum:]_./-])\.\./lql(/|$)|/home/[^[:space:]"]+/g/lql(/|$)|/Users/[^[:space:]"]+/g/lql(/|$)' \
        {} + 2>/dev/null || true
  )
)

if [ -n "$hits" ]; then
  printf 'repository boundary check: forbidden adjacent lql source reference\n' >&2
  printf '%s\n' "$hits" >&2
  exit 1
fi
