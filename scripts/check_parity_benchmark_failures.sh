#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runner="$root/scripts/run_parity_benchmarks.sh"
tmp="${TMPDIR:-/tmp}/liblql-benchmark-negative-$$"

cleanup() {
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmp"
go_bin="${GO:-go}"

expect_failure() {
  name=$1
  expected=$2
  shift 2
  log="$tmp/$name.log"
  if "$@" > "$log" 2>&1; then
    printf 'benchmark negative check unexpectedly passed: %s\n' "$name" >&2
    return 1
  fi
  if ! grep -q "$expected" "$log"; then
    printf 'benchmark negative check did not report %s: %s\n' "$expected" "$name" >&2
    cat "$log" >&2
    return 1
  fi
}

expect_validator_failure() {
  name=$1
  expected=$2
  log="$tmp/$name.jsonl"
  output="$tmp/$name.out"
  cat > "$log"
  if (cd "$root/parity" && "$go_bin" run ./cmd/benchvalidate) < "$log" > "$output" 2>&1; then
    printf 'benchmark validator negative check unexpectedly passed: %s\n' "$name" >&2
    return 1
  fi
  if ! grep -q "$expected" "$output"; then
    printf 'benchmark validator negative check did not report %s: %s\n' "$expected" "$name" >&2
    cat "$output" >&2
    return 1
  fi
}

expect_validator_failure "missing-required-timing" "must report ns_per_op" <<'JSONL'
{"schema":"liblql.parity_benchmark.v1","impl":"go","dataset":"large_ndjson","selector":"eq_status_open","expr":"/status=\"open\"","mode":"decision_only_selector","submode":"warmup_included","bytes_per_iter":1,"candidates":1,"matches":1,"payloads":0,"payload_bytes":0,"payload_source_type":"none","fixture_sha256":"0000000000000000000000000000000000000000000000000000000000000000","ns_per_op":null,"peak_rss_bytes":1,"allocs_per_op":null,"unsupported":false,"unsupported_reason":""}
JSONL

expect_validator_failure "missing-required-peak-rss" "must report peak_rss_bytes" <<'JSONL'
{"schema":"liblql.parity_benchmark.v1","impl":"c","dataset":"large_ndjson","selector":"eq_status_open","expr":"/status=\"open\"","mode":"decision_only_selector","submode":"warmup_included","bytes_per_iter":1,"candidates":1,"matches":1,"payloads":0,"payload_bytes":0,"payload_source_type":"none","fixture_sha256":"0000000000000000000000000000000000000000000000000000000000000000","ns_per_op":1,"peak_rss_bytes":null,"allocs_per_op":null,"unsupported":false,"unsupported_reason":""}
JSONL

expect_validator_failure "c-plus-value-spool" "must use seekable_range payloads" <<'JSONL'
{"schema":"liblql.parity_benchmark.v1","impl":"c","dataset":"large_ndjson","selector":"eq_status_open","expr":"/status=\"open\"","mode":"plus_value_openjson_selector","submode":"warmup_included","bytes_per_iter":1,"candidates":1,"matches":1,"payloads":1,"payload_bytes":1,"payload_source_type":"spool","fixture_sha256":"0000000000000000000000000000000000000000000000000000000000000000","ns_per_op":1,"peak_rss_bytes":1,"allocs_per_op":null,"unsupported":false,"unsupported_reason":""}
JSONL

expect_validator_failure "c-source-plus-value-seekable" "must use spooled payloads" <<'JSONL'
{"schema":"liblql.parity_benchmark.v1","impl":"c","dataset":"large_ndjson","selector":"eq_status_open","expr":"/status=\"open\"","mode":"plus_value_source_selector","submode":"warmup_included","bytes_per_iter":1,"candidates":1,"matches":1,"payloads":1,"payload_bytes":1,"payload_source_type":"seekable_range","fixture_sha256":"0000000000000000000000000000000000000000000000000000000000000000","ns_per_op":1,"peak_rss_bytes":1,"allocs_per_op":null,"unsupported":false,"unsupported_reason":""}
JSONL

log="$tmp/unsupported-report-only.jsonl"
cat > "$log" <<'JSONL'
{"schema":"liblql.parity_benchmark.v1","impl":"lua","dataset":"large_ndjson","selector":"eq_status_open","expr":"/status=\"open\"","mode":"decision_only_selector","submode":"warmup_included","bytes_per_iter":1,"candidates":0,"matches":0,"payloads":0,"payload_bytes":0,"payload_source_type":"none","fixture_sha256":"0000000000000000000000000000000000000000000000000000000000000000","ns_per_op":null,"peak_rss_bytes":null,"allocs_per_op":null,"unsupported":true,"unsupported_reason":"fixture report-only unsupported backend"}
{"schema":"liblql.parity_benchmark.v1","impl":"lua","dataset":"large_ndjson","selector":"eq_status_open","expr":"/status=\"open\"","mode":"decision_only_selector","submode":"steady_state","bytes_per_iter":1,"candidates":0,"matches":0,"payloads":0,"payload_bytes":0,"payload_source_type":"none","fixture_sha256":"0000000000000000000000000000000000000000000000000000000000000000","ns_per_op":null,"peak_rss_bytes":null,"allocs_per_op":null,"unsupported":true,"unsupported_reason":"fixture report-only unsupported backend"}
JSONL
if ! (cd "$root/parity" && "$go_bin" run ./cmd/benchvalidate) < "$log" > "$tmp/unsupported-report-only.out" 2>&1; then
  printf 'benchmark validator report-only unsupported fixture unexpectedly failed\n' >&2
  cat "$tmp/unsupported-report-only.out" >&2
  exit 1
fi
if (cd "$root/parity" && "$go_bin" run ./cmd/benchvalidate --forbid-unsupported) < "$log" > "$tmp/unsupported-strict.out" 2>&1; then
  printf 'benchmark validator strict unsupported fixture unexpectedly passed\n' >&2
  exit 1
fi
if ! grep -q "unsupported record forbidden" "$tmp/unsupported-strict.out"; then
  printf 'benchmark validator strict unsupported fixture did not report forbidden unsupported record\n' >&2
  cat "$tmp/unsupported-strict.out" >&2
  exit 1
fi

log="$tmp/c-peak-rss-limit.jsonl"
cat > "$log" <<'JSONL'
{"schema":"liblql.parity_benchmark.v1","impl":"c","dataset":"large_ndjson","selector":"eq_status_open","expr":"/status=\"open\"","mode":"decision_only_selector","submode":"warmup_included","bytes_per_iter":1,"candidates":1,"matches":1,"payloads":0,"payload_bytes":0,"payload_source_type":"none","fixture_sha256":"0000000000000000000000000000000000000000000000000000000000000000","ns_per_op":1,"peak_rss_bytes":2,"allocs_per_op":null,"unsupported":false,"unsupported_reason":""}
{"schema":"liblql.parity_benchmark.v1","impl":"c","dataset":"large_ndjson","selector":"eq_status_open","expr":"/status=\"open\"","mode":"decision_only_selector","submode":"steady_state","bytes_per_iter":1,"candidates":1,"matches":1,"payloads":0,"payload_bytes":0,"payload_source_type":"none","fixture_sha256":"0000000000000000000000000000000000000000000000000000000000000000","ns_per_op":1,"peak_rss_bytes":2,"allocs_per_op":null,"unsupported":false,"unsupported_reason":""}
JSONL
expect_failure "c-peak-rss-limit" "exceeds max" \
  env GO="$go_bin" LQL_BENCH_MAX_C_PEAK_RSS_BYTES=1 \
    "$root/scripts/check_parity_benchmark_memory.sh" "$log"

expect_failure "candidate-mismatch" "benchmark candidate-count mismatch" \
  env LQL_BENCH_FIXTURE_DIR="$tmp/candidate" \
    LQL_BENCH_SUITE=smoke \
    LQL_BENCH_INJECT_CANDIDATE_MISMATCH=1 \
    "$runner" --impl go,c --format json --check --require go,c

expect_failure "match-mismatch" "benchmark match-count mismatch" \
  env LQL_BENCH_FIXTURE_DIR="$tmp/match" \
    LQL_BENCH_SUITE=smoke \
    LQL_BENCH_INJECT_MATCH_MISMATCH=1 \
    "$runner" --impl go,c --format json --check --require go,c

expect_failure "payload-count-mismatch" "benchmark payload-count mismatch" \
  env LQL_BENCH_FIXTURE_DIR="$tmp/payload-count" \
    LQL_BENCH_SUITE=smoke \
    LQL_BENCH_INJECT_PAYLOAD_MISMATCH=1 \
    "$runner" --impl go,c --format json --check --require go,c

expect_failure "payload-byte-mismatch" "benchmark payload-byte mismatch" \
  env LQL_BENCH_FIXTURE_DIR="$tmp/payload-byte" \
    LQL_BENCH_SUITE=smoke \
    LQL_BENCH_INJECT_PAYLOAD_BYTE_MISMATCH=1 \
    "$runner" --impl go,c --format json --check --require go,c

expect_failure "missing-required-impl" "benchmark missing required implementation" \
  env LQL_BENCH_FIXTURE_DIR="$tmp/missing-required" \
    LQL_BENCH_SUITE=smoke \
    "$runner" --impl go --format json --check --require go,c
