#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work=$(mktemp -d "$root/build/tmp/lua-sdk-prefix.XXXXXXXX")
trap 'sh "$root/scripts/remove_path.sh" "$work"' EXIT
fail() { printf 'Lua SDK prefix: %s\n' "$*" >&2; exit 1; }
cd "$root"

# Exercise the public Make entrypoint with distinct owned SDK installations.
# An explicitly selected runner still has to match; automatic reuse must pick
# the prepared default runner or build a runner for the alternate installation.
for selected in alternate-one alternate-two default; do
  if [[ "$selected" == default ]]; then sdk=$root/build/lua-sdk; else sdk=$work/$selected; fi
  env -u LQL_LUA_RUNNER LQL_LUA_SDK_PREFIX="$sdk" \
    LQL_LUAROCKS_TREE="$work/rock" LQL_LUA_RUNNER_BUILD_DIR="$work/runner" \
    make --no-print-directory lua-rock >"$work/$selected.log" 2>&1 || {
      cat "$work/$selected.log"; fail "$selected SDK failed";
    }
  grep -Fx 'lua-rock: installed rock smoke passed' "$work/$selected.log"
  if [[ "$selected" != default ]]; then
    [[ -x "$work/runner/lql_lua_runner" ]] || fail 'alternate SDK runner was not built'
    env LQL_LUA_SDK_PREFIX="$sdk" LQL_LUAROCKS_TREE="$work/rock" \
      LQL_LUA_RUNNER="$root/build/debug-lua/lql_lua_runner" \
      sh scripts/check_lua_rock.sh >"$work/mismatch.log" 2>&1 &&
      fail 'accepted explicitly mismatched runner'
    grep -F 'runner does not load the requested SDK' "$work/mismatch.log" >/dev/null
  else
    # Reconfiguring the alternate runner would change this cached SDK prefix.
    # Leaving it alone proves the matching prepared default runner was reused.
    grep -Fx "LQL_LUA_SDK_PREFIX:PATH=$work/alternate-two" "$work/runner/CMakeCache.txt" >/dev/null
  fi
done
printf '%s\n' 'Lua SDK prefix: alternate runner, matching reuse and explicit mismatch checks passed'
