#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$root/build/lua-rock-toolchain-override-check

if ! command -v gcc >/dev/null 2>&1; then
  printf '%s\n' 'lua-rock override: gcc is required to exercise the documented CC=gcc path' >&2
  exit 1
fi

sh "$root/scripts/remove_path.sh" "$work"
mkdir -p "$work"

env LIBLQL_TOOLCHAIN_OVERRIDE=1 CC=gcc \
  CPKT_TOOLCHAIN_CACHE="$work/empty-toolchain-cache" \
  LQL_LUA_SDK_PREFIX="$work/sdk" LQL_LUAROCKS_TREE="$work/luarocks" \
  make -C "$root" lua-rock

if [ -e "$work/empty-toolchain-cache" ]; then
  printf '%s\n' 'lua-rock override: unexpectedly consulted the Bootlin toolchain cache' >&2
  exit 1
fi

printf '%s\n' 'lua-rock override: caller-supplied compiler path passed'
