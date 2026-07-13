#!/bin/sh
set -eu

project=liblql
dist_dir=${LQL_DIST_DIR:-dist}
target=${1:-x86_64-linux-gnu}
work=${LQL_PACKAGE_VERIFY_DIR:-build/package-verify}

checksums=$(find "$dist_dir" -maxdepth 1 -name "$project-*-CHECKSUMS" -type f | sort)
count=$(printf '%s\n' "$checksums" | sed '/^$/d' | wc -l | tr -d ' ')
if [ "$count" != 1 ]; then
  printf 'package verify: expected exactly one checksum manifest, found %s\n' "$count" >&2
  printf '%s\n' "$checksums" >&2
  exit 1
fi

manifest=$checksums
version=$(basename "$manifest" | sed "s/^$project-//;s/-CHECKSUMS\$//")
archive=$dist_dir/$project-$version-$target.tar.gz
root=$project-$version-$target

if [ ! -f "$archive" ]; then
  printf 'package verify: missing target archive: %s\n' "$archive" >&2
  exit 1
fi

(
  cd "$dist_dir"
  sha256sum -c "$(basename "$manifest")" >/dev/null
)

listed=$(awk '{print $2}' "$manifest" | sort)
release_artifacts=$(find "$dist_dir" -maxdepth 1 -type f \
  \( -name "$project-*.tar.gz" \) -printf '%f\n' | sort)
if [ "$listed" != "$release_artifacts" ]; then
  printf 'package verify: checksum manifest does not match release artifacts\n' >&2
  printf 'listed:\n%s\nartifacts:\n%s\n' "$listed" "$release_artifacts" >&2
  exit 1
fi

rm -rf "$work"
mkdir -p "$work/extract"
tar -xzf "$archive" -C "$work/extract"

roots=$(find "$work/extract" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort)
if [ "$roots" != "$root" ]; then
  printf 'package verify: archive root mismatch: expected %s got %s\n' \
    "$root" "$roots" >&2
  exit 1
fi

prefix=$work/extract/$root
for path in \
  include/lql/lql.h \
  include/lql/version.h \
  lib/liblql.a \
  lib/liblql.so \
  lib/liblql.so.0 \
  lib/cmake/liblql/liblqlConfig.cmake \
  lib/pkgconfig/liblql.pc \
  bin/clql \
  share/doc/liblql/LICENSE \
  share/doc/liblql/README.md
do
  if [ ! -e "$prefix/$path" ]; then
    printf 'package verify: missing payload path: %s\n' "$path" >&2
    exit 1
  fi
done

tools=$work/tools.env
scripts/discover_target_tools.sh "build/$target-release" "$target" >"$tools"
readelf_tool=$(sed -n 's/^READELF=//p' "$tools")
if [ -z "$readelf_tool" ] || [ ! -x "$readelf_tool" ]; then
  printf 'package verify: missing readelf for %s\n' "$target" >&2
  exit 1
fi

for elf in "$prefix/lib/liblql.so.0" "$prefix/bin/clql"; do
  if "$readelf_tool" -d "$elf" | grep -E 'RPATH|RUNPATH' >/dev/null 2>&1; then
    if "$readelf_tool" -d "$elf" | grep -E 'RPATH|RUNPATH' |
      grep -v '\$ORIGIN' >/dev/null 2>&1; then
      printf 'package verify: non-relocatable runtime path in %s\n' "$elf" >&2
      "$readelf_tool" -d "$elf" | grep -E 'RPATH|RUNPATH' >&2
      exit 1
    fi
  fi
done

repo=$(pwd -P)
home=${HOME:-}
if grep -R -a -n -F "$repo" "$prefix" >/dev/null 2>&1; then
  printf 'package verify: repository path leaked into %s\n' "$archive" >&2
  grep -R -a -n -F "$repo" "$prefix" >&2
  exit 1
fi
if [ -n "$home" ] && grep -R -a -n -F "$home" "$prefix" >/dev/null 2>&1; then
  printf 'package verify: HOME path leaked into %s\n' "$archive" >&2
  grep -R -a -n -F "$home" "$prefix" >&2
  exit 1
fi
if grep -R -a -n -E 'file://|/tmp/|/var/tmp/' "$prefix" >/dev/null 2>&1; then
  printf 'package verify: local URL or temp path leaked into %s\n' "$archive" >&2
  grep -R -a -n -E 'file://|/tmp/|/var/tmp/' "$prefix" >&2
  exit 1
fi

LQL_INSTALL_PREFIX="$prefix" \
LQL_INSTALL_CONSUMER_DIR="$work/install-consumer" \
  sh scripts/check_install_tree.sh

printf 'package verify: verified %s\n' "$archive"
