#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

target=${1:-x86_64-linux-gnu}
project=clql
library_project=liblql
preset=${target}-release
build_dir=build/$preset
dist_dir=${LQL_DIST_DIR:-dist}
checksum_mode=${LQL_PACKAGE_CHECKSUM_MODE:-reset}
version_header=$build_dir/generated/include/lql/version.h

cmake --preset "$preset" \
  -DBUILD_TESTING=OFF \
  -DLQL_BUILD_STATIC=ON \
  -DLQL_BUILD_SHARED=OFF \
  -DLQL_BUILD_BINARY=ON \
  -DLQL_CLQL_STATIC=ON \
  -DLQL_BUILD_DIRECT_PROBE=OFF
cmake --build --preset "$preset" --target clql

if [ ! -f "$version_header" ]; then
  printf 'package clql: missing generated version header: %s\n' "$version_header" >&2
  exit 1
fi
version=$(sed -n 's/^#define LQL_VERSION "\(.*\)"/\1/p' "$version_header")
if [ -z "$version" ]; then
  printf 'package clql: unable to resolve version from %s\n' "$version_header" >&2
  exit 1
fi

root_name=$project-$version-$target
stage=build/package/$root_name
archive=$dist_dir/$root_name.tar.gz
checksums=$dist_dir/$library_project-$version-CHECKSUMS

rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/share/doc/clql" "$dist_dir"
if [ "$checksum_mode" != "append" ]; then
  rm -f "$dist_dir"/$library_project-*.tar.gz \
    "$dist_dir"/clql-*.tar.gz \
    "$dist_dir"/$library_project-*-1.rockspec \
    "$dist_dir"/$library_project-*-1.src.rock \
    "$dist_dir"/$library_project-*-CHECKSUMS
fi
cp "$build_dir/clql" "$stage/bin/clql"
cp LICENSE README.md "$stage/share/doc/clql/"

tar_args='--sort=name --owner=0 --group=0 --numeric-owner'
if tar --version >/dev/null 2>&1; then
  tar -C build/package $tar_args -czf "$archive" "$root_name"
else
  tar -C build/package -czf "$archive" "$root_name"
fi

(
  cd "$dist_dir"
  if [ "$checksum_mode" = "append" ]; then
    sha256sum "$(basename "$archive")" >>"$(basename "$checksums")"
  else
    sha256sum "$(basename "$archive")" >"$(basename "$checksums")"
  fi
)

printf 'package clql: wrote %s and %s\n' "$archive" "$checksums"
