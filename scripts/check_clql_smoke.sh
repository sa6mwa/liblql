#!/bin/sh
set -eu

clql=${1:?usage: check_clql_smoke.sh CLQL VERSION}
version=${2:?usage: check_clql_smoke.sh CLQL VERSION}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$script_dir/.." && pwd)
help_corpus=$root/tests/fixtures/clql_help_corpus.ndjson
tmpdir=
cleanup() {
  if [ -n "$tmpdir" ] && [ -d "$tmpdir" ]; then
    rm -rf "$tmpdir"
  fi
}
trap cleanup EXIT HUP INT TERM

version_out=$("$clql" --version)
if [ "$version_out" != "clql $version" ]; then
  printf 'clql smoke: unexpected --version output: %s\n' "$version_out" >&2
  printf 'clql smoke: expected: clql %s\n' "$version" >&2
  exit 1
fi
short_version_out=$("$clql" -v)
if [ "$short_version_out" != "clql $version" ]; then
  printf 'clql smoke: unexpected -v output: %s\n' "$short_version_out" >&2
  printf 'clql smoke: expected: clql %s\n' "$version" >&2
  exit 1
fi

help_out=$("$clql" --help)
short_help_out=$("$clql" -h)
if [ "$short_help_out" != "$help_out" ]; then
  printf 'clql smoke: -h and --help output differ\n' >&2
  exit 1
fi

for needle in \
  "clql - query, project, and mutate JSON with LQL selectors" \
  "Usage:" \
  "Input:" \
  "Selection:" \
  "Projection:" \
  "Mutation:" \
  "Commands:" \
  "Selector examples (shorthand):" \
  "Selector examples (full LQL):" \
  "Projection and mutation examples:" \
  "Notes:" \
  "clql [options] <selector> [file]" \
  "-O, --or[=bool]" \
  "-M, --matches-only[=bool]" \
  "-c, --compact[=bool]" \
  "-f, --field <path>" \
  "-m, --mutate <expr>" \
  "-i, --inline[=bool]" \
  "-w, --write[=bool]" \
  "-F, --enable-file-mutations[=bool]" \
  "clql --help" \
  "clql --version" \
  "clql '/status=\"open\"' data.json" \
  "clql '/status!=closed' data.json" \
  "clql '/progress>=50' data.json" \
  "clql '/timestamp>=\"2025-01-01T00:00:00Z\"' data.json" \
  "clql '/devices/0/status=\"online\"' data.json" \
  "clql '/labels/*=\"production\"' data.json" \
  "clql '/items[]/sku=\"ABC-123\"' data.json" \
  "clql '/items/**/sku=\"ABC-123\"' data.json" \
  "clql '/items/.../sku=\"ABC-123\"' data.json" \
  "clql 'eq{field=/status,value=open}' data.json" \
  "clql 'contains{field=/msg,value=timeout,ic=t}' data.json" \
  "clql 'contains{field=/msg,any=timeout|degraded}' data.json" \
  "clql 'icontains{field=/msg,value=timeout}' data.json" \
  "clql 'icontains{field=/service,a=AUTH|EDGE}' data.json" \
  "clql 'iprefix{field=/service,value=auth}' data.json" \
  "clql 'date{field=/timestamp,after=2025-01-01,before=2025-02-01}' data.json" \
  "clql 'date{f=/timestamp,since=yesterday}' data.json" \
  "clql 'and.eq{field=/status,value=open},and.range{field=/progress,gte=50}' data.json" \
  "clql 'or.eq{field=/region,value=us},or.eq{field=/region,value=eu}' data.json" \
  "clql 'not.eq{field=/state,value=disabled}' data.json" \
  "clql 'exists{/metadata/etag}' data.json" \
  "clql -c -f /owner/name '/priority>=3' < data.json" \
  "clql -m '/status=\"closed\"' -i '/id=\"42\"' data.json" \
  "clql -O '/status=\"open\"' '/status=\"queued\"' data.json"
do
  case "$help_out" in
    *"$needle"*) ;;
    *)
      printf 'clql smoke: --help output missing: %s\n' "$needle" >&2
      exit 1
      ;;
  esac
done

expect_example_match() {
  selector=$1
  input_file=$2
  expected=$3
  actual=$("$clql" -c "$selector" "$input_file")
  if [ "$actual" != "$expected" ]; then
    printf 'clql smoke: help example selector failed: %s\n' "$selector" >&2
    printf 'clql smoke: actual: %s\n' "$actual" >&2
    printf 'clql smoke: expected: %s\n' "$expected" >&2
    exit 1
  fi
}

primary_json=$(sed -n '1p' "$help_corpus")
future_json=$(sed -n '2p' "$help_corpus")
closed_json=$(sed -n '3p' "$help_corpus")
queued_json=$(sed -n '4p' "$help_corpus")
primary_closed_json=$(printf '%s' "$primary_json" |
  sed 's/"status":"open"/"status":"closed"/')
nonclosed_json=$(printf '%s\n%s\n%s' "$primary_json" "$future_json" "$queued_json")
date_gte_json=$(printf '%s\n%s\n%s' "$primary_json" "$future_json" "$queued_json")
timeout_json=$(printf '%s\n%s' "$primary_json" "$closed_json")
service_any_json=$(printf '%s\n%s' "$primary_json" "$queued_json")
region_or_json=$(printf '%s\n%s' "$primary_json" "$queued_json")
not_disabled_json=$(printf '%s\n%s' "$primary_json" "$queued_json")

expect_example_match '/status="open"' \
  "$help_corpus" \
  "$primary_json"
expect_example_match '/status!=closed' \
  "$help_corpus" \
  "$nonclosed_json"
expect_example_match '/progress>=50' \
  "$help_corpus" \
  "$primary_json"
expect_example_match '/timestamp>="2025-01-01T00:00:00Z"' \
  "$help_corpus" \
  "$date_gte_json"
expect_example_match '/devices/0/status="online"' \
  "$help_corpus" \
  "$primary_json"
expect_example_match '/labels/*="production"' \
  "$help_corpus" \
  "$primary_json"
expect_example_match '/items[]/sku="ABC-123"' \
  "$help_corpus" \
  "$primary_json"
expect_example_match '/items/**/sku="ABC-123"' \
  "$help_corpus" \
  "$primary_json"
expect_example_match '/items/.../sku="ABC-123"' \
  "$help_corpus" \
  "$primary_json"
expect_example_match 'eq{field=/status,value=open}' \
  "$help_corpus" \
  "$primary_json"
expect_example_match 'contains{field=/msg,value=timeout,ic=t}' \
  "$help_corpus" \
  "$timeout_json"
expect_example_match 'contains{field=/msg,any=timeout|degraded}' \
  "$help_corpus" \
  "$timeout_json"
expect_example_match 'icontains{field=/msg,value=timeout}' \
  "$help_corpus" \
  "$timeout_json"
expect_example_match 'icontains{field=/service,a=AUTH|EDGE}' \
  "$help_corpus" \
  "$service_any_json"
expect_example_match 'iprefix{field=/service,value=auth}' \
  "$help_corpus" \
  "$primary_json"
expect_example_match 'date{field=/timestamp,after=2025-01-01,before=2025-02-01}' \
  "$help_corpus" \
  "$primary_json"
expect_example_match 'date{f=/timestamp,since=yesterday}' \
  "$help_corpus" \
  "$future_json"
expect_example_match 'and.eq{field=/status,value=open},and.range{field=/progress,gte=50}' \
  "$help_corpus" \
  "$primary_json"
expect_example_match 'or.eq{field=/region,value=us},or.eq{field=/region,value=eu}' \
  "$help_corpus" \
  "$region_or_json"
expect_example_match 'not.eq{field=/state,value=disabled}' \
  "$help_corpus" \
  "$not_disabled_json"
expect_example_match 'exists{/metadata/etag}' \
  "$help_corpus" \
  "$primary_json"

projection_out=$("$clql" -c -f /owner/name '/priority>=3' "$help_corpus")
if [ "$projection_out" != '{"owner":{"name":"alice"}}' ]; then
  printf 'clql smoke: projection help example failed: %s\n' "$projection_out" >&2
  exit 1
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/clql-smoke.XXXXXX")
or_input=$tmpdir/or.json
inline_input=$tmpdir/inline.json
cp "$help_corpus" "$or_input"
or_out=$("$clql" -c -O '/status="open"' '/status="queued"' "$or_input")
if [ "$or_out" != "$region_or_json" ]; then
  printf 'clql smoke: OR help example failed: %s\n' "$or_out" >&2
  exit 1
fi

printf '%s' "$primary_json" >"$inline_input"
"$clql" -m '/status="closed"' -i '/id="42"' "$inline_input"
inline_out=$(cat "$inline_input")
if [ "$inline_out" != "$primary_closed_json" ]; then
  printf 'clql smoke: inline mutation help example failed: %s\n' \
    "$inline_out" >&2
  exit 1
fi

for theme_args in \
  '--theme default' \
  '--theme=default' \
  '-t default' \
  '-tdefault' \
  '-t=default' \
  '-ctdefault' \
  '-ct=default'
do
  # shellcheck disable=SC2086
  if theme_out=$(printf '%s' '{"status":"open"}' | "$clql" $theme_args '/status="open"' 2>&1); then
    printf 'clql smoke: unsupported theme flag unexpectedly succeeded for %s: %s\n' \
      "$theme_args" "$theme_out" >&2
    exit 1
  fi
  case "$theme_out" in
    *"unknown option"*) ;;
    *)
      printf 'clql smoke: unsupported theme flag diagnostic mismatch for %s: %s\n' \
        "$theme_args" "$theme_out" >&2
      exit 1
      ;;
  esac
done

if root_array_out=$(printf '%s\n' '[{"status":"open"}]' |
  "$clql" '/status="open"' 2>&1); then
  printf 'clql smoke: root array stdin unexpectedly succeeded: %s\n' \
    "$root_array_out" >&2
  exit 1
fi
case "$root_array_out" in
  *"root arrays are not valid NDJSON candidate streams"*) ;;
  *)
    printf 'clql smoke: root array stdin diagnostic mismatch: %s\n' \
      "$root_array_out" >&2
    exit 1
    ;;
esac

if "$clql" --help | grep -E -- '(^|[[:space:]])(-t|--theme)([[:space:],=]|$)' >/dev/null; then
  printf 'clql smoke: help still advertises unsupported theme flag\n' >&2
  exit 1
fi
if "$clql" --help | grep -F 'colorized JSON output is not implemented' >/dev/null; then
  printf 'clql smoke: help still documents colorized JSON caveat\n' >&2
  exit 1
fi
