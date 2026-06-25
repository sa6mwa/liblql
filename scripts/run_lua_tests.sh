#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lua_bin="${LUA:-lua}"
clql="${CLQL_PATH:-$root/build/debug/clql}"

if ! command -v "$lua_bin" >/dev/null 2>&1; then
  printf 'lua-test: lua executable not found: %s\n' "$lua_bin" >&2
  exit 1
fi

if [ ! -x "$clql" ]; then
  printf 'lua-test: clql binary not found; run make build-debug or set CLQL_PATH\n' >&2
  exit 1
fi

LUA_PATH="$root/lua/?.lua;$root/lua/?/init.lua;;" \
CLQL_PATH="$clql" \
  "$lua_bin" "$root/lua/tests/lql_smoke.lua"
