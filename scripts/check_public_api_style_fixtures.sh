#!/bin/sh
set -eu

checker=${1:?usage: check_public_api_style_fixtures.sh CHECKER}

tmp=${TMPDIR:-/tmp}/lql-api-style-$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/src" "$tmp/tests" "$tmp/parity" "$tmp/lua" "$tmp/examples" \
  "$tmp/bench" "$tmp/include/lql"
printf '%s\n' '# fixture' >"$tmp/README.md"

header=$tmp/include/lql/lql.h
write_clean_header() {
  cat >"$header" <<'EOF'
#ifndef LQL_LQL_H
#define LQL_LQL_H
typedef struct lql lql;
struct lql {
  /* Private implementation pointer owned by the receiver. */
  void *impl;
  /* Destroys the receiver. */
  void (*destroy)(lql *self);
};
#endif
EOF
}

write_clean_header

run_expect_ok() {
  if ! sh "$checker" "$header" "" "$tmp"; then
    printf 'public API fixture: expected clean tree to pass\n' >&2
    exit 1
  fi
}

expect_diagnostic() {
  output=$1
  expected=$2
  if ! grep -q "$expected" "$output"; then
    printf 'public API fixture: expected diagnostic missing: %s\n' \
      "$expected" >&2
    cat "$output" >&2
    exit 1
  fi
}

run_expect_ok

sed '/void (\*destroy)/i\
  /* Forbidden cleanup spelling. */\
  void (*selector_free)(void *selector);' "$header" >"$tmp/header.bad"
mv "$tmp/header.bad" "$header"
cat >>"$header" <<'EOF'
void lql_free(lql *self);
int lql_payload_project_json(lql *self, void *payload, void *projection);
void lql_dealloc(void *ptr);
#define lql_matches_json(self, selector, json, json_len, out, error) \
  (self)->matches_json((self), (selector), (json), (json_len), (out), (error))
EOF

cat >"$tmp/src/wrapper.c" <<'EOF'
typedef struct lql_selector lql_selector;
static void lql_selector_destroy(lql_selector *selector) { (void)selector; }
EOF

cat >"$tmp/src/alloc.c" <<'EOF'
#include <stdlib.h>
void *bad_alloc(void) { return malloc(1); }
EOF

cat >"$tmp/src/alloc_wrapper.c" <<'EOF'
void *bad_alloc_wrapper(void) { return lql_alloc(1); }
EOF

cat >"$tmp/src/alloc_macro_wrapper.c" <<'EOF'
void *bad_alloc_macro_wrapper(void) { return LQL_ALLOCATOR_ALLOC(1); }
EOF

cat >"$tmp/src/lql_eval.c" <<'EOF'
void *bad_null_receiver_allocator(void) { return lql_allocator_from_receiver(NULL); }
void bad_lonejson_default_allocator(void) { runtime = lonejson_new(NULL, error); }
void bad_core_private_operation_call(void) { lql_matches_json_impl(ctx, 0, 0, 0, 0, 0); }
static int lql_query_file_decisions_impl(void) { return 0; }
EOF

cat >"$tmp/src/lql_selector.c" <<'EOF'
void bad_null_private_receiver(void) { lql_selector_destroy_impl(NULL, selector); }
EOF

cat >"$tmp/src/lql.c" <<'EOF'
void bad_cleanup_default_allocator(void) { allocator = lql_allocator_default(); }
EOF

printf '%s\n' 'stale cleanup surface is `lql_dealloc()`' >"$tmp/README.md"

cat >"$tmp/lua/private.c" <<'EOF'
#include "lql_internal.h"
void lua_private(void) {}
EOF

cat >"$tmp/src/clql.c" <<'EOF'
void *bad_cli_null_receiver_allocator(void) { return lql_allocator_from_receiver(NULL); }
void cli_private_exec(void) { (void)lql_eval_query_file_spooled_matches; }
void *clql_allocator(void) { return lql_allocator_from_receiver(clql_ctx); }
EOF

cat >"$tmp/examples/global_query.c" <<'EOF'
void consumer_global_query(void) { (void)lql_version(); }
EOF

cat >"$tmp/tests/private_operation.c" <<'EOF'
void test_private_operation(void) { (void)lql_mutate_json_impl(0, 0, 0, 0, 0, 0); }
EOF

out="$tmp/batched-negative.out"
if sh "$checker" "$header" "" "$tmp" >"$out" 2>&1; then
  printf 'public API fixture: expected batched negative tree to fail\n' >&2
  exit 1
fi

expect_diagnostic "$out" "forbidden free-operation/cleanup prototype"
expect_diagnostic "$out" "forbidden public free-operation/cleanup macro wrapper"
expect_diagnostic "$out" "forbidden receiver cleanup field named"
expect_diagnostic "$out" "forbidden static free-operation/cleanup wrapper"
expect_diagnostic "$out" "direct C runtime allocation"
expect_diagnostic "$out" "forbidden liblql allocator wrapper call"
expect_diagnostic "$out" "forbidden liblql allocator macro wrapper call"
expect_diagnostic "$out" "project runtime must not use null receiver allocator fallback"
expect_diagnostic "$out" "cleanup paths must use explicit receiver/handle allocators"
expect_diagnostic "$out" "lonejson runtimes must use receiver allocator bridge"
expect_diagnostic "$out" "private receiver implementations must not be called with NULL receivers"
expect_diagnostic "$out" "receiver methods must be file-local methods"
expect_diagnostic "$out" "README documents forbidden operation/cleanup wrapper"
expect_diagnostic "$out" "Lua facade must use public liblql APIs only"
expect_diagnostic "$out" "clql must route execution through receiver methods"
expect_diagnostic "$out" "clql glue allocation must not depend on liblql receiver ownership"
expect_diagnostic "$out" "project-owned consumers must query version/capabilities through the receiver"
expect_diagnostic "$out" "tests and examples must exercise operations through receiver methods"

rm -rf "$tmp/src" "$tmp/tests" "$tmp/parity" "$tmp/lua" "$tmp/examples" \
  "$tmp/bench"
mkdir -p "$tmp/src" "$tmp/tests" "$tmp/parity" "$tmp/lua" "$tmp/examples" \
  "$tmp/bench"
printf '%s\n' '# fixture' >"$tmp/README.md"
write_clean_header

run_expect_ok
