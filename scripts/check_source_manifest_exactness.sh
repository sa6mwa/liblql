#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
dist_dir=${LQL_DIST_DIR:-dist}
work=${LQL_SOURCE_MANIFEST_EXACTNESS_DIR:-"$root/build/source-manifest-exactness"}
project=liblql

manifest=$(find "$dist_dir" -maxdepth 1 -name "$project-*-CHECKSUMS" -type f | sort | sed -n '1p')
if [ -z "$manifest" ]; then
  printf 'source manifest exactness: missing checksum manifest in %s\n' "$dist_dir" >&2
  exit 1
fi
version=$(basename "$manifest" | sed "s/^$project-//;s/-CHECKSUMS\$//")
archive=$dist_dir/$project-$version.tar.gz
archive_name=$(basename "$archive")
archive_root=$project-$version

if ! awk '{print $2}' "$manifest" | grep -Fx "$archive_name" >/dev/null; then
  printf 'source manifest exactness: source archive is not checksum-listed: %s\n' \
    "$archive_name" >&2
  exit 1
fi
if [ ! -f "$archive" ]; then
  printf 'source manifest exactness: missing source archive: %s\n' "$archive" >&2
  exit 1
fi

sh "$root/scripts/remove_path.sh" "$work"
mkdir -p "$work/extract"
tar -xzf "$archive" -C "$work/extract"
prefix=$work/extract/$archive_root
if [ ! -f "$prefix/RELEASE_MANIFEST" ]; then
  printf 'source manifest exactness: archive lacks RELEASE_MANIFEST\n' >&2
  exit 1
fi

(
  cd "$prefix"
  find . -type f | sed 's#^\./##' | sort
) >"$work/actual.txt"
sort "$prefix/RELEASE_MANIFEST" >"$work/manifest.txt"

if ! cmp -s "$work/manifest.txt" "$work/actual.txt"; then
  printf 'source manifest exactness: payload differs from RELEASE_MANIFEST\n' >&2
  printf '--- manifest-only / actual-only diff ---\n' >&2
  diff -u "$work/manifest.txt" "$work/actual.txt" >&2 || true
  exit 1
fi

printf 'source manifest exactness: verified %s\n' "$archive"
