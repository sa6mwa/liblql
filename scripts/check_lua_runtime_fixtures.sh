#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/liblql-lua-runtime-fixtures.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

cat >"$tmp/lua55" <<'EOF'
#!/bin/sh
printf '%s\n' 'Lua 5.5'
EOF
chmod +x "$tmp/lua55"

cat >"$tmp/lua54" <<'EOF'
#!/bin/sh
printf '%s\n' 'Lua 5.4'
EOF
chmod +x "$tmp/lua54"

if ! LUA="$tmp/lua55" LQL_LUA_RUNTIME_CHECK_ONLY=1 \
  "$root/scripts/run_lua_tests.sh"; then
  printf 'lua runtime fixture: expected Lua 5.5 runtime check to pass\n' >&2
  exit 1
fi

if LUA="$tmp/lua54" LQL_LUA_RUNTIME_CHECK_ONLY=1 \
  "$root/scripts/run_lua_tests.sh" >/dev/null 2>&1; then
  printf 'lua runtime fixture: expected non-5.5 runtime check to fail\n' >&2
  exit 1
fi
