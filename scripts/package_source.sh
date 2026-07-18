#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

project=liblql
dist_dir=${LQL_DIST_DIR:-dist}
checksum_mode=${LQL_PACKAGE_CHECKSUM_MODE:-reset}
build_dir=${LQL_SOURCE_PACKAGE_BUILD_DIR:-build/package-source}
version_build=build/package-source-version
version_header=$version_build/generated/include/lql/version.h

remove_path() {
  path=$1
  if [ ! -e "$path" ] && [ ! -L "$path" ]; then
    return 0
  fi
  if [ -d "$path" ] && [ ! -L "$path" ]; then
    find "$path" ! -type d -exec rm -- {} \;
    find "$path" -depth -type d -exec rmdir -- {} \;
  else
    rm -- "$path"
  fi
}

remove_matching_files() {
  for path in "$@"; do
    if [ -e "$path" ]; then
      rm -- "$path"
    fi
  done
}

remove_path "$version_build"
cmake -S . -B "$version_build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/cpkt-toolchain.cmake \
  -DLQL_TARGET_ID=x86_64-linux-gnu \
  -DBUILD_TESTING=OFF \
  -DLQL_BUILD_DIRECT_PROBE=OFF \
  >/dev/null

if [ ! -f "$version_header" ]; then
  printf 'package source: missing generated version header: %s\n' "$version_header" >&2
  exit 1
fi
version=$(sed -n 's/^#define LQL_VERSION "\(.*\)"/\1/p' "$version_header")
if [ -z "$version" ]; then
  printf 'package source: unable to resolve version from %s\n' "$version_header" >&2
  exit 1
fi

root_name=$project-$version
stage=$build_dir/$root_name
archive=$dist_dir/$root_name.tar.gz
checksums=$dist_dir/$project-$version-CHECKSUMS
manifest_tmp=$build_dir/source-files.txt

exact_lightweight_version() {
  tags=$(git tag --points-at HEAD --list 'v[0-9]*.[0-9]*.[0-9]*' | sort)
  found=
  for tag in $tags; do
    if ! printf '%s\n' "$tag" | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' >/dev/null; then
      continue
    fi
    type=$(git cat-file -t "refs/tags/$tag" 2>/dev/null || true)
    if [ "$type" != commit ]; then
      continue
    fi
    candidate=${tag#v}
    if [ -n "$found" ] && [ "$found" != "$candidate" ]; then
      printf 'package source: multiple lightweight release tags point at HEAD: %s and %s\n' \
        "$found" "$candidate" >&2
      exit 1
    fi
    found=$candidate
  done
  printf '%s\n' "$found"
}

exact_version=$(exact_lightweight_version)
if [ -n "$exact_version" ] && ! git diff-index --quiet HEAD --; then
  printf 'package source: exact tagged release v%s requires a clean tracked worktree\n' \
    "$exact_version" >&2
  exit 1
fi

remove_path "$stage"
mkdir -p "$stage" "$dist_dir" "$build_dir"
if [ "$checksum_mode" != "append" ]; then
  remove_matching_files "$dist_dir"/$project-*.tar.gz \
    "$dist_dir"/clql-*.tar.gz \
    "$dist_dir"/$project-*-1.rockspec \
    "$dist_dir"/$project-*-1.src.rock \
    "$dist_dir"/$project-*-CHECKSUMS
fi

git ls-files --cached --modified |
  sed '/^dist\//d;/^build\//d;/^VERSION$/d;/^RELEASE_MANIFEST$/d' |
  sort -u |
  while IFS= read -r path; do
    if [ -n "$path" ] && [ -f "$path" ]; then
      printf '%s\n' "$path"
    fi
  done >"$manifest_tmp"

while IFS= read -r path; do
  if [ -z "$path" ] || [ ! -f "$path" ]; then
    continue
  fi
  mkdir -p "$stage/$(dirname "$path")"
  cp "$path" "$stage/$path"
done <"$manifest_tmp"

printf '%s\n' "$version" >"$stage/VERSION"
{
  cat "$manifest_tmp"
  printf '%s\n' VERSION RELEASE_MANIFEST
} | sort -u >"$stage/RELEASE_MANIFEST"

tar_args='--sort=name --owner=0 --group=0 --numeric-owner'
if tar --version >/dev/null 2>&1; then
  tar -C "$build_dir" $tar_args -czf "$archive" "$root_name"
else
  tar -C "$build_dir" -czf "$archive" "$root_name"
fi

(
  cd "$dist_dir"
  if [ "$checksum_mode" = "append" ]; then
    sha256sum "$(basename "$archive")" >>"$(basename "$checksums")"
  else
    sha256sum "$(basename "$archive")" >"$(basename "$checksums")"
  fi
)

printf 'package source: wrote %s\n' "$archive"
