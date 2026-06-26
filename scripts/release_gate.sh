#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MAKE_CMD=${MAKE:-make}

printf '%s\n' 'release: cleaning generated state'
"$ROOT_DIR/scripts/clean.sh"

printf '%s\n' 'release: running full local test gate'
"$MAKE_CMD" -C "$ROOT_DIR" test-all

printf '%s\n' 'release: running benchmark smoke gate'
"$MAKE_CMD" -C "$ROOT_DIR" bench-check

printf '%s\n' 'release: running scalable memory benchmark gate'
"$MAKE_CMD" -C "$ROOT_DIR" bench-memory-check

printf '%s\n' 'release: building and verifying release matrix'
"$MAKE_CMD" -C "$ROOT_DIR" release-matrix

manifest=$("$ROOT_DIR/scripts/package.sh" print-manifest 2>/dev/null || true)
if [ -z "$manifest" ]; then
  version=$("$ROOT_DIR/scripts/release_version.sh")
  manifest="$ROOT_DIR/dist/liblql-${version}-CHECKSUMS"
fi
if [ ! -f "$manifest" ]; then
  printf 'release: checksum manifest missing after release matrix: %s\n' \
    "$manifest" >&2
  exit 1
fi

printf 'release: verified checksum manifest %s\n' "$manifest"
