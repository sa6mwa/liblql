#!/usr/bin/env sh
set -eu

limit_bytes=262144
expected_cases=22
checked_cases=0
binary=${LQL_DIRECT_BENCH_PATH:-build/release-scanner/lql_direct_bench}
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

sh scripts/ensure_scanner_parity_fixtures.sh

check_case() {
  fixture=$1
  dataset=$2
  selector_name=$3
  expr=$4
  mode=$5
  samples=${6:-2}
  if [ ! -f "$fixture" ]; then
    printf 'SKIP: fixture is unavailable: %s\n' "$fixture"
    return 0
  fi
  LQL_BENCH_SAMPLES=$samples valgrind --tool=massif --stacks=no --time-unit=B \
    --massif-out-file="$massif_out" "$binary" \
    --fixture "$fixture" --dataset "$dataset" \
    --selector-name "$selector_name" --expr "$expr" \
    --mode "$mode" --submode steady_state >/dev/null

  peak=$(awk -F= '/^mem_heap_B=/{ if ($2 > peak) peak=$2 } END { print peak+0 }' \
    "$massif_out")
  if [ "$peak" -gt "$limit_bytes" ]; then
    printf 'direct live heap exceeded for %s/%s/%s samples=%s: %s bytes (limit %s bytes)\n' \
      "$dataset" "$selector_name" "$mode" "$samples" "$peak" "$limit_bytes" >&2
    exit 1
  fi
  printf 'direct live heap %s/%s/%s samples=%s: %s bytes (limit %s bytes)\n' \
    "$dataset" "$selector_name" "$mode" "$samples" "$peak" "$limit_bytes"
  checked_cases=$((checked_cases + 1))
}

for mode in \
  decision_only_selector \
  plus_value_selector \
  plus_value_source_selector \
  plus_value_openjson_selector \
  project_file_selector \
  project_source_selector \
  mutate_file_selector \
  mutate_source_selector
do
  check_case build/direct-probe/status-100k.ndjson status_100k \
    eq_status_open '/status="open"' "$mode"
  check_case build/direct-probe/large-4x25m.ndjson large_ndjson \
    eq_status_open '/status="open"' "$mode"
done

check_case build/direct-probe/project-mutate-status-100k.ndjson \
  project_mutate_status_100k project_mutation_status '/status="open"' \
  project_mutate_file_selector
check_case build/direct-probe/large-4x25m.ndjson large_ndjson \
  eq_status_open_top_set '/status="open"' project_mutate_file_selector

check_case build/direct-probe/realworld-100k.ndjson realworld_sparse \
  realworld_eq_sparse '/event="session_sync"' decision_only_selector
check_case build/direct-probe/realworld-100k.ndjson realworld_sparse_callback \
  realworld_eq_sparse '/event="session_sync"' plus_value_selector
check_case build/direct-probe/large-100m.ndjson large_100m \
  eq_status_open '/status="open"' plus_value_selector
check_case build/direct-probe/status-100k.ndjson status_100k_repeated \
  eq_status_open '/status="open"' decision_only_selector 8

if [ "$checked_cases" -ne "$expected_cases" ]; then
  printf 'direct live heap expected %s cases, checked %s cases\n' \
    "$expected_cases" "$checked_cases" >&2
  exit 1
fi
