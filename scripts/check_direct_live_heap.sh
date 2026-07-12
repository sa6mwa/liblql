#!/usr/bin/env sh
set -eu

limit_bytes=262144
binary=${LQL_DIRECT_BENCH_PATH:-build/verify-gcc-path/lql_direct_bench}
massif_out=${TMPDIR:-/tmp}/liblql-direct-live-heap.$$

cleanup() {
  rm -f "$massif_out"
}
trap cleanup EXIT HUP INT TERM

if ! command -v valgrind >/dev/null 2>&1; then
  printf '%s\n' 'SKIP: valgrind is required for the direct live-heap gate'
  exit 0
fi
if [ ! -x "$binary" ]; then
  printf '%s\n' 'SKIP: direct benchmark binary is unavailable'
  exit 0
fi

check_case() {
  fixture=$1
  dataset=$2
  mode=$3
  if [ ! -f "$fixture" ]; then
    printf 'SKIP: fixture is unavailable: %s\n' "$fixture"
    return 0
  fi
  LQL_BENCH_SAMPLES=2 valgrind --tool=massif --stacks=no --time-unit=B \
    --massif-out-file="$massif_out" "$binary" \
    --fixture "$fixture" --dataset "$dataset" \
    --selector-name eq_status_open --expr '/status="open"' \
    --mode "$mode" --submode steady_state >/dev/null

  peak=$(awk -F= '/^mem_heap_B=/{ if ($2 > peak) peak=$2 } END { print peak+0 }' \
    "$massif_out")
  if [ "$peak" -gt "$limit_bytes" ]; then
    printf 'direct live heap exceeded for %s/%s: %s bytes (limit %s bytes)\n' \
      "$dataset" "$mode" "$peak" "$limit_bytes" >&2
    exit 1
  fi
  printf 'direct live heap %s/%s: %s bytes (limit %s bytes)\n' \
    "$dataset" "$mode" "$peak" "$limit_bytes"
}

for mode in decision_only_selector project_file_selector mutate_file_selector
do
  check_case build/direct-probe/status-100k.ndjson status_100k "$mode"
  check_case build/direct-probe/large-4x25m.ndjson large_ndjson "$mode"
done
