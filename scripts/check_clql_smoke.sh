#!/bin/sh
set -eu

clql=${CLQL_PATH:-build/release/clql}
fixture=${LQL_CLQL_FIXTURE:-examples/status.ndjson}
tmp=${TMPDIR:-/tmp}/liblql-clql-smoke.$$

cleanup() {
  rm -f "$tmp.out" "$tmp.err"
}
trap cleanup EXIT HUP INT TERM

if [ ! -x "$clql" ]; then
  printf 'clql smoke: missing clql binary: %s\n' "$clql" >&2
  exit 1
fi

"$clql" --help >"$tmp.out"
grep 'usage: clql' "$tmp.out" >/dev/null

"$clql" --version >"$tmp.out"
grep '^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$' "$tmp.out" >/dev/null

"$clql" '/status="open"' "$fixture" >"$tmp.out"
printf '%s\n%s\n' '{"status":"open","n":1}' '{"status":"open","n":3}' |
  cmp -s - "$tmp.out"

"$clql" --count '/status="open"' "$fixture" >"$tmp.out"
printf '2\n' | cmp -s - "$tmp.out"

if "$clql" '/status="open"' - >"$tmp.out" 2>"$tmp.err" <<'EOF'
[{"status":"open"}]
EOF
then
  printf 'clql smoke: root array unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'JSON' "$tmp.err" >/dev/null

printf 'clql smoke: selected output, count, stdin error, help, and version passed\n'
