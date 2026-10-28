#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

target=${1:-x86_64-linux-gnu}
project=liblql
preset=${target}-release
build_dir=build/$preset
dist_dir=${LQL_DIST_DIR:-dist}
sh "$root/scripts/check_owned_path.sh" "$dist_dir"
checksum_mode=${LQL_PACKAGE_CHECKSUM_MODE:-reset}
version_header=$build_dir/generated/include/lql/version.h
build_testing=OFF

case "$target" in
  x86_64-linux-gnu|arm64-apple-darwin) build_testing=ON ;;
esac

sh scripts/deps.sh "$target" >/dev/null
cmake --preset "$preset" \
  -DBUILD_TESTING="$build_testing" \
  -DLQL_BUILD_STATIC=ON \
  -DLQL_BUILD_SHARED=ON \
  -DLQL_BUILD_BINARY=ON \
  -DLQL_CLQL_STATIC=ON \
  -DLQL_BUILD_DIRECT_PROBE=OFF
cmake --build --preset "$preset"
if [ "$target" = "x86_64-linux-gnu" ]; then
  # The host package is executable on this release machine, so it runs the
  # comprehensive optimized CTest suite before staging. Darwin tests compile
  # under Clang but cannot run here; ARM runtime smokes use extracted archives.
  ctest --test-dir "$build_dir" --stop-on-failure --output-on-failure --no-tests=error
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

sh scripts/remove_path.sh "$stage"
mkdir -p "$dist_dir" "$(dirname "$stage")"
if [ "$checksum_mode" != "append" ]; then
  sh scripts/remove_path.sh "$dist_dir"/$project-*.tar.gz \
    "$dist_dir"/clql-*.tar.gz \
    "$dist_dir"/$project-*-1.rockspec \
    "$dist_dir"/$project-*-1.src.rock \
    "$dist_dir"/$project-*-CHECKSUMS
fi
cmake --install "$build_dir" --prefix "$stage"

sh scripts/create_tar_gz.sh build/package "$archive" "$root_name"

(
  cd "$dist_dir"
  if [ "$checksum_mode" = "append" ]; then
    sha256sum "$(basename "$archive")" >>"$(basename "$checksums")"
  else
    sha256sum "$(basename "$archive")" >"$(basename "$checksums")"
  fi
)

printf 'package: wrote %s and %s\n' "$archive" "$checksums"
