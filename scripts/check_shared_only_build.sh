#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${LQL_SHARED_ONLY_BUILD_DIR:-"$root/build/shared-only"}
install_dir=$build_dir/install

sh "$root/scripts/remove_path.sh" "$build_dir"
cmake -S "$root" -B "$build_dir" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$root/cmake/cpkt-toolchain.cmake" \
  -DLQL_TARGET_ID=x86_64-linux-gnu \
  -DLQL_BUILD_STATIC=OFF \
  -DLQL_BUILD_SHARED=ON
cmake --build "$build_dir"
ctest --test-dir "$build_dir" --output-on-failure

if [ ! -f "$build_dir/liblql.so" ]; then
  printf 'shared-only build: missing shared liblql artifact\n' >&2
  exit 1
fi
if [ -e "$build_dir/liblql.a" ]; then
  printf 'shared-only build: unexpectedly produced liblql.a\n' >&2
  exit 1
fi

cmake --install "$build_dir" --prefix "$install_dir"
if [ -e "$install_dir/lib/liblql.a" ]; then
  printf 'shared-only build: unexpectedly installed liblql.a\n' >&2
  exit 1
fi
LQL_INSTALL_PREFIX="$install_dir" \
LQL_INSTALL_CONSUMER_DIR="$build_dir/install-consumer" \
  sh "$root/scripts/check_install_tree.sh"

printf '%s\n' 'shared-only build: configured, built, and tested successfully'
