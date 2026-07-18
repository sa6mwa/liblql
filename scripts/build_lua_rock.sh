#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sdk_prefix=${LQL_LUA_SDK_PREFIX:-$root/build/lua-sdk}
tree=${LQL_LUAROCKS_TREE:-$root/build/luarocks}
rockspec_dir=$root/build/luarocks-rockspec
source_dir=$root/build/luarocks-src
version_header=$root/build/release/generated/include/lql/version.h

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
cp "$root/lua/lql/cli.lua" "$source_dir/lua/lql/"
rockspec=$rockspec_dir/liblql-$version-1.rockspec
sed \
  -e "s|@LQL_ROCK_VERSION@|$version-1|g" \
  -e "s|@LQL_ROCK_SOURCE_URL@|.|g" \
  -e "s|@LQL_ROCK_SOURCE_DIR@|$source_dir|g" \
  "$root/liblql-dev-1.rockspec.in" >"$rockspec"

(
  cd "$source_dir"
  luarocks --lua-version=5.5 --tree "$tree" make "$rockspec" \
    "LIBLQL_DIR=$sdk_prefix"
)

printf 'lua-rock: installed liblql %s-1 into %s\n' "$version" "$tree"
