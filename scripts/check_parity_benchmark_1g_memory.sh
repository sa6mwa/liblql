#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
log=${LQL_BENCH_1G_LOG:-$root/build/bench-1g-check.jsonl}
min_bytes=${LQL_BENCH_1G_MIN_BYTES:-1073741824}
count=${LQL_BENCH_1G_COUNT:-262144}
blob_bytes=${LQL_BENCH_1G_BLOB_BYTES:-4096}

LQL_BENCH_MEMORY_LOG="$log" \
  LQL_BENCH_MEMORY_MIN_BYTES="$min_bytes" \
  LQL_BENCH_MEMORY_COUNT="$count" \
  LQL_BENCH_MEMORY_BLOB_BYTES="$blob_bytes" \
  "$root/scripts/check_parity_benchmark_large_memory.sh"

printf 'benchmark 1g memory check: wrote %s\n' "$log"
