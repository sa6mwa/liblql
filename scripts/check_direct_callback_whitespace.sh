#!/bin/sh
set -eu

c_bench=${LQL_DIRECT_BENCH_PATH:-build/release/lql_direct_bench}

if [ ! -x "$c_bench" ]; then
  printf 'direct callback whitespace: missing C benchmark binary: %s\n' "$c_bench" >&2
  exit 1
fi

sh scripts/generate_direct_probe_fixture.sh \
  build/direct-probe/status-100k.ndjson 100000 24 status
sh scripts/generate_direct_probe_fixture.sh \
  build/direct-probe/status-whitespace-100k.ndjson 100000 24 statusws

check_output() {
  name=$1
  output=$2
  expected_source=$3
  expected_payload_bytes=$4

  case "$output" in
    *'"candidates":100000,"matches":25000'*'"payloads":25000'*)
      ;;
    *)
      printf 'direct callback whitespace: unexpected counters for %s:\n%s\n' \
        "$name" "$output" >&2
      exit 1
      ;;
  esac

  case "$output" in
    *'"payload_bytes":'"$expected_payload_bytes"*)
      ;;
    *)
      printf 'direct callback whitespace: unexpected payload bytes for %s:\n%s\n' \
        "$name" "$output" >&2
      exit 1
      ;;
  esac

  case "$output" in
    *'"payload_source_type":"'"$expected_source"'"'*)
      ;;
    *)
      printf 'direct callback whitespace: unexpected payload source for %s:\n%s\n' \
        "$name" "$output" >&2
      exit 1
      ;;
  esac
}

compact_output=$(LQL_BENCH_SAMPLES=3 "$c_bench" \
  --fixture build/direct-probe/status-100k.ndjson \
  --dataset status_100k \
  --selector-name eq_status_open \
  --expr '/status="open"' \
  --mode plus_value_selector \
  --submode steady_state)
check_output compact "$compact_output" seekable_range 2238888

whitespace_output=$(LQL_BENCH_SAMPLES=3 "$c_bench" \
  --fixture build/direct-probe/status-whitespace-100k.ndjson \
  --dataset status_whitespace_100k \
  --selector-name eq_status_open \
  --expr '/status="open"' \
  --mode plus_value_selector \
  --submode steady_state)
check_output whitespace "$whitespace_output" spooled 2238888

printf '%s\n' 'direct callback whitespace: compact input uses source range; whitespace input uses compact spool'
