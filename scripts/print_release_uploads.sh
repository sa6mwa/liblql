#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

project=liblql
dist_dir=${LQL_DIST_DIR:-dist}

manifests=$(find "$dist_dir" -maxdepth 1 -name "$project-*-CHECKSUMS" -type f |
  sort)
count=$(printf '%s\n' "$manifests" | sed '/^$/d' | wc -l | tr -d ' ')
if [ "$count" != 1 ]; then
  printf 'release upload list: expected exactly one checksum manifest, found %s\n' \
    "$count" >&2
  printf '%s\n' "$manifests" >&2
  exit 1
fi

manifest=$(printf '%s\n' "$manifests" | sed -n '1p')
printf '%s\n' "$manifest"

awk '{print $2}' "$manifest" | sort | while IFS= read -r artifact; do
  path=$dist_dir/$artifact
  if [ ! -f "$path" ]; then
    printf 'release upload list: checksum-listed artifact is missing: %s\n' \
      "$path" >&2
    exit 1
  fi
  printf '%s\n' "$path"
done
