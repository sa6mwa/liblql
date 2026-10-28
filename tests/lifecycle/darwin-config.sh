#!/usr/bin/env bash
# Verify actual presets and package entrypoints without a compiler or osxcross.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
work=$(mktemp -d "$root/build/tmp/darwin-config.XXXXXXXX")
trap 'sh "$root/scripts/remove_path.sh" "$work"' EXIT
fixture=$work/checkout
mkdir -p "$fixture/cmake" "$fixture/scripts" "$work/bin"
cp "$root/CMakePresets.json" "$fixture/"
cp "$root/scripts/"{package.sh,package_clql.sh,remove_path.sh} "$fixture/scripts/"
: >"$fixture/cmake/cpkt-toolchain.cmake"
printf fixture >"$fixture/LICENSE"
printf fixture >"$fixture/README.md"
for script in deps.sh check_owned_path.sh; do
  printf '#!/bin/sh\nexit 0\n' >"$fixture/scripts/$script"
done
# Stop at staging after configuration/build. Running CTest for Darwin is a
# separate failure, so enabling test compilation cannot enable cross execution.
printf '#!/bin/sh\nexit 73\n' >"$fixture/scripts/create_tar_gz.sh"
cat >"$fixture/CMakeLists.txt" <<'PROJECT'
cmake_minimum_required(VERSION 3.20)
project(darwin_configuration NONE)
option(BUILD_TESTING "" ON)
option(LQL_BUILD_STATIC "" ON)
option(LQL_BUILD_SHARED "" ON)
option(LQL_BUILD_BINARY "" ON)
option(LQL_CLQL_STATIC "" OFF)
option(LQL_BUILD_DIRECT_PROBE "" ON)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/include/lql")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/include/lql/version.h"
  "#define LQL_VERSION \"0.0.0\"\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/clql" "fixture")
PROJECT
export REAL_CMAKE
REAL_CMAKE=$(command -v cmake)
cat >"$work/bin/cmake" <<'CMAKE'
#!/bin/sh
case "$1" in
  --build) exit 0 ;;
  --install) exit 73 ;;
  *) exec "$REAL_CMAKE" "$@" ;;
esac
CMAKE
printf '#!/bin/sh\nexit 89\n' >"$work/bin/ctest"
chmod +x "$work/bin/cmake" "$work/bin/ctest"
export PATH="$work/bin:$PATH"
cd "$fixture"
preset=arm64-apple-darwin-release
cache=$fixture/build/$preset/CMakeCache.txt
fail() { printf 'Darwin configuration: %s\n' "$*" >&2; exit 1; }
check_cache() {
  local variable expected actual
  for variable in BUILD_TESTING LQL_BUILD_STATIC LQL_BUILD_SHARED LQL_BUILD_BINARY LQL_CLQL_STATIC LQL_BUILD_DIRECT_PROBE; do
    expected=ON
    [[ "$variable" != LQL_BUILD_DIRECT_PROBE ]] || expected=OFF
    actual=$(sed -n "s/^$variable:BOOL=//p" "$cache")
    [[ "$actual" == "$expected" ]] || fail "$variable=$actual, expected $expected"
  done
}
poison_cache() {
  cmake --preset "$preset" -DBUILD_TESTING=OFF -DLQL_BUILD_STATIC=OFF \
    -DLQL_BUILD_SHARED=OFF -DLQL_BUILD_BINARY=OFF -DLQL_CLQL_STATIC=OFF \
    -DLQL_BUILD_DIRECT_PROBE=ON >"$work/configure.log" 2>&1
}
cmake --preset "$preset" >"$work/configure.log" 2>&1
check_cache
poison_cache
cmake --preset "$preset" >"$work/configure.log" 2>&1
check_cache
for script in package.sh package_clql.sh; do
  poison_cache
  status=0
  LQL_PACKAGE_CHECKSUM_MODE=append sh "scripts/$script" arm64-apple-darwin \
    >"$work/$script.log" 2>&1 || status=$?
  if [[ "$status" != 73 ]]; then
    cat "$work/$script.log"
    fail "$script stopped with $status instead of reaching staging without CTest"
  fi
  check_cache
done
printf 'Darwin configuration: cold/warm presets and SDK/CLI packaging preserve test compilation without cross execution\n'
