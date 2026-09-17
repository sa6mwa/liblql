#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sdk_prefix=${LQL_LUA_SDK_PREFIX:-$root/build/lua-sdk}
tree=${LQL_LUAROCKS_TREE:-$root/build/luarocks}
rockspec_dir=$root/build/luarocks-rockspec
source_dir=$root/build/luarocks-src
version_header=$root/build/release/generated/include/lql/version.h
lua_include_dir=${LQL_LUA_ROCK_LUA_INCDIR:-$root/build/debug-lua/lua-5.5.1/src}
lua_libflag=${LQL_LUA_ROCK_LIBFLAG:--shared -Wl,--disable-new-dtags}

if [ ! -f "$lua_include_dir/lua.h" ]; then
  printf 'lua-rock: missing Bootlin Lua headers: %s\n' "$lua_include_dir" >&2
  printf '%s\n' 'lua-rock: run make build-debug-lua first' >&2
  exit 1
fi

case ${LIBLQL_TOOLCHAIN_OVERRIDE:-} in
  "")
    toolchain=$($root/scripts/cpkt-toolchains.sh discover x86_64-linux-gnu)
    lua_cc=$(printf '%s\n' "$toolchain" | sed -n 's/^cc=//p')
    compiler_source=Bootlin
    ;;
  1)
    lua_cc_name=${LQL_LUA_ROCK_CC:-${CC:-cc}}
    lua_cc=$(command -v "$lua_cc_name" 2>/dev/null || :)
    compiler_source=caller-supplied
    ;;
  *)
    printf '%s\n' 'lua-rock: LIBLQL_TOOLCHAIN_OVERRIDE must be exactly 1 when bypassing Bootlin' >&2
    exit 1
    ;;
esac
if [ -z "$lua_cc" ] || [ ! -x "$lua_cc" ]; then
  printf 'lua-rock: selected %s compiler is unavailable: %s\n' \
    "$compiler_source" "${lua_cc_name:-$lua_cc}" >&2
  exit 1
fi

cmake --preset release
cmake --build --preset release
sh "$root/scripts/remove_path.sh" "$sdk_prefix" "$tree" "$rockspec_dir" "$source_dir"
cmake --install "$root/build/release" --prefix "$sdk_prefix"

if [ ! -f "$version_header" ]; then
  printf 'lua-rock: missing generated version header: %s\n' "$version_header" >&2
  exit 1
fi
version=$(sed -n 's/^#define LQL_VERSION "\(.*\)"/\1/p' "$version_header")
if [ -z "$version" ]; then
  printf 'lua-rock: unable to resolve version from %s\n' "$version_header" >&2
  exit 1
fi

mkdir -p "$rockspec_dir" "$source_dir/lua/bin" "$source_dir/lua/lql"
cp "$root/lua/lql_core.c" "$source_dir/lua/"
cp "$root/lua/bin/lql.lua" "$source_dir/lua/bin/"
cp "$root/lua/lql/init.lua" "$source_dir/lua/lql/"
rockspec=$rockspec_dir/liblql-$version-1.rockspec
sed \
  -e "s|@LQL_ROCK_VERSION@|$version-1|g" \
  -e "s|@LQL_ROCK_SOURCE_URL@|.|g" \
  -e "s|@LQL_ROCK_SOURCE_DIR@|$source_dir|g" \
  "$root/liblql-dev-1.rockspec.in" >"$rockspec"

(
  cd "$source_dir"
  set -- "LIBLQL_DIR=$sdk_prefix" "CC=$lua_cc" "LD=$lua_cc" \
    "LUA_INCDIR=$lua_include_dir" "LIBFLAG=$lua_libflag"
  if [ -n "${LQL_LUA_ROCK_CFLAGS:-}" ]; then
    set -- "$@" "CFLAGS=$LQL_LUA_ROCK_CFLAGS"
  fi
  if [ -n "${LQL_LUA_ROCK_OBJ_EXTENSION:-}" ]; then
    set -- "$@" "OBJ_EXTENSION=$LQL_LUA_ROCK_OBJ_EXTENSION"
  fi
  if [ -n "${LQL_LUA_ROCK_LIB_EXTENSION:-}" ]; then
    set -- "$@" "LIB_EXTENSION=$LQL_LUA_ROCK_LIB_EXTENSION"
  fi
  luarocks --lua-version=5.5 --tree "$tree" make "$rockspec" \
    "$@"
)

printf 'lua-rock: installed liblql %s-1 into %s\n' "$version" "$tree"
