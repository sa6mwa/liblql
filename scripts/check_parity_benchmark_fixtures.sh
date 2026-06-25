#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp="${TMPDIR:-/tmp}/liblql-benchmark-fixtures-$$"

cleanup() {
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmp/one" "$tmp/two"

generate() {
  dir=$1
  log=$2
  LQL_BENCH_FIXTURE_DIR="$dir" \
    LQL_BENCH_SUITE=smoke \
    "$root/scripts/run_parity_benchmarks.sh" --impl lua --format json > "$log"
}

manifest() {
  dir=$1
  find "$dir" -maxdepth 1 -type f \
    \( -name '*.json' -o -name '*.jsonl' \) \
    -exec sh -c '
      for path do
        if command -v sha256sum >/dev/null 2>&1; then
          digest=$(sha256sum "$path" | awk "{print \$1}")
        else
          digest=$(shasum -a 256 "$path" | awk "{print \$1}")
        fi
        printf "%s  %s\n" "$digest" "$(basename "$path")"
      done
    ' sh {} + | sort
}

normalize_cases() {
  dir=$1
  awk '{
    sub(".*/", "", $2)
    print
  }' "$dir/cases.tsv"
}

generate "$tmp/one" "$tmp/one.jsonl"
generate "$tmp/two" "$tmp/two.jsonl"

manifest "$tmp/one" > "$tmp/one.manifest"
manifest "$tmp/two" > "$tmp/two.manifest"
normalize_cases "$tmp/one" > "$tmp/one.cases"
normalize_cases "$tmp/two" > "$tmp/two.cases"

if ! cmp -s "$tmp/one.manifest" "$tmp/two.manifest"; then
  printf 'benchmark fixture generation is not deterministic\n' >&2
  diff -u "$tmp/one.manifest" "$tmp/two.manifest" >&2 || true
  exit 1
fi

if ! cmp -s "$tmp/one.cases" "$tmp/two.cases"; then
  printf 'benchmark case matrix generation is not deterministic\n' >&2
  diff -u "$tmp/one.cases" "$tmp/two.cases" >&2 || true
  exit 1
fi
