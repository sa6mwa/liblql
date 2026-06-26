#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
build_dir="$root/build/clql-static"

compiler_for() {
  target=$1
  case "$target" in
    x86_64-linux-musl)
      command -v "${LQL_CC_X86_64_LINUX_MUSL:-x86_64-linux-musl-gcc}" 2>/dev/null
      ;;
    x86_64-linux-gnu)
      command -v "${CC:-cc}" 2>/dev/null
      ;;
    aarch64-linux-musl)
      command -v "${LQL_CC_AARCH64_LINUX_MUSL:-aarch64-linux-musl-gcc}" 2>/dev/null ||
        command -v "$HOME/.local/cross/aarch64-linux-musl/bin/aarch64-linux-musl-gcc" 2>/dev/null
      ;;
    aarch64-linux-gnu)
      command -v "${CC:-cc}" 2>/dev/null
      ;;
    armhf-linux-musl)
      command -v "${LQL_CC_ARMHF_LINUX_MUSL:-arm-linux-musleabihf-gcc}" 2>/dev/null ||
        command -v armhf-linux-musl-gcc 2>/dev/null ||
        command -v "$HOME/.local/cross/arm-linux-musleabihf/bin/arm-linux-musleabihf-gcc" 2>/dev/null
      ;;
    armhf-linux-gnu)
      command -v "${CC:-cc}" 2>/dev/null
      ;;
    arm64-apple-darwin)
      command -v "${CC:-cc}" 2>/dev/null
      ;;
    *)
      return 1
      ;;
  esac
}

compiler_can_link() {
  cc=$1
  tmp="$root/build/clql-static-compiler-smoke"
  rm -rf "$tmp"
  mkdir -p "$tmp"
  printf '%s\n' 'int main(void) { return 0; }' >"$tmp/smoke.c"
  "$cc" -static "$tmp/smoke.c" -o "$tmp/smoke" >/dev/null 2>&1
}

select_target() {
  os=$(uname -s)
  machine=$(uname -m)
  case "$machine" in
    x86_64|amd64) arch=x86_64 ;;
    aarch64|arm64) arch=aarch64 ;;
    armv7l|armv6l|armhf) arch=armhf ;;
    *) arch= ;;
  esac

  if [ "$os" = Darwin ]; then
    if [ "$machine" = arm64 ] || [ "$machine" = aarch64 ]; then
      cc=$(compiler_for arm64-apple-darwin || true)
      if [ -n "$cc" ]; then
        printf '%s %s\n' arm64-apple-darwin "$cc"
        return 0
      fi
    fi
    printf 'build-clql: no supported native lonejson SDK target for %s/%s\n' "$os" "$machine" >&2
    exit 1
  fi

  if [ "$os" != Linux ] || [ -z "$arch" ]; then
    printf 'build-clql: unsupported native target for static clql: %s/%s\n' "$os" "$machine" >&2
    exit 1
  fi

  musl_target="${arch}-linux-musl"
  gnu_target="${arch}-linux-gnu"

  musl_cc=$(compiler_for "$musl_target" || true)
  if [ -n "$musl_cc" ] && compiler_can_link "$musl_cc"; then
    printf '%s %s\n' "$musl_target" "$musl_cc"
    return 0
  fi

  gnu_cc=$(compiler_for "$gnu_target" || true)
  if [ -n "$gnu_cc" ] && compiler_can_link "$gnu_cc"; then
    printf '%s %s\n' "$gnu_target" "$gnu_cc"
    return 0
  fi

  printf 'build-clql: no static-capable host musl or GNU compiler found for %s\n' "$arch" >&2
  printf 'build-clql: install a host musl compiler or static libc support for CC\n' >&2
  exit 1
}

set -- $(select_target)
target_id=$1
cc=$2

case "$target_id" in
  *-linux-musl)
    target_libc=musl
    target_os=linux
    ;;
  *-linux-gnu)
    target_libc=gnu
    target_os=linux
    ;;
  arm64-apple-darwin)
    target_libc=
    target_os=darwin
    ;;
  *)
    printf 'build-clql: unsupported selected target: %s\n' "$target_id" >&2
    exit 1
    ;;
esac
target_arch=${target_id%%-*}

"$root/scripts/deps.sh" "$target_id"

cmake -S "$root" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$cc" \
  -DLQL_TARGET_ID="$target_id" \
  -DLQL_TARGET_ARCH="$target_arch" \
  -DLQL_TARGET_OS="$target_os" \
  -DLQL_TARGET_LIBC="$target_libc" \
  -DLQL_DEPENDENCY_MODE=bundled \
  -DLQL_LONEJSON_STATIC=ON \
  -DLQL_BUILD_STATIC=ON \
  -DLQL_BUILD_SHARED=OFF \
  -DLQL_BUILD_BINARY=ON \
  -DLQL_CLQL_STATIC_LINK=ON \
  -DLQL_BUILD_TESTS=OFF \
  -DLQL_BUILD_EXAMPLES=OFF \
  -DLQL_BUILD_LUA_MODULE=OFF
cmake --build "$build_dir" --target clql

if command -v file >/dev/null 2>&1; then
  file "$build_dir/clql"
fi
printf 'build-clql: wrote %s for %s\n' "$build_dir/clql" "$target_id"
