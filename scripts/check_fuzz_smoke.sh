#!/bin/sh
set -eu

binary=${LQL_JSON_FUZZ_PATH:-build/fuzz/lql_json_fuzz}
afl_showmap=${CPKT_AFL_SHOWMAP:-}
out_dir=${LQL_FUZZ_SMOKE_DIR:-build/fuzz-smoke}
corpus=${LQL_FUZZ_CORPUS:-fuzz/corpus/json}

if [ ! -x "$binary" ]; then
  printf 'fuzz smoke: missing fuzz target: %s\n' "$binary" >&2
  exit 1
fi

if [ -z "$afl_showmap" ]; then
  afl_showmap=$(scripts/cpkt-aflpp.sh discover | sed -n 's/^afl_showmap=//p')
fi
if [ ! -x "$afl_showmap" ]; then
  printf 'fuzz smoke: missing afl-showmap: %s\n' "$afl_showmap" >&2
  exit 1
fi

sh scripts/remove_path.sh "$out_dir"
mkdir -p "$out_dir"

count=0
for seed in "$corpus"/*; do
  [ -f "$seed" ] || continue
  map="$out_dir/$(basename "$seed").map"
  AFL_SKIP_CPUFREQ=1 "$afl_showmap" -q -o "$map" -- "$binary" "$seed"
  if [ ! -s "$map" ]; then
    printf 'fuzz smoke: empty coverage map for %s\n' "$seed" >&2
    exit 1
  fi
  count=$((count + 1))
done

if [ "$count" -lt 3 ]; then
  printf 'fuzz smoke: expected at least 3 seeds, saw %s\n' "$count" >&2
  exit 1
fi

unique=$(sha256sum "$out_dir"/*.map | awk '{print $1}' | sort -u | wc -l | tr -d ' ')
if [ "$unique" -lt 2 ]; then
  printf 'fuzz smoke: AFL++ did not observe distinct execution maps\n' >&2
  exit 1
fi

printf 'fuzz smoke: AFL++ observed %s seeds with %s distinct maps\n' "$count" "$unique"
