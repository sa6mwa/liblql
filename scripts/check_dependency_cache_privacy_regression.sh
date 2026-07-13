#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${LQL_DEPENDENCY_CACHE_PRIVACY_DIR:-"$root/build/dependency-cache-privacy-regression"}
dist=$work/dist
stage=$work/stage
verify=$work/verify
cache=$(mktemp -d "${TMPDIR:-/tmp}/liblql-shared-dependency-cache.XXXXXX")
project=liblql

trap 'rm -rf "$cache"' EXIT HUP INT TERM
rm -rf "$work"
mkdir -p "$stage"

(
  cd "$root"
  LQL_DIST_DIR="$dist" \
  LQL_SOURCE_PACKAGE_BUILD_DIR="$work/source-package" \
  CPKT_DEPENDENCY_CACHE="$cache" \
    sh scripts/package_source.sh >/dev/null
)

manifest=$(find "$dist" -maxdepth 1 -name "$project-*-CHECKSUMS" -type f | sort | sed -n '1p')
if [ -z "$manifest" ]; then
  printf '%s\n' 'dependency cache privacy: package source did not create a checksum manifest' >&2
  exit 1
fi
version=$(basename "$manifest" | sed "s/^$project-//;s/-CHECKSUMS\$//")
archive=$dist/$project-$version.tar.gz
root_name=$project-$version

tar -xzf "$archive" -C "$stage"
printf '%s\n' "$cache" >>"$stage/$root_name/README.md"
tar -C "$stage" -czf "$archive" "$root_name"
(
  cd "$dist"
  sha256sum "$(basename "$archive")" >"$(basename "$manifest")"
)

if (
  cd "$root"
  LQL_DIST_DIR="$dist" \
  LQL_PACKAGE_VERIFY_DIR="$verify" \
  CPKT_DEPENDENCY_CACHE="$cache" \
    sh scripts/package_verify.sh source >"$work/verify.log" 2>&1
); then
  printf '%s\n' 'dependency cache privacy: accepted a release archive containing the shared cache path' >&2
  exit 1
fi

if ! grep -F 'dependency cache path leaked' "$work/verify.log" >/dev/null; then
  printf '%s\n' 'dependency cache privacy: verifier failed without reporting the cache-path leak' >&2
  cat "$work/verify.log" >&2
  exit 1
fi

printf '%s\n' 'dependency cache privacy: regression fixture rejected'
