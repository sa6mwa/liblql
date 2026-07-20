#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$root/build/arm64-apple-darwin-release
cache=$build_dir/CMakeCache.txt
ninja=$build_dir/build.ninja
target=arm64-apple-darwin

if ! "$root/scripts/osxcross_available.sh"; then
  printf 'darwin linker route: skipped; osxcross is not ready\n'
  exit 0
fi

ld_path=$("$root/scripts/cpkt-toolchains.sh" discover "$target" |
  sed -n 's/^ld=//p')
if [ -z "$ld_path" ] || [ ! -x "$ld_path" ]; then
  printf 'darwin linker route: missing target linker for %s\n' "$target" >&2
  exit 1
fi

if ! cmake --preset arm64-apple-darwin-release; then
  printf 'darwin linker route: configure failed for %s\n' "$target" >&2
  exit 1
fi
if ! cmake --build --preset arm64-apple-darwin-release; then
  printf 'darwin linker route: build failed for %s\n' "$target" >&2
  exit 1
fi

if grep -R -- '-fuse-ld=' "$cache" "$ninja" >/dev/null 2>&1; then
  printf 'darwin linker route: deprecated -fuse-ld=/path remains\n' >&2
  grep -R -- '-fuse-ld=' "$cache" "$ninja" >&2
  exit 1
fi

if ! grep -F -- "--ld-path=$ld_path" "$cache" >/dev/null ||
  ! grep -F -- "--ld-path=$ld_path" "$ninja" >/dev/null; then
  printf 'darwin linker route: generated build does not use --ld-path=%s\n' \
    "$ld_path" >&2
  exit 1
fi

printf 'darwin linker route: verified --ld-path=%s\n' "$ld_path"
