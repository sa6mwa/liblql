#!/usr/bin/env sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
mkdir -p "$root/build/tmp"

limit_bytes=262144
expected_cases=24
checked_cases=0
binary=${LQL_DIRECT_BENCH_PATH:-build/release/lql_direct_bench}
massif_out=$root/build/tmp/liblql-direct-live-heap.$$

cleanup() {
  sh scripts/remove_path.sh "$massif_out"
}
trap cleanup EXIT HUP INT TERM

if ! command -v valgrind >/dev/null 2>&1; then
  printf '%s\n' 'direct live heap: valgrind is required' >&2
  exit 1
fi
if [ ! -x "$binary" ]; then
  printf '%s\n' 'direct live heap: direct benchmark binary is unavailable' >&2
  exit 1
fi

sh scripts/ensure_direct_parity_fixtures.sh

check_case() {
  fixture=$1
  dataset=$2
  selector_name=$3
  expr=$4
  mode=$5
  # One measured pass plus the benchmark's initial pass covers cold and reused
  # execution. Heap peaks do not need repeated timing samples. Keep the explicit
  # eight-repeat retention case below.
  samples=${6:-1}
  started=$(date +%s)
  if [ ! -f "$fixture" ]; then
    printf 'direct live heap: fixture is unavailable: %s\n' "$fixture" >&2
    exit 1
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
  elapsed=$(( $(date +%s) - started ))
  printf 'direct live heap %s/%s/%s samples=%s: %s bytes (limit %s bytes), %ss\n' \
    "$dataset" "$selector_name" "$mode" "$samples" "$peak" "$limit_bytes" "$elapsed"
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
check_case build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open_file_backed_text '/status="open"' mutate_file_backed_text
check_case build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open_file_backed_base64 '/status="open"' mutate_file_backed_base64

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
