#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${LUA:-}" ]; then
  lua_bin=$LUA
elif command -v lua5.5 >/dev/null 2>&1; then
  lua_bin=lua5.5
else
  lua_bin=lua
fi
lua_cpath="${LUA_CPATH:-$root/build/debug/?.so;$root/build/debug/?/core.so;;}"
lib_path="$root/build/debug:$root/.cache/deps/x86_64-linux-gnu/install/lib"

if ! command -v "$lua_bin" >/dev/null 2>&1; then
  printf 'lua-test: lua executable not found: %s\n' "$lua_bin" >&2
  exit 1
fi
lua_version=$("$lua_bin" -e 'print(_VERSION)')
if [ "$lua_version" != "Lua 5.5" ]; then
  printf 'lua-test: unsupported Lua runtime: %s\n' "$lua_version" >&2
  printf 'lua-test: liblql Lua facade supports Lua 5.5 only\n' >&2
  exit 1
fi
if [ "${LQL_LUA_RUNTIME_CHECK_ONLY:-0}" = "1" ]; then
  exit 0
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
