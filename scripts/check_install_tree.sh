#!/bin/sh
set -eu

prefix=${LQL_INSTALL_PREFIX:-build/install-smoke}
work=${LQL_INSTALL_CONSUMER_DIR:-build/install-consumer}
cc=${CC:-}
prefix_abs=$(CDPATH= cd -- "$prefix" && pwd -P)

if [ ! -f "$prefix/include/lql/lql.h" ] ||
   [ ! -f "$prefix/include/lql/version.h" ] ||
   [ ! -f "$prefix/lib/cmake/liblql/liblqlConfig.cmake" ] ||
   [ ! -f "$prefix/lib/pkgconfig/liblql.pc" ]; then
  printf 'install-tree check: incomplete install prefix: %s\n' "$prefix" >&2
  exit 1
fi

if [ -z "$cc" ]; then
  cc=$(scripts/cpkt-toolchains.sh discover x86_64-linux-gnu | sed -n 's/^cc=//p')
fi
if [ ! -x "$cc" ]; then
  printf 'install-tree check: missing C compiler: %s\n' "$cc" >&2
  exit 1
fi

rm -rf "$work"
mkdir -p "$work/cmake" "$work/pkgconfig"

cat >"$work/smoke.c" <<'SMOKE'
#include <lql/lql.h>

int main(void) {
  lql *ctx;
  lql_error error;
  lql_error_init(&error);
  ctx = 0;
  if (lql_new(&ctx, &error) != LQL_STATUS_OK) {
    return 1;
  }
  if (ctx == 0 || ctx->destroy == 0 || LQL_VERSION_MAJOR < 0) {
    return 2;
  }
  ctx->destroy(ctx);
  return 0;
}
SMOKE

cat >"$work/cmake/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(liblql_cmake_consumer C)
find_package(liblql CONFIG REQUIRED)
add_executable(consumer ../smoke.c)
target_link_libraries(consumer PRIVATE liblql::lql_shared)
CMAKE

cmake -S "$work/cmake" -B "$work/cmake-build" \
  -G Ninja \
  -DCMAKE_C_COMPILER="$cc" \
  -DCMAKE_PREFIX_PATH="$prefix_abs" >/dev/null
cmake --build "$work/cmake-build" >/dev/null

PKG_CONFIG_PATH="$prefix_abs/lib/pkgconfig" pkg-config --exists liblql
cflags=$(PKG_CONFIG_PATH="$prefix_abs/lib/pkgconfig" pkg-config --cflags liblql)
libs=$(PKG_CONFIG_PATH="$prefix_abs/lib/pkgconfig" pkg-config --libs liblql)
"$cc" $cflags "$work/smoke.c" $libs -o "$work/pkgconfig/consumer"

printf 'install-tree check: CMake and pkg-config consumers built from %s\n' "$prefix_abs"
