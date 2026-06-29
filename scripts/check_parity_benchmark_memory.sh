#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
go_bin="${GO:-go}"
log=${1:-}
c_max=${LQL_BENCH_MAX_C_PEAK_RSS_BYTES:-134217728}
c_max_ns_per_byte=${LQL_BENCH_MAX_C_STEADY_STATE_NS_PER_BYTE:-500}
lua_max=${LQL_BENCH_MAX_LUA_PEAK_RSS_BYTES:-$c_max}
require_lua_rss=${LQL_BENCH_REQUIRE_LUA_RSS:-0}

if [ -z "$log" ]; then
  printf 'usage: check_parity_benchmark_memory.sh BENCHMARK_JSONL\n' >&2
  exit 2
fi

if [ ! -s "$log" ]; then
  printf 'benchmark memory check input is empty: %s\n' "$log" >&2
  exit 1
fi

args="--forbid-unsupported --max-c-peak-rss-bytes $c_max --max-c-steady-state-ns-per-byte $c_max_ns_per_byte --max-lua-peak-rss-bytes $lua_max"
if [ "$require_lua_rss" = "1" ]; then
  args="$args --require-lua-peak-rss"
fi

(cd "$root/parity" && "$go_bin" run ./cmd/benchvalidate $args) < "$log"
