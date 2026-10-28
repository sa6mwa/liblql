#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/require-host-bash.sh"
fail() { printf 'discover-target-tools: %s\n' "$*" >&2; exit 1; }
[[ $# == 2 ]] || fail 'usage: discover_target_tools.sh <build-dir> <target-id>'
cache=$1/CMakeCache.txt
target=$2
[[ -f "$cache" ]] || fail "missing CMake cache: $cache"
value() { sed -n "s/^$1:[^=]*=//p" "$cache" | tail -1; }
configured_target=$(value LQL_TARGET_ID)
[[ -z "$configured_target" || "$configured_target" == "$target" ]] ||
  fail "target mismatch: cache has $configured_target, requested $target"
compiler=$(value CMAKE_C_COMPILER)
compiler_dir=${compiler%/*}
prefix=${compiler##*/}
case "$prefix" in
  *-gcc) prefix=${prefix%-gcc} ;;
  *-clang) prefix=${prefix%-clang} ;;
  *-cc) prefix=${prefix%-cc} ;;
  *) prefix= ;;
esac
for name in CC AR RANLIB STRIP NM OBJCOPY OBJDUMP READELF OTOOL INSTALL_NAME_TOOL; do
  case "$name:$target" in
    OTOOL:*linux*|INSTALL_NAME_TOOL:*linux*|OBJCOPY:*darwin*|OBJDUMP:*darwin*|READELF:*darwin*) continue ;;
  esac
  override=LQL_$name
  tool=${name,,}
  key=CMAKE_$name
  [[ "$name" != CC ]] || key=CMAKE_C_COMPILER
  if [[ -n "${!override:-}" ]]; then
    selected=${!override}
    [[ -f "$selected" && -x "$selected" ]] || fail "$override is not executable: $selected"
  else
    selected=$(value "$key")
    if [[ ! -f "$selected" || ! -x "$selected" ]]; then
      selected=
      for candidate in "$compiler_dir/$prefix-$tool" "$compiler_dir/$tool"; do
        if [[ -f "$candidate" && -x "$candidate" ]]; then selected=$candidate; break; fi
      done
    fi
    [[ -n "$selected" ]] || fail "unable to discover $name for $target; set $override to an explicit target tool"
  fi
  printf '%s=%s\n' "$name" "$selected"
done
printf 'TARGET_ID=%s\n' "$target"
