#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
log=${LQL_BENCH_1G_LOG:-$root/build/bench-1g-check.jsonl}
min_bytes=${LQL_BENCH_1G_MIN_BYTES:-1073741824}
count=${LQL_BENCH_1G_COUNT:-262144}
blob_bytes=${LQL_BENCH_1G_BLOB_BYTES:-4096}
expected_open=$(((count + 2) / 4))

mkdir -p "$root/build"
printf 'benchmark 1g memory check: generating and scanning C/Lua focused profile\n' >&2
LQL_BENCH_SUITE=memory \
  LQL_BENCH_MODE_PROFILE=one-gig \
  LQL_BENCH_REQUIRE_LUA_RSS=1 \
  LQL_BENCH_NDJSON_COUNT="$count" \
  LQL_BENCH_RECORD_BLOB_BYTES="$blob_bytes" \
  "$root/scripts/run_parity_benchmarks.sh" --impl c,lua --format json --check --require c,lua > "$log"

printf 'benchmark 1g memory check: validating RSS/time ceilings\n' >&2
LQL_BENCH_REQUIRE_LUA_RSS=1 "$root/scripts/check_parity_benchmark_memory.sh" "$log"

printf 'benchmark 1g memory check: validating generated count invariants\n' >&2
awk -v min_bytes="$min_bytes" \
  -v count="$count" \
  -v expected_open="$expected_open" '
function field_string(line, name, pattern) {
  pattern = "\"" name "\":\"[^\"]*\""
  if (match(line, pattern) == 0) {
    return ""
  }
  return substr(line, RSTART + length(name) + 4, RLENGTH - length(name) - 5)
}
function field_number(line, name, pattern, value) {
  pattern = "\"" name "\":[0-9][0-9]*"
  if (match(line, pattern) == 0) {
    return -1
  }
  value = substr(line, RSTART + length(name) + 3, RLENGTH - length(name) - 3)
  return value + 0
}
{
  impl = field_string($0, "impl")
  selector = field_string($0, "selector")
  mode = field_string($0, "mode")
  submode = field_string($0, "submode")
  payload_source = field_string($0, "payload_source_type")
  bytes = field_number($0, "bytes_per_iter")
  candidates = field_number($0, "candidates")
  matches = field_number($0, "matches")
  payloads = field_number($0, "payloads")
  key = impl SUBSEP selector SUBSEP mode SUBSEP submode
  seen[key] = 1
  if (bytes > max_bytes) {
    max_bytes = bytes
  }
  if (selector != "eq_status_open") {
    printf "1g benchmark unexpected selector: %s\n", selector > "/dev/stderr"
    exit 1
  }
  if (candidates != count) {
    printf "1g benchmark candidate mismatch: impl=%s mode=%s submode=%s got=%d want=%d\n", impl, mode, submode, candidates, count > "/dev/stderr"
    exit 1
  }
  if (matches != expected_open) {
    printf "1g benchmark match mismatch: impl=%s mode=%s submode=%s got=%d want=%d\n", impl, mode, submode, matches, expected_open > "/dev/stderr"
    exit 1
  }
  if (mode == "decision_only_selector" && payloads != 0) {
    printf "1g benchmark decision-only payload mismatch: impl=%s submode=%s payloads=%d\n", impl, submode, payloads > "/dev/stderr"
    exit 1
  }
  if ((mode == "plus_value_selector" || mode == "plus_value_source_selector") && payloads != expected_open) {
    printf "1g benchmark plus-value payload mismatch: impl=%s submode=%s got=%d want=%d\n", impl, submode, payloads, expected_open > "/dev/stderr"
    exit 1
  }
  if (mode == "decision_only_selector" && payload_source != "none") {
    printf "1g benchmark decision-only payload source mismatch: impl=%s submode=%s got=%s\n", impl, submode, payload_source > "/dev/stderr"
    exit 1
  }
  if (mode == "plus_value_source_selector" && payload_source != "spooled") {
    printf "1g benchmark callback-source payload source mismatch: impl=%s submode=%s got=%s\n", impl, submode, payload_source > "/dev/stderr"
    exit 1
  }
  if (impl == "c" && mode == "plus_value_selector" && payload_source != "seekable_range") {
    printf "1g benchmark C seekable payload source mismatch: submode=%s got=%s\n", submode, payload_source > "/dev/stderr"
    exit 1
  }
  if (impl == "lua" && mode == "plus_value_selector" && payload_source != "lua_liblql") {
    printf "1g benchmark Lua payload source mismatch: submode=%s got=%s\n", submode, payload_source > "/dev/stderr"
    exit 1
  }
}
END {
  if (NR == 0) {
    print "1g benchmark emitted no records" > "/dev/stderr"
    exit 1
  }
  if (max_bytes < min_bytes) {
    printf "1g benchmark fixture too small: got=%d want-at-least=%d\n", max_bytes, min_bytes > "/dev/stderr"
    exit 1
  }
  split("c lua", impls, " ")
  split("decision_only_selector plus_value_selector plus_value_source_selector", modes, " ")
  split("warmup_included steady_state", submodes, " ")
  for (i in impls) {
    for (m in modes) {
      for (s in submodes) {
        key = impls[i] SUBSEP "eq_status_open" SUBSEP modes[m] SUBSEP submodes[s]
        if (!(key in seen)) {
          printf "1g benchmark missing record: impl=%s selector=eq_status_open mode=%s submode=%s\n", impls[i], modes[m], submodes[s] > "/dev/stderr"
          exit 1
        }
      }
    }
  }
}
' "$log"

printf 'benchmark 1g memory check: wrote %s\n' "$log"
