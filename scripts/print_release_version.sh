#!/bin/sh
set -eu

build_dir=${LQL_VERSION_BUILD_DIR:-build/print-release-version}
version_header=$build_dir/generated/include/lql/version.h

rm -rf "$build_dir"
cmake -S . -B "$build_dir" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/cpkt-toolchain.cmake \
  -DLQL_TARGET_ID=x86_64-linux-gnu \
  -DBUILD_TESTING=OFF \
  -DLQL_BUILD_DIRECT_PROBE=OFF \
  >/dev/null

if [ ! -f "$version_header" ]; then
  printf 'print release version: missing generated version header: %s\n' \
    "$version_header" >&2
  exit 1
fi

version=$(sed -n 's/^#define LQL_VERSION "\(.*\)"/\1/p' "$version_header")
if [ -z "$version" ]; then
  printf 'print release version: unable to resolve version from %s\n' \
    "$version_header" >&2
  exit 1
fi
printf '%s\n' "$version"
