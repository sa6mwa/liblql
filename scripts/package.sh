#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

target=${1:-x86_64-linux-gnu}
project=liblql
preset=${target}-release
build_dir=build/$preset
dist_dir=${LQL_DIST_DIR:-dist}
checksum_mode=${LQL_PACKAGE_CHECKSUM_MODE:-reset}
version_header=$build_dir/generated/include/lql/version.h
build_testing=OFF

if [ "$target" = "x86_64-linux-gnu" ]; then
  build_testing=ON
fi

cmake --preset "$preset" \
  -DBUILD_TESTING="$build_testing" \
  -DLQL_BUILD_STATIC=ON \
  -DLQL_BUILD_SHARED=ON \
  -DLQL_BUILD_BINARY=OFF \
  -DLQL_CLQL_STATIC=OFF \
  -DLQL_BUILD_DIRECT_PROBE=OFF
cmake --build --preset "$preset"
if [ "$build_testing" = "ON" ]; then
  # The host package is executable on this release machine, so it runs the
  # comprehensive optimized CTest suite before staging. Cross packages are
  # build/link/package verified unless the project opts into a runner contract.
  ctest --test-dir "$build_dir" --output-on-failure
fi

if [ ! -f "$version_header" ]; then
  printf 'package: missing generated version header: %s\n' "$version_header" >&2
  exit 1
fi
version=$(sed -n 's/^#define LQL_VERSION "\(.*\)"/\1/p' "$version_header")
if [ -z "$version" ]; then
  printf 'package: unable to resolve version from %s\n' "$version_header" >&2
  exit 1
fi

root_name=$project-$version-$target
stage=build/package/$root_name
archive=$dist_dir/$root_name.tar.gz
checksums=$dist_dir/$project-$version-CHECKSUMS

rm -rf "$stage"
mkdir -p "$dist_dir" "$(dirname "$stage")"
if [ "$checksum_mode" != "append" ]; then
  rm -f "$dist_dir"/$project-*.tar.gz \
    "$dist_dir"/clql-*.tar.gz \
    "$dist_dir"/$project-*-1.rockspec \
    "$dist_dir"/$project-*-1.src.rock \
    "$dist_dir"/$project-*-CHECKSUMS
fi
cmake --install "$build_dir" --prefix "$stage"

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

printf 'package: wrote %s and %s\n' "$archive" "$checksums"
