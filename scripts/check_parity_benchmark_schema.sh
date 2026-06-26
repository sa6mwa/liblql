#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
go_bin="${GO:-go}"
tmp="${TMPDIR:-/tmp}/liblql-benchmark-schema-$$"

cleanup() {
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmp"
log="$tmp/benchmarks.jsonl"

LQL_BENCH_FIXTURE_DIR="$tmp/fixtures" \
  LQL_BENCH_SUITE=smoke \
  "$root/scripts/run_parity_benchmarks.sh" --impl go,c,lua --format json > "$log"

(cd "$root/parity" && "$go_bin" run ./cmd/benchvalidate) < "$log"

if ! grep -E '"unsupported":false,"unsupported_reason":""' "$log" | \
  grep -E '"ns_per_op":[0-9]+' >/dev/null; then
  printf 'benchmark schema check did not find any timed supported records\n' >&2
  exit 1
fi

if ! grep -E '"unsupported":false,"unsupported_reason":""' "$log" | \
  grep -E '"peak_rss_bytes":[1-9][0-9]*' >/dev/null; then
  printf 'benchmark schema check did not find any supported peak RSS records\n' >&2
  exit 1
fi
