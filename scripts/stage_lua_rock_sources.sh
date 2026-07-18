#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
  printf 'usage: %s <stage-dir> <version> <version-header> <rockspec>\n' "$0" >&2
  exit 2
fi

stage=$1
version=$2
version_header=$3
rockspec=$4
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
project=liblql

sh "$root/scripts/remove_path.sh" "$stage"
mkdir -p "$stage/lua/bin" "$stage/lua/lql" "$stage/include/lql" "$stage/scripts"

cp "$root/LICENSE" "$root/README.md" "$stage/"
cp "$root/liblql-dev-1.rockspec.in" "$stage/"
cp "$root/lua/lql_core.c" "$stage/lua/"
cp "$root/lua/bin/lql.lua" "$stage/lua/bin/"
cp "$root/lua/lql/init.lua" "$stage/lua/lql/"
cp "$root/lua/lql/cli.lua" "$stage/lua/lql/"
cp "$root/include/lql/lql.h" "$stage/include/lql/"
cp "$version_header" "$stage/include/lql/version.h"
cp "$root/scripts/build_lua_rock.sh" "$stage/scripts/"
cp "$root/scripts/render_release_rockspec.sh" "$stage/scripts/"
cp "$root/scripts/run_lua_tests.sh" "$stage/scripts/"
cp "$root/scripts/stage_lua_rock_sources.sh" "$stage/scripts/"
cp "$rockspec" "$stage/$project-$version-1.rockspec"
printf '%s\n' "$version" >"$stage/VERSION"
{
  printf '%s\n' \
    LICENSE \
    README.md \
    VERSION \
    RELEASE_MANIFEST \
    liblql-dev-1.rockspec.in \
    include/lql/lql.h \
    include/lql/version.h \
    lua/bin/lql.lua \
    lua/lql/cli.lua \
    lua/lql_core.c \
    lua/lql/init.lua \
    scripts/build_lua_rock.sh \
    scripts/render_release_rockspec.sh \
    scripts/run_lua_tests.sh \
    scripts/stage_lua_rock_sources.sh \
    "$project-$version-1.rockspec"
} | sort -u >"$stage/RELEASE_MANIFEST"
