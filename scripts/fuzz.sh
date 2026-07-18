#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

# The AFL++ cache root includes the selected Bootlin collection identity.  A
# collection update therefore changes the compiler wrapper path; CMake cannot
# safely switch a configured compiler in place, so discard only this disposable
# build tree before configuring through the new wrapper.
afl_description=$("$root/scripts/cpkt-aflpp.sh" discover)
afl_cc=$(printf '%s\n' "$afl_description" | sed -n 's/^cc=//p')
[ -n "$afl_cc" ] || {
  printf '%s\n' 'fuzz: AFL++ resolver did not report a compiler wrapper' >&2
  exit 1
}
cache_file="$root/build/fuzz/CMakeCache.txt"
if [ -f "$cache_file" ]; then
  configured_cc=$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$cache_file")
  if [ "$configured_cc" != "$afl_cc" ]; then
    sh "$root/scripts/remove_path.sh" "$root/build/fuzz"
  fi
fi

cmake --preset fuzz
cmake --build --preset fuzz --target lql_json_fuzz
LQL_JSON_FUZZ_PATH=build/fuzz/lql_json_fuzz sh scripts/check_fuzz_smoke.sh
