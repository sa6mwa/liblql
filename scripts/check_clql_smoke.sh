#!/bin/sh
set -eu

clql=${1:?usage: check_clql_smoke.sh CLQL VERSION}
version=${2:?usage: check_clql_smoke.sh CLQL VERSION}
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
  "Compatibility:" \
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
  "-t, --theme <name>" \
  "colorized JSON output is not implemented" \
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
  input=$2
  expected=$3
  actual=$(printf '%s' "$input" | "$clql" -c "$selector")
  if [ "$actual" != "$expected" ]; then
    printf 'clql smoke: help example selector failed: %s\n' "$selector" >&2
    printf 'clql smoke: actual: %s\n' "$actual" >&2
    printf 'clql smoke: expected: %s\n' "$expected" >&2
    exit 1
  fi
}

complex_json='{"id":"42","status":"open","state":"enabled","progress":75,"priority":3,"timestamp":"2025-01-15T12:00:00Z","region":"eu","msg":"Timeout degraded while reading","service":"AUTH-edge","owner":{"name":"alice","team":"platform"},"devices":[{"status":"online","id":"pos-1"},{"status":"offline","id":"pos-2"}],"labels":{"env":"production","tier":"edge"},"items":[{"sku":"ABC-123","price":125,"nested":{"sku":"ABC-123"}},{"sku":"ZZZ-999","price":5}],"metadata":{"etag":"abc","trace":{"id":"t1"}}}'
complex_future_json='{"id":"42","status":"open","state":"enabled","progress":75,"priority":3,"timestamp":"2999-01-01T00:00:00Z","region":"eu","msg":"Timeout degraded while reading","service":"AUTH-edge","owner":{"name":"alice","team":"platform"},"devices":[{"status":"online","id":"pos-1"},{"status":"offline","id":"pos-2"}],"labels":{"env":"production","tier":"edge"},"items":[{"sku":"ABC-123","price":125,"nested":{"sku":"ABC-123"}},{"sku":"ZZZ-999","price":5}],"metadata":{"etag":"abc","trace":{"id":"t1"}}}'
complex_mutated_json='{"id":"42","status":"closed","state":"enabled","progress":75,"priority":3,"timestamp":"2025-01-15T12:00:00Z","region":"eu","msg":"Timeout degraded while reading","service":"AUTH-edge","owner":{"name":"alice","team":"platform"},"devices":[{"status":"online","id":"pos-1"},{"status":"offline","id":"pos-2"}],"labels":{"env":"production","tier":"edge"},"items":[{"sku":"ABC-123","price":125,"nested":{"sku":"ABC-123"}},{"sku":"ZZZ-999","price":5}],"metadata":{"etag":"abc","trace":{"id":"t1"}}}'

expect_example_match '/status="open"' \
  "$complex_json" \
  "$complex_json"
expect_example_match '/status!=closed' \
  "$complex_json" \
  "$complex_json"
expect_example_match '/progress>=50' \
  "$complex_json" \
  "$complex_json"
expect_example_match '/timestamp>="2025-01-01T00:00:00Z"' \
  "$complex_json" \
  "$complex_json"
expect_example_match '/devices/0/status="online"' \
  "$complex_json" \
  "$complex_json"
expect_example_match '/labels/*="production"' \
  "$complex_json" \
  "$complex_json"
expect_example_match '/items[]/sku="ABC-123"' \
  "$complex_json" \
  "$complex_json"
expect_example_match '/items/**/sku="ABC-123"' \
  "$complex_json" \
  "$complex_json"
expect_example_match '/items/.../sku="ABC-123"' \
  "$complex_json" \
  "$complex_json"
expect_example_match 'eq{field=/status,value=open}' \
  "$complex_json" \
  "$complex_json"
expect_example_match 'contains{field=/msg,value=timeout,ic=t}' \
  "$complex_json" \
  "$complex_json"
expect_example_match 'contains{field=/msg,any=timeout|degraded}' \
  "$complex_json" \
  "$complex_json"
expect_example_match 'icontains{field=/msg,value=timeout}' \
  "$complex_json" \
  "$complex_json"
expect_example_match 'icontains{field=/service,a=AUTH|EDGE}' \
  "$complex_json" \
  "$complex_json"
expect_example_match 'iprefix{field=/service,value=auth}' \
  "$complex_json" \
  "$complex_json"
expect_example_match 'date{field=/timestamp,after=2025-01-01,before=2025-02-01}' \
  "$complex_json" \
  "$complex_json"
expect_example_match 'date{f=/timestamp,since=yesterday}' \
  "$complex_future_json" \
  "$complex_future_json"
expect_example_match 'and.eq{field=/status,value=open},and.range{field=/progress,gte=50}' \
  "$complex_json" \
  "$complex_json"
expect_example_match 'or.eq{field=/region,value=us},or.eq{field=/region,value=eu}' \
  "$complex_json" \
  "$complex_json"
expect_example_match 'not.eq{field=/state,value=disabled}' \
  "$complex_json" \
  "$complex_json"
expect_example_match 'exists{/metadata/etag}' \
  "$complex_json" \
  "$complex_json"

projection_out=$(printf '%s' "$complex_json" |
  "$clql" -c -f /owner/name '/priority>=3')
if [ "$projection_out" != '{"owner":{"name":"alice"}}' ]; then
  printf 'clql smoke: projection help example failed: %s\n' "$projection_out" >&2
  exit 1
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/clql-smoke.XXXXXX")
or_input=$tmpdir/or.json
inline_input=$tmpdir/inline.json
printf '%s\n%s\n' '{"status":"closed","region":"apac"}' "$complex_json" >"$or_input"
or_out=$("$clql" -c -O '/status="open"' '/status="queued"' "$or_input")
if [ "$or_out" != "$complex_json" ]; then
  printf 'clql smoke: OR help example failed: %s\n' "$or_out" >&2
  exit 1
fi

printf '%s' "$complex_json" >"$inline_input"
"$clql" -m '/status="closed"' -i '/id="42"' "$inline_input"
inline_out=$(cat "$inline_input")
if [ "$inline_out" != "$complex_mutated_json" ]; then
  printf 'clql smoke: inline mutation help example failed: %s\n' \
    "$inline_out" >&2
  exit 1
fi

theme_input='{"status":"open"}'
theme_expected='{"status":"open"}'
for theme_args in \
  '-c --theme default' \
  '-c --theme=default' \
  '-c -t default' \
  '-c -tdefault' \
  '-c -t=default' \
  '-ctdefault' \
  '-ct=default'
do
  # shellcheck disable=SC2086
  theme_out=$(printf '%s' "$theme_input" | "$clql" $theme_args '/status="open"')
  if [ "$theme_out" != "$theme_expected" ]; then
    printf 'clql smoke: theme compatibility output mismatch for %s: %s\n' \
      "$theme_args" "$theme_out" >&2
    printf 'clql smoke: expected: %s\n' "$theme_expected" >&2
    exit 1
  fi
done
