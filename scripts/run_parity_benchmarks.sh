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
go_bin="${GO:-go}"
mkdir -p "$fixture_dir"
fixture="$fixture_dir/large_ndjson.jsonl"

json_string() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
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
  ns_per_op=${12}
  unsupported=${13}
  reason=${14}
  printf '{"schema":"liblql.parity_benchmark.v1","impl":"%s","dataset":"%s","selector":"%s","expr":"%s","mode":"%s","submode":"%s","bytes_per_iter":%s,"candidates":%s,"matches":%s,"payloads":%s,"payload_bytes":%s,"payload_source_type":"%s","ns_per_op":%s,"allocs_per_op":null,"unsupported":%s,"unsupported_reason":"%s"}\n' \
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
    "none" \
    "$ns_per_op" \
    "$unsupported" \
    "$(json_string "$reason")"
}

emit_unsupported_impl() {
  impl=$1
  reason=$2
  emit_record "$impl" "large_ndjson" "eq_status_open" "/status=\"open\"" \
    "decision_only_selector" "steady_state" 0 0 0 0 0 null true "$reason"
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

generate_fixture() {
  i=0
  : > "$fixture"
  while [ "$i" -lt "$count" ]; do
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
    printf '{"id":"id-%d","status":"%s","metrics":{"retries":%d,"qps":%d},"timestamp":"%s","blob":"xxxxxxxxxxxxxxxx"}\n' \
      "$i" "$status" $((i % 7)) $((i + 1)) "$timestamp" >> "$fixture"
    i=$((i + 1))
  done
}

run_c() {
  out="$fixture_dir/c-eq-status-open.out"
  expr='/status="open"'
  expected=$(((count + 2) / 4))
  bytes=$(wc -c < "$fixture" | tr -d ' ')
  if [ ! -x "$clql" ]; then
    emit_unsupported_impl "c" "clql binary not found; run make build-debug or set CLQL_PATH"
    return 1
  fi
  "$clql" "$expr" "$fixture" > "$out"
  matches=$(wc -l < "$out" | tr -d ' ')
  if [ "$matches" -ne "$expected" ]; then
    printf 'C benchmark mismatch: selector=%s got_matches=%s expected_matches=%s fixture=%s\n' \
      "$expr" "$matches" "$expected" "$fixture" >&2
    return 1
  fi
  emit_record "c" "large_ndjson" "eq_status_open" "$expr" \
    "decision_only_selector" "steady_state" "$bytes" "$count" "$matches" 0 0 null false ""
}

run_go() {
  expr='/status="open"'
  if ! command -v "$go_bin" >/dev/null 2>&1; then
    emit_unsupported_impl "go" "go executable not found"
    return 1
  fi
  (cd "$root/parity" && "$go_bin" run ./cmd/lqlbench \
    --fixture "$fixture" \
    --dataset large_ndjson \
    --selector-name eq_status_open \
    --expr "$expr" \
    --mode decision_only_selector \
    --submode steady_state)
}

exit_status=0
generate_fixture

if is_selected go; then
  if ! run_go; then
    exit_status=1
  fi
fi

if is_selected c; then
  if ! run_c; then
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
  if [ ! -s "$fixture" ]; then
    printf 'benchmark check failed: fixture was not generated\n' >&2
    exit_status=1
  fi
fi

exit "$exit_status"
