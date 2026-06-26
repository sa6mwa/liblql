#!/bin/sh
set -eu

check_modes() {
  root=$1

  if [ ! -d "$root" ]; then
    printf 'script modes check: missing root: %s\n' "$root" >&2
    return 2
  fi

  missing=$(
    find "$root/scripts" -maxdepth 1 -type f -name '*.sh' \
      ! -perm -111 -print 2>/dev/null || true
  )
  if [ -n "$missing" ]; then
    printf 'script modes check: shell entrypoints must be executable\n' >&2
    printf '%s\n' "$missing" >&2
    return 1
  fi

  return 0
}

if [ "${1:-}" = "--fixtures" ]; then
  tmp=${TMPDIR:-/tmp}/liblql-script-modes.$$
  trap 'rm -rf "$tmp"' EXIT HUP INT TERM
  mkdir -p "$tmp/scripts"

  printf '%s\n' '#!/bin/sh' 'exit 0' >"$tmp/scripts/ok.sh"
  chmod +x "$tmp/scripts/ok.sh"
  if ! check_modes "$tmp"; then
    printf 'script modes fixture: executable script failed\n' >&2
    exit 1
  fi

  printf '%s\n' '#!/bin/sh' 'exit 0' >"$tmp/scripts/bad.sh"
  chmod -x "$tmp/scripts/bad.sh"
  if check_modes "$tmp" >/dev/null 2>&1; then
    printf 'script modes fixture: non-executable script passed\n' >&2
    exit 1
  fi
  exit 0
fi

if [ "$#" -ne 1 ]; then
  printf 'usage: %s ROOT\n' "$0" >&2
  exit 2
fi

check_modes "$1"
