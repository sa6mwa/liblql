#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lua_bin="${LUA:-lua}"
lua_cpath="${LUA_CPATH:-$root/build/debug/?.so;$root/build/debug/?/core.so;;}"
lib_path="$root/build/debug:$root/.cache/deps/x86_64-linux-gnu/install/lib"

if ! command -v "$lua_bin" >/dev/null 2>&1; then
  printf 'lua-test: lua executable not found: %s\n' "$lua_bin" >&2
  exit 1
fi

if [ ! -f "$root/build/debug/lql/core.so" ]; then
  printf 'lua-test: lql.core module not found; run make build-debug\n' >&2
  exit 1
fi

LUA_PATH="$root/lua/?.lua;$root/lua/?/init.lua;;" \
LUA_CPATH="$lua_cpath" \
LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$lib_path" \
DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-}:$lib_path" \
LQL_REQUIRE_CORE=1 \
  "$lua_bin" "$root/lua/tests/lql_smoke.lua"

LUA_PATH="$root/lua/?.lua;$root/lua/?/init.lua;;" \
LUA_CPATH="$lua_cpath" \
LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$lib_path" \
DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-}:$lib_path" \
LQL_REQUIRE_CORE=1 \
  "$lua_bin" "$root/lua/tests/lql_core_smoke.lua"
