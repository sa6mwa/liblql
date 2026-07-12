#!/usr/bin/env sh
set -eu

limit_bytes=262144
binary=${LQL_DIRECT_BENCH_PATH:-build/verify-gcc-path/lql_direct_bench}
fixture=${LQL_DIRECT_HEAP_FIXTURE:-build/direct-probe/status-100k.ndjson}
massif_out=${TMPDIR:-/tmp}/liblql-direct-live-heap.$$

cleanup() {
  rm -f "$massif_out"
}
trap cleanup EXIT HUP INT TERM

if ! command -v valgrind >/dev/null 2>&1; then
  printf '%s\n' 'SKIP: valgrind is required for the direct live-heap gate'
  exit 0
fi
if [ ! -x "$binary" ] || [ ! -f "$fixture" ]; then
  printf '%s\n' 'SKIP: direct benchmark binary or status fixture is unavailable'
  exit 0
fi

LQL_BENCH_SAMPLES=1 valgrind --tool=massif --stacks=no --time-unit=B \
  --massif-out-file="$massif_out" "$binary" \
  --fixture "$fixture" --dataset status_100k \
  --selector-name eq_status_open --expr '/status="open"' \
  --mode mutate_file_selector --submode steady_state >/dev/null

peak=$(awk -F= '/^mem_heap_B=/{ if ($2 > peak) peak=$2 } END { print peak+0 }' \
  "$massif_out")
if [ "$peak" -gt "$limit_bytes" ]; then
  printf 'direct live heap exceeded: %s bytes (limit %s bytes)\n' \
    "$peak" "$limit_bytes" >&2
  exit 1
fi
printf 'direct live heap: %s bytes (limit %s bytes)\n' "$peak" "$limit_bytes"
