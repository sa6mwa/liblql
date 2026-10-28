#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[[ $# == 3 ]] || { echo 'usage: check_library_symbols.sh <shared-library> <target> <build-dir>' >&2; exit 1; }
description=$("$root/scripts/discover_target_tools.sh" "$3" "$2")
nm_tool=$(sed -n 's/^NM=//p' <<< "$description")
case "$2" in
  # musl's CRT exports its ELF initialization/finalization entry points.
  *-linux-*) actual=$("$nm_tool" -D --defined-only "$1" | awk '$3 != "_init" && $3 != "_fini" {print $3}' | LC_ALL=C sort) ;;
  arm64-apple-darwin) actual=$("$nm_tool" -gU "$1" | awk '{print $NF}' | sed 's/^_//' | LC_ALL=C sort) ;;
  *) echo "symbol check: unsupported target: $2" >&2; exit 1 ;;
esac
expected=$(LC_ALL=C sort "$root/scripts/lql_public_symbols.txt")
if [[ "$actual" != "$expected" ]]; then
  printf 'symbol check: public exports differ in %s\nexpected:\n%s\nactual:\n%s\n' "$1" "$expected" "$actual" >&2
  exit 1
fi
