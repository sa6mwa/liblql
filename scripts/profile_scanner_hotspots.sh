#!/bin/sh
set -eu

c_bench=${LQL_DIRECT_BENCH_PATH:-build/release-scanner/lql_direct_bench}
out_dir=${LQL_SCANNER_PROFILE_DIR:-build/scanner-profiles}
status_samples=${LQL_SCANNER_PROFILE_STATUS_SAMPLES:-120}
scalar_samples=${LQL_SCANNER_PROFILE_SCALAR_SAMPLES:-120}
plus_value_source_samples=${LQL_SCANNER_PROFILE_PLUS_VALUE_SOURCE_SAMPLES:-120}
recursive_samples=${LQL_SCANNER_PROFILE_RECURSIVE_SAMPLES:-120}
large_samples=${LQL_SCANNER_PROFILE_LARGE_SAMPLES:-60}
freq=${LQL_SCANNER_PROFILE_FREQ:-999}

if [ ! -x "$c_bench" ]; then
  printf 'scanner profile: missing C benchmark binary: %s\n' "$c_bench" >&2
  exit 1
fi
if ! command -v perf >/dev/null 2>&1; then
  printf 'scanner profile: perf is required\n' >&2
  exit 1
fi

mkdir -p "$out_dir"

profile_row() {
  name=$1
  samples=$2
  fixture=$3
  dataset=$4
  selector=$5
  expr=$6
  mode=$7
  data=$out_dir/$name.data
  report=$out_dir/$name.report.txt

  if [ ! -f "$fixture" ]; then
    printf 'scanner profile: missing fixture: %s\n' "$fixture" >&2
    exit 1
  fi

  printf 'scanner profile: recording %s (%s samples per benchmark)\n' "$name" "$samples"
  LQL_BENCH_SAMPLES=$samples perf record -F "$freq" -g --call-graph fp \
    -o "$data" -- "$c_bench" \
    --fixture "$fixture" \
    --dataset "$dataset" \
    --selector-name "$selector" \
    --expr "$expr" \
    --mode "$mode" \
    --submode steady_state >/dev/null

  perf report --stdio --no-children --call-graph=none \
    --sort=dso,symbol --percent-limit 1 -i "$data" >"$report"
  printf 'scanner profile: wrote %s\n' "$report"
  sed -n '1,80p' "$report"
}

profile_row status-decision "$status_samples" \
  build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' decision_only_selector

profile_row status-ne-decision "$status_samples" \
  build/direct-probe/status-100k.ndjson status_100k \
  ne_status_open '/status!="open"' decision_only_selector

profile_row status-reuse "$status_samples" \
  build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' reuse_selector

profile_row status-and-decision "$status_samples" \
  build/direct-probe/status-100k.ndjson status_100k \
  and_status_open_region_west '/status="open",/region="us-west"' \
  decision_only_selector

profile_row scalar-bool-decision "$scalar_samples" \
  build/direct-probe/scalar-100k.ndjson scalar_100k \
  bool_enabled_true '/enabled=true' decision_only_selector

profile_row scalar-number-decision "$scalar_samples" \
  build/direct-probe/scalar-100k.ndjson scalar_100k \
  code_eq_one '/code=1' decision_only_selector

profile_row plus-value-source "$plus_value_source_samples" \
  build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' plus_value_source_selector

profile_row ne-plus-value-source "$plus_value_source_samples" \
  build/direct-probe/status-100k.ndjson status_100k \
  ne_status_open '/status!="open"' plus_value_source_selector

profile_row recursive-decision "$recursive_samples" \
  build/direct-probe/recursive-10k.ndjson recursive_10k \
  recursive_eq '/.../sku="needle"' decision_only_selector

profile_row large-mutation "$large_samples" \
  build/direct-probe/large-4x25m.ndjson large_4x25m \
  eq_status_open_top_set '/status="open"' mutate_file_selector
