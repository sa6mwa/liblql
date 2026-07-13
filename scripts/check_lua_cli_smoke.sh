#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tree=${LQL_LUAROCKS_TREE:-$root/build/luarocks}
sdk_prefix=${LQL_LUA_SDK_PREFIX:-$root/build/lua-sdk}
fixture=${LQL_LUA_CLI_FIXTURE:-$root/examples/status.ndjson}
lua_cli=$tree/bin/lql.lua

if [ ! -x "$lua_cli" ]; then
  printf 'lua cli smoke: missing installed lql.lua CLI: %s\n' "$lua_cli" >&2
  exit 1
fi

tmp=${TMPDIR:-/tmp}/liblql-lua-cli.$$
cleanup() {
  rm -f "$tmp.out" "$tmp.err"
}
trap cleanup EXIT HUP INT TERM

env \
  LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
  LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
  LD_LIBRARY_PATH="$sdk_prefix/lib:${LD_LIBRARY_PATH:-}" \
  "$lua_cli" '/status="open"' "$fixture" >"$tmp.out"
grep '"status":"open"' "$tmp.out" >/dev/null

env \
  LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
  LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
  LD_LIBRARY_PATH="$sdk_prefix/lib:${LD_LIBRARY_PATH:-}" \
  "$lua_cli" --count '/status="open"' "$fixture" >"$tmp.out"
grep '^2$' "$tmp.out" >/dev/null

if env \
  LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
  LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
  LD_LIBRARY_PATH="$sdk_prefix/lib:${LD_LIBRARY_PATH:-}" \
  "$lua_cli" -m '/status=ready' '/status="open"' "$fixture" \
    >"$tmp.out" 2>"$tmp.err"; then
  printf 'lua cli smoke: unsupported mutation flag unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'unsupported Go lql flag' "$tmp.err" >/dev/null

printf 'lua cli smoke: selected output, count, and unsupported flag passed\n'
