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
lua_module_dir="${LQL_LUA_MODULE_DIR:-$root/build/debug-lua}"
bench_library_dir="${LQL_BENCH_LIBRARY_DIR:-$root/build/debug}"
bench_dep_library_dir="${LQL_BENCH_DEP_LIBRARY_DIR:-$root/.cache/deps/x86_64-linux-gnu/install/lib}"
time_bin="${LQL_BENCH_TIME:-/usr/bin/time}"
require_lua_rss="${LQL_BENCH_REQUIRE_LUA_RSS:-0}"
mkdir -p "$fixture_dir"
ndjson_fixture="$fixture_dir/large_ndjson.jsonl"
single_fixture="$fixture_dir/large_single_json.json"
cli_ndjson_fixture="$fixture_dir/selection_ndjson.jsonl"
cli_single_fixture="$fixture_dir/selection_single_json.json"
lockd_ndjson_fixture="$fixture_dir/lockd_ndjson.jsonl"
mixed_root_ndjson_fixture="$fixture_dir/mixed_root_ndjson.jsonl"
realworld_compact_fixture="$fixture_dir/realworld_compact_ndjson.jsonl"
realworld_pretty_nested_fixture="$fixture_dir/realworld_pretty_nested.jsonl"
lockd_perf_contains_fixture="$fixture_dir/lockd_perf_contains.jsonl"
lockd_file_backed_text_fixture="$fixture_dir/lockd_file_backed_text.jsonl"
lockd_file_backed_base64_fixture="$fixture_dir/lockd_file_backed_base64.jsonl"
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

detect_time_mode() {
  time_probe="$fixture_dir/time-probe.$$"
  if "$time_bin" -f 'peak_rss_kb=%M' -o "$time_probe" true >/dev/null 2>&1; then
    rm -f "$time_probe"
    printf 'gnu\n'
    return 0
  fi
  if "$time_bin" -l true >"$time_probe" 2>&1; then
    rm -f "$time_probe"
    printf 'darwin\n'
    return 0
  fi
  rm -f "$time_probe"
  printf 'none\n'
}

parse_time_peak_rss_bytes() {
  mode=$1
  file=$2
  case "$mode" in
    gnu)
      awk -F= '/^peak_rss_kb=/{ printf "%d\n", $2 * 1024 }' "$file"
      ;;
    darwin)
      awk '/maximum resident set size/{ printf "%d\n", $1 }' "$file"
      ;;
    *)
      return 1
      ;;
  esac
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
      "reuse_selector" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "reparse_selector_each_run" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "decision_only_source_selector" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "plus_value_selector" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "plus_value_plan" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "plus_value_source_selector" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "plus_value_openjson_selector" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "plus_value_openjson_plan" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "mutate_file_selector" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "mutate_file_plan" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "mutate_source_selector" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "project_file_selector" 0 0 0 0 0 "none" null null true "$reason" \
      "$(file_sha256 "$fixture_path")"
    emit_submode_records "$impl" "$dataset_name" "$selector_name" "$expr" \
      "project_source_selector" 0 0 0 0 0 "none" null null true "$reason" \
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

generate_repeat() {
  text=$1
  count=$2
  awk -v text="$text" -v n="$count" 'BEGIN { for (i = 0; i < n; ++i) printf "%s", text }'
}

record_json() {
  i=$1
  numeric_amount=$((i % 200))
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
  if [ $((i % 2)) -eq 0 ]; then
    voucher=$(printf '"voucher":{"lines":{"10":{"amount":%d,"status":"%s"}}}' \
      "$numeric_amount" "$status")
  else
    voucher=$(printf '"voucher":{"lines":[{"amount":0},{"amount":1},{"amount":2},{"amount":3},{"amount":4},{"amount":5},{"amount":6},{"amount":7},{"amount":8},{"amount":9},{"amount":%d,"status":"%s"}]}' \
      "$numeric_amount" "$status")
  fi
  printf '{"id":"id-%d","status":"%s","metrics":{"retries":%d,"qps":%d},%s,"timestamp":"%s","blob":"%s"}' \
    "$i" "$status" $((i % 7)) $((i + 1)) "$voucher" "$timestamp" \
    "$record_blob"
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

lockd_record_json() {
  i=$1
  if [ -z "$record_blob" ]; then
    record_blob=$(generate_blob)
  fi
  case $((i % 4)) in
    0) event=session_sync ;;
    1) event=tabs_update ;;
    2) event=lock_request ;;
    *) event=heartbeat ;;
  esac
  case $((i % 3)) in
    0) op=write ;;
    1) op=delete ;;
    *) op=read ;;
  esac
  printf '{"event":"%s","op":"%s","session_id":"session-%d","lockd":{"key":"browser/session/%d","owner":"pid-%d","tab":{"id":"tab-%d","url":"https://example.test/%d","active":%s}},"timestamp":"2026-03-05T11:28:21Z","payload":"%s"}' \
    "$event" "$op" "$i" "$i" $((1000 + i)) "$i" "$i" \
    "$([ $((i % 2)) -eq 0 ] && printf true || printf false)" \
    "$record_blob"
}

realworld_blob() {
  seed=$1
  n=$2
  awk -v seed="$seed" -v n="$n" 'BEGIN {
    alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (i = 0; i < n; ++i) {
      pos = ((seed + i * 7) % length(alphabet)) + 1;
      printf "%s", substr(alphabet, pos, 1);
    }
  }'
}

realworld_event() {
  i=$1
  if [ $((i % 61)) -eq 0 ]; then
    printf 'session_sync'
    return 0
  fi
  case $((i % 4)) in
    0) printf 'heartbeat' ;;
    1) printf 'cache_refresh' ;;
    2) printf 'ui_render' ;;
    *) printf 'snapshot_emit' ;;
  esac
}

realworld_component() {
  i=$1
  if [ $((i % 5)) -ne 0 ]; then
    printf 'edge'
    return 0
  fi
  case $((i % 3)) in
    0) printf 'worker' ;;
    1) printf 'ingest' ;;
    *) printf 'scheduler' ;;
  esac
}

realworld_hash() {
  i=$1
  if [ $((i % 97)) -eq 0 ]; then
    printf 'c5d2460186f7233c927e7db2dcc703c0a3a8e0d5f0d8a3c5b4f1e2d3c4b5a697'
    return 0
  fi
  printf '%08x%08x%08x%08x%08x%08x%08x%08x' \
    $((i * 17 + 3)) $((i * 19 + 5)) $((i * 23 + 7)) \
    $((i * 29 + 11)) $((i * 31 + 13)) $((i * 37 + 17)) \
    $((i * 41 + 19)) $((i * 43 + 23))
}

realworld_session_ids_json() {
  i=$1
  event=$2
  if [ $((i % 73)) -eq 0 ]; then
    printf ',"session_ids":["sid-0a3f-target","sid-%04d-extra"]' "$i"
  elif [ $((i % 2)) -eq 0 ]; then
    printf ',"session_ids":["sid-%04d-a","sid-%04d-b"]' "$i" "$i"
  elif [ "$event" = "session_sync" ]; then
    printf ',"session_ids":["sid-%04d-a","sid-%04d-b"]' "$i" "$i"
  fi
}

realworld_record_json() {
  i=$1
  nested=$2
  event=$(realworld_event "$i")
  component=$(realworld_component "$i")
  code=$((3 + (i % 11)))
  active_idx=$((i % 4))
  tab_count=$((2 + (i % 4)))
  hash=$(realworld_hash "$i")
  session_ids=$(realworld_session_ids_json "$i" "$event")
  if [ "$event" = "session_sync" ]; then
    active_idx=0
    tab_count=1
    if [ "$code" -lt 10 ]; then
      code=$((10 + (i % 4)))
    fi
  fi
  case $((i % 3)) in
    0) zone=eu-north ;;
    1) zone=us-east ;;
    *) zone=ap-south ;;
  esac
  if [ $((i % 5)) -eq 0 ]; then
    retryable=true
  else
    retryable=false
  fi
  if [ "$nested" = "1" ]; then
    blob_a=$(realworld_blob "$i" $((72 + (i % 24))))
    printf '{"event":"%s","component":"%s","code":%d,"active_idx":%d,"tab_count":%d,"query":{"hash":"%s","latency_ms":%d,"fingerprint":"fp-%04d"},"payload":[{"kind":"segment","meta":{"label":"seg-%03d","rank":%d},"blob":"%s"},{"kind":"summary","children":[{"id":"child-%03d","enabled":%s},{"id":"child-%03d","enabled":%s}],"notes":["synthetic","anonymous","shape-%d"]}],"timestamp":"2026-03-10T12:%02d:%02dZ","meta":{"zone":"%s","build":"build-%03d","retryable":%s}%s}' \
      "$event" "$component" "$code" "$active_idx" "$tab_count" "$hash" \
      $((12 + (i % 180))) $(((i * 17) % 4096)) $((i % 128)) \
      $((i % 9)) "$blob_a" $((i % 64)) \
      "$([ $((i % 3)) -ne 0 ] && printf true || printf false)" \
      $(((i + 7) % 64)) \
      "$([ $((i % 4)) -ne 0 ] && printf true || printf false)" \
      $((i % 11)) $((i % 60)) $(((i * 7) % 60)) "$zone" \
      $((i % 200)) "$retryable" "$session_ids"
  else
    blob_b=$(realworld_blob "$i" $((96 + (i % 32))))
    case $((i % 3)) in
      0) payload_status=ok ;;
      1) payload_status=warm ;;
      *) payload_status=cold ;;
    esac
    printf '{"event":"%s","component":"%s","code":%d,"active_idx":%d,"tab_count":%d,"query":{"hash":"%s","latency_ms":%d,"fingerprint":"fp-%04d"},"payload":{"blob":"%s","status":"%s","frames":[{"id":%d,"kind":"header"},{"id":%d,"kind":"body"}],"lookup":{"active":%s,"score":%d}},"timestamp":"2026-03-10T12:%02d:%02dZ","meta":{"zone":"%s","build":"build-%03d","retryable":%s}%s}' \
      "$event" "$component" "$code" "$active_idx" "$tab_count" "$hash" \
      $((12 + (i % 180))) $(((i * 17) % 4096)) "$blob_b" \
      "$payload_status" $((i % 8)) $(((i + 3) % 8)) \
      "$([ $((i % 2)) -eq 0 ] && printf true || printf false)" \
      $(((i * 29) % 1000)) $((i % 60)) $(((i * 7) % 60)) \
      "$zone" $((i % 200)) "$retryable" "$session_ids"
  fi
}

realworld_pretty_record_json() {
  i=$1
  nested=$2
  record=$(realworld_record_json "$i" "$nested")
  printf '%s\n' "$record" | sed 's/[{},]/&\
  /g'
}

mixed_root_record_json() {
  i=$1
  case $((i % 4)) in
    0)
      printf '"scalar-%d"' "$i"
      ;;
    1)
      printf '%d' "$i"
      ;;
    2)
      record_json "$i"
      ;;
    *)
      printf 'false'
      ;;
  esac
}

lockd_perf_contains_record_json() {
  i=$1
  msg=$(generate_repeat "x" 640)
  printf '{"msg":"%s","idx":%d}' "$msg" "$i"
}

generate_lockd_perf_fixtures() {
  i=0
  : > "$lockd_perf_contains_fixture"
  : > "$lockd_file_backed_text_fixture"
  : > "$lockd_file_backed_base64_fixture"
  while [ "$i" -lt "$count" ]; do
    lockd_perf_contains_record_json "$i" >> "$lockd_perf_contains_fixture"
    printf '\n' >> "$lockd_perf_contains_fixture"
    i=$((i + 1))
  done
  printf '{"payload":"old"}\n' > "$lockd_file_backed_text_fixture"
  printf '{"payload":"old"}\n' > "$lockd_file_backed_base64_fixture"
  generate_repeat "hello world\n" 512 > "$lockd_file_backed_text_fixture.payload"
  i=0
  : > "$lockd_file_backed_base64_fixture.payload"
  while [ "$i" -lt 2048 ]; do
    printf '\000\001\002\003' >> "$lockd_file_backed_base64_fixture.payload"
    i=$((i + 1))
  done
}

generate_fixtures() {
  i=0
  : > "$ndjson_fixture"
  : > "$single_fixture"
  : > "$cli_ndjson_fixture"
  : > "$cli_single_fixture"
  : > "$lockd_ndjson_fixture"
  : > "$mixed_root_ndjson_fixture"
  : > "$realworld_compact_fixture"
  : > "$realworld_pretty_nested_fixture"
  while [ "$i" -lt "$count" ]; do
    record_json "$i" >> "$ndjson_fixture"
    printf '\n' >> "$ndjson_fixture"
    selection_record_json "$i" >> "$cli_ndjson_fixture"
    printf '\n' >> "$cli_ndjson_fixture"
    lockd_record_json "$i" >> "$lockd_ndjson_fixture"
    printf '\n' >> "$lockd_ndjson_fixture"
    mixed_root_record_json "$i" >> "$mixed_root_ndjson_fixture"
    printf '\n' >> "$mixed_root_ndjson_fixture"
    realworld_record_json "$i" 0 >> "$realworld_compact_fixture"
    printf '\n' >> "$realworld_compact_fixture"
    realworld_pretty_record_json "$i" 1 >> "$realworld_pretty_nested_fixture"
    printf '\n' >> "$realworld_pretty_nested_fixture"
    i=$((i + 1))
  done
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
    printf '%s %s %s %s %s\n' "large_ndjson" "$ndjson_fixture" "$count" \
      "contains_blob" 'contains{field=/blob,value=xxxx}' >> "$case_matrix"
    printf '%s %s %s %s %s\n' "large_ndjson" "$ndjson_fixture" "$count" \
      "icontains_blob" 'icontains{field=/blob,value=XXXX}' >> "$case_matrix"
    printf '%s %s %s %s %s\n' "large_ndjson" "$ndjson_fixture" "$count" \
      "temporal_eq_date_only" '/timestamp="2026-03-05"' >> "$case_matrix"
    printf '%s %s %s %s %s\n' "large_ndjson" "$ndjson_fixture" "$count" \
      "temporal_range_shorthand_gte" '/timestamp>=2026-03-05T10:28:21Z' >> "$case_matrix"
    printf '%s %s %s %s %s\n' "large_ndjson" "$ndjson_fixture" "$count" \
      "temporal_range_selector" 'range{field=/timestamp,gte=2026-03-05T10:28:21Z,lt=2026-03-05T10:30:00Z}' >> "$case_matrix"
    printf '%s %s %s %s %s\n' "large_ndjson" "$ndjson_fixture" "$count" \
      "date_selector_after_before" 'date{field=/timestamp,after=2026-03-05T10:28:21Z,before=2026-03-05T10:30:00Z}' >> "$case_matrix"
    printf '%s %s %s %s %s\n' "large_ndjson" "$ndjson_fixture" "$count" \
      "date_selector_since_macro" 'date{f=/timestamp,since=yesterday}' >> "$case_matrix"
    return 0
  fi
  if [ "$suite" = "lockd-perf" ]; then
    generate_lockd_perf_fixtures
    : > "$case_matrix"
    printf '%s %s %s %s %s\n' "lockd_perf_contains" \
      "$lockd_perf_contains_fixture" "$count" "contains_any_msg" \
      'icontains{field=/msg,any=alpha|beta|gamma}' >> "$case_matrix"
    printf '%s %s %s %s %s\n' "lockd_perf_contains" \
      "$lockd_perf_contains_fixture" "$count" "explicit_or_msg" \
      'or.icontains{field=/msg,value=alpha},or.icontains{field=/msg,value=beta},or.icontains{field=/msg,value=gamma}' >> "$case_matrix"
    printf '%s %s %s %s %s\n' "lockd_file_backed_text" \
      "$lockd_file_backed_text_fixture" 1 "file_backed_text" \
      'contains{field=/}' >> "$case_matrix"
    printf '%s %s %s %s %s\n' "lockd_file_backed_base64" \
      "$lockd_file_backed_base64_fixture" 1 "file_backed_base64" \
      'contains{field=/}' >> "$case_matrix"
    return 0
  fi
  generate_fixtures
  : > "$case_matrix"
  add_dataset_selector_cases "large_ndjson" "$ndjson_fixture" "$count"
  add_selection_selector_cases "selection_ndjson" "$cli_ndjson_fixture" "$count" ""
  add_lockd_selector_cases "lockd_ndjson" "$lockd_ndjson_fixture" "$count"
  add_capture_selector_cases "mixed_root_ndjson" "$mixed_root_ndjson_fixture" "$count"
  add_realworld_selector_cases "realworld_compact_ndjson" \
    "$realworld_compact_fixture" "$count"
  add_realworld_selector_cases "realworld_pretty_nested" \
    "$realworld_pretty_nested_fixture" "$count"
  printf '%s %s %s %s %s\n' "large_single_json" "$single_fixture" 1 \
    "records_status_open" '/records[]/status="open"' >> "$case_matrix"
  add_selection_selector_cases "selection_single_json" "$cli_single_fixture" 1 \
    "/records[]"
  case "$suite" in
    full) ;;
    smoke)
      awk '
        ($1 == "large_ndjson" && $4 == "eq_status_open") ||
        ($1 == "large_ndjson" && $4 == "numeric_path_amount") ||
        ($1 == "lockd_ndjson" && $4 == "lockd_session_sync") ||
        ($1 == "mixed_root_ndjson" && $4 == "mixed_low_match_id") ||
        ($1 == "realworld_compact_ndjson" && $4 == "realworld_multi_clause_and") ||
        ($1 == "realworld_pretty_nested" && $4 == "realworld_recursive_nested_eq_sparse") ||
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
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "numeric_path_amount" '/voucher/lines/10/amount>=100'
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

add_lockd_selector_cases() {
  dataset_name=$1
  fixture_path=$2
  candidates=$3
  {
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "lockd_session_sync" '/event="session_sync"'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "lockd_tabs_update" '/event="tabs_update"'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "lockd_write_event" 'and.eq{field=/op,value=write},and.exists{field=/lockd/key}'
  } >> "$case_matrix"
}

add_realworld_selector_cases() {
  dataset_name=$1
  fixture_path=$2
  candidates=$3
  {
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "realworld_eq_sparse" '/event="session_sync"'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "realworld_eq_dense" '/component="edge"'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "realworld_eq_none" '/event="__nope__"'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "realworld_range_sparse" '/code>=11'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "realworld_nested_eq_sparse" '/query/hash="c5d2460186f7233c927e7db2dcc703c0a3a8e0d5f0d8a3c5b4f1e2d3c4b5a697"'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "realworld_array_eq_sparse" '/session_ids[]="sid-0a3f-target"'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "realworld_recursive_eq_sparse" '/.../event="session_sync"'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "realworld_recursive_nested_eq_sparse" '/.../hash="c5d2460186f7233c927e7db2dcc703c0a3a8e0d5f0d8a3c5b4f1e2d3c4b5a697"'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "realworld_contains_event_sparse" 'contains{field=/event,value=sync}'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "realworld_icontains_component_dense" 'icontains{field=/component,value=EDGE}'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "realworld_contains_any_event_sparse" 'contains{field=/event,any=sync|__nope__}'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "realworld_icontains_any_component_dense" 'icontains{field=/component,any=EDGE|__nope__}'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "realworld_multi_clause_and" '/component="edge",/event="session_sync",/active_idx=0,/tab_count=1,exists{/session_ids},/code>=10'
  } >> "$case_matrix"
}

add_capture_selector_cases() {
  dataset_name=$1
  fixture_path=$2
  candidates=$3
  {
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "mixed_object_root_status" '/status="closed"'
    printf '%s %s %s %s %s\n' "$dataset_name" "$fixture_path" "$candidates" \
      "mixed_low_match_id" '/id="id-2"'
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
  case "$mode" in
    mutate_file_backed_text|mutate_file_backed_base64)
      if [ -f "$fixture_path.payload" ]; then
        payload_bytes_for_mode=$(wc -c < "$fixture_path.payload" | tr -d ' ')
        bytes=$((bytes + payload_bytes_for_mode))
      fi
      ;;
  esac
  if [ ! -x "$payload_bench" ]; then
    emit_submode_records "c" "$dataset_name" "$selector_name" "$expr" \
      "$mode" 0 0 0 0 0 "none" null null true \
      "lql_payload_bench binary not found; run make build-debug or set LQL_PAYLOAD_BENCH_PATH" \
      "$(file_sha256 "$fixture_path")"
    return 1
  fi
  fixture_sha=$(file_sha256 "$fixture_path")
  : "$candidates"
  payload_source_type=none
  case "$mode" in
    plus_value_source_selector) payload_source_type=spooled ;;
    plus_value_*) payload_source_type=seekable_range ;;
    project_*) payload_source_type=projection ;;
  esac
  for submode in warmup_included steady_state; do
    record=$("$payload_bench" "$mode" "$expr" "$fixture_path" "$selector_name" "$submode")
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
    printf '%s %s %s %s %s %s %s %s\n' "$dataset_name" "$selector_name" \
      "$mode" "$submode" "$c_candidates" "$c_matches" "$c_payloads" \
      "$c_payload_bytes" >> "$c_counts_file"
    emit_record "c" "$dataset_name" "$selector_name" "$expr" \
      "$mode" "$submode" "$bytes" "$c_candidates" \
      "$c_matches" "$c_payloads" "$c_payload_bytes" "$payload_source_type" \
      "$c_elapsed_ns" "$c_peak_rss_bytes" false "" "$fixture_sha"
  done
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
  if [ ! -f "$lua_module_dir/lql/core.so" ]; then
    emit_unsupported_impl "lua" "lql.core module not found; run make build-debug-lua"
    return 1
  fi
  lua_time_mode=$(detect_time_mode)
  if [ "$require_lua_rss" = "1" ] && [ "$lua_time_mode" = "none" ]; then
    printf 'Lua benchmark RSS is required, but %s does not support GNU -f/-o or Darwin -l time output\n' "$time_bin" >&2
    return 1
  fi
  payload_source_type=none
  case "$mode" in
    plus_value_source_selector) payload_source_type=spooled ;;
    plus_value_*) payload_source_type=lua_liblql ;;
  esac
  for submode in warmup_included steady_state; do
    safe_tag=$(printf '%s-%s-%s-%s' "$dataset_name" "$selector_name" "$mode" "$submode" |
      sed 's/[^A-Za-z0-9_.-]/_/g')
    lua_out="$fixture_dir/lua-$safe_tag.out"
    lua_time="$fixture_dir/lua-$safe_tag.time"
    lua_peak_rss_bytes=null
    if [ "$lua_time_mode" = "gnu" ]; then
      if ! "$time_bin" -f 'peak_rss_kb=%M' -o "$lua_time" \
        env \
        LUA_PATH="$root/lua/?.lua;$root/lua/?/init.lua;;" \
        LUA_CPATH="$lua_module_dir/?.so;$lua_module_dir/?/core.so;;" \
        LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$bench_library_dir:$bench_dep_library_dir" \
        DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-}:$bench_library_dir:$bench_dep_library_dir" \
        "$lua_bin" "$root/lua/benchmarks/parity.lua" "$mode" "$expr" \
        "$fixture_path" "$candidates" "$submode" > "$lua_out"; then
        cat "$lua_out" >&2
        cat "$lua_time" >&2
        return 1
      fi
      lua_peak_rss_bytes=$(parse_time_peak_rss_bytes "$lua_time_mode" "$lua_time")
    elif [ "$lua_time_mode" = "darwin" ]; then
      if ! "$time_bin" -l \
        env \
        LUA_PATH="$root/lua/?.lua;$root/lua/?/init.lua;;" \
        LUA_CPATH="$lua_module_dir/?.so;$lua_module_dir/?/core.so;;" \
        LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$bench_library_dir:$bench_dep_library_dir" \
        DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-}:$bench_library_dir:$bench_dep_library_dir" \
        "$lua_bin" "$root/lua/benchmarks/parity.lua" "$mode" "$expr" \
        "$fixture_path" "$candidates" "$submode" > "$lua_out" 2> "$lua_time"; then
        cat "$lua_out" >&2
        cat "$lua_time" >&2
        return 1
      fi
      lua_peak_rss_bytes=$(parse_time_peak_rss_bytes "$lua_time_mode" "$lua_time")
    else
      if ! env \
        LUA_PATH="$root/lua/?.lua;$root/lua/?/init.lua;;" \
        LUA_CPATH="$lua_module_dir/?.so;$lua_module_dir/?/core.so;;" \
        LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$bench_library_dir:$bench_dep_library_dir" \
        DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-}:$bench_library_dir:$bench_dep_library_dir" \
        "$lua_bin" "$root/lua/benchmarks/parity.lua" "$mode" "$expr" \
        "$fixture_path" "$candidates" "$submode" > "$lua_out"; then
        cat "$lua_out" >&2
        return 1
      fi
    fi
    if [ -z "$lua_peak_rss_bytes" ]; then
      printf 'Lua benchmark RSS parser produced no value for %s\n' "$lua_time" >&2
      return 1
    fi
    record=$(cat "$lua_out")
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
      "$lua_elapsed_ns" "$lua_peak_rss_bytes" false "" "$fixture_sha"
  done
}

selected_modes() {
  case "$mode_profile" in
    all)
      printf '%s\n' \
        decision_only_selector \
        decision_only_plan \
        reuse_selector \
        reparse_selector_each_run \
        decision_only_source_selector \
        plus_value_selector \
        plus_value_plan \
        plus_value_source_selector \
        plus_value_openjson_selector \
        plus_value_openjson_plan \
        mutate_file_selector \
        mutate_file_plan \
        mutate_source_selector
      ;;
    memory)
      printf '%s\n' \
        decision_only_selector \
        reuse_selector \
        reparse_selector_each_run \
        decision_only_source_selector \
        plus_value_selector \
        plus_value_source_selector \
        plus_value_openjson_selector
      ;;
    large-json)
      printf '%s\n' \
        decision_only_selector \
        plus_value_selector \
        plus_value_source_selector
      ;;
    mutation-memory)
      printf '%s\n' \
        mutate_file_selector \
        mutate_file_plan \
        mutate_source_selector
      ;;
    projection-memory)
      printf '%s\n' \
        project_file_selector \
        project_source_selector
      ;;
    lockd-perf)
      printf '%s\n' \
        decision_only_plan \
        mutate_file_backed_text \
        mutate_file_backed_base64
      ;;
    *)
      printf 'unsupported benchmark mode profile: %s\n' "$mode_profile" >&2
      return 2
      ;;
  esac
}

mode_applies_to_case() {
  mode=$1
  selector_name=$2
  case "$mode_profile" in
    large-json)
      case "$selector_name:$mode" in
        eq_status_open:decision_only_selector|eq_status_open:plus_value_selector|eq_status_open:plus_value_source_selector)
          return 0
          ;;
        *)
          return 1
          ;;
      esac
      ;;
    lockd-perf)
      case "$selector_name:$mode" in
        contains_any_msg:decision_only_plan|explicit_or_msg:decision_only_plan)
          return 0
          ;;
        file_backed_text:mutate_file_backed_text)
          return 0
          ;;
        file_backed_base64:mutate_file_backed_base64)
          return 0
          ;;
        *)
          return 1
          ;;
      esac
      ;;
  esac
  return 0
}

run_matrix_for_impl() {
  impl=$1
  while read dataset_name fixture_path candidates selector_name expr; do
    case "$impl" in
      go)
        for mode in $(selected_modes); do
          mode_applies_to_case "$mode" "$selector_name" || continue
          run_go_mode "$mode" "$dataset_name" "$fixture_path" "$candidates" \
            "$selector_name" "$expr" || return 1
        done
        ;;
      c)
        for mode in $(selected_modes); do
          mode_applies_to_case "$mode" "$selector_name" || continue
          run_c_native_mode "$mode" "$dataset_name" "$fixture_path" \
            "$candidates" "$selector_name" "$expr" || return 1
        done
        ;;
      lua)
        for mode in $(selected_modes); do
          mode_applies_to_case "$mode" "$selector_name" || continue
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
  if [ "$suite" = "memory" ]; then
    fixtures_ready=1
    [ -s "$ndjson_fixture" ] || fixtures_ready=0
  elif [ "$suite" = "lockd-perf" ]; then
    fixtures_ready=1
    [ -s "$lockd_perf_contains_fixture" ] || fixtures_ready=0
    [ -s "$lockd_file_backed_text_fixture" ] || fixtures_ready=0
    [ -s "$lockd_file_backed_base64_fixture" ] || fixtures_ready=0
    [ -s "$lockd_file_backed_text_fixture.payload" ] || fixtures_ready=0
    [ -s "$lockd_file_backed_base64_fixture.payload" ] || fixtures_ready=0
  else
    fixtures_ready=1
    [ -s "$ndjson_fixture" ] || fixtures_ready=0
    [ -s "$single_fixture" ] || fixtures_ready=0
    [ -s "$cli_ndjson_fixture" ] || fixtures_ready=0
    [ -s "$cli_single_fixture" ] || fixtures_ready=0
    [ -s "$lockd_ndjson_fixture" ] || fixtures_ready=0
    [ -s "$mixed_root_ndjson_fixture" ] || fixtures_ready=0
    [ -s "$realworld_compact_fixture" ] || fixtures_ready=0
    [ -s "$realworld_pretty_nested_fixture" ] || fixtures_ready=0
  fi
  if [ "$fixtures_ready" -ne 1 ]; then
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
