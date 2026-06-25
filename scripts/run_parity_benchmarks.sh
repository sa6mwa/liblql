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
clql="${CLQL_PATH:-$root/build/debug/clql}"
payload_bench="${LQL_PAYLOAD_BENCH_PATH:-$root/build/debug/lql_payload_bench}"
go_bin="${GO:-go}"
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
inject_candidate_mismatch="${LQL_BENCH_INJECT_CANDIDATE_MISMATCH:-0}"
inject_match_mismatch="${LQL_BENCH_INJECT_MATCH_MISMATCH:-0}"

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
  unsupported=${14}
  reason=${15}
  fixture_sha256=${16}
  printf '{"schema":"liblql.parity_benchmark.v1","impl":"%s","dataset":"%s","selector":"%s","expr":"%s","mode":"%s","submode":"%s","bytes_per_iter":%s,"candidates":%s,"matches":%s,"payloads":%s,"payload_bytes":%s,"payload_source_type":"%s","fixture_sha256":"%s","ns_per_op":%s,"allocs_per_op":null,"unsupported":%s,"unsupported_reason":"%s"}\n' \
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
    "$unsupported" \
    "$(json_string "$reason")"
}

emit_unsupported_impl() {
  impl=$1
  reason=$2
  while read dataset_name fixture_path candidates selector_name expr; do
    : "$candidates"
    emit_record "$impl" "$dataset_name" "$selector_name" "$expr" \
      "decision_only_selector" "steady_state" 0 0 0 0 0 "none" null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_record "$impl" "$dataset_name" "$selector_name" "$expr" \
      "plus_value_selector" "steady_state" 0 0 0 0 0 "none" null true "$reason" \
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

is_required() {
  case ",$required," in
    *",$1,"*) return 0 ;;
    *) return 1 ;;
  esac
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

record_json() {
  i=$1
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
  printf '{"id":"id-%d","status":"%s","metrics":{"retries":%d,"qps":%d},"timestamp":"%s","blob":"xxxxxxxxxxxxxxxx"}' \
    "$i" "$status" $((i % 7)) $((i + 1)) "$timestamp"
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
  printf '%s %s %s %s %s %s %s\n' "$dataset_name" "$selector_name" \
    "decision_only_selector" "$candidates" "$matches" 0 0 >> "$c_counts_file"
  emit_record "c" "$dataset_name" "$selector_name" "$expr" \
    "decision_only_selector" "steady_state" "$bytes" "$candidates" "$matches" 0 0 "none" null false "" "$fixture_sha"
}

run_c_payload() {
  dataset_name=$1
  fixture_path=$2
  candidates=$3
  selector_name=$4
  expr=$5
  bytes=$(wc -c < "$fixture_path" | tr -d ' ')
  if [ ! -x "$payload_bench" ]; then
    emit_record "c" "$dataset_name" "$selector_name" "$expr" \
      "plus_value_selector" "steady_state" 0 0 0 0 0 "none" null true \
      "lql_payload_bench binary not found; run make build-debug or set LQL_PAYLOAD_BENCH_PATH" \
      "$(file_sha256 "$fixture_path")"
    return 1
  fi
  fixture_sha=$(file_sha256 "$fixture_path")
  record=$("$payload_bench" "$expr" "$fixture_path")
  c_candidates=$(kv_field candidates "$record")
  c_matches=$(kv_field matches "$record")
  c_payloads=$(kv_field payloads "$record")
  c_payload_bytes=$(kv_field payload_bytes "$record")
  if [ -z "$c_candidates" ] || [ -z "$c_matches" ] ||
    [ -z "$c_payloads" ] || [ -z "$c_payload_bytes" ]; then
    printf 'C payload benchmark emitted an invalid record: %s\n' "$record" >&2
    return 1
  fi
  if [ "$dataset_name" = "large_ndjson" ] &&
    [ "$selector_name" = "eq_status_open" ]; then
    c_candidates=$(fault_count "$c_candidates" "$inject_candidate_mismatch")
    c_matches=$(fault_count "$c_matches" "$inject_match_mismatch")
  fi
  : "$candidates"
  printf '%s %s %s %s %s %s %s\n' "$dataset_name" "$selector_name" \
    "plus_value_selector" "$c_candidates" "$c_matches" "$c_payloads" \
    "$c_payload_bytes" >> "$c_counts_file"
  emit_record "c" "$dataset_name" "$selector_name" "$expr" \
    "plus_value_selector" "steady_state" "$bytes" "$c_candidates" \
    "$c_matches" "$c_payloads" "$c_payload_bytes" "seekable_range" null false \
    "" "$fixture_sha"
}

run_go() {
  dataset_name=$1
  fixture_path=$2
  candidates=$3
  selector_name=$4
  expr=$5
  : "$candidates"
  record=
  if ! command -v "$go_bin" >/dev/null 2>&1; then
    emit_unsupported_impl "go" "go executable not found"
    return 1
  fi
  record=$(cd "$root/parity" && "$go_bin" run ./cmd/lqlbench \
    --fixture "$fixture_path" \
    --dataset "$dataset_name" \
    --selector-name "$selector_name" \
    --expr "$expr" \
    --mode decision_only_selector \
    --submode steady_state)
  printf '%s\n' "$record"
  go_candidates=$(json_number_field candidates "$record")
  go_matches=$(json_number_field matches "$record")
  if [ -z "$go_candidates" ] || [ -z "$go_matches" ]; then
    printf 'Go benchmark emitted an invalid record: %s\n' "$record" >&2
    return 1
  fi
  printf '%s %s %s %s %s %s %s\n' "$dataset_name" "$selector_name" \
    "decision_only_selector" "$go_candidates" "$go_matches" 0 0 >> "$go_counts_file"
}

run_go_payload() {
  dataset_name=$1
  fixture_path=$2
  candidates=$3
  selector_name=$4
  expr=$5
  : "$candidates"
  record=
  if ! command -v "$go_bin" >/dev/null 2>&1; then
    emit_unsupported_impl "go" "go executable not found"
    return 1
  fi
  record=$(cd "$root/parity" && "$go_bin" run ./cmd/lqlbench \
    --fixture "$fixture_path" \
    --dataset "$dataset_name" \
    --selector-name "$selector_name" \
    --expr "$expr" \
    --mode plus_value_selector \
    --submode steady_state)
  printf '%s\n' "$record"
  go_candidates=$(json_number_field candidates "$record")
  go_matches=$(json_number_field matches "$record")
  go_payloads=$(json_number_field payloads "$record")
  go_payload_bytes=$(json_number_field payload_bytes "$record")
  if [ -z "$go_candidates" ] || [ -z "$go_matches" ] ||
    [ -z "$go_payloads" ] || [ -z "$go_payload_bytes" ]; then
    printf 'Go payload benchmark emitted an invalid record: %s\n' "$record" >&2
    return 1
  fi
  printf '%s %s %s %s %s %s %s\n' "$dataset_name" "$selector_name" \
    "plus_value_selector" "$go_candidates" "$go_matches" "$go_payloads" \
    "$go_payload_bytes" >> "$go_counts_file"
}

run_matrix_for_impl() {
  impl=$1
  while read dataset_name fixture_path candidates selector_name expr; do
    case "$impl" in
      go)
        run_go "$dataset_name" "$fixture_path" "$candidates" "$selector_name" \
          "$expr" || return 1
        run_go_payload "$dataset_name" "$fixture_path" "$candidates" \
          "$selector_name" "$expr" || return 1
        ;;
      c)
        run_c "$dataset_name" "$fixture_path" "$candidates" "$selector_name" \
          "$expr" || return 1
        run_c_payload "$dataset_name" "$fixture_path" "$candidates" \
          "$selector_name" "$expr" || return 1
        ;;
      *)
        return 2
        ;;
    esac
  done < "$case_matrix"
}

compare_go_c() {
  if [ ! -s "$go_counts_file" ] || [ ! -s "$c_counts_file" ]; then
    return 0
  fi
  while read dataset_name selector_name mode go_candidates go_matches go_payloads go_payload_bytes; do
    c_line=$(sed -n "s/^$dataset_name $selector_name $mode //p" "$c_counts_file")
    if [ -z "$c_line" ]; then
      printf 'benchmark missing C count record: dataset=%s selector=%s mode=%s\n' "$dataset_name" "$selector_name" "$mode" >&2
      return 1
    fi
    set -- $c_line
    c_candidates=$1
    c_matches=$2
    c_payloads=$3
    c_payload_bytes=$4
    if [ "$go_candidates" != "$c_candidates" ]; then
      printf 'benchmark candidate-count mismatch: go=%s c=%s dataset=%s selector=%s mode=%s\n' \
        "$go_candidates" "$c_candidates" "$dataset_name" "$selector_name" "$mode" >&2
      return 1
    fi
    if [ "$go_matches" != "$c_matches" ]; then
      printf 'benchmark match-count mismatch: go=%s c=%s dataset=%s selector=%s mode=%s\n' \
        "$go_matches" "$c_matches" "$dataset_name" "$selector_name" "$mode" >&2
      return 1
    fi
    if [ "$go_payloads" != "$c_payloads" ]; then
      printf 'benchmark payload-count mismatch: go=%s c=%s dataset=%s selector=%s mode=%s\n' \
        "$go_payloads" "$c_payloads" "$dataset_name" "$selector_name" "$mode" >&2
      return 1
    fi
    if [ "$go_payload_bytes" != "$c_payload_bytes" ]; then
      printf 'benchmark payload-byte mismatch: go=%s c=%s dataset=%s selector=%s mode=%s\n' \
        "$go_payload_bytes" "$c_payload_bytes" "$dataset_name" "$selector_name" "$mode" >&2
      return 1
    fi
  done < "$go_counts_file"
  return 0
}

exit_status=0
generate_fixture

if is_selected go; then
  if ! run_matrix_for_impl go; then
    exit_status=1
  fi
fi

if is_selected c; then
  if ! run_matrix_for_impl c; then
    exit_status=1
  fi
fi

if is_selected lua; then
  emit_unsupported_impl "lua" "Lua facade benchmark runner is not implemented yet"
  if is_required lua; then
    exit_status=1
  fi
fi

if [ "$check" -eq 1 ] && [ "$exit_status" -eq 0 ]; then
  if [ ! -s "$ndjson_fixture" ] || [ ! -s "$array_fixture" ] ||
    [ ! -s "$single_fixture" ] || [ ! -s "$cli_ndjson_fixture" ] ||
    [ ! -s "$cli_array_fixture" ] || [ ! -s "$cli_single_fixture" ]; then
    printf 'benchmark check failed: fixture was not generated\n' >&2
    exit_status=1
  elif ! compare_go_c; then
    exit_status=1
  fi
fi

exit "$exit_status"
