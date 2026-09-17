#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
build_dir=${LQL_DEVELOPMENT_RUNTIME_BUILD_DIR:-$root/build/debug}
lua_build_dir=${LQL_LUA_DEVELOPMENT_RUNTIME_BUILD_DIR:-$root/build/debug-lua}
lua_sdk_prefix=${LQL_LUA_SDK_PREFIX:-$root/build/lua-sdk}
resolver=$root/scripts/cpkt-toolchains.sh

if [ ! -f "$build_dir/CMakeCache.txt" ]; then
  printf 'development runtime: missing configured build: %s\n' "$build_dir" >&2
  exit 1
fi

description=$($resolver discover x86_64-linux-gnu)
value() {
  printf '%s\n' "$description" | sed -n "s/^$1=//p"
}

cc=$(value cc)
readelf_tool=$(value readelf)
loader=$(value dynamic_loader)
runtime_dirs=$(value runtime_library_dirs)
configured_cc=$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$build_dir/CMakeCache.txt")

if [ "$configured_cc" != "$cc" ]; then
  printf 'development runtime: configured compiler is not the selected Bootlin compiler\nexpected=%s\nactual=%s\n' \
    "$cc" "$configured_cc" >&2
  exit 1
fi
if [ ! -x "$readelf_tool" ] || [ -z "$loader" ] || [ -z "$runtime_dirs" ]; then
  printf '%s\n' 'development runtime: incomplete Bootlin resolver metadata' >&2
  exit 1
fi

check_runtime() {
  path=$1
  extra_runtime_dir=${2:-}
  if [ ! -x "$path" ]; then
    printf 'development runtime: missing local executable: %s\n' "$path" >&2
    exit 1
  fi
  if ! "$readelf_tool" -l "$path" | \
    grep -F "Requesting program interpreter: $loader]" >/dev/null; then
    printf 'development runtime: wrong interpreter in %s\n' "$path" >&2
    "$readelf_tool" -l "$path" >&2
    exit 1
  fi
  dynamic=$($readelf_tool -d "$path")
  if printf '%s\n' "$dynamic" | grep -E 'RUNPATH' >/dev/null; then
    printf 'development runtime: %s uses RUNPATH; development paths must be transitive RPATH\n' \
      "$path" >&2
    exit 1
  fi
  old_ifs=$IFS
  IFS=:
  set -- $runtime_dirs
  IFS=$old_ifs
  for runtime_dir
  do
    if ! printf '%s\n' "$dynamic" | grep -F "$runtime_dir" >/dev/null; then
      printf 'development runtime: %s is missing Bootlin RPATH %s\n' \
        "$path" "$runtime_dir" >&2
      exit 1
    fi
  done
  if [ -n "$extra_runtime_dir" ] && ! printf '%s\n' "$dynamic" | \
    grep -F "$extra_runtime_dir" >/dev/null; then
    printf 'development runtime: %s is missing local SDK RPATH %s\n' \
      "$path" "$extra_runtime_dir" >&2
    exit 1
  fi
}

for binary in \
  lql_filter_file_spooled_example \
  lql_rewrite_file_inline_spooled_example \
  lql_json_scan_test \
  lql_direct_stream_test \
  lql_selector_ast_test \
  lql_direct_probe \
  lql_direct_bench \
  clql
do
  check_runtime "$build_dir/$binary"
done

check_runtime "$lua_build_dir/lql_lua_runner" "$lua_sdk_prefix/lib"
lua_module=$lua_build_dir/lua/lql/core.so
if [ ! -f "$lua_module" ]; then
  printf 'development runtime: missing Lua facade module: %s\n' "$lua_module" >&2
  exit 1
fi
lua_dynamic=$($readelf_tool -d "$lua_module")
if printf '%s\n' "$lua_dynamic" | grep -E 'RPATH|RUNPATH' >/dev/null; then
  printf 'development runtime: Lua facade module must inherit the runner runtime, not embed one\n' >&2
  exit 1
fi
if ! printf '%s\n' "$lua_dynamic" | grep -F 'Shared library: [liblql.so' >/dev/null; then
  printf 'development runtime: Lua facade module is not linked to shared liblql\n' >&2
  exit 1
fi

printf '%s\n' 'development runtime check passed'
