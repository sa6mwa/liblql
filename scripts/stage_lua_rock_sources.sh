#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
  printf 'usage: %s <staging-root> <version>\n' "$0" >&2
  exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
staging_root=$1
version=$2
manifest="$staging_root/RELEASE_MANIFEST"

rm -rf "$staging_root"
mkdir -p "$staging_root/lua" "$staging_root/scripts"

cp "$root/LICENSE" "$staging_root/LICENSE"
cp "$root/README.md" "$staging_root/README.md"
cp "$root/liblql.rockspec.in" "$staging_root/liblql.rockspec.in"
cp -R "$root/lua/." "$staging_root/lua/"
cp "$root/scripts/build_lua_rock.sh" "$staging_root/scripts/build_lua_rock.sh"
cp "$root/scripts/release_version.sh" "$staging_root/scripts/release_version.sh"
cp "$root/scripts/render_release_rockspec.sh" "$staging_root/scripts/render_release_rockspec.sh"
cp "$root/scripts/run_lua_tests.sh" "$staging_root/scripts/run_lua_tests.sh"
cp "$root/scripts/stage_lua_rock_sources.sh" "$staging_root/scripts/stage_lua_rock_sources.sh"
printf '%s\n' "$version" >"$staging_root/VERSION"

(cd "$staging_root" && find . -type f | sed 's#^\./##' | LC_ALL=C sort) >"$manifest"
