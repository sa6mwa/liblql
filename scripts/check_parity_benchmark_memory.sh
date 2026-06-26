#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
go_bin="${GO:-go}"
log=${1:-}
max=${LQL_BENCH_MAX_C_PEAK_RSS_BYTES:-134217728}

if [ -z "$log" ]; then
  printf 'usage: check_parity_benchmark_memory.sh BENCHMARK_JSONL\n' >&2
  exit 2
fi

if [ ! -s "$log" ]; then
  printf 'benchmark memory check input is empty: %s\n' "$log" >&2
  exit 1
fi

(cd "$root/parity" && "$go_bin" run ./cmd/benchvalidate --max-c-peak-rss-bytes "$max") < "$log"
