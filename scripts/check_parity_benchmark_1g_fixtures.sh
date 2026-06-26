#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp="${TMPDIR:-/tmp}/liblql-benchmark-1g-fixtures-$$"

cleanup() {
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmp"

run_small_gate() {
  log=$1
  LQL_BENCH_1G_COUNT=16 \
    LQL_BENCH_1G_BLOB_BYTES=64 \
    LQL_BENCH_1G_MIN_BYTES=1 \
    LQL_BENCH_1G_LOG="$log" \
    "$root/scripts/check_parity_benchmark_1g_memory.sh" > "$tmp/small.out" 2> "$tmp/small.err"
}

run_small_gate "$tmp/bench-1g-small.jsonl"

awk '
function field_string(line, name, pattern) {
  pattern = "\"" name "\":\"[^\"]*\""
  if (match(line, pattern) == 0) {
    return ""
  }
  return substr(line, RSTART + length(name) + 4, RLENGTH - length(name) - 5)
}
{
  impl = field_string($0, "impl")
  mode = field_string($0, "mode")
  submode = field_string($0, "submode")
  key = impl SUBSEP mode SUBSEP submode
  seen[key] = 1
  if (impl == "go") {
    print "1g fixture unexpectedly emitted Go records" > "/dev/stderr"
    exit 1
  }
  if (mode != "decision_only_selector" && mode != "plus_value_source_selector") {
    printf "1g fixture emitted unexpected mode: %s\n", mode > "/dev/stderr"
    exit 1
  }
}
END {
  split("c lua", impls, " ")
  split("decision_only_selector plus_value_source_selector", modes, " ")
  split("warmup_included steady_state", submodes, " ")
  for (i in impls) {
    for (m in modes) {
      for (s in submodes) {
        key = impls[i] SUBSEP modes[m] SUBSEP submodes[s]
        if (!(key in seen)) {
          printf "1g fixture missing record: impl=%s mode=%s submode=%s\n", impls[i], modes[m], submodes[s] > "/dev/stderr"
          exit 1
        }
      }
    }
  }
}
' "$tmp/bench-1g-small.jsonl"

if LQL_BENCH_1G_COUNT=16 \
  LQL_BENCH_1G_BLOB_BYTES=64 \
  LQL_BENCH_1G_MIN_BYTES=1073741824 \
  LQL_BENCH_1G_LOG="$tmp/bench-1g-too-small.jsonl" \
  "$root/scripts/check_parity_benchmark_1g_memory.sh" > "$tmp/too-small.out" 2> "$tmp/too-small.err"; then
  printf '1g fixture size negative check unexpectedly passed\n' >&2
  exit 1
fi

if ! grep -q '1g benchmark fixture too small' "$tmp/too-small.err"; then
  printf '1g fixture size negative check did not report fixture size\n' >&2
  cat "$tmp/too-small.err" >&2
  exit 1
fi
