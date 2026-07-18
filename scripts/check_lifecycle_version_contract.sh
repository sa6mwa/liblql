#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

reserved_tag=v99.99.99
invalid_reserved_tag=v99.99.99-rc1
cmake_build=build/lifecycle-version-contract-cmake
script_build=build/lifecycle-version-contract-script
make_build=build/lifecycle-version-contract-make
artifact_dist=build/lifecycle-version-contract-dist
dirty_dist=build/lifecycle-version-contract-dirty-dist
dirty_path=build/lifecycle-version-contract-dirty-path.tmp
deleted_dist=build/lifecycle-version-contract-deleted-dist
deleted_path=.lifecycle-version-contract-deleted.tmp
created_reserved_tag=0
created_invalid_reserved_tag=0
created_version_file=0
added_dirty_path=0
added_deleted_path=0

remove_path() {
  path=$1
  if [ ! -e "$path" ]; then
    return 0
  fi
  if [ -d "$path" ] && [ ! -L "$path" ]; then
    find "$path" ! -type d -exec rm -- {} \;
    find "$path" -depth -type d -exec rmdir -- {} \;
  else
    rm -- "$path"
  fi
}

cleanup() {
  if [ "$added_dirty_path" = 1 ]; then
    git reset -q -- "$dirty_path" >/dev/null 2>&1 || true
    remove_path "$dirty_path"
  fi
  if [ "$added_deleted_path" = 1 ]; then
    git reset -q -- "$deleted_path" >/dev/null 2>&1 || true
    remove_path "$deleted_path"
  fi
  if [ "$created_reserved_tag" = 1 ]; then
    git tag -d "$reserved_tag" >/dev/null 2>&1 || true
  fi
  if [ "$created_invalid_reserved_tag" = 1 ]; then
    git tag -d "$invalid_reserved_tag" >/dev/null 2>&1 || true
  fi
  if [ "$created_version_file" = 1 ]; then
    remove_path VERSION
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
    if ! printf '%s\n' "$tag" | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' >/dev/null; then
      continue
    fi
    type=$(git cat-file -t "refs/tags/$tag" 2>/dev/null || true)
    if [ "$type" != commit ]; then
      continue
    fi
    version=${tag#v}
    if [ -n "$found" ] && [ "$found" != "$version" ]; then
      fail "multiple lightweight release tags point at HEAD: $found and $version"
    fi
    found=$version
  done
  printf '%s\n' "$found"
}

expect_version() {
  expected=$1
  override=${2:-}

  remove_path "$cmake_build"
  remove_path "$script_build"
  remove_path "$make_build"
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
  remove_path "$artifact_dist"
  mkdir -p "$artifact_dist"

  LQL_VERSION_OVERRIDE=$expected \
    LQL_DIST_DIR="$artifact_dist" \
    LQL_PACKAGE_CHECKSUM_MODE=reset \
    sh scripts/package_source.sh >/dev/null
  if [ ! -f "$artifact_dist/liblql-$expected.tar.gz" ] ||
     [ ! -f "$artifact_dist/liblql-$expected-CHECKSUMS" ]; then
    fail "source archive did not use override version $expected"
  fi
  remove_path build/lifecycle-version-contract-source-extract
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
  remove_path build/lifecycle-version-contract-lua-extract
  mkdir -p build/lifecycle-version-contract-lua-extract
  tar -xzf "$artifact_dist/liblql-lua-$expected.tar.gz" \
    -C build/lifecycle-version-contract-lua-extract
  lua_version=$(sed -n '1p' \
    "build/lifecycle-version-contract-lua-extract/liblql-lua-$expected/VERSION")
  if [ "$lua_version" != "$expected" ]; then
    fail "Lua source VERSION is $lua_version, expected $expected"
  fi
}

expect_exact_tag_dirty_source_rejected() {
  remove_path "$dirty_dist"
  remove_path "$dirty_path"
  printf '%s\n' dirty >"$dirty_path"
  git add -N -f "$dirty_path"
  added_dirty_path=1
  if LQL_DIST_DIR="$dirty_dist" sh scripts/package_source.sh \
    >build/lifecycle-version-contract-dirty.out \
    2>build/lifecycle-version-contract-dirty.err; then
    fail 'exact tagged source archive accepted a dirty tracked worktree'
  fi
  if ! grep 'requires a clean tracked worktree' \
    build/lifecycle-version-contract-dirty.err >/dev/null; then
    fail 'exact tagged source archive dirty failure was not actionable'
  fi
  git reset -q -- "$dirty_path"
  remove_path "$dirty_path"
  added_dirty_path=0
}

expect_deleted_manifest_excluded() {
  expected=$1
  remove_path "$deleted_dist"
  remove_path "$deleted_path"
  printf '%s\n' deleted >"$deleted_path"
  git add -N -f "$deleted_path"
  added_deleted_path=1
  remove_path "$deleted_path"

  LQL_VERSION_OVERRIDE=$expected \
    LQL_DIST_DIR="$deleted_dist" \
    LQL_PACKAGE_CHECKSUM_MODE=reset \
    sh scripts/package_source.sh >/dev/null
  if tar -xOf "$deleted_dist/liblql-$expected.tar.gz" \
      "liblql-$expected/RELEASE_MANIFEST" |
      grep -Fx "$deleted_path" >/dev/null; then
    fail "source archive RELEASE_MANIFEST included deleted file $deleted_path"
  fi
  git reset -q -- "$deleted_path"
  added_deleted_path=0
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
expect_deleted_manifest_excluded 98.97.96

git tag "$invalid_reserved_tag"
created_invalid_reserved_tag=1
expect_version 0.0.0
git tag -d "$invalid_reserved_tag" >/dev/null
created_invalid_reserved_tag=0

git tag "$reserved_tag"
created_reserved_tag=1
expect_version 99.99.99
expect_version 99.99.99 98.97.96
expect_exact_tag_dirty_source_rejected
git tag -d "$reserved_tag" >/dev/null
created_reserved_tag=0

expect_version 0.0.0
printf 'lifecycle version contract: untagged, override, exact-tag, dirty-tag, and VERSION-ignore checks passed\n'
