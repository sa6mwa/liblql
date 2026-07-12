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

run_memcheck build/debug-scanner/lql_json_scan_test
run_memcheck build/debug-scanner/lql_direct_stream_test
