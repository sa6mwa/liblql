#!/bin/sh
set -eu

impls="go,c,lua"
format="json"
check=0
required=""

usage() {
  cat <<'USAGE'
usage: scripts/run_parity_benchmarks.sh [--impl go,c,lua] [--format json] [--check] [--require go,c,lua]
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --impl)
      shift
      impls=${1:?missing --impl value}
      ;;
    --format)
      shift
      format=${1:?missing --format value}
      ;;
    --check)
      check=1
      ;;
    --require)
      shift
      required=${1:?missing --require value}
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'unknown argument: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

if [ "$format" != "json" ]; then
  printf 'unsupported benchmark format: %s\n' "$format" >&2
  exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture_dir="${LQL_BENCH_FIXTURE_DIR:-$root/build/bench-fixtures}"
count="${LQL_BENCH_NDJSON_COUNT:-128}"
suite="${LQL_BENCH_SUITE:-full}"
mode_profile="${LQL_BENCH_MODE_PROFILE:-all}"
record_blob_size="${LQL_BENCH_RECORD_BLOB_BYTES:-16}"
clql="${CLQL_PATH:-$root/build/debug/clql}"
payload_bench="${LQL_PAYLOAD_BENCH_PATH:-$root/build/debug/lql_payload_bench}"
go_bin="${GO:-go}"
go_bench="${LQL_GO_BENCH_PATH:-$root/build/bench-tools/lqlbench}"
go_bench_ready=0
lua_bin="${LUA:-lua}"
mkdir -p "$fixture_dir"
ndjson_fixture="$fixture_dir/large_ndjson.jsonl"
array_fixture="$fixture_dir/large_array.json"
single_fixture="$fixture_dir/large_single_json.json"
cli_ndjson_fixture="$fixture_dir/selection_ndjson.jsonl"
cli_array_fixture="$fixture_dir/selection_array.json"
cli_single_fixture="$fixture_dir/selection_single_json.json"
case_matrix="$fixture_dir/cases.tsv"
go_counts_file="$fixture_dir/go-counts.txt"
c_counts_file="$fixture_dir/c-counts.txt"
lua_counts_file="$fixture_dir/lua-counts.txt"
inject_candidate_mismatch="${LQL_BENCH_INJECT_CANDIDATE_MISMATCH:-0}"
inject_match_mismatch="${LQL_BENCH_INJECT_MATCH_MISMATCH:-0}"
inject_payload_mismatch="${LQL_BENCH_INJECT_PAYLOAD_MISMATCH:-0}"
inject_payload_byte_mismatch="${LQL_BENCH_INJECT_PAYLOAD_BYTE_MISMATCH:-0}"
record_blob=""

json_string() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

file_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

emit_record() {
  impl=$1
  dataset=$2
  selector=$3
  expr=$4
  mode=$5
  submode=$6
  bytes=$7
  candidates=$8
  matches=$9
  payloads=${10}
  payload_bytes=${11}
  payload_source_type=${12}
  ns_per_op=${13}
  peak_rss_bytes=${14}
  unsupported=${15}
  reason=${16}
  fixture_sha256=${17}
  printf '{"schema":"liblql.parity_benchmark.v1","impl":"%s","dataset":"%s","selector":"%s","expr":"%s","mode":"%s","submode":"%s","bytes_per_iter":%s,"candidates":%s,"matches":%s,"payloads":%s,"payload_bytes":%s,"payload_source_type":"%s","fixture_sha256":"%s","ns_per_op":%s,"peak_rss_bytes":%s,"allocs_per_op":null,"unsupported":%s,"unsupported_reason":"%s"}\n' \
    "$(json_string "$impl")" \
    "$(json_string "$dataset")" \
    "$(json_string "$selector")" \
    "$(json_string "$expr")" \
    "$(json_string "$mode")" \
    "$(json_string "$submode")" \
    "$bytes" \
    "$candidates" \
    "$matches" \
    "$payloads" \
    "$payload_bytes" \
    "$(json_string "$payload_source_type")" \
    "$(json_string "$fixture_sha256")" \
    "$ns_per_op" \
    "$peak_rss_bytes" \
    "$unsupported" \
    "$(json_string "$reason")"
}

emit_submode_records() {
  impl=$1
  dataset=$2
  selector=$3
  expr=$4
  mode=$5
  bytes=$6
  candidates=$7
  matches=$8
  payloads=$9
  payload_bytes=${10}
  payload_source_type=${11}
  ns_per_op=${12}
  peak_rss_bytes=${13}
  unsupported=${14}
  reason=${15}
  fixture_sha256=${16}
  emit_record "$impl" "$dataset" "$selector" "$expr" "$mode" \
    "warmup_included" "$bytes" "$candidates" "$matches" "$payloads" \
    "$payload_bytes" "$payload_source_type" "$ns_per_op" "$peak_rss_bytes" "$unsupported" \
    "$reason" "$fixture_sha256"
  emit_record "$impl" "$dataset" "$selector" "$expr" "$mode" \
    "steady_state" "$bytes" "$candidates" "$matches" "$payloads" \
    "$payload_bytes" "$payload_source_type" "$ns_per_op" "$peak_rss_bytes" "$unsupported" \
    "$reason" "$fixture_sha256"
}

emit_unsupported_impl() {
  impl=$1
  reason=$2
  while read dataset_name fixture_path candidates selector_name expr; do
    : "$candidates"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "decision_only_selector" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "decision_only_plan" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "plus_value_selector" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "plus_value_plan" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "plus_value_openjson_selector" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "plus_value_openjson_plan" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
  done < "$case_matrix"
}

json_number_field() {
  field=$1
  record=$2
  printf '%s\n' "$record" | sed -n "s/.*\"$field\":\\([0-9][0-9]*\\).*/\\1/p"
}

kv_field() {
  field=$1
  record=$2
  printf '%s\n' "$record" | sed -n "s/.*$field=\\([0-9][0-9]*\\).*/\\1/p"
}

is_selected() {
  case ",$impls," in
    *",$1,"*) return 0 ;;
    *) return 1 ;;
  esac
}

validate_required_impls() {
  if [ -z "$required" ]; then
    return 0
  fi
  old_ifs=$IFS
  IFS=,
  set -- $required
  IFS=$old_ifs
  for impl do
    case "$impl" in
      go|c|lua) ;;
      "")
        continue
        ;;
      *)
        printf 'benchmark required implementation is unknown: %s\n' "$impl" >&2
        return 2
        ;;
    esac
    if ! is_selected "$impl"; then
      printf 'benchmark missing required implementation: %s\n' "$impl" >&2
      return 1
    fi
  done
  return 0
}

ensure_go_bench() {
  if [ "$go_bench_ready" -eq 1 ]; then
    return 0
  fi
  if ! command -v "$go_bin" >/dev/null 2>&1; then
    return 1
  fi
  mkdir -p "$(dirname "$go_bench")"
  (cd "$root/parity" && "$go_bin" build -o "$go_bench" ./cmd/lqlbench)
  go_bench_ready=1
}

fault_count() {
  value=$1
  enabled=$2
  if [ "$enabled" = "1" ]; then
    printf '%s\n' $((value + 1))
  else
    printf '%s\n' "$value"
  fi
}

generate_blob() {
  awk -v n="$record_blob_size" 'BEGIN { for (i = 0; i < n; ++i) printf "x" }'
}

record_json() {
  i=$1
  if [ -z "$record_blob" ]; then
    record_blob=$(generate_blob)
  fi
  case $((i % 4)) in
    0) status=new ;;
    1) status=open ;;
    2) status=closed ;;
    *) status=queued ;;
  esac
  if [ $((i % 2)) -eq 0 ]; then
    timestamp="2026-03-05T11:28:21+01:00"
  else
    timestamp="2026-03-05T11:29:41.265+01:00"
  fi
  printf '{"id":"id-%d","status":"%s","metrics":{"retries":%d,"qps":%d},"timestamp":"%s","blob":"%s"}' \
    "$i" "$status" $((i % 7)) $((i + 1)) "$timestamp" "$record_blob"
}

selection_record_json() {
  i=$1
  case $((i % 4)) in
    0) status=open ;;
    1) status=queued ;;
    2) status=closed ;;
    *) status=new ;;
  esac
  case $((i % 3)) in
    0) region=us-west ;;
    1) region=eu-north ;;
    *) region=us-east ;;
  esac
  case $((i % 4)) in
    0) service=auth-api ;;
    1) service=search-api ;;
    2) service=gateway ;;
    *) service=billing-api ;;
  esac
  printf '{"id":"id-%07d","region":"%s","service":"%s","status":"%s","metrics":{"latency_ms":%d,"qps":%d,"errors":%d},"message":"request-%d service=%s region=%s","tags":["prod","blue","v2"]}' \
    "$i" "$region" "$service" "$status" $(((i * 17) % 700 + 10)) \
    $((i + 1)) $((i % 5)) "$i" "$service" "$region"
}

generate_fixtures() {
  i=0
  : > "$ndjson_fixture"
  : > "$array_fixture"
  : > "$single_fixture"
  : > "$cli_ndjson_fixture"
  : > "$cli_array_fixture"
  : > "$cli_single_fixture"
  while [ "$i" -lt "$count" ]; do
    record_json "$i" >> "$ndjson_fixture"
    printf '\n' >> "$ndjson_fixture"
    selection_record_json "$i" >> "$cli_ndjson_fixture"
    printf '\n' >> "$cli_ndjson_fixture"
    i=$((i + 1))
  done
  printf '[' > "$array_fixture"
  printf '[' > "$cli_array_fixture"
  i=0
  while [ "$i" -lt "$count" ]; do
    if [ "$i" -ne 0 ]; then
      printf ',' >> "$array_fixture"
      printf ',' >> "$cli_array_fixture"
    fi
    record_json "$i" >> "$array_fixture"
    selection_record_json "$i" >> "$cli_array_fixture"
    i=$((i + 1))
  done
  printf ']\n' >> "$array_fixture"
  printf ']\n' >> "$cli_array_fixture"
  printf '{"records":[' > "$single_fixture"
  printf '{"records":[' > "$cli_single_fixture"
  i=0
  while [ "$i" -lt "$count" ]; do
    if [ "$i" -ne 0 ]; then
      printf ',' >> "$single_fixture"
      printf ',' >> "$cli_single_fixture"
    fi
    record_json "$i" >> "$single_fixture"
    selection_record_json "$i" >> "$cli_single_fixture"
    i=$((i + 1))
  done
  printf ']}\n' >> "$single_fixture"
  printf ']}\n' >> "$cli_single_fixture"
}

generate_fixture() {
  : > "$go_counts_file"
  : > "$c_counts_file"
  : > "$lua_counts_file"
  if [ "$suite" = "memory" ]; then
    : > "$ndjson_fixture"
    i=0
    while [ "$i" -lt "$count" ]; do
      record_json "$i" >> "$ndjson_fixture"
      printf '\n' >> "$ndjson_fixture"
      i=$((i + 1))
    done
    : > "$case_matrix"
    printf '%s %s %s %s %s\n' "large_ndjson" "$ndjson_fixture" "$count" \
      "eq_status_open" '/status="open"' >> "$case_matrix"
    return 0
  fi
  generate_fixtures
  : > "$case_matrix"
  add_dataset_selector_cases "large_ndjson" "$ndjson_fixture" "$count"
  add_dataset_selector_cases "large_array" "$array_fixture" "$count"
  add_selection_selector_cases "selection_ndjson" "$cli_ndjson_fixture" "$count" ""
  add_selection_selector_cases "selection_array" "$cli_array_fixture" "$count" ""
  printf '%s %s %s %s %s\n' "large_single_json" "$single_fixture" 1 \
    "records_status_open" '/records[]/status="open"' >> "$case_matrix"
  add_selection_selector_cases "selection_single_json" "$cli_single_fixture" 1 \
    "/records[]"
  case "$suite" in
    full) ;;
    smoke)
      awk '
        ($1 == "large_ndjson" && $4 == "eq_status_open") ||
        ($1 == "large_array" && $4 == "date_window") ||
        ($1 == "selection_single_json" && $4 == "contains_service")
      ' "$case_matrix" > "$case_matrix.smoke"
      mv "$case_matrix.smoke" "$case_matrix"
      ;;
    memory) ;;
    *)
      printf 'unsupported benchmark suite: %s\n' "$suite" >&2
      return 2
      ;;
  esac
}

add_dataset_selector_cases() {
  dataset_name=$1
  fixture_path=$2
  candidates=$3
  {
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "eq_status_open" '/status="open"'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "contains_blob" 'contains{field=/blob,value=xxxx}'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "contains_any_blob" 'contains{field=/blob,any=xxxx|nomatch}'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "icontains_blob" 'icontains{field=/blob,value=XXXX}'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "timestamp_gte" '/timestamp>=2026-03-05T10:28:21Z'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "date_window" 'date{field=/timestamp,after=2026-03-05T10:28:21Z,before=2026-03-05T10:29:50Z}'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "range_qps" 'range{field=/metrics/qps,gte=100,lte=130}'
  } >> "$case_matrix"
}

add_selection_selector_cases() {
  dataset_name=$1
  fixture_path=$2
  candidates=$3
  prefix=$4
  {
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "and_region_latency" "and.eq{field=$prefix/region,value=us-west},and.range{field=$prefix/metrics/latency_ms,lt=350}"
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "contains_service" "contains{field=$prefix/service,value=auth}"
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "icontains_service" "icontains{field=$prefix/service,value=AUTH}"
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "contains_any_service" "contains{field=$prefix/service,any=auth|search}"
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "icontains_any_service" "icontains{field=$prefix/service,any=AUTH|GATEWAY}"
  } >> "$case_matrix"
}

run_c() {
  dataset_name=$1
  fixture_path=$2
  candidates=$3
  selector_name=$4
  expr=$5
  out="$fixture_dir/c-$dataset_name-$selector_name.out"
  bytes=$(wc -c < "$fixture_path" | tr -d ' ')
  if [ ! -x "$clql" ]; then
    emit_unsupported_impl "c" "clql binary not found; run make build-debug or set CLQL_PATH"
    return 1
  fi
  fixture_sha=$(file_sha256 "$fixture_path")
  "$clql" "$expr" "$fixture_path" > "$out"
  matches=$(wc -l < "$out" | tr -d ' ')
  if [ "$dataset_name" = "large_ndjson" ] &&
    [ "$selector_name" = "eq_status_open" ]; then
    candidates=$(fault_count "$candidates" "$inject_candidate_mismatch")
    matches=$(fault_count "$matches" "$inject_match_mismatch")
  fi
  printf '%s %s %s %s %s %s %s %s\n' "$dataset_name" "$selector_name" \
    "decision_only_selector" "warmup_included" "$candidates" "$matches" 0 0 >> "$c_counts_file"
  printf '%s %s %s %s %s %s %s %s\n' "$dataset_name" "$selector_name" \
    "decision_only_selector" "steady_state" "$candidates" "$matches" 0 0 >> "$c_counts_file"
  emit_submode_records "c" "$dataset_name" "$selector_name" "$expr" \
    "decision_only_selector" "$bytes" "$candidates" "$matches" 0 0 "none" null null false "" "$fixture_sha"
}

run_c_native_mode() {
  mode=$1
  dataset_name=$2
  fixture_path=$3
  candidates=$4
  selector_name=$5
  expr=$6
  bytes=$(wc -c < "$fixture_path" | tr -d ' ')
  if [ ! -x "$payload_bench" ]; then
    emit_submode_records "c" "$dataset_name" "$selector_name" "$expr" \
      "$mode" 0 0 0 0 0 "none" null null true \
      "lql_payload_bench binary not found; run make build-debug or set LQL_PAYLOAD_BENCH_PATH" \
      "$(file_sha256 "$fixture_path")"
    return 1
  fi
  fixture_sha=$(file_sha256 "$fixture_path")
  record=$("$payload_bench" "$mode" "$expr" "$fixture_path")
  c_candidates=$(kv_field candidates "$record")
  c_matches=$(kv_field matches "$record")
  c_payloads=$(kv_field payloads "$record")
  c_payload_bytes=$(kv_field payload_bytes "$record")
  c_elapsed_ns=$(kv_field elapsed_ns "$record")
  c_peak_rss_bytes=$(kv_field peak_rss_bytes "$record")
  if [ -z "$c_candidates" ] || [ -z "$c_matches" ] ||
    [ -z "$c_payloads" ] || [ -z "$c_payload_bytes" ] ||
    [ -z "$c_elapsed_ns" ] || [ -z "$c_peak_rss_bytes" ]; then
    printf 'C payload benchmark emitted an invalid record: %s\n' "$record" >&2
    return 1
  fi
  if [ "$dataset_name" = "large_ndjson" ] &&
    [ "$selector_name" = "eq_status_open" ]; then
    c_candidates=$(fault_count "$c_candidates" "$inject_candidate_mismatch")
    c_matches=$(fault_count "$c_matches" "$inject_match_mismatch")
    c_payloads=$(fault_count "$c_payloads" "$inject_payload_mismatch")
    c_payload_bytes=$(fault_count "$c_payload_bytes" "$inject_payload_byte_mismatch")
  fi
  : "$candidates"
  printf '%s %s %s %s %s %s %s %s\n' "$dataset_name" "$selector_name" \
    "$mode" "warmup_included" "$c_candidates" "$c_matches" "$c_payloads" \
    "$c_payload_bytes" >> "$c_counts_file"
  printf '%s %s %s %s %s %s %s %s\n' "$dataset_name" "$selector_name" \
    "$mode" "steady_state" "$c_candidates" "$c_matches" "$c_payloads" \
    "$c_payload_bytes" >> "$c_counts_file"
  payload_source_type=none
  case "$mode" in
    plus_value_openjson_*) payload_source_type=spool ;;
    plus_value_*) payload_source_type=seekable_range ;;
  esac
  emit_submode_records "c" "$dataset_name" "$selector_name" "$expr" \
    "$mode" "$bytes" "$c_candidates" \
    "$c_matches" "$c_payloads" "$c_payload_bytes" "$payload_source_type" \
    "$c_elapsed_ns" "$c_peak_rss_bytes" false "" "$fixture_sha"
}

run_go_mode() {
  mode=$1
  dataset_name=$2
  fixture_path=$3
  candidates=$4
  selector_name=$5
  expr=$6
  : "$candidates"
  record=
  if ! ensure_go_bench; then
    emit_unsupported_impl "go" "go executable not found"
    return 1
  fi
  for submode in warmup_included steady_state; do
    record=$("$go_bench" \
      --fixture "$fixture_path" \
      --dataset "$dataset_name" \
      --selector-name "$selector_name" \
      --expr "$expr" \
      --mode "$mode" \
      --submode "$submode")
    printf '%s\n' "$record"
    go_candidates=$(json_number_field candidates "$record")
    go_matches=$(json_number_field matches "$record")
    go_payloads=$(json_number_field payloads "$record")
    go_payload_bytes=$(json_number_field payload_bytes "$record")
    if [ -z "$go_candidates" ] || [ -z "$go_matches" ] ||
      [ -z "$go_payloads" ] || [ -z "$go_payload_bytes" ]; then
      printf 'Go benchmark emitted an invalid record: %s\n' "$record" >&2
      return 1
    fi
    printf '%s %s %s %s %s %s %s %s\n' "$dataset_name" "$selector_name" \
      "$mode" "$submode" "$go_candidates" "$go_matches" "$go_payloads" \
      "$go_payload_bytes" >> "$go_counts_file"
  done
}

run_lua_mode() {
  mode=$1
  dataset_name=$2
  fixture_path=$3
  candidates=$4
  selector_name=$5
  expr=$6
  bytes=$(wc -c < "$fixture_path" | tr -d ' ')
  fixture_sha=$(file_sha256 "$fixture_path")
  if ! command -v "$lua_bin" >/dev/null 2>&1; then
    emit_unsupported_impl "lua" "lua executable not found"
    return 1
  fi
  if [ ! -f "$root/build/debug/lql/core.so" ]; then
    emit_unsupported_impl "lua" "lql.core module not found; run make build-debug"
    return 1
  fi
  payload_source_type=none
  case "$mode" in
    plus_value_*) payload_source_type=lua_liblql ;;
  esac
  for submode in warmup_included steady_state; do
    record=$(LUA_PATH="$root/lua/?.lua;$root/lua/?/init.lua;;" \
      LUA_CPATH="$root/build/debug/?.so;$root/build/debug/?/core.so;;" \
      LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$root/build/debug:$root/.cache/deps/x86_64-linux-gnu/install/lib" \
      DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-}:$root/build/debug:$root/.cache/deps/x86_64-linux-gnu/install/lib" \
      "$lua_bin" "$root/lua/benchmarks/parity.lua" "$mode" "$expr" \
      "$fixture_path" "$candidates" "$submode")
    lua_candidates=$(kv_field candidates "$record")
    lua_matches=$(kv_field matches "$record")
    lua_payloads=$(kv_field payloads "$record")
    lua_payload_bytes=$(kv_field payload_bytes "$record")
    lua_elapsed_ns=$(kv_field elapsed_ns "$record")
    if [ -z "$lua_candidates" ] || [ -z "$lua_matches" ] ||
      [ -z "$lua_payloads" ] || [ -z "$lua_payload_bytes" ] ||
      [ -z "$lua_elapsed_ns" ]; then
      printf 'Lua benchmark emitted an invalid record: %s\n' "$record" >&2
      return 1
    fi
    printf '%s %s %s %s %s %s %s %s\n' "$dataset_name" "$selector_name" \
      "$mode" "$submode" "$lua_candidates" "$lua_matches" "$lua_payloads" \
      "$lua_payload_bytes" >> "$lua_counts_file"
    emit_record "lua" "$dataset_name" "$selector_name" "$expr" \
      "$mode" "$submode" "$bytes" "$lua_candidates" "$lua_matches" \
      "$lua_payloads" "$lua_payload_bytes" "$payload_source_type" \
      "$lua_elapsed_ns" null false "" "$fixture_sha"
  done
}

selected_modes() {
  case "$mode_profile" in
    all)
      printf '%s\n' \
        decision_only_selector \
        decision_only_plan \
        plus_value_selector \
        plus_value_plan \
        plus_value_openjson_selector \
        plus_value_openjson_plan
      ;;
    memory)
      printf '%s\n' \
        decision_only_selector \
        plus_value_selector \
        plus_value_openjson_selector
      ;;
    *)
      printf 'unsupported benchmark mode profile: %s\n' "$mode_profile" >&2
      return 2
      ;;
  esac
}

run_matrix_for_impl() {
  impl=$1
  while read dataset_name fixture_path candidates selector_name expr; do
    case "$impl" in
      go)
        for mode in $(selected_modes); do
          run_go_mode "$mode" "$dataset_name" "$fixture_path" "$candidates" \
            "$selector_name" "$expr" || return 1
        done
        ;;
      c)
        for mode in $(selected_modes); do
          run_c_native_mode "$mode" "$dataset_name" "$fixture_path" \
            "$candidates" "$selector_name" "$expr" || return 1
        done
        ;;
      lua)
        for mode in $(selected_modes); do
          run_lua_mode "$mode" "$dataset_name" "$fixture_path" "$candidates" \
            "$selector_name" "$expr" || return 1
        done
        ;;
      *)
        return 2
        ;;
    esac
  done < "$case_matrix"
}

compare_go_impl() {
  impl=$1
  impl_counts_file=$2
  if [ ! -s "$go_counts_file" ] || [ ! -s "$impl_counts_file" ]; then
    return 0
  fi
  while read dataset_name selector_name mode submode go_candidates go_matches go_payloads go_payload_bytes; do
    impl_line=$(sed -n "s/^$dataset_name $selector_name $mode $submode //p" "$impl_counts_file")
    if [ -z "$impl_line" ]; then
      printf 'benchmark missing %s count record: dataset=%s selector=%s mode=%s submode=%s\n' \
        "$impl" "$dataset_name" "$selector_name" "$mode" "$submode" >&2
      return 1
    fi
    set -- $impl_line
    impl_candidates=$1
    impl_matches=$2
    impl_payloads=$3
    impl_payload_bytes=$4
    if [ "$go_candidates" != "$impl_candidates" ]; then
      printf 'benchmark candidate-count mismatch: go=%s %s=%s dataset=%s selector=%s mode=%s submode=%s\n' \
        "$go_candidates" "$impl" "$impl_candidates" "$dataset_name" "$selector_name" "$mode" "$submode" >&2
      return 1
    fi
    if [ "$go_matches" != "$impl_matches" ]; then
      printf 'benchmark match-count mismatch: go=%s %s=%s dataset=%s selector=%s mode=%s submode=%s\n' \
        "$go_matches" "$impl" "$impl_matches" "$dataset_name" "$selector_name" "$mode" "$submode" >&2
      return 1
    fi
    if [ "$go_payloads" != "$impl_payloads" ]; then
      printf 'benchmark payload-count mismatch: go=%s %s=%s dataset=%s selector=%s mode=%s submode=%s\n' \
        "$go_payloads" "$impl" "$impl_payloads" "$dataset_name" "$selector_name" "$mode" "$submode" >&2
      return 1
    fi
    if [ "$go_payload_bytes" != "$impl_payload_bytes" ]; then
      printf 'benchmark payload-byte mismatch: go=%s %s=%s dataset=%s selector=%s mode=%s submode=%s\n' \
        "$go_payload_bytes" "$impl" "$impl_payload_bytes" "$dataset_name" "$selector_name" "$mode" "$submode" >&2
      return 1
    fi
  done < "$go_counts_file"
  return 0
}

exit_status=0
if ! validate_required_impls; then
  exit_status=1
fi
generate_fixture

if [ "$exit_status" -eq 0 ] && is_selected go; then
  if ! run_matrix_for_impl go; then
    exit_status=1
  fi
fi

if [ "$exit_status" -eq 0 ] && is_selected c; then
  if ! run_matrix_for_impl c; then
    exit_status=1
  fi
fi

if [ "$exit_status" -eq 0 ] && is_selected lua; then
  if ! run_matrix_for_impl lua; then
    exit_status=1
  fi
fi

if [ "$check" -eq 1 ] && [ "$exit_status" -eq 0 ]; then
  if [ ! -s "$ndjson_fixture" ] || [ ! -s "$array_fixture" ] ||
    [ ! -s "$single_fixture" ] || [ ! -s "$cli_ndjson_fixture" ] ||
    [ ! -s "$cli_array_fixture" ] || [ ! -s "$cli_single_fixture" ]; then
    printf 'benchmark check failed: fixture was not generated\n' >&2
    exit_status=1
  else
    if ! compare_go_impl c "$c_counts_file"; then
      exit_status=1
    elif ! compare_go_impl lua "$lua_counts_file"; then
      exit_status=1
    fi
  fi
fi

exit "$exit_status"
