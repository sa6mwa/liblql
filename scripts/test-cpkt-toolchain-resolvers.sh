#!/usr/bin/env bash
set -euo pipefail

skill_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
bootlin_resolver="$skill_dir/scripts/cpkt-toolchains.sh"
cache=$(mktemp -d "${TMPDIR:-/tmp}/cpkt-toolchain-test.XXXXXX")
trap 'sh "$skill_dir/scripts/remove_path.sh" "$cache"' EXIT HUP INT TERM

fail() {
  printf 'test-cpkt-toolchain-resolvers: %s\n' "$*" >&2
  exit 1
}

grep -Fq 'with_cache_lock "$(cache_root)/locks/bootlin-$name.lock" install_bootlin_locked "$target"' "$bootlin_resolver" || fail 'Bootlin root publication is not serialized'
grep -Fq 'if bootlin_ready "$root" "$prefix" "$root/$sysroot_rel"; then' "$bootlin_resolver" || fail 'Bootlin root readiness is not rechecked under the lock'

require_line() {
  local expected=$1 output=$2
  printf '%s\n' "$output" | grep -Fqx "$expected" || fail "missing output: $expected"
}

make_executable() {
  printf '%b\n' "$2" > "$1"
  chmod +x "$1"
}

make_bootlin_collection() {
  local name=$1 prefix=$2 sysroot_rel=$3 root sysroot tool
  root="$cache/roots/$name"
  sysroot="$root/$sysroot_rel"
  mkdir -p "$root/bin" "$sysroot/usr/include" "$sysroot/usr/lib" "$sysroot/lib" "$root/runtime"
  : > "$sysroot/usr/include/stdio.h"
  : > "$sysroot/usr/lib/libc.so"
  : > "$sysroot/usr/lib/libc.so.6"
  : > "$sysroot/usr/lib/libgcc_s.so.1"
  : > "$sysroot/usr/lib/libstdc++.so.6"
  : > "$sysroot/lib/ld-linux-x86-64.so.2"
  chmod +x "$sysroot/lib/ld-linux-x86-64.so.2"
  : > "$root/runtime/libstdc++.a"
  : > "$root/runtime/libgcc.a"
  for tool in ld ar ranlib strip nm objcopy objdump addr2line gdb readelf; do
    make_executable "$root/bin/$prefix-$tool" '#!/bin/sh\nexit 0'
  done
  make_executable "$root/bin/$prefix-gcc" "#!/bin/sh\ncase \"\$1\" in\n  -print-file-name=libc.so.6) printf '%s\\n' '$sysroot/usr/lib/libc.so.6' ;;\n  -print-file-name=libgcc_s.so.1) printf '%s\\n' '$sysroot/usr/lib/libgcc_s.so.1' ;;\n  *) exit 1 ;;\nesac"
  make_executable "$root/bin/$prefix-g++" "#!/bin/sh\ncase \"\$1\" in\n  -print-file-name=libstdc++.a) printf '%s\\n' '$root/runtime/libstdc++.a' ;;\n  -print-file-name=libgcc.a) printf '%s\\n' '$root/runtime/libgcc.a' ;;\n  -print-file-name=libstdc++.so.6) printf '%s\\n' '$sysroot/usr/lib/libstdc++.so.6' ;;\n  *) exit 1 ;;\nesac"
}

make_bootlin_collection \
  x86-64--glibc--stable-2026.08-1 \
  x86_64-linux \
  x86_64-buildroot-linux-gnu/sysroot

bootlin_description=$(CPKT_TOOLCHAIN_CACHE="$cache" "$bootlin_resolver" discover x86_64-linux-gnu)
require_line 'source=bootlin' "$bootlin_description"
require_line 'status=ready' "$bootlin_description"
require_line "cc=$cache/roots/x86-64--glibc--stable-2026.08-1/bin/x86_64-linux-gcc" "$bootlin_description"
require_line "ld=$cache/roots/x86-64--glibc--stable-2026.08-1/bin/x86_64-linux-ld" "$bootlin_description"
require_line "nm=$cache/roots/x86-64--glibc--stable-2026.08-1/bin/x86_64-linux-nm" "$bootlin_description"
require_line "dynamic_loader=$cache/roots/x86-64--glibc--stable-2026.08-1/x86_64-buildroot-linux-gnu/sysroot/lib/ld-linux-x86-64.so.2" "$bootlin_description"
require_line "runtime_library_dirs=$cache/roots/x86-64--glibc--stable-2026.08-1/x86_64-buildroot-linux-gnu/sysroot/lib:$cache/roots/x86-64--glibc--stable-2026.08-1/x86_64-buildroot-linux-gnu/sysroot/usr/lib" "$bootlin_description"
bootlin_env=$(CPKT_TOOLCHAIN_CACHE="$cache" "$bootlin_resolver" env x86_64-linux-gnu)
printf '%s\n' "$bootlin_env" | grep -Fq "export CC=$cache/roots/x86-64--glibc--stable-2026.08-1/bin/x86_64-linux-gcc" || fail 'Bootlin env did not export the pinned compiler'
printf '%s\n' "$bootlin_env" | grep -Fq "export LD=$cache/roots/x86-64--glibc--stable-2026.08-1/bin/x86_64-linux-ld" || fail 'Bootlin env did not export the pinned linker'
printf '%s\n' "$bootlin_env" | grep -Fq "export NM=$cache/roots/x86-64--glibc--stable-2026.08-1/bin/x86_64-linux-nm" || fail 'Bootlin env did not export the pinned nm'

make_bootlin_collection \
  x86-64--musl--stable-2026.08-1 \
  x86_64-linux \
  x86_64-buildroot-linux-musl/sysroot
musl_sysroot=$cache/roots/x86-64--musl--stable-2026.08-1/x86_64-buildroot-linux-musl/sysroot
rm -f "$musl_sysroot/lib/ld-linux-x86-64.so.2"
: > "$musl_sysroot/lib/libc.so"
chmod +x "$musl_sysroot/lib/libc.so"
ln -s libc.so "$musl_sysroot/lib/ld-musl-x86_64.so.1"
musl_description=$(CPKT_TOOLCHAIN_CACHE="$cache" "$bootlin_resolver" discover x86_64-linux-musl)
require_line "dynamic_loader=$musl_sysroot/lib/ld-musl-x86_64.so.1" "$musl_description"
require_line "runtime_library_dirs=$musl_sysroot/lib:$musl_sysroot/usr/lib" "$musl_description"

printf 'toolchain resolver tests passed\n'
