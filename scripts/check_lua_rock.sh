#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tree=${LQL_LUAROCKS_TREE:-$root/build/luarocks}
sdk_prefix=${LQL_LUA_SDK_PREFIX:-$root/build/lua-sdk}
if [ -n "${LUA:-}" ]; then
  lua_bin=$LUA
elif command -v lua5.5 >/dev/null 2>&1; then
  lua_bin=lua5.5
else
  lua_bin=lua
fi

if [ ! -f "$tree/share/lua/5.5/lql/init.lua" ]; then
  printf 'lua-rock: missing installed Lua module in %s\n' "$tree" >&2
  exit 1
fi
if [ ! -f "$tree/lib/lua/5.5/lql/core.so" ]; then
  printf 'lua-rock: missing installed Lua C module in %s\n' "$tree" >&2
  exit 1
fi
if ! readelf -d "$tree/lib/lua/5.5/lql/core.so" |
  grep 'Shared library: \[liblql\.so' >/dev/null; then
  printf 'lua-rock: Lua C module does not depend on shared liblql\n' >&2
  readelf -d "$tree/lib/lua/5.5/lql/core.so" >&2
  exit 1
fi
if nm -D "$tree/lib/lua/5.5/lql/core.so" |
  awk '$2 ~ /^[TDB]$/ && $3 ~ /^lql_/ { found = 1 } END { exit !found }'; then
  printf 'lua-rock: Lua C module exports liblql implementation symbols\n' >&2
  nm -D "$tree/lib/lua/5.5/lql/core.so" >&2
  exit 1
fi
if [ ! -x "$tree/bin/lql.lua" ]; then
  printf 'lua-rock: missing installed lql.lua CLI in %s\n' "$tree" >&2
  exit 1
fi
if [ -e "$root/lua/lql_core.o" ]; then
  printf 'lua-rock: LuaRocks generated object in source tree: lua/lql_core.o\n' >&2
  exit 1
fi

LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
LD_LIBRARY_PATH="$sdk_prefix/lib:${LD_LIBRARY_PATH:-}" \
  "$lua_bin" "$root/lua/tests/lql_smoke.lua"

printf 'lua-rock: installed rock smoke passed\n'
