#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
log="$root/build/bench-memory-check.jsonl"
min_bytes=${LQL_BENCH_MEMORY_MIN_BYTES:-16777216}
count=${LQL_BENCH_MEMORY_COUNT:-4096}
blob_bytes=${LQL_BENCH_MEMORY_BLOB_BYTES:-4096}

mkdir -p "$root/build"
LQL_BENCH_SUITE=memory \
  LQL_BENCH_MODE_PROFILE=memory \
  LQL_BENCH_NDJSON_COUNT="$count" \
  LQL_BENCH_RECORD_BLOB_BYTES="$blob_bytes" \
  "$root/scripts/run_parity_benchmarks.sh" --impl c --format json > "$log"

"$root/scripts/check_parity_benchmark_memory.sh" "$log"

max_bytes=$(sed -n 's/.*"bytes_per_iter":\([0-9][0-9]*\).*/\1/p' "$log" |
  sort -n | tail -1)
if [ -z "$max_bytes" ] || [ "$max_bytes" -lt "$min_bytes" ]; then
  printf 'benchmark memory fixture too small: got=%s want-at-least=%s\n' \
    "${max_bytes:-0}" "$min_bytes" >&2
  exit 1
fi
