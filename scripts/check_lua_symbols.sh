#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[[ $# == 2 ]] || { echo 'usage: check_lua_symbols.sh <module> <build-dir>' >&2; exit 1; }
description=$("$root/scripts/discover_target_tools.sh" "$2" x86_64-linux-gnu)
nm_tool=$(sed -n 's/^NM=//p' <<< "$description")
imports=$("$nm_tool" -D --undefined-only "$1" | awk '$NF ~ /^lql_/ {print $NF}')
while IFS= read -r symbol; do
  [[ -z "$symbol" ]] || grep -Fx "$symbol" "$root/scripts/lql_public_symbols.txt" >/dev/null || {
    printf 'Lua symbol check: private liblql import: %s\n' "$symbol" >&2; exit 1;
  }
done <<< "$imports"
exports=$("$nm_tool" -D --defined-only "$1" | awk '{print $NF}')
[[ "$exports" == luaopen_lql_core ]] || { printf 'Lua symbol check: unexpected exports: %s\n' "$exports" >&2; exit 1; }
