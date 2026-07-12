#!/bin/sh
set -eu

c_bench=${LQL_DIRECT_BENCH_PATH:-build/release-scanner/lql_direct_bench}
go_bench=${LQL_GO_BENCH_PATH:-build/reference-lqlbench}
validator=${LQL_BENCHVALIDATE_PATH:-build/reference-benchvalidate}
out=${LQL_SCANNER_PARITY_OUT:-build/scanner-parity-smoke.jsonl}
samples=${LQL_BENCH_SAMPLES:-3}

if [ ! -x "$c_bench" ]; then
  printf 'scanner parity smoke: missing C benchmark binary: %s\n' "$c_bench" >&2
  exit 1
fi
if [ ! -x "$go_bench" ]; then
  printf 'scanner parity smoke: missing Go benchmark binary: %s\n' "$go_bench" >&2
  exit 1
fi
if [ ! -x "$validator" ]; then
  printf 'scanner parity smoke: missing benchmark validator: %s\n' "$validator" >&2
  exit 1
fi

mkdir -p "$(dirname "$out")"
: >"$out"
export LQL_BENCH_SAMPLES=$samples

run_one() {
  impl=$1
  submode=$2
  fixture=$3
  dataset=$4
  selector=$5
  expr=$6
  mode=$7
  projection=$8

  if [ ! -f "$fixture" ]; then
    printf 'scanner parity smoke: missing fixture: %s\n' "$fixture" >&2
    exit 1
  fi

  if [ "$impl" = go ]; then
    "$go_bench" \
      --fixture "$fixture" \
      --dataset "$dataset" \
      --selector-name "$selector" \
      --expr "$expr" \
      --mode "$mode" \
      --submode "$submode" \
      --projection-path "$projection" >>"$out"
  else
    "$c_bench" \
      --fixture "$fixture" \
      --dataset "$dataset" \
      --selector-name "$selector" \
      --expr "$expr" \
      --mode "$mode" \
      --submode "$submode" \
      --projection-path "$projection" >>"$out"
  fi
}

run_row() {
  fixture=$1
  dataset=$2
  selector=$3
  expr=$4
  mode=$5
  projection=$6

  run_one go warmup_included "$fixture" "$dataset" "$selector" "$expr" "$mode" "$projection"
  run_one c warmup_included "$fixture" "$dataset" "$selector" "$expr" "$mode" "$projection"
  run_one go steady_state "$fixture" "$dataset" "$selector" "$expr" "$mode" "$projection"
  run_one c steady_state "$fixture" "$dataset" "$selector" "$expr" "$mode" "$projection"
}

run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' decision_only_selector /id
run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' plus_value_source_selector /id
run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' project_file_selector /id
run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open_top_set '/status="open"' mutate_file_selector /id
run_row build/direct-probe/nested-set-status-100k.ndjson nested_set_status_100k \
  eq_status_open_nested_set '/status="open"' mutate_file_selector /id
run_row build/direct-probe/top-increment-code-100k.ndjson top_increment_code_100k \
  eq_code_one_top_increment '/code=1' mutate_source_selector /id
run_row build/direct-probe/project-mutate-status-100k.ndjson project_mutate_status_100k \
  project_mutation_status '/status="open"' project_mutate_file_selector /id
run_row build/direct-probe/temporal-100k.ndjson temporal_100k \
  date_window 'date{field=/timestamp,after=2026-03-05T10:28:21Z,before=2026-03-05T10:30:00Z}' decision_only_selector /id
run_row build/direct-probe/array-scalar-100k.ndjson array_scalar_100k \
  array_scalar_indexed_eq '/values/1="B"' decision_only_selector /id
run_row build/direct-probe/large-4x25m.ndjson large_4x25m \
  eq_status_open_top_set '/status="open"' mutate_file_selector /id

"$validator" --forbid-unsupported --min-c-go-speedup=1.0 <"$out"
printf 'scanner parity smoke: wrote %s\n' "$out"
