#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
baseline_dir=${LQL_BENCH_BASELINE_DIR:-$root/bench/baselines}
tmp=${LQL_BENCH_FREEZE_TMP:-$root/build/bench-baseline-freeze}
go_bin="${GO:-go}"

cpu_count() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
    return
  fi
  getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1\n'
}

read_load1() {
  if [ -r /proc/loadavg ]; then
    awk '{print $1}' /proc/loadavg
    return
  fi
  uptime | awk -F'load averages?: ' '{print $2}' | awk -F, '{gsub(/^[[:space:]]+/, "", $1); print $1}'
}

wait_for_quiet_host() {
  cpus=$(cpu_count)
  max_load=${LQL_BENCH_FREEZE_MAX_LOAD:-}
  max_load_per_cpu=${LQL_BENCH_FREEZE_MAX_LOAD_PER_CPU:-0.50}
  interval=${LQL_BENCH_FREEZE_POLL_SECONDS:-30}
  stable_polls=${LQL_BENCH_FREEZE_STABLE_POLLS:-3}
  max_polls=${LQL_BENCH_FREEZE_MAX_POLLS:-0}
  quiet_seen=0
  polls=0

  if [ -z "$max_load" ]; then
    max_load=$(awk -v c="$cpus" -v p="$max_load_per_cpu" 'BEGIN { printf "%.2f", c * p }')
  fi

  printf 'benchmark baseline freeze: waiting for host load <= %s for %s consecutive polls\n' \
    "$max_load" "$stable_polls" >&2
  while :; do
    load1=$(read_load1)
    polls=$((polls + 1))
    if awk -v observed="$load1" -v max="$max_load" \
      'BEGIN { exit !(observed <= max) }'; then
      quiet_seen=$((quiet_seen + 1))
      printf 'benchmark baseline freeze: load1=%s quiet=%s/%s\n' \
        "$load1" "$quiet_seen" "$stable_polls" >&2
      if [ "$quiet_seen" -ge "$stable_polls" ]; then
        return 0
      fi
    else
      quiet_seen=0
      printf 'benchmark baseline freeze: load1=%s above threshold %s\n' \
        "$load1" "$max_load" >&2
    fi
    if [ "$max_polls" -ne 0 ] && [ "$polls" -ge "$max_polls" ]; then
      printf 'benchmark baseline freeze: host did not become quiet after %s polls\n' \
        "$polls" >&2
      exit 1
    fi
    sleep "$interval"
  done
}

validate_log() {
  log=$1
  if [ ! -s "$log" ]; then
    printf 'benchmark baseline freeze: empty log: %s\n' "$log" >&2
    exit 1
  fi
  (cd "$root/parity" && "$go_bin" run ./cmd/benchvalidate --forbid-unsupported) < "$log"
}

write_manifest() {
  (
    cd "$baseline_dir"
    if command -v sha256sum >/dev/null 2>&1; then
      sha256sum *.jsonl
    else
      shasum -a 256 *.jsonl
    fi
  ) | sort > "$baseline_dir/SHA256SUMS"
}

wait_for_quiet_host

rm -rf "$tmp"
mkdir -p "$tmp" "$baseline_dir"

printf 'benchmark baseline freeze: running smoke/parity benchmark\n' >&2
LQL_BENCH_SUITE=smoke \
  "$root/scripts/run_parity_benchmarks.sh" --impl go,c,lua --format json --check --require go,c,lua > "$tmp/bench-check.jsonl"
validate_log "$tmp/bench-check.jsonl"

printf 'benchmark baseline freeze: running lockd performance benchmark\n' >&2
LQL_BENCH_LOCKD_PERF_LOG="$tmp/bench-lockd-perf-check.jsonl" \
  "$root/scripts/check_lockd_perf_benchmark.sh"
validate_log "$tmp/bench-lockd-perf-check.jsonl"

printf 'benchmark baseline freeze: running scalable memory benchmarks\n' >&2
LQL_BENCH_MEMORY_LOG="$tmp/bench-memory-check.jsonl" \
  LQL_BENCH_MUTATION_MEMORY_LOG="$tmp/bench-memory-mutation-check.jsonl" \
  LQL_BENCH_PROJECTION_MEMORY_LOG="$tmp/bench-memory-projection-check.jsonl" \
  "$root/scripts/check_parity_benchmark_large_memory.sh"
validate_log "$tmp/bench-memory-check.jsonl"
validate_log "$tmp/bench-memory-mutation-check.jsonl"
validate_log "$tmp/bench-memory-projection-check.jsonl"

printf 'benchmark baseline freeze: running 100 MiB memory benchmark\n' >&2
LQL_BENCH_LARGE_JSON_LOG="$tmp/bench-large-json-check.jsonl" \
  "$root/scripts/check_parity_benchmark_large_json_memory.sh"
validate_log "$tmp/bench-large-json-check.jsonl"

cp "$tmp"/*.jsonl "$baseline_dir"/
write_manifest

printf 'benchmark baseline freeze: wrote %s\n' "$baseline_dir" >&2
