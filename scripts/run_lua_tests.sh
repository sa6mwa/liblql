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
lua_module_dir=${LQL_LUA_MODULE_DIR:-$root/build/debug-lua/lua}

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
if [ ! -f "$lua_module_dir/lql/core.so" ]; then
  printf 'lua-test: lql.core module not found in %s; run make build-debug-lua\n' "$lua_module_dir" >&2
  exit 1
fi

LUA_PATH="$root/lua/?.lua;$root/lua/?/init.lua;;" \
LUA_CPATH="$lua_module_dir/?.so;$lua_module_dir/?/core.so;;" \
LD_LIBRARY_PATH="$root/build/debug-lua:${LD_LIBRARY_PATH:-}" \
  "$lua_bin" "$root/lua/tests/lql_smoke.lua"
