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
