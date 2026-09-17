#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lua_module_dir=${LQL_LUA_MODULE_DIR:-$root/build/debug-lua/lua}
lua_runner=${LQL_LUA_RUNNER:-$root/build/debug-lua/lql_lua_runner}

if [ ! -x "$lua_runner" ]; then
  printf 'lua-test: Bootlin Lua runner not found: %s\n' "$lua_runner" >&2
  exit 1
fi
if [ ! -f "$lua_module_dir/lql/core.so" ]; then
  printf 'lua-test: lql.core module not found in %s; run make build-debug-lua\n' "$lua_module_dir" >&2
  exit 1
fi

LUA_PATH="$root/lua/?.lua;$root/lua/?/init.lua;;" \
LUA_CPATH="$lua_module_dir/?.so;$lua_module_dir/?/core.so;;" \
  "$lua_runner" "$root/lua/tests/lql_smoke.lua"
