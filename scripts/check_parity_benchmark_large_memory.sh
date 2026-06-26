#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
log=${LQL_BENCH_MEMORY_LOG:-$root/build/bench-memory-check.jsonl}
mutation_log=${LQL_BENCH_MUTATION_MEMORY_LOG:-$root/build/bench-memory-mutation-check.jsonl}
min_bytes=${LQL_BENCH_MEMORY_MIN_BYTES:-16777216}
count=${LQL_BENCH_MEMORY_COUNT:-4096}
blob_bytes=${LQL_BENCH_MEMORY_BLOB_BYTES:-4096}

mkdir -p "$root/build"
LQL_BENCH_SUITE=memory \
  LQL_BENCH_MODE_PROFILE=memory \
  LQL_BENCH_REQUIRE_LUA_RSS=1 \
  LQL_BENCH_NDJSON_COUNT="$count" \
  LQL_BENCH_RECORD_BLOB_BYTES="$blob_bytes" \
  "$root/scripts/run_parity_benchmarks.sh" --impl go,c,lua --format json --check --require go,c,lua > "$log"

LQL_BENCH_REQUIRE_LUA_RSS=1 "$root/scripts/check_parity_benchmark_memory.sh" "$log"

LQL_BENCH_SUITE=memory \
  LQL_BENCH_MODE_PROFILE=mutation-memory \
  LQL_BENCH_NDJSON_COUNT="$count" \
  LQL_BENCH_RECORD_BLOB_BYTES="$blob_bytes" \
  "$root/scripts/run_parity_benchmarks.sh" --impl go,c --format json --check --require go,c > "$mutation_log"

"$root/scripts/check_parity_benchmark_memory.sh" "$mutation_log"

max_bytes=$(sed -n 's/.*"bytes_per_iter":\([0-9][0-9]*\).*/\1/p' "$log" "$mutation_log" |
  sort -n | tail -1)
if [ -z "$max_bytes" ] || [ "$max_bytes" -lt "$min_bytes" ]; then
  printf 'benchmark memory fixture too small: got=%s want-at-least=%s\n' \
    "${max_bytes:-0}" "$min_bytes" >&2
  exit 1
fi
