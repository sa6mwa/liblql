#!/bin/sh
set -eu

prefix=${LQL_INSTALL_PREFIX:-build/install-smoke}
work=${LQL_INSTALL_CONSUMER_DIR:-build/install-consumer}
prefix_abs=$(CDPATH= cd -- "$prefix" && pwd -P)

if [ ! -f "$prefix/include/lql/lql.h" ] ||
   [ ! -f "$prefix/include/lql/version.h" ] ||
   [ ! -f "$prefix/lib/cmake/liblql/liblqlConfig.cmake" ] ||
   [ ! -f "$prefix/lib/pkgconfig/liblql.pc" ]; then
  printf 'install-tree check: incomplete install prefix: %s\n' "$prefix" >&2
  exit 1
fi

toolchain_override=OFF
runtime_loader=
runtime_dirs=
runtime_flags=
consumer_rpath=
if [ "${LIBLQL_TOOLCHAIN_OVERRIDE:-}" = "1" ]; then
  toolchain_override=ON
  cc_name=${CC:-cc}
  cc=$(command -v "$cc_name" 2>/dev/null || :)
  consumer_rpath="-Wl,-rpath,$prefix_abs/lib"
else
  toolchain_description=$(scripts/cpkt-toolchains.sh discover x86_64-linux-gnu)
  toolchain_value() {
    printf '%s\n' "$toolchain_description" | sed -n "s/^$1=//p"
  }
  cc=$(toolchain_value cc)
  runtime_loader=$(toolchain_value dynamic_loader)
  runtime_dirs=$(toolchain_value runtime_library_dirs)
  if [ -z "$runtime_loader" ] || [ -z "$runtime_dirs" ]; then
    printf '%s\n' 'install-tree check: selected Bootlin runtime metadata is incomplete' >&2
    exit 1
  fi
  runtime_flags="-Wl,--dynamic-linker=$runtime_loader"
  old_ifs=$IFS
  IFS=:
  set -- $runtime_dirs
  IFS=$old_ifs
  for runtime_dir
  do
    runtime_flags="$runtime_flags -Wl,--disable-new-dtags -Wl,-rpath,$runtime_dir"
  done
  consumer_rpath="-Wl,--disable-new-dtags -Wl,-rpath,$prefix_abs/lib"
fi
if [ ! -x "$cc" ]; then
  printf 'install-tree check: missing C compiler: %s\n' "${cc_name:-$cc}" >&2
  exit 1
fi

sh scripts/remove_path.sh "$work"
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
if(NOT LQL_SMOKE_TOOLCHAIN_OVERRIDE)
  target_link_options(consumer PRIVATE
    "-Wl,--dynamic-linker=${LQL_SMOKE_DYNAMIC_LOADER}"
    "-Wl,--disable-new-dtags"
    "-Wl,-rpath,${LQL_SMOKE_PREFIX_LIB}")
  string(REPLACE ":" ";" LQL_SMOKE_RUNTIME_DIRS_LIST
    "${LQL_SMOKE_RUNTIME_DIRS}")
  foreach(runtime_dir IN LISTS LQL_SMOKE_RUNTIME_DIRS_LIST)
    target_link_options(consumer PRIVATE
      "-Wl,--disable-new-dtags"
      "-Wl,-rpath,${runtime_dir}")
  endforeach()
else()
  target_link_options(consumer PRIVATE "-Wl,-rpath,${LQL_SMOKE_PREFIX_LIB}")
endif()
CMAKE

if [ "$toolchain_override" = ON ]; then
  cmake -S "$work/cmake" -B "$work/cmake-build" \
    -G Ninja \
    -DCMAKE_C_COMPILER="$cc" \
    -DLQL_SMOKE_PREFIX_LIB="$prefix_abs/lib" \
    -DLQL_SMOKE_TOOLCHAIN_OVERRIDE=ON \
    -DCMAKE_PREFIX_PATH="$prefix_abs" >/dev/null
else
  cmake -S "$work/cmake" -B "$work/cmake-build" \
    -G Ninja \
    -DCMAKE_C_COMPILER="$cc" \
    -DLQL_SMOKE_DYNAMIC_LOADER="$runtime_loader" \
    -DLQL_SMOKE_RUNTIME_DIRS="$runtime_dirs" \
    -DLQL_SMOKE_PREFIX_LIB="$prefix_abs/lib" \
    -DLQL_SMOKE_TOOLCHAIN_OVERRIDE=OFF \
    -DCMAKE_PREFIX_PATH="$prefix_abs" >/dev/null
fi
cmake --build "$work/cmake-build" >/dev/null

PKG_CONFIG_PATH="$prefix_abs/lib/pkgconfig" pkg-config --exists liblql
cflags=$(PKG_CONFIG_PATH="$prefix_abs/lib/pkgconfig" pkg-config --cflags liblql)
libs=$(PKG_CONFIG_PATH="$prefix_abs/lib/pkgconfig" pkg-config --libs liblql)
"$cc" $cflags "$work/smoke.c" $libs $runtime_flags $consumer_rpath \
  -o "$work/pkgconfig/consumer"

"$work/cmake-build/consumer"
"$work/pkgconfig/consumer"

printf 'install-tree check: CMake and pkg-config consumers built and ran from %s\n' "$prefix_abs"
