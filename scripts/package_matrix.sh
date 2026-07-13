#!/bin/sh
set -eu

project=liblql
dist_dir=${LQL_DIST_DIR:-dist}
targets=${LQL_RELEASE_TARGETS:-"x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl"}

if scripts/cpkt-toolchains.sh discover arm64-apple-darwin |
  grep '^status=ready$' >/dev/null 2>&1; then
  targets="$targets arm64-apple-darwin"
else
  printf 'package matrix: skipping arm64-apple-darwin; osxcross is not ready\n'
fi

mkdir -p "$dist_dir"
rm -f "$dist_dir"/$project-*.tar.gz "$dist_dir"/$project-*-CHECKSUMS

mode=reset
for target in $targets; do
  printf 'package matrix: building %s\n' "$target"
  LQL_PACKAGE_CHECKSUM_MODE=$mode sh scripts/package.sh "$target"
  mode=append
done

LQL_PACKAGE_CHECKSUM_MODE=append sh scripts/package_source.sh
LQL_PACKAGE_CHECKSUM_MODE=append sh scripts/package_lua.sh

sh scripts/package_verify.sh all
