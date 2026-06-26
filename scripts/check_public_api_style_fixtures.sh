#!/bin/sh
set -eu

checker=${1:?usage: check_public_api_style_fixtures.sh CHECKER}

tmp=${TMPDIR:-/tmp}/lql-api-style-$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/src" "$tmp/tests" "$tmp/parity" "$tmp/lua" "$tmp/examples" \
  "$tmp/bench" "$tmp/include/lql"
printf '%s\n' '# fixture' >"$tmp/README.md"

header=$tmp/include/lql/lql.h
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

run_expect_ok() {
  if ! sh "$checker" "$header" "" "$tmp"; then
    printf 'public API fixture: expected clean tree to pass\n' >&2
    exit 1
  fi
}

run_expect_fail() {
  label=$1
  if sh "$checker" "$header" "" "$tmp" >/dev/null 2>&1; then
    printf 'public API fixture: expected %s to fail\n' "$label" >&2
    exit 1
  fi
}

run_expect_ok

cat >>"$header" <<'EOF'
void lql_free(lql *self);
EOF
run_expect_fail "public free cleanup wrapper"
sed '$d' "$header" >"$tmp/header.clean"
mv "$tmp/header.clean" "$header"

cat >>"$header" <<'EOF'
int lql_payload_project_json(lql *self, void *payload, void *projection);
EOF
run_expect_fail "public payload operation wrapper"
sed '$d' "$header" >"$tmp/header.clean"
mv "$tmp/header.clean" "$header"

cat >>"$header" <<'EOF'
void lql_dealloc(void *ptr);
EOF
run_expect_fail "public allocator free wrapper"
sed '$d' "$header" >"$tmp/header.clean"
mv "$tmp/header.clean" "$header"

cat >>"$header" <<'EOF'
#define lql_matches_json(self, selector, json, json_len, out, error) \
  (self)->matches_json((self), (selector), (json), (json_len), (out), (error))
EOF
run_expect_fail "public free-operation macro wrapper"
sed '$d' "$header" | sed '$d' >"$tmp/header.clean"
mv "$tmp/header.clean" "$header"

sed '/void (\*destroy)/i\
  /* Forbidden cleanup spelling. */\
  void (*selector_free)(void *selector);' "$header" >"$tmp/header.bad"
mv "$tmp/header.bad" "$header"
run_expect_fail "receiver cleanup field named *_free"
sed '/selector_free/d' "$header" | sed '/Forbidden cleanup spelling/d' \
  >"$tmp/header.clean"
mv "$tmp/header.clean" "$header"

cat >"$tmp/src/wrapper.c" <<'EOF'
typedef struct lql_selector lql_selector;
static void lql_selector_destroy(lql_selector *selector) { (void)selector; }
EOF
run_expect_fail "static free-operation wrapper"
rm -f "$tmp/src/wrapper.c"

cat >"$tmp/src/alloc.c" <<'EOF'
#include <stdlib.h>
void *bad_alloc(void) { return malloc(1); }
EOF
run_expect_fail "direct runtime allocation"
rm -f "$tmp/src/alloc.c"

cat >"$tmp/src/alloc_wrapper.c" <<'EOF'
void *bad_alloc_wrapper(void) { return lql_alloc(1); }
EOF
run_expect_fail "liblql allocator wrapper call"
rm -f "$tmp/src/alloc_wrapper.c"

cat >"$tmp/src/alloc_macro_wrapper.c" <<'EOF'
void *bad_alloc_macro_wrapper(void) { return LQL_ALLOCATOR_ALLOC(1); }
EOF
run_expect_fail "liblql allocator macro wrapper call"
rm -f "$tmp/src/alloc_macro_wrapper.c"

cat >"$tmp/src/lql_eval.c" <<'EOF'
void *bad_null_receiver_allocator(void) { return lql_allocator_from_receiver(NULL); }
EOF
run_expect_fail "library null receiver allocator fallback"
rm -f "$tmp/src/lql_eval.c"

cat >"$tmp/src/clql.c" <<'EOF'
void *bad_cli_null_receiver_allocator(void) { return lql_allocator_from_receiver(NULL); }
EOF
run_expect_fail "clql null receiver allocator fallback"
rm -f "$tmp/src/clql.c"

cat >"$tmp/src/lql.c" <<'EOF'
void bad_cleanup_default_allocator(void) { allocator = lql_allocator_default(); }
EOF
run_expect_fail "cleanup default allocator fallback"
rm -f "$tmp/src/lql.c"

cat >"$tmp/src/lql_eval.c" <<'EOF'
void bad_lonejson_default_allocator(void) { runtime = lonejson_new(NULL, error); }
EOF
run_expect_fail "lonejson default allocator runtime"
rm -f "$tmp/src/lql_eval.c"

cat >"$tmp/src/lql_selector.c" <<'EOF'
void bad_null_private_receiver(void) { lql_selector_destroy_impl(NULL, selector); }
EOF
run_expect_fail "private receiver implementation called with NULL receiver"
rm -f "$tmp/src/lql_selector.c"

cat >"$tmp/src/lql_eval.c" <<'EOF'
void bad_core_private_operation_call(void) { lql_matches_json_impl(ctx, 0, 0, 0, 0, 0); }
EOF
run_expect_fail "core private receiver operation call"
rm -f "$tmp/src/lql_eval.c"

cat >"$tmp/src/lql_eval.c" <<'EOF'
static int lql_query_file_decisions_impl(void) { return 0; }
EOF
run_expect_fail "private receiver implementation symbol"
rm -f "$tmp/src/lql_eval.c"

printf '%s\n' 'stale cleanup surface is `lql_dealloc()`' >"$tmp/README.md"
run_expect_fail "README allocator wrapper documentation"
printf '%s\n' '# fixture' >"$tmp/README.md"

cat >"$tmp/lua/private.c" <<'EOF'
#include "lql_internal.h"
void lua_private(void) {}
EOF
run_expect_fail "Lua private API use"
rm -f "$tmp/lua/private.c"

cat >"$tmp/src/clql.c" <<'EOF'
void cli_private_exec(void) { (void)lql_eval_query_file_spooled_matches; }
EOF
run_expect_fail "clql private evaluator use"
rm -f "$tmp/src/clql.c"

cat >"$tmp/src/clql.c" <<'EOF'
void *clql_allocator(void) { return lql_allocator_from_receiver(clql_ctx); }
EOF
run_expect_fail "clql receiver-owned glue allocator use"
rm -f "$tmp/src/clql.c"

cat >"$tmp/tests/private_operation.c" <<'EOF'
void test_private_operation(void) { (void)lql_mutate_json_impl(0, 0, 0, 0, 0, 0); }
EOF
run_expect_fail "test private operation bypass"
rm -f "$tmp/tests/private_operation.c"

run_expect_ok
