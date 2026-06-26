#!/bin/sh
set -eu

root=${1:?usage: check_release_version.sh REPO_ROOT}
version_script=$root/scripts/release_version.sh
cmake_module=$root/cmake/LqlVersion.cmake

if [ ! -x "$version_script" ]; then
  printf 'release version check: script is not executable: %s\n' \
    "$version_script" >&2
  exit 2
fi
if [ ! -f "$cmake_module" ]; then
  printf 'release version check: CMake module missing: %s\n' \
    "$cmake_module" >&2
  exit 2
fi

expect_eq() {
  label=$1
  want=$2
  got=$3
  if [ "$got" != "$want" ]; then
    printf 'release version check: %s mismatch: got %s want %s\n' \
      "$label" "$got" "$want" >&2
    exit 1
  fi
}

git_cmd() {
  git -c user.name='liblql test' -c user.email='liblql-test@example.invalid' "$@"
}

resolve_shell() {
  repo=$1
  (cd "$repo" && "$version_script")
}

resolve_cmake() {
  repo=$1
  build=$2
  override=${3:-}
  cat >"$repo/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.20)
project(version_fixture LANGUAGES NONE)
set(LQL_VERSION_OVERRIDE "" CACHE STRING "Version override for tests")
include("$cmake_module")
file(WRITE "\${CMAKE_BINARY_DIR}/resolved.txt" "\${LQL_RESOLVED_VERSION}\\n")
EOF
  if [ -n "$override" ]; then
    cmake -S "$repo" -B "$build" -DLQL_VERSION_OVERRIDE="$override" \
      >/dev/null
  else
    cmake -S "$repo" -B "$build" >/dev/null
  fi
  sed -n '1p' "$build/resolved.txt"
}

tmp=${TMPDIR:-/tmp}/liblql-release-version.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

repo=$tmp/repo
mkdir -p "$repo"
(cd "$repo" && git init -q)
printf 'source-version-should-be-ignored\n' >"$repo/VERSION"
printf 'fixture\n' >"$repo/file.txt"
(cd "$repo" && git_cmd add file.txt VERSION)
(cd "$repo" && git_cmd commit -q -m 'fixture')

expect_eq "untagged git shell version" "0.0.0" "$(resolve_shell "$repo")"
expect_eq "untagged git CMake version" "0.0.0" \
  "$(resolve_cmake "$repo" "$tmp/build-untagged")"

(cd "$repo" && git_cmd tag -a v1.2.3 -m 'annotated fixture')
expect_eq "annotated git shell version" "0.0.0" "$(resolve_shell "$repo")"
expect_eq "annotated git CMake version" "0.0.0" \
  "$(resolve_cmake "$repo" "$tmp/build-annotated")"

(cd "$repo" && git tag v1.2.4)
expect_eq "lightweight git shell version" "1.2.4" "$(resolve_shell "$repo")"
expect_eq "lightweight git CMake version" "1.2.4" \
  "$(resolve_cmake "$repo" "$tmp/build-lightweight")"

archive=$tmp/archive
mkdir -p "$archive"
cp "$version_script" "$archive/release_version.sh"
printf '2.3.4\n' >"$archive/VERSION"
expect_eq "source archive shell version" "2.3.4" \
  "$(cd "$archive" && ./release_version.sh)"

expect_eq "shell override version" "9.8.7" \
  "$(cd "$repo" && LQL_VERSION_OVERRIDE=9.8.7 "$version_script")"
expect_eq "Make override version" "9.8.7" \
  "$(cd "$root" && LQL_VERSION_OVERRIDE=9.8.7 make -s print-release-version)"
expect_eq "CMake override version" "9.8.7" \
  "$(resolve_cmake "$repo" "$tmp/build-override" "9.8.7")"
