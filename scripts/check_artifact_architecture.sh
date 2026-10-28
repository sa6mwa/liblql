#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[[ $# == 3 ]] || { echo 'usage: check_artifact_architecture.sh <binary> <target> <build-dir>' >&2; exit 1; }
description=$("$root/scripts/discover_target_tools.sh" "$3" "$2")
case "$2" in
  *-linux-*)
    reader=$(sed -n 's/^READELF=//p' <<< "$description")
    header=$("$reader" -h "$1")
    case "$2" in
      x86_64-*) machine='Advanced Micro Devices X86-64'; class=ELF64 ;;
      aarch64-*) machine=AArch64; class=ELF64 ;;
      armhf-*) machine=ARM; class=ELF32 ;;
      *) echo "architecture check: unsupported target: $2" >&2; exit 1 ;;
    esac
    actual_machine=$(awk -F: '/^[[:space:]]*Machine:/{sub(/^[[:space:]]*/, "",$2);print $2}' <<< "$header")
    actual_class=$(awk -F: '/^[[:space:]]*Class:/{sub(/^[[:space:]]*/, "",$2);print $2}' <<< "$header")
    [[ "$actual_machine" == "$machine" && "$actual_class" == "$class" ]] || {
      printf 'architecture check: %s expected %s/%s, got %s/%s\n' "$1" "$machine" "$class" "$actual_machine" "$actual_class" >&2; exit 1;
    }
    ;;
  arm64-apple-darwin)
    reader=$(sed -n 's/^OTOOL=//p' <<< "$description")
    header=$("$reader" -hv "$1")
    grep -Eq '^MH_MAGIC_64[[:space:]]+ARM64[[:space:]]' <<< "$header" || {
      printf 'architecture check: %s is not an arm64 Mach-O\n' "$1" >&2; exit 1;
    }
    ;;
  *) echo "architecture check: unsupported target: $2" >&2; exit 1 ;;
esac
