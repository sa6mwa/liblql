#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=$("$root/scripts/release_version.sh")
tree=${LQL_LUAROCKS_TREE:-"$root/build/luarocks"}
rockspec="$root/build/luarocks/liblql-${version}-1.rockspec"

if ! command -v luarocks >/dev/null 2>&1; then
  printf 'lua-rock: luarocks executable not found\n' >&2
  exit 1
fi

LQL_ALLOW_LOCAL_LUA_SOURCE_URL=1 \
LQL_LUA_SOURCE_URL="file://$root" \
  "$root/scripts/render_release_rockspec.sh" "$version" "$rockspec"

(cd "$root" && luarocks --tree "$tree" make "$rockspec")
printf 'lua-rock: installed liblql %s into %s\n' "$version" "$tree"
