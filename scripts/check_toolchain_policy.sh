#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work=$root/build/toolchain-policy-check

sh "$root/scripts/remove_path.sh" "$work"
mkdir -p "$work"

if cmake -S "$root" -B "$work/rejected" -G Ninja >"$work/rejected.log" 2>&1; then
  printf '%s\n' 'toolchain policy: host Linux compiler was accepted without override' >&2
  exit 1
fi
if ! grep -F 'liblql Linux builds require the pinned Bootlin toolchain' \
  "$work/rejected.log" >/dev/null; then
  printf '%s\n' 'toolchain policy: missing actionable Bootlin rejection' >&2
  cat "$work/rejected.log" >&2
  exit 1
fi

LIBLQL_TOOLCHAIN_OVERRIDE=1 cmake -S "$root" -B "$work/override" -G Ninja \
  >"$work/override.log" 2>&1
if ! grep -F 'accepting caller-supplied Linux toolchain' "$work/override.log" \
  >/dev/null; then
  printf '%s\n' 'toolchain policy: explicit override was not reported' >&2
  cat "$work/override.log" >&2
  exit 1
fi
LIBLQL_TOOLCHAIN_OVERRIDE=1 cmake --build "$work/override" \
  --target lql_json_scan_test >/dev/null
"$work/override/lql_json_scan_test"

printf '%s\n' 'toolchain policy check passed'
