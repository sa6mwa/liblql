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
  "$root/scripts/run_parity_benchmarks.sh" --impl go,c,lua --format json > "$log"

cat "$log"

(cd "$root/parity" && "$go_bin" run ./cmd/benchvalidate) < "$log"
