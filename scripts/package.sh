#!/bin/sh
set -eu

target=${1:-x86_64-linux-gnu}
project=liblql
preset=${target}-release
build_dir=build/$preset
dist_dir=${LQL_DIST_DIR:-dist}
version_header=$build_dir/generated/include/lql/version.h

cmake --preset "$preset"
cmake --build --preset "$preset"

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
cmake --install "$build_dir" --prefix "$stage"

tar_args='--sort=name --owner=0 --group=0 --numeric-owner'
if tar --version >/dev/null 2>&1; then
  tar -C build/package $tar_args -czf "$archive" "$root_name"
else
  tar -C build/package -czf "$archive" "$root_name"
fi

(
  cd "$dist_dir"
  sha256sum "$(basename "$archive")" >"$(basename "$checksums")"
)

printf 'package: wrote %s and %s\n' "$archive" "$checksums"
