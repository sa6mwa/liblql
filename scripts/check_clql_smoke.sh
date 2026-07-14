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
grep 'explicitly spooled compatibility path' "$tmp.out" >/dev/null

"$clql" --version >"$tmp.out"
grep '^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$' "$tmp.out" >/dev/null

"$clql" '/status="open"' "$fixture" >"$tmp.out"
printf '%s\n%s\n' '{"status":"open","n":1}' '{"status":"open","n":3}' |
  cmp -s - "$tmp.out"

"$clql" --count '/status="open"' "$fixture" >"$tmp.out"
printf '2\n' | cmp -s - "$tmp.out"

"$clql" -c -M '/status="open"' "$fixture" >"$tmp.out"
printf '%s\n%s\n' '{"status":"open","n":1}' '{"status":"open","n":3}' |
  cmp -s - "$tmp.out"

"$clql" -O '/status="open"' '/status="closed"' "$fixture" >"$tmp.out"
printf '%s\n%s\n%s\n' \
  '{"status":"open","n":1}' \
  '{"status":"closed","n":2}' \
  '{"status":"open","n":3}' |
  cmp -s - "$tmp.out"

"$clql" '/status="open"' '/n=3' "$fixture" >"$tmp.out"
printf '%s\n' '{"status":"open","n":3}' | cmp -s - "$tmp.out"

if "$clql" -m '/status=ready' '/status="open"' "$fixture" \
  >"$tmp.out" 2>"$tmp.err"; then
  printf 'clql smoke: unsupported mutation flag unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'unsupported Go lql flag' "$tmp.err" >/dev/null

if "$clql" '/status="open"' - >"$tmp.out" 2>"$tmp.err" <<'EOF'
[{"status":"open"}]
EOF
then
  printf 'clql smoke: root array unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'JSON' "$tmp.err" >/dev/null

printf 'clql smoke: selected output, count, stdin error, spill help, and version passed\n'
