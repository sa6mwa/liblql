#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${LQL_PKGCONFIG_MULTIARCH_BUILD_DIR:-"$root/build/pkgconfig-multiarch"}
toolchain_override=${LIBLQL_TOOLCHAIN_OVERRIDE:-}
toolchain=$($root/scripts/cpkt-toolchains.sh discover x86_64-linux-gnu)

toolchain_value() {
  printf '%s\n' "$toolchain" | sed -n "s/^$1=//p"
}

if [ "$toolchain_override" = "1" ]; then
  cc_name=${CC:-cc}
  cc=$(command -v "$cc_name" 2>/dev/null || :)
else
  cc=$(toolchain_value cc)
fi
if [ ! -x "$cc" ]; then
  printf 'pkg-config multiarch: missing C compiler: %s\n' "${cc_name:-$cc}" >&2
  exit 1
fi
runtime_loader=$(toolchain_value dynamic_loader)
runtime_dirs=$(toolchain_value runtime_library_dirs)
readelf_tool=$(toolchain_value readelf)
if [ "$toolchain_override" != "1" ] && \
   { [ -z "$runtime_loader" ] || [ -z "$runtime_dirs" ] || [ ! -x "$readelf_tool" ]; }; then
  printf '%s\n' 'pkg-config multiarch: incomplete Bootlin runtime metadata' >&2
  exit 1
fi

sh "$root/scripts/remove_path.sh" "$build_root"

normalize_path() {
  python3 -c 'import os, sys; print(os.path.normpath(sys.argv[1]))' "$1"
}

check_case() {
  name=$1
  libdir=$2
  install_prefix=$3
  includedir=${4:-include}
  destdir=${5:-}
  build_dir=$build_root/$name/build
  consumer_dir=$build_root/$name/consumer
  mkdir -p "$consumer_dir"

  cmake -S "$root" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/cpkt-toolchain.cmake" \
    -DLQL_TARGET_ID=x86_64-linux-gnu \
    -DCMAKE_INSTALL_PREFIX="$install_prefix" \
    -DCMAKE_INSTALL_LIBDIR="$libdir" \
    -DCMAKE_INSTALL_INCLUDEDIR="$includedir" \
    -DBUILD_TESTING=OFF \
    -DLQL_BUILD_BINARY=OFF \
    -DLQL_BUILD_DIRECT_PROBE=OFF >/dev/null
  cmake --build "$build_dir" >/dev/null
  if [ -n "$destdir" ]; then
    DESTDIR="$destdir" cmake --install "$build_dir" >/dev/null
  else
    cmake --install "$build_dir" >/dev/null
  fi

  if [ "${libdir#/}" != "$libdir" ]; then
    pc_dir=$(normalize_path "$destdir$libdir/pkgconfig")
  else
    pc_dir=$(normalize_path "$destdir$install_prefix/$libdir/pkgconfig")
  fi
  pc_path=$pc_dir/liblql.pc
  if [ ! -f "$pc_path" ]; then
    printf 'pkg-config multiarch: missing generated pc file: %s\n' "$pc_path" >&2
    exit 1
  fi

  cat >"$consumer_dir/smoke.c" <<'SMOKE'
#include <lql/lql.h>

int main(void) {
  lql *ctx;
  lql_error error;
  lql_error_init(&error);
  ctx = 0;
  if (lql_new(&ctx, &error) != LQL_STATUS_OK) {
    return 1;
  }
  ctx->destroy(ctx);
  return 0;
}
SMOKE

  PKG_CONFIG_PATH="$pc_dir" PKG_CONFIG_SYSROOT_DIR="$destdir" \
    pkg-config --exists liblql
  cflags=$(PKG_CONFIG_PATH="$pc_dir" PKG_CONFIG_SYSROOT_DIR="$destdir" \
    pkg-config --cflags liblql)
  libs=$(PKG_CONFIG_PATH="$pc_dir" PKG_CONFIG_SYSROOT_DIR="$destdir" \
    pkg-config --libs liblql)
  loader_libdir=$(PKG_CONFIG_PATH="$pc_dir" pkg-config --variable=libdir liblql)
  case $loader_libdir in
    "$destdir"/* | "")
      ;;
    /*)
      loader_libdir=$destdir$loader_libdir
      ;;
  esac
  runtime_flags=
  if [ "$toolchain_override" = "1" ]; then
    runtime_flags="-Wl,-rpath,$loader_libdir"
  else
    runtime_flags="-Wl,--dynamic-linker=$runtime_loader"
    old_ifs=$IFS
    IFS=:
    set -- $runtime_dirs
    IFS=$old_ifs
    for runtime_dir
    do
      runtime_flags="$runtime_flags -Wl,--disable-new-dtags -Wl,-rpath,$runtime_dir"
    done
    runtime_flags="$runtime_flags -Wl,--disable-new-dtags -Wl,-rpath,$loader_libdir"
  fi
  "$cc" $cflags "$consumer_dir/smoke.c" $libs $runtime_flags \
    -o "$consumer_dir/smoke"
  if [ "$toolchain_override" != "1" ]; then
    if ! "$readelf_tool" -l "$consumer_dir/smoke" | \
      grep -F "Requesting program interpreter: $runtime_loader]" >/dev/null; then
      printf 'pkg-config multiarch: consumer has wrong Bootlin interpreter: %s\n' \
        "$consumer_dir/smoke" >&2
      exit 1
    fi
    dynamic=$($readelf_tool -d "$consumer_dir/smoke")
    if printf '%s\n' "$dynamic" | grep 'RUNPATH' >/dev/null; then
      printf 'pkg-config multiarch: consumer uses RUNPATH: %s\n' \
        "$consumer_dir/smoke" >&2
      exit 1
    fi
    old_ifs=$IFS
    IFS=:
    set -- $runtime_dirs "$loader_libdir"
    IFS=$old_ifs
    for runtime_dir
    do
      if ! printf '%s\n' "$dynamic" | grep -F "$runtime_dir" >/dev/null; then
        printf 'pkg-config multiarch: consumer is missing RPATH %s\n' \
          "$runtime_dir" >&2
        exit 1
      fi
    done
  fi
  "$consumer_dir/smoke"

  printf 'pkg-config multiarch: %s consumer built from %s\n' "$name" "$pc_path"
}

check_case relative \
  lib/x86_64-linux-gnu \
  "$build_root/relative/install"
check_case dot-libdir \
  . \
  "$build_root/dot-libdir/install"
check_case normalized-relative \
  lib/../lib64 \
  "$build_root/normalized-relative/install"
check_case absolute-prefix \
  "$build_root/absolute-prefix/install/lib/x86_64-linux-gnu" \
  "$build_root/absolute-prefix/install"
check_case absolute-outside-prefix \
  "$build_root/absolute-outside/lib/x86_64-linux-gnu" \
  "$build_root/absolute-outside/prefix"
check_case absolute-include-outside-prefix \
  lib/x86_64-linux-gnu \
  /opt/liblql-absolute-include \
  /opt/liblql-absolute-headers \
  "$build_root/absolute-include/rootfs"
