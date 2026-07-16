#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

reserved_tag=v99.99.99
cmake_build=build/lifecycle-version-contract-cmake
script_build=build/lifecycle-version-contract-script
make_build=build/lifecycle-version-contract-make
artifact_dist=build/lifecycle-version-contract-dist
created_reserved_tag=0
created_version_file=0

cleanup() {
  if [ "$created_reserved_tag" = 1 ]; then
    git tag -d "$reserved_tag" >/dev/null 2>&1 || true
  fi
  if [ "$created_version_file" = 1 ]; then
    rm -f VERSION
  fi
}
trap cleanup EXIT HUP INT TERM

fail() {
  printf 'lifecycle version contract: %s\n' "$1" >&2
  exit 1
}

exact_lightweight_version() {
  tags=$(git tag --points-at HEAD --list 'v[0-9]*.[0-9]*.[0-9]*' | sort)
  found=
  for tag in $tags; do
    case $tag in
      v[0-9]*.[0-9]*.[0-9]*)
        type=$(git cat-file -t "refs/tags/$tag" 2>/dev/null || true)
        if [ "$type" != commit ]; then
          continue
        fi
        version=${tag#v}
        if [ -n "$found" ] && [ "$found" != "$version" ]; then
          fail "multiple lightweight release tags point at HEAD: $found and $version"
        fi
        found=$version
        ;;
    esac
  done
  printf '%s\n' "$found"
}

expect_version() {
  expected=$1
  override=${2:-}

  rm -rf "$cmake_build" "$script_build" "$make_build"
  if [ -n "$override" ]; then
    LQL_VERSION_OVERRIDE=$override cmake -S . -B "$cmake_build" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/cpkt-toolchain.cmake \
      -DLQL_TARGET_ID=x86_64-linux-gnu \
      -DBUILD_TESTING=OFF \
      -DLQL_BUILD_DIRECT_PROBE=OFF \
      >/dev/null
  else
    cmake -S . -B "$cmake_build" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/cpkt-toolchain.cmake \
      -DLQL_TARGET_ID=x86_64-linux-gnu \
      -DBUILD_TESTING=OFF \
      -DLQL_BUILD_DIRECT_PROBE=OFF \
      >/dev/null
  fi

  if ! grep "#define LQL_VERSION \"$expected\"" \
    "$cmake_build/generated/include/lql/version.h" >/dev/null; then
    fail "CMake resolved version does not match $expected"
  fi

  if [ -n "$override" ]; then
    script_version=$(LQL_VERSION_OVERRIDE=$override \
      LQL_VERSION_BUILD_DIR="$script_build" sh scripts/release_version.sh)
    make_version=$(LQL_VERSION_OVERRIDE=$override \
      LQL_VERSION_BUILD_DIR="$make_build" \
      make --no-print-directory print-release-version)
  else
    script_version=$(LQL_VERSION_BUILD_DIR="$script_build" \
      sh scripts/release_version.sh)
    make_version=$(LQL_VERSION_BUILD_DIR="$make_build" \
      make --no-print-directory print-release-version)
  fi

  if [ "$script_version" != "$expected" ]; then
    fail "script version is $script_version, expected $expected"
  fi
  if [ "$make_version" != "$expected" ]; then
    fail "Make version is $make_version, expected $expected"
  fi
}

expect_release_candidate_artifacts() {
  expected=$1
  rm -rf "$artifact_dist"
  mkdir -p "$artifact_dist"

  LQL_VERSION_OVERRIDE=$expected \
    LQL_DIST_DIR="$artifact_dist" \
    LQL_PACKAGE_CHECKSUM_MODE=reset \
    sh scripts/package_source.sh >/dev/null
  if [ ! -f "$artifact_dist/liblql-$expected.tar.gz" ] ||
     [ ! -f "$artifact_dist/liblql-$expected-CHECKSUMS" ]; then
    fail "source archive did not use override version $expected"
  fi
  rm -rf build/lifecycle-version-contract-source-extract
  mkdir -p build/lifecycle-version-contract-source-extract
  tar -xzf "$artifact_dist/liblql-$expected.tar.gz" \
    -C build/lifecycle-version-contract-source-extract
  source_version=$(sed -n '1p' \
    "build/lifecycle-version-contract-source-extract/liblql-$expected/VERSION")
  if [ "$source_version" != "$expected" ]; then
    fail "source archive VERSION is $source_version, expected $expected"
  fi

  LQL_VERSION_OVERRIDE=$expected \
    LQL_DIST_DIR="$artifact_dist" \
    LQL_PACKAGE_CHECKSUM_MODE=append \
    sh scripts/package_lua.sh >/dev/null
  for path in \
    "$artifact_dist/liblql-lua-$expected.tar.gz" \
    "$artifact_dist/liblql-$expected-1.rockspec" \
    "$artifact_dist/liblql-$expected-1.src.rock"; do
    if [ ! -f "$path" ]; then
      fail "Lua artifact did not use override version $expected: $path"
    fi
  done
  rm -rf build/lifecycle-version-contract-lua-extract
  mkdir -p build/lifecycle-version-contract-lua-extract
  tar -xzf "$artifact_dist/liblql-lua-$expected.tar.gz" \
    -C build/lifecycle-version-contract-lua-extract
  lua_version=$(sed -n '1p' \
    "build/lifecycle-version-contract-lua-extract/liblql-lua-$expected/VERSION")
  if [ "$lua_version" != "$expected" ]; then
    fail "Lua source VERSION is $lua_version, expected $expected"
  fi
}

exact_version=$(exact_lightweight_version)
if [ -n "$exact_version" ]; then
  expect_version "$exact_version"
  expect_version "$exact_version" 98.97.96
  printf 'lifecycle version contract: exact lightweight tag v%s wins\n' \
    "$exact_version"
  exit 0
fi

git tag -d "$reserved_tag" >/dev/null 2>&1 || true
if [ -e VERSION ]; then
  fail 'refusing to overwrite existing VERSION in git worktree'
fi

printf '%s\n' 12.34.56 >VERSION
created_version_file=1
expect_version 0.0.0
expect_version 98.97.96 98.97.96
expect_release_candidate_artifacts 98.97.96

git tag "$reserved_tag"
created_reserved_tag=1
expect_version 99.99.99
expect_version 99.99.99 98.97.96
git tag -d "$reserved_tag" >/dev/null
created_reserved_tag=0

expect_version 0.0.0
printf 'lifecycle version contract: untagged, override, exact-tag, and VERSION-ignore checks passed\n'
