#!/bin/sh
set -eu

project=liblql
dist_dir=${LQL_DIST_DIR:-dist}
checksum_mode=${LQL_PACKAGE_CHECKSUM_MODE:-reset}
build_dir=${LQL_LUA_PACKAGE_BUILD_DIR:-build/package-lua}
version_build=build/package-lua-version
version_header=$version_build/generated/include/lql/version.h

cmake -S . -B "$version_build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/cpkt-toolchain.cmake \
  -DLQL_TARGET_ID=x86_64-linux-gnu \
  -DBUILD_TESTING=OFF \
  -DLQL_BUILD_DIRECT_PROBE=OFF \
  >/dev/null

if [ ! -f "$version_header" ]; then
  printf 'package lua: missing generated version header: %s\n' "$version_header" >&2
  exit 1
fi
version=$(sed -n 's/^#define LQL_VERSION "\(.*\)"/\1/p' "$version_header")
if [ -z "$version" ]; then
  printf 'package lua: unable to resolve version from %s\n' "$version_header" >&2
  exit 1
fi

root_name=$project-lua-$version
stage=$build_dir/$root_name
source_archive=$dist_dir/$root_name.tar.gz
rockspec=$dist_dir/$project-$version-1.rockspec
src_rock=$dist_dir/$project-$version-1.src.rock
checksums=$dist_dir/$project-$version-CHECKSUMS

rm -rf "$stage" "$build_dir/rockpack"
mkdir -p "$stage/lua/lql" "$stage/include/lql" "$stage/scripts" "$dist_dir"
if [ "$checksum_mode" != "append" ]; then
  rm -f "$dist_dir"/$project-*.tar.gz \
    "$dist_dir"/$project-*-1.rockspec \
    "$dist_dir"/$project-*-1.src.rock \
    "$dist_dir"/$project-*-CHECKSUMS
fi

cp LICENSE README.md "$stage/"
cp lua/lql_core.c "$stage/lua/"
cp lua/lql/init.lua "$stage/lua/lql/"
cp include/lql/lql.h "$stage/include/lql/"
cp "$version_header" "$stage/include/lql/version.h"
cp scripts/run_lua_tests.sh "$stage/scripts/"
printf '%s\n' "$version" >"$stage/VERSION"
{
  printf '%s\n' \
    LICENSE \
    README.md \
    VERSION \
    RELEASE_MANIFEST \
    include/lql/lql.h \
    include/lql/version.h \
    lua/lql_core.c \
    lua/lql/init.lua \
    scripts/run_lua_tests.sh
} | sort -u >"$stage/RELEASE_MANIFEST"

sed \
  -e "s|@LQL_ROCK_VERSION@|$version-1|g" \
  -e "s|@LQL_ROCK_SOURCE_URL@|$root_name.tar.gz|g" \
  -e "s|@LQL_ROCK_SOURCE_DIR@|$root_name|g" \
  liblql-dev-1.rockspec.in >"$rockspec"
cp "$rockspec" "$stage/$project-$version-1.rockspec"

tar_args='--sort=name --owner=0 --group=0 --numeric-owner'
if tar --version >/dev/null 2>&1; then
  tar -C "$build_dir" $tar_args -czf "$source_archive" "$root_name"
else
  tar -C "$build_dir" -czf "$source_archive" "$root_name"
fi

mkdir -p "$build_dir/rockpack"
cp "$rockspec" "$source_archive" "$build_dir/rockpack/"
rm -f "$src_rock"
(
  cd "$build_dir/rockpack"
  zip -X -q "../$(basename "$src_rock")" \
    "$(basename "$rockspec")" "$(basename "$source_archive")"
)
cp "$build_dir/$(basename "$src_rock")" "$src_rock"

(
  cd "$dist_dir"
  if [ "$checksum_mode" = "append" ]; then
    sha256sum "$(basename "$source_archive")" "$(basename "$rockspec")" \
      "$(basename "$src_rock")" >>"$(basename "$checksums")"
  else
    sha256sum "$(basename "$source_archive")" "$(basename "$rockspec")" \
      "$(basename "$src_rock")" >"$(basename "$checksums")"
  fi
)

printf 'package lua: wrote %s, %s, and %s\n' \
  "$source_archive" "$rockspec" "$src_rock"
