#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tree=${LQL_LUAROCKS_TREE:-$root/build/luarocks}
sdk_prefix=${LQL_LUA_SDK_PREFIX:-$root/build/lua-sdk}
runner_build=${LQL_LUA_RUNNER_BUILD_DIR:-$root/build/lua-rock-check-runner}

if [ ! -f "$tree/share/lua/5.5/lql/init.lua" ]; then
  printf 'lua-rock: missing installed Lua module in %s\n' "$tree" >&2
  exit 1
fi
if [ ! -f "$tree/lib/lua/5.5/lql/core.so" ]; then
  printf 'lua-rock: missing installed Lua C module in %s\n' "$tree" >&2
  exit 1
fi
if [ ! -f "$sdk_prefix/lib/liblql.so.0" ]; then
  printf 'lua-rock: missing requested SDK shared library: %s/lib/liblql.so.0\n' \
    "$sdk_prefix" >&2
  exit 1
fi
sdk_prefix=$(CDPATH= cd -- "$sdk_prefix" && pwd)
if ! readelf -d "$tree/lib/lua/5.5/lql/core.so" |
  grep 'Shared library: \[liblql\.so' >/dev/null; then
  printf 'lua-rock: Lua C module does not depend on shared liblql\n' >&2
  readelf -d "$tree/lib/lua/5.5/lql/core.so" >&2
  exit 1
fi
if readelf -d "$tree/lib/lua/5.5/lql/core.so" | grep 'RUNPATH' >/dev/null; then
  printf '%s\n' 'lua-rock: Lua module must use transitive RPATH, not RUNPATH' >&2
  exit 1
fi
if ! readelf -d "$tree/lib/lua/5.5/lql/core.so" |
  grep -F "$sdk_prefix/lib" >/dev/null; then
  printf 'lua-rock: Lua module is missing local SDK RPATH: %s/lib\n' "$sdk_prefix" >&2
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
if [ -e "$tree/share/lua/5.5/lql/cli.lua" ]; then
  printf 'lua-rock: lql.cli must not be installed as a Lua facade module\n' >&2
  exit 1
fi
if [ -e "$root/lua/lql_core.o" ]; then
  printf 'lua-rock: LuaRocks generated object in source tree: lua/lql_core.o\n' >&2
  exit 1
fi
if [ -n "${LQL_LUA_RUNNER:-}" ]; then
  lua_runner=$LQL_LUA_RUNNER
else
  sh "$root/scripts/remove_path.sh" "$runner_build"
  cmake -S "$root" -B "$runner_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/cpkt-toolchain.cmake" \
    -DLQL_TARGET_ID=x86_64-linux-gnu \
    -DLQL_BUILD_LUA=ON \
    -DLQL_LUA_SDK_PREFIX="$sdk_prefix" \
    -DLQL_INSTALL=OFF \
    -DBUILD_TESTING=OFF \
    -DLQL_BUILD_DIRECT_PROBE=OFF
  cmake --build "$runner_build" --target lql_lua_runner
  lua_runner=$runner_build/lql_lua_runner
fi
if [ ! -x "$lua_runner" ]; then
  printf 'lua-rock: Lua runner not found: %s\n' "$lua_runner" >&2
  exit 1
fi
requested_sdk_library=$(readlink -f "$sdk_prefix/lib/liblql.so.0")
runner_trace=$(LD_TRACE_LOADED_OBJECTS=1 "$lua_runner")
loaded_sdk_library=$(printf '%s\n' "$runner_trace" |
  sed -n 's/^[[:space:]]*liblql\.so\.0 => \([^[:space:]]*\).*/\1/p' | sed -n '1p')
if [ -n "$loaded_sdk_library" ]; then
  loaded_sdk_library=$(readlink -f "$loaded_sdk_library" 2>/dev/null || :)
fi
if [ "$loaded_sdk_library" != "$requested_sdk_library" ]; then
  printf 'lua-rock: runner does not load the requested SDK: %s/lib/liblql.so.0\n' \
    "$sdk_prefix" >&2
  printf '%s\n' "$runner_trace" >&2
  exit 1
fi

LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
  "$lua_runner" "$root/lua/tests/lql_smoke.lua"

printf 'lua-rock: installed rock smoke passed\n'
