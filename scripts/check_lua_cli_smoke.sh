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
  sh "$root/scripts/remove_path.sh" "$tmp.out" "$tmp.err" "$tmp.json" \
    "$tmp.content" "$tmp.empty"
}
trap cleanup EXIT HUP INT TERM

env \
  LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
  LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
  LD_LIBRARY_PATH="$sdk_prefix/lib:${LD_LIBRARY_PATH:-}" \
  "$lua_cli" --help >"$tmp.out"
grep 'explicitly spooled path' "$tmp.out" >/dev/null

env \
  LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
  LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
  LD_LIBRARY_PATH="$sdk_prefix/lib:${LD_LIBRARY_PATH:-}" \
  "$lua_cli" '/status="open"' "$fixture" >"$tmp.out"
grep '"status":"open"' "$tmp.out" >/dev/null

cat "$fixture" | env \
  LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
  LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
  LD_LIBRARY_PATH="$sdk_prefix/lib:${LD_LIBRARY_PATH:-}" \
  "$lua_cli" '/status="open"' - >"$tmp.out"
grep '"status":"open"' "$tmp.out" >/dev/null

env \
  LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
  LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
  LD_LIBRARY_PATH="$sdk_prefix/lib:${LD_LIBRARY_PATH:-}" \
  "$lua_cli" --count '/status="open"' "$fixture" >"$tmp.out"
grep '^2$' "$tmp.out" >/dev/null

env \
  LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
  LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
  LD_LIBRARY_PATH="$sdk_prefix/lib:${LD_LIBRARY_PATH:-}" \
  "$lua_cli" -f /status '/status="open"' "$fixture" >"$tmp.out"
grep '^{"status":"open"}$' "$tmp.out" >/dev/null

env \
  LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
  LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
  LD_LIBRARY_PATH="$sdk_prefix/lib:${LD_LIBRARY_PATH:-}" \
  "$lua_cli" -m '/status=ready' -M '/status="open"' "$fixture" >"$tmp.out"
grep '"status":"ready"' "$tmp.out" >/dev/null

: >"$tmp.empty"
env \
  LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
  LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
  LD_LIBRARY_PATH="$sdk_prefix/lib:${LD_LIBRARY_PATH:-}" \
  "$lua_cli" -m '/status=ready' "$tmp.empty" "$fixture" >"$tmp.out"
grep '"status":"ready"' "$tmp.out" >/dev/null

printf 'héllo' >"$tmp.content"
printf '{}\n' >"$tmp.json"
env \
  LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
  LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
  LD_LIBRARY_PATH="$sdk_prefix/lib:${LD_LIBRARY_PATH:-}" \
  "$lua_cli" -F -m "textfile:/content=$tmp.content" "$tmp.json" \
    >"$tmp.out"
grep '"content":"héllo"' "$tmp.out" >/dev/null

printf '{"status":"open"}\n' >"$tmp.json"
env \
  LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
  LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
  LD_LIBRARY_PATH="$sdk_prefix/lib:${LD_LIBRARY_PATH:-}" \
  "$lua_cli" -i -m '/status=ready' "$tmp.json"
grep '"status":"ready"' "$tmp.json" >/dev/null

printf 'lua cli smoke: selected output, count, projection, mutation, file-backed mutation, inline, and spill help passed\n'
