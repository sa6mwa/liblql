#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tree=${LQL_LUAROCKS_TREE:-$root/build/luarocks}
sdk_prefix=${LQL_LUA_SDK_PREFIX:-$root/build/lua-sdk}
fixture=${LQL_LUA_CLI_FIXTURE:-$root/examples/status.ndjson}
lua_cli=$tree/bin/lql.lua
lua_runner=${LQL_LUA_RUNNER:-$root/build/debug-lua/lql_lua_runner}
lua_script=${LQL_LUA_CLI_SCRIPT:-}

if [ ! -x "$lua_cli" ]; then
  printf 'lua cli smoke: missing installed lql.lua CLI: %s\n' "$lua_cli" >&2
  exit 1
fi
if [ ! -x "$lua_runner" ]; then
  printf 'lua cli smoke: missing Bootlin Lua runner: %s\n' "$lua_runner" >&2
  exit 1
fi
if [ -z "$lua_script" ]; then
  lua_script=$(find "$tree/lib/luarocks/rocks-5.5/liblql" -type f \
    -path '*/bin/lql.lua' -print | sort | sed -n '1p')
fi
if [ ! -f "$lua_script" ]; then
  printf 'lua cli smoke: missing installed Lua script: %s\n' "$lua_script" >&2
  exit 1
fi

run_lua() {
  env \
    LUA_PATH="$tree/share/lua/5.5/?.lua;$tree/share/lua/5.5/?/init.lua;;" \
    LUA_CPATH="$tree/lib/lua/5.5/?.so;$tree/lib/lua/5.5/?/core.so;;" \
    "$lua_runner" "$lua_script" "$@"
}

tmp=${TMPDIR:-/tmp}/liblql-lua-cli.$$
cleanup() {
  sh "$root/scripts/remove_path.sh" "$tmp.out" "$tmp.err" "$tmp.json" \
    "$tmp.content" "$tmp.empty"
}
trap cleanup EXIT HUP INT TERM

run_lua --help >"$tmp.out"
grep 'explicitly spooled path' "$tmp.out" >/dev/null

run_lua '/status="open"' "$fixture" >"$tmp.out"
grep '"status":"open"' "$tmp.out" >/dev/null

cat "$fixture" | run_lua '/status="open"' - >"$tmp.out"
grep '"status":"open"' "$tmp.out" >/dev/null

run_lua --count '/status="open"' "$fixture" >"$tmp.out"
grep '^2$' "$tmp.out" >/dev/null

run_lua -f /status '/status="open"' "$fixture" >"$tmp.out"
grep '^{"status":"open"}$' "$tmp.out" >/dev/null

run_lua -m '/status=ready' -M '/status="open"' "$fixture" >"$tmp.out"
grep '"status":"ready"' "$tmp.out" >/dev/null

: >"$tmp.empty"
run_lua -m '/status=ready' "$tmp.empty" "$fixture" >"$tmp.out"
grep '"status":"ready"' "$tmp.out" >/dev/null

printf 'héllo' >"$tmp.content"
printf '{}\n' >"$tmp.json"
run_lua -F -m "textfile:/content=$tmp.content" "$tmp.json" >"$tmp.out"
grep '"content":"héllo"' "$tmp.out" >/dev/null

printf '{"status":"open"}\n' >"$tmp.json"
run_lua -i -m '/status=ready' "$tmp.json"
grep '"status":"ready"' "$tmp.json" >/dev/null

printf 'lua cli smoke: selected output, count, projection, mutation, file-backed mutation, inline, and spill help passed\n'
