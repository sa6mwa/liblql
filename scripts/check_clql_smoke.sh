#!/bin/sh
set -eu

clql=${CLQL_PATH:-build/release/clql}
go_lql=${LQL_GO_CLI_PATH:-build/reference-lql}
fixture=${LQL_CLQL_FIXTURE:-examples/status.ndjson}
tmp=${TMPDIR:-/tmp}/liblql-clql-smoke.$$
tmpdir=${TMPDIR:-/tmp}/liblql-clql-smoke.dir.$$

remove_path() {
  path=$1
  if [ ! -e "$path" ]; then
    return 0
  fi
  if [ -d "$path" ] && [ ! -L "$path" ]; then
    find "$path" ! -type d -exec rm -- {} \;
    find "$path" -depth -type d -exec rmdir -- {} \;
  else
    rm -- "$path"
  fi
}

cleanup() {
  remove_path "$tmp.out"
  remove_path "$tmp.err"
  remove_path "$tmp.c.out"
  remove_path "$tmp.go.out"
  remove_path "$tmpdir"
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

cat "$fixture" | "$clql" '/status="open"' - >"$tmp.out"
printf '%s\n%s\n' '{"status":"open","n":1}' '{"status":"open","n":3}' |
  cmp -s - "$tmp.out"

"$clql" --count '/status="open"' "$fixture" >"$tmp.out"
printf '2\n' | cmp -s - "$tmp.out"

"$clql" -c -M '/status="open"' "$fixture" >"$tmp.out"
printf '%s\n%s\n' '{"status":"open","n":1}' '{"status":"open","n":3}' |
  cmp -s - "$tmp.out"

compare_go_lql 'clustered compact matches-only flags' \
  -cM '/status="open"' "$fixture"

"$clql" --compact=false --matches-only=false '/status="open"' "$fixture" \
  >"$tmp.out"
printf '%s\n%s\n' '{"status":"open","n":1}' '{"status":"open","n":3}' |
  cmp -s - "$tmp.out"

"$clql" -O '/status="open"' '/status="closed"' "$fixture" >"$tmp.out"
printf '%s\n%s\n%s\n' \
  '{"status":"open","n":1}' \
  '{"status":"closed","n":2}' \
  '{"status":"open","n":3}' |
  cmp -s - "$tmp.out"

"$clql" --or=false '/status="open"' '/n=3' "$fixture" >"$tmp.out"
printf '%s\n' '{"status":"open","n":3}' | cmp -s - "$tmp.out"

"$clql" '/status="open"' '/n=3' "$fixture" >"$tmp.out"
printf '%s\n' '{"status":"open","n":3}' | cmp -s - "$tmp.out"

mkdir -p "$tmpdir"
cat >"$tmpdir/input.ndjson" <<'EOF'
{"id":"a","status":"new","keep":1}
{"id":"b","status":"old","keep":2}
EOF

"$clql" -f /id '/status="new"' "$tmpdir/input.ndjson" >"$tmp.out"
printf '%s\n' '{"id":"a"}' | cmp -s - "$tmp.out"

"$clql" -f/id '/status="new"' "$tmpdir/input.ndjson" >"$tmp.out"
printf '%s\n' '{"id":"a"}' | cmp -s - "$tmp.out"

"$clql" '/status="new"' -f /id "$tmpdir/input.ndjson" >"$tmp.out"
printf '%s\n' '{"id":"a"}' | cmp -s - "$tmp.out"

printf '%s\n' '{"a":[10,20]}' >"$tmpdir/mixed-array.ndjson"
compare_go_lql 'mixed numeric projection on array' \
  -c -f /a/0 -f /a/x "$tmpdir/mixed-array.ndjson"

"$clql" -m '/status=ready' '/id="b"' "$tmpdir/input.ndjson" >"$tmp.out"
printf '%s\n%s\n' \
  '{"id":"a","status":"new","keep":1}' \
  '{"id":"b","status":"ready","keep":2}' |
  cmp -s - "$tmp.out"

"$clql" -m/status=ready '/id="b"' "$tmpdir/input.ndjson" >"$tmp.out"
printf '%s\n%s\n' \
  '{"id":"a","status":"new","keep":1}' \
  '{"id":"b","status":"ready","keep":2}' |
  cmp -s - "$tmp.out"

"$clql" '/id="b"' -m '/status=ready' "$tmpdir/input.ndjson" >"$tmp.out"
printf '%s\n%s\n' \
  '{"id":"a","status":"new","keep":1}' \
  '{"id":"b","status":"ready","keep":2}' |
  cmp -s - "$tmp.out"

"$clql" -M -m '/status=ready' '/id="b"' "$tmpdir/input.ndjson" >"$tmp.out"
printf '%s\n' '{"id":"b","status":"ready","keep":2}' | cmp -s - "$tmp.out"

"$clql" --count -m '/status=ready' '/id="b"' "$tmpdir/input.ndjson" >"$tmp.out"
printf '1\n' | cmp -s - "$tmp.out"

"$clql" --count -m '/status=ready' "$tmpdir/input.ndjson" \
  "$tmpdir/input.ndjson" >"$tmp.out"
printf '4\n' | cmp -s - "$tmp.out"

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

printf 'hello file' >"$tmpdir/ lql spaced value "
compare_go_lql 'quoted textfile mutation path with spaces' \
  -c -F -M -m "textfile:/payload=\"$tmpdir/ lql spaced value \"" \
  '/id="a"' "$tmpdir/input.ndjson"

printf 'a\000b' >"$tmpdir/blob-nul.txt"
if "$clql" -F -M -m "textfile:/payload=$tmpdir/blob-nul.txt" \
  '/id="a"' "$tmpdir/input.ndjson" >"$tmp.out" 2>"$tmp.err"; then
  printf 'clql smoke: NUL textfile mutation unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'contains NUL byte' "$tmp.err" >/dev/null
test ! -s "$tmp.out"

printf '\000\001\002\003hello\377' >"$tmpdir/blob.bin"
compare_go_lql 'base64file mutation values' \
  -c -F -M -m "base64file:/payload=$tmpdir/blob.bin" \
  '/id="a"' "$tmpdir/input.ndjson"
compare_go_lql 'auto file mutation values' \
  -c -F -M -m "file:/payload=$tmpdir/blob.bin" \
  '/id="a"' "$tmpdir/input.ndjson"

cp "$tmpdir/input.ndjson" "$tmpdir/inline.ndjson"
chmod 2755 "$tmpdir/inline.ndjson"
inline_meta_before=$(stat -c '%u:%g:%a' "$tmpdir/inline.ndjson")
"$clql" -i -m '/status=ready' '/id="a"' "$tmpdir/inline.ndjson"
inline_meta_after=$(stat -c '%u:%g:%a' "$tmpdir/inline.ndjson")
if [ "$inline_meta_before" != "$inline_meta_after" ]; then
  printf 'clql smoke: inline metadata changed: %s -> %s\n' \
    "$inline_meta_before" "$inline_meta_after" >&2
  exit 1
fi
if python3 - "$tmpdir/inline.ndjson" <<'PY'
import os, sys
path = sys.argv[1]
try:
    os.setxattr(path, b"user.lqltest", b"kept")
except OSError:
    sys.exit(77)
sys.exit(0)
PY
then
  "$clql" -i -m '/status=ready2' '/id="a"' "$tmpdir/inline.ndjson"
  python3 - "$tmpdir/inline.ndjson" <<'PY'
import os, sys
path = sys.argv[1]
if os.getxattr(path, b"user.lqltest") != b"kept":
    raise SystemExit("xattr changed")
PY
fi
printf '%s\n%s\n' \
  '{"id":"a","status":"ready2","keep":1}' \
  '{"id":"b","status":"old","keep":2}' |
  cmp -s - "$tmpdir/inline.ndjson"

name_max=$(getconf NAME_MAX "$tmpdir" 2>/dev/null || printf '255\n')
if [ "$name_max" -ge 32 ]; then
  long_name=$(python3 - "$name_max" <<'PY'
import sys
print("n" * int(sys.argv[1]))
PY
)
  cp "$tmpdir/input.ndjson" "$tmpdir/$long_name"
  "$clql" -i -m '/status=maxname' '/id="a"' "$tmpdir/$long_name"
  printf '%s\n%s\n' \
    '{"id":"a","status":"maxname","keep":1}' \
    '{"id":"b","status":"old","keep":2}' |
    cmp -s - "$tmpdir/$long_name"
fi

cp "$tmpdir/input.ndjson" "$tmpdir/inline-count.ndjson"
if "$clql" --count -i -m '/status=ready' '/id="a"' \
  "$tmpdir/inline-count.ndjson" >"$tmp.out" 2>"$tmp.err"; then
  printf 'clql smoke: inline count mutation unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'inline mutation cannot be combined with --count' "$tmp.err" >/dev/null
cmp -s "$tmpdir/input.ndjson" "$tmpdir/inline-count.ndjson"

printf '   \n' >"$tmpdir/inline-whitespace.ndjson"
if "$clql" -i -m '/status=ready' "$tmpdir/inline-whitespace.ndjson" \
  >"$tmp.out" 2>"$tmp.err"; then
  printf 'clql smoke: inline whitespace-only mutation unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'no JSON input' "$tmp.err" >/dev/null
printf '   \n' | cmp -s - "$tmpdir/inline-whitespace.ndjson"

cp "$tmpdir/input.ndjson" "$tmpdir/inline-target.ndjson"
ln -s inline-target.ndjson "$tmpdir/inline-link.ndjson"
if "$clql" -i -m '/status=ready' "$tmpdir/inline-link.ndjson" \
  >"$tmp.out" 2>"$tmp.err"; then
  printf 'clql smoke: inline symlink mutation unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'inline mode does not rewrite symlink paths' "$tmp.err" >/dev/null
test -L "$tmpdir/inline-link.ndjson"
cmp -s "$tmpdir/input.ndjson" "$tmpdir/inline-target.ndjson"

mkfifo "$tmpdir/inline-fifo.ndjson"
if "$clql" -i -m '/status=ready' "$tmpdir/inline-fifo.ndjson" \
  >"$tmp.out" 2>"$tmp.err"; then
  printf 'clql smoke: inline FIFO mutation unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'inline mode requires a single JSON file' "$tmp.err" >/dev/null
test -p "$tmpdir/inline-fifo.ndjson"

cp "$tmpdir/input.ndjson" "$tmpdir/inline-readonly.ndjson"
if [ "$(id -u)" != "0" ]; then
  chmod 500 "$tmpdir"
  if "$clql" -i -m '/status=ready' "$tmpdir/inline-readonly.ndjson" \
    >"$tmp.out" 2>"$tmp.err"; then
    chmod 700 "$tmpdir"
    printf 'clql smoke: inline unexpectedly succeeded in read-only directory\n' >&2
    exit 1
  fi
  chmod 700 "$tmpdir"
fi
cmp -s "$tmpdir/input.ndjson" "$tmpdir/inline-readonly.ndjson"

if "$clql" -m 'file:/payload=blob.txt' "$tmpdir/input.ndjson" \
  >"$tmp.out" 2>"$tmp.err"; then
  printf 'clql smoke: file-backed mutation unexpectedly succeeded without -F\n' >&2
  exit 1
fi
grep 'file-backed mutations are disabled' "$tmp.err" >/dev/null

if printf '{"a":"x"}\n' | "$clql" -m '/a=+1' >"$tmp.out" 2>"$tmp.err"; then
  printf 'clql smoke: invalid increment mutation unexpectedly succeeded\n' >&2
  exit 1
fi
test ! -s "$tmp.out"
grep 'increment target number is invalid' "$tmp.err" >/dev/null

if "$clql" -t jq '/status="open"' "$fixture" >"$tmp.out" 2>"$tmp.err"; then
  printf 'clql smoke: theme flag unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'prettyx is not linked' "$tmp.err" >/dev/null

if "$clql" -hZ >"$tmp.out" 2>"$tmp.err"; then
  printf 'clql smoke: invalid help cluster unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'unknown flag: -hZ' "$tmp.err" >/dev/null

if "$clql" -vZ >"$tmp.out" 2>"$tmp.err"; then
  printf 'clql smoke: invalid version cluster unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'unknown flag: -vZ' "$tmp.err" >/dev/null

"$clql" -vh >"$tmp.out" 2>"$tmp.err"
grep 'usage: clql' "$tmp.out" >/dev/null
test ! -s "$tmp.err"

if "$clql" '/status="open"' - >"$tmp.out" 2>"$tmp.err" <<'EOF'
[{"status":"open"}]
EOF
then
  printf 'clql smoke: root array unexpectedly succeeded\n' >&2
  exit 1
fi
grep 'JSON' "$tmp.err" >/dev/null

printf 'clql smoke: selection, projection, mutation, inline, file-backed values, Go CLI parity, stdin error, spill help, and version passed\n'
