#!/bin/sh
set -eu

if ! command -v valgrind >/dev/null 2>&1; then
  printf '%s\n' 'valgrind is required for the native Memcheck gate' >&2
  exit 1
fi

run_memcheck() {
  test_binary=$1

  if [ ! -x "$test_binary" ]; then
    printf 'Memcheck test binary is unavailable: %s\n' "$test_binary" >&2
    exit 1
  fi

  printf 'valgrind memcheck: %s\n' "$test_binary"
  valgrind \
    --quiet \
    --tool=memcheck \
    --leak-check=full \
    --show-leak-kinds=definite,possible \
    --track-origins=yes \
    --errors-for-leak-kinds=definite,possible \
    --error-exitcode=97 \
    "$test_binary"
}

run_memcheck build/debug/lql_json_scan_test
run_memcheck build/debug/lql_direct_stream_test
run_memcheck build/debug/lql_workspace_alloc_test
run_memcheck build/debug/lql_thread_stack_test
printf '%s\n' 'valgrind memcheck: Lua allocation failures and native cleanup'
valgrind --quiet --leak-check=full --show-leak-kinds=definite,possible \
  --errors-for-leak-kinds=definite,possible --error-exitcode=97 \
  build/debug-lua/lql_lua_fault_test lua/tests/lql_faults.lua

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lua_runner=$root/build/debug-lua/lql_lua_runner
lua_module_dir=$root/build/debug-lua/lua
lua_test=$root/lua/tests/lql_smoke.lua

if [ ! -x "$lua_runner" ] || [ ! -f "$lua_module_dir/lql/core.so" ]; then
  printf '%s\n' 'Memcheck Lua facade artifacts are unavailable; run make build-debug-lua' >&2
  exit 1
fi

printf 'valgrind memcheck: %s %s\n' "$lua_runner" "$lua_test"
LUA_PATH="$root/lua/?.lua;$root/lua/?/init.lua;;" \
LUA_CPATH="$lua_module_dir/?.so;$lua_module_dir/?/core.so;;" \
  valgrind \
    --quiet \
    --tool=memcheck \
    --leak-check=full \
    --show-leak-kinds=definite,possible \
    --track-origins=yes \
    --errors-for-leak-kinds=definite,possible \
    --error-exitcode=97 \
    "$lua_runner" "$lua_test"

for suite in lifetime close; do
  TMPDIR="$root/build/tmp" LUA_CPATH="$lua_module_dir/?.so;;" \
    valgrind --quiet --leak-check=full --show-leak-kinds=definite,possible \
      --errors-for-leak-kinds=definite,possible --error-exitcode=97 \
      "$lua_runner" "$root/lua/tests/lql_$suite.lua"
done
