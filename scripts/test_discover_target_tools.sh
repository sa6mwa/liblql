#!/bin/sh
set -eu

tmp=${TMPDIR:-/tmp}/liblql-discover-tools.$$

cleanup() {
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmp/build" "$tmp/toolchain/bin" "$tmp/path"

touch_exec() {
  path=$1
  printf '#!/bin/sh\nexit 0\n' >"$path"
  chmod +x "$path"
}

for tool in gcc ar ranlib strip nm objcopy objdump readelf; do
  touch_exec "$tmp/toolchain/bin/x86_64-linux-$tool"
done

cat >"$tmp/build/CMakeCache.txt" <<EOF_CACHE
CMAKE_C_COMPILER:FILEPATH=$tmp/toolchain/bin/x86_64-linux-gcc
CMAKE_AR:FILEPATH=$tmp/toolchain/bin/x86_64-linux-ar
CMAKE_RANLIB:FILEPATH=$tmp/toolchain/bin/x86_64-linux-ranlib
CMAKE_STRIP:FILEPATH=$tmp/toolchain/bin/x86_64-linux-strip
CMAKE_NM:FILEPATH=$tmp/toolchain/bin/x86_64-linux-nm
CMAKE_OBJCOPY:FILEPATH=$tmp/toolchain/bin/x86_64-linux-objcopy
CMAKE_OBJDUMP:FILEPATH=$tmp/toolchain/bin/x86_64-linux-objdump
CMAKE_READELF:FILEPATH=$tmp/toolchain/bin/x86_64-linux-readelf
LQL_TARGET_ID:STRING=x86_64-linux-gnu
EOF_CACHE

scripts/discover_target_tools.sh "$tmp/build" x86_64-linux-gnu >"$tmp/out"
grep "CC=$tmp/toolchain/bin/x86_64-linux-gcc" "$tmp/out" >/dev/null
grep "READELF=$tmp/toolchain/bin/x86_64-linux-readelf" "$tmp/out" >/dev/null

rm -f "$tmp/toolchain/bin/x86_64-linux-readelf"
touch_exec "$tmp/path/readelf"
PATH="$tmp/path:$PATH" scripts/discover_target_tools.sh \
  "$tmp/build" x86_64-linux-gnu >"$tmp/out"
grep "READELF=$tmp/path/readelf" "$tmp/out" >/dev/null

if scripts/discover_target_tools.sh "$tmp/build" x86_64-linux-musl \
  >"$tmp/out" 2>"$tmp/err"; then
  printf 'discover target tools: target mismatch unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'target mismatch' "$tmp/err" >/dev/null

printf 'target tool discovery tests passed\n'
