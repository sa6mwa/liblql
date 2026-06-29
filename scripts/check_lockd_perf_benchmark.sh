#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
log=${LQL_BENCH_LOCKD_PERF_LOG:-$root/build/bench-lockd-perf-check.jsonl}
count=${LQL_BENCH_LOCKD_PERF_COUNT:-4096}
c_max=${LQL_BENCH_MAX_C_PEAK_RSS_BYTES:-134217728}
c_max_ns_per_byte=${LQL_BENCH_MAX_C_STEADY_STATE_NS_PER_BYTE:-500}
file_max_ns_per_byte=${LQL_BENCH_LOCKD_FILE_MAX_NS_PER_BYTE:-$c_max_ns_per_byte}
contains_any_max_ratio=${LQL_BENCH_LOCKD_CONTAINS_ANY_MAX_NS_RATIO:-1.10}
contains_any_max_delta=${LQL_BENCH_LOCKD_CONTAINS_ANY_MAX_NS_DELTA:-2.00}
go_bin="${GO:-go}"

mkdir -p "$root/build"
LQL_BENCH_SUITE=lockd-perf \
  LQL_BENCH_MODE_PROFILE=lockd-perf \
  LQL_BENCH_NDJSON_COUNT="$count" \
  "$root/scripts/run_parity_benchmarks.sh" --impl go,c --format json --check --require go,c > "$log"

(cd "$root/parity" && "$go_bin" run ./cmd/benchvalidate \
  --forbid-unsupported \
  --max-c-peak-rss-bytes "$c_max" \
  --max-c-steady-state-ns-per-byte "$c_max_ns_per_byte") < "$log"

awk -v file_max="$file_max_ns_per_byte" -v ratio_max="$contains_any_max_ratio" \
  -v delta_max="$contains_any_max_delta" '
function field_number(line, key, s) {
  s = line
  sub(".*\"" key "\":", "", s)
  sub("[,}].*", "", s)
  return s + 0
}
function has(line, key, value) {
  return index(line, "\"" key "\":\"" value "\"") > 0
}
function record_metric(name, line, ns, bytes) {
  ns = field_number(line, "ns_per_op")
  bytes = field_number(line, "bytes_per_iter")
  if (bytes <= 0 || ns <= 0) {
    printf "lockd perf invalid timing record for %s\n", name > "/dev/stderr"
    exit 1
  }
  metrics[name] = ns / bytes
}
has($0, "impl", "c") && has($0, "submode", "steady_state") &&
  has($0, "selector", "contains_any_msg") &&
  has($0, "mode", "decision_only_plan") {
  record_metric("contains_any", $0)
}
has($0, "impl", "c") && has($0, "submode", "steady_state") &&
  has($0, "selector", "explicit_or_msg") &&
  has($0, "mode", "decision_only_plan") {
  record_metric("explicit_or", $0)
}
has($0, "impl", "c") && has($0, "submode", "steady_state") &&
  has($0, "mode", "mutate_file_backed_text") {
  record_metric("file_text", $0)
}
has($0, "impl", "c") && has($0, "submode", "steady_state") &&
  has($0, "mode", "mutate_file_backed_base64") {
  record_metric("file_base64", $0)
}
END {
  required[1] = "contains_any"
  required[2] = "explicit_or"
  required[3] = "file_text"
  required[4] = "file_base64"
  for (i = 1; i <= 4; ++i) {
    if (!(required[i] in metrics)) {
      printf "lockd perf missing C metric: %s\n", required[i] > "/dev/stderr"
      exit 1
    }
  }
  if (metrics["file_text"] > file_max) {
    printf "lockd text file-backed mutation ns_per_byte %.3f exceeds max %.3f\n", metrics["file_text"], file_max > "/dev/stderr"
    exit 1
  }
  if (metrics["file_base64"] > file_max) {
    printf "lockd base64 file-backed mutation ns_per_byte %.3f exceeds max %.3f\n", metrics["file_base64"], file_max > "/dev/stderr"
    exit 1
  }
  if (metrics["contains_any"] > metrics["explicit_or"] * ratio_max &&
      metrics["contains_any"] - metrics["explicit_or"] > delta_max) {
    printf "lockd contains.any ns_per_byte %.3f exceeds explicit-or %.3f by ratio max %.3f and delta max %.3f\n", metrics["contains_any"], metrics["explicit_or"], ratio_max, delta_max > "/dev/stderr"
    exit 1
  }
}
' "$log"
