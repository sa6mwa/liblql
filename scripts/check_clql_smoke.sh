#!/bin/sh
set -eu

clql=${CLQL_PATH:-build/release/clql}
go_lql=${LQL_GO_CLI_PATH:-build/reference-lql}
fixture=${LQL_CLQL_FIXTURE:-examples/status.ndjson}
tmp=${TMPDIR:-/tmp}/liblql-clql-smoke.$$
tmpdir=${TMPDIR:-/tmp}/liblql-clql-smoke.dir.$$

cleanup() {
  rm -f "$tmp.out" "$tmp.err" "$tmp.c.out" "$tmp.go.out"
  rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

if [ ! -x "$clql" ]; then
  printf 'clql smoke: missing clql binary: %s\n' "$clql" >&2
  exit 1
fi
if [ ! -x "$go_lql" ]; then
  printf 'clql smoke: missing Go lql reference binary: %s\n' "$go_lql" >&2
  exit 1
fi

compare_go_lql() {
  label=$1
  shift
  "$clql" "$@" >"$tmp.c.out"
  "$go_lql" "$@" >"$tmp.go.out"
  if ! cmp -s "$tmp.go.out" "$tmp.c.out"; then
    printf 'clql smoke: Go lql CLI parity mismatch for %s\n' "$label" >&2
    diff -u "$tmp.go.out" "$tmp.c.out" >&2 || true
    exit 1
  fi
}

"$clql" --help >"$tmp.out"
grep 'usage: clql' "$tmp.out" >/dev/null
grep 'Selector examples (shorthand):' "$tmp.out" >/dev/null
grep 'Selector examples (full LQL):' "$tmp.out" >/dev/null
grep 'Invocation examples:' "$tmp.out" >/dev/null
grep 'File-backed mutation example:' "$tmp.out" >/dev/null
awk '
  /^Selector examples \(shorthand\):$/ { section = "shorthand"; next }
  /^Selector examples \(full LQL\):$/ { section = "full"; next }
  /^Invocation examples:$/ { section = ""; next }
  section == "shorthand" && /^  [^ ]/ { shorthand++ }
  section == "full" && /^  [^ ]/ { full++ }
  END { exit !(shorthand >= 9 && full >= 12) }
' "$tmp.out"
grep 'clql -O' "$tmp.out" >/dev/null
grep 'clql --count' "$tmp.out" >/dev/null
grep 'explicitly spooled path' "$tmp.out" >/dev/null

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

mkdir -p "$tmpdir"
cat >"$tmpdir/input.ndjson" <<'EOF'
{"id":"a","status":"new","keep":1}
{"id":"b","status":"old","keep":2}
EOF

"$clql" -f /id '/status="new"' "$tmpdir/input.ndjson" >"$tmp.out"
printf '%s\n' '{"id":"a"}' | cmp -s - "$tmp.out"

"$clql" -m '/status=ready' '/id="b"' "$tmpdir/input.ndjson" >"$tmp.out"
printf '%s\n%s\n' \
  '{"id":"a","status":"new","keep":1}' \
  '{"id":"b","status":"ready","keep":2}' |
  cmp -s - "$tmp.out"

"$clql" -M -m '/status=ready' '/id="b"' "$tmpdir/input.ndjson" >"$tmp.out"
printf '%s\n' '{"id":"b","status":"ready","keep":2}' | cmp -s - "$tmp.out"

"$clql" -f /id -m '/status=ready' '/id="b"' "$tmpdir/input.ndjson" >"$tmp.out"
printf '%s\n%s\n' '{"id":"a"}' '{"id":"b","status":"ready"}' |
  cmp -s - "$tmp.out"

printf 'hello file' >"$tmpdir/blob.txt"
"$clql" -F -m "textfile:/payload=$tmpdir/blob.txt" \
  "$tmpdir/input.ndjson" >"$tmp.out"
printf '%s\n%s\n' \
  '{"id":"a","status":"new","keep":1,"payload":"hello file"}' \
  '{"id":"b","status":"old","keep":2,"payload":"hello file"}' |
  cmp -s - "$tmp.out"

compare_go_lql 'textfile mutation values' \
  -c -F -M -m "textfile:/payload=$tmpdir/blob.txt" \
  '/id="a"' "$tmpdir/input.ndjson"

printf '日本語 😀 こんにちは' >"$tmpdir/blob-utf8.txt"
compare_go_lql 'UTF-8 textfile mutation values' \
  -c -F -M -m "textfile:/payload=$tmpdir/blob-utf8.txt" \
  '/id="a"' "$tmpdir/input.ndjson"

printf '\000\001\002\003hello\377' >"$tmpdir/blob.bin"
compare_go_lql 'base64file mutation values' \
  -c -F -M -m "base64file:/payload=$tmpdir/blob.bin" \
  '/id="a"' "$tmpdir/input.ndjson"
compare_go_lql 'auto file mutation values' \
  -c -F -M -m "file:/payload=$tmpdir/blob.bin" \
  '/id="a"' "$tmpdir/input.ndjson"

cp "$tmpdir/input.ndjson" "$tmpdir/inline.ndjson"
"$clql" -i -m '/status=ready' '/id="a"' "$tmpdir/inline.ndjson"
printf '%s\n%s\n' \
  '{"id":"a","status":"ready","keep":1}' \
  '{"id":"b","status":"old","keep":2}' |
  cmp -s - "$tmpdir/inline.ndjson"

if "$clql" -m 'file:/payload=blob.txt' "$tmpdir/input.ndjson" \
  >"$tmp.out" 2>"$tmp.err"; then
  printf 'clql smoke: file-backed mutation unexpectedly succeeded without -F\n' >&2
  exit 1
fi
grep 'file-backed mutations are disabled' "$tmp.err" >/dev/null

if "$clql" -t jq '/status="open"' "$fixture" >"$tmp.out" 2>"$tmp.err"; then
  printf 'clql smoke: theme flag unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'prettyx is not linked' "$tmp.err" >/dev/null

if "$clql" '/status="open"' - >"$tmp.out" 2>"$tmp.err" <<'EOF'
[{"status":"open"}]
EOF
then
  printf 'clql smoke: root array unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'JSON' "$tmp.err" >/dev/null

printf 'clql smoke: selection, projection, mutation, inline, file-backed values, Go CLI parity, stdin error, spill help, and version passed\n'
