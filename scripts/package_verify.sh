#!/bin/sh
set -eu

project=liblql
dist_dir=${LQL_DIST_DIR:-dist}
target_arg=${1:-all}
work=${LQL_PACKAGE_VERIFY_DIR:-build/package-verify}

find_manifest() {
  manifests=$(find "$dist_dir" -maxdepth 1 -name "$project-*-CHECKSUMS" -type f | sort)
  count=$(printf '%s\n' "$manifests" | sed '/^$/d' | wc -l | tr -d ' ')
  if [ "$count" != 1 ]; then
    printf 'package verify: expected exactly one checksum manifest, found %s\n' "$count" >&2
    printf '%s\n' "$manifests" >&2
    exit 1
  fi
  printf '%s\n' "$manifests"
}

manifest=$(find_manifest)
version=$(basename "$manifest" | sed "s/^$project-//;s/-CHECKSUMS\$//")

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

verify_privacy() {
  archive=$1
  prefix=$2
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
}

verify_linux_runtime_paths() {
  target=$1
  prefix=$2
  tools=$work/tools-$target.env
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
}

verify_darwin_runtime_paths() {
  target=$1
  prefix=$2
  tools=$work/tools-$target.env
  scripts/discover_target_tools.sh "build/$target-release" "$target" >"$tools"
  otool=$(sed -n 's/^OTOOL=//p' "$tools")
  if [ -z "$otool" ] || [ ! -x "$otool" ]; then
    printf 'package verify: missing otool for %s\n' "$target" >&2
    exit 1
  fi
  for macho in "$prefix/lib/liblql.0.dylib" "$prefix/bin/clql"; do
    if "$otool" -L "$macho" |
      grep -E "$HOME|$(pwd -P)|/tmp/|/var/tmp/|/usr/local/" >/dev/null 2>&1; then
      printf 'package verify: non-relocatable Darwin dependency in %s\n' "$macho" >&2
      "$otool" -L "$macho" >&2
      exit 1
    fi
  done
  if ! "$otool" -D "$prefix/lib/liblql.0.dylib" |
    grep '@rpath/liblql.0.dylib' >/dev/null 2>&1; then
    printf 'package verify: Darwin install name is not @rpath/liblql.0.dylib\n' >&2
    "$otool" -D "$prefix/lib/liblql.0.dylib" >&2
    exit 1
  fi
}

verify_payload() {
  target=$1
  archive=$dist_dir/$project-$version-$target.tar.gz
  root=$project-$version-$target
  extract_dir=$work/extract/$target
  prefix=$extract_dir/$root

  if [ ! -f "$archive" ]; then
    printf 'package verify: missing target archive: %s\n' "$archive" >&2
    exit 1
  fi
  mkdir -p "$extract_dir"
  tar -xzf "$archive" -C "$extract_dir"
  roots=$(find "$extract_dir" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort)
  if [ "$roots" != "$root" ]; then
    printf 'package verify: archive root mismatch: expected %s got %s\n' \
      "$root" "$roots" >&2
    exit 1
  fi

  for path in \
    include/lql/lql.h \
    include/lql/version.h \
    lib/liblql.a \
    lib/cmake/liblql/liblqlConfig.cmake \
    lib/pkgconfig/liblql.pc \
    bin/clql \
    share/doc/liblql/LICENSE \
    share/doc/liblql/README.md
  do
    if [ ! -e "$prefix/$path" ]; then
      printf 'package verify: missing payload path: %s in %s\n' "$path" "$target" >&2
      exit 1
    fi
  done

  case "$target" in
    *-darwin)
      for path in lib/liblql.dylib lib/liblql.0.dylib; do
        if [ ! -e "$prefix/$path" ]; then
          printf 'package verify: missing Darwin payload path: %s\n' "$path" >&2
          exit 1
        fi
      done
      verify_darwin_runtime_paths "$target" "$prefix"
      ;;
    *-linux-*)
      for path in lib/liblql.so lib/liblql.so.0; do
        if [ ! -e "$prefix/$path" ]; then
          printf 'package verify: missing Linux payload path: %s\n' "$path" >&2
          exit 1
        fi
      done
      verify_linux_runtime_paths "$target" "$prefix"
      ;;
    *)
      printf 'package verify: unsupported target: %s\n' "$target" >&2
      exit 1
      ;;
  esac

  verify_privacy "$archive" "$prefix"

  if [ "$target" = "x86_64-linux-gnu" ] && [ "$(uname -m)" = "x86_64" ]; then
    LQL_INSTALL_PREFIX="$prefix" \
    LQL_INSTALL_CONSUMER_DIR="$work/install-consumer-$target" \
      sh scripts/check_install_tree.sh
  else
    printf 'package verify: skipped executable consumer smoke for cross target %s\n' "$target"
  fi

  printf 'package verify: verified %s\n' "$archive"
}

if [ "$target_arg" = "all" ]; then
  awk '{print $2}' "$manifest" | sort | while IFS= read -r artifact; do
    target=$(printf '%s\n' "$artifact" |
      sed "s/^$project-$version-//;s/\\.tar\\.gz\$//")
    verify_payload "$target"
  done
else
  verify_payload "$target_arg"
fi
