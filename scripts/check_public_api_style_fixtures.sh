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
  void (*selector_free)(void *selector);\
  /* Forbidden generic allocator wrapper. */\
  void *(*memory_alloc)(lql *self, unsigned long size);' "$header" >"$tmp/header.bad"
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
void bad_core_shared_operation_call(void) { (void)lql_project_spooled(0, 0, 0, 0, 0, 0); }
static int lql_query_file_decisions_impl(void) { return 0; }
EOF

cat >"$tmp/src/lql_selector.c" <<'EOF'
void bad_null_private_receiver(void) { lql_selector_destroy_impl(NULL, selector); }
void bad_selector_allocator_entry(lql_allocator *allocator) { lql_parse_selector_internal(allocator, expr, 0, out, error); }
EOF

cat >"$tmp/src/lql.c" <<'EOF'
void bad_cleanup_default_allocator(void) { allocator = lql_allocator_default(); }
EOF

cat >"$tmp/src/lql_allocator.c" <<'EOF'
void *lql_allocator_from_receiver(void *self) {
  if (self == 0) {
    return lql_allocator_default();
  }
  return self;
}
EOF

cat >"$tmp/src/lql_project.c" <<'EOF'
void *projection_allocator(void) { return projection->allocator != NULL ? projection->allocator : lql_allocator_from_receiver(self); }
static void projection_path_cleanup(lql_allocator *allocator, void *path) { (void)allocator; (void)path; }
static int parse_projection_path(lql *self, lql_allocator *allocator, const char *raw, void *out) { (void)self; (void)allocator; (void)raw; (void)out; return 0; }
static int append_buf(lql_allocator *allocator, char **buf) { (void)allocator; (void)buf; return 0; }
struct lql_projection {
  lql_allocator *allocator;
};
EOF

cat >"$tmp/src/lql_mutation.c" <<'EOF'
static void mutation_plan_cleanup_items(lql_allocator *allocator, void *plan) { (void)allocator; (void)plan; }
static int parse_mutation_expr(lql *self, lql_allocator *allocator, const char *expr, void *plan) { (void)self; (void)allocator; (void)expr; (void)plan; return 0; }
static char *resolve_file_value_path(lql_allocator *allocator, const char *raw) { (void)allocator; return (char *)raw; }
static int parse_number_slice(lql_allocator *allocator, const char *s) { (void)allocator; return s != 0; }
static int write_mutation_set_value(lql_allocator *allocator, void *writer) { (void)allocator; return writer != 0; }
EOF

printf '%s\n' 'stale cleanup surface is `lql_dealloc()`' >"$tmp/README.md"

cat >"$tmp/lua/private.c" <<'EOF'
#include "lql_internal.h"
void lua_private(void) {}
EOF

cat >"$tmp/src/clql.c" <<'EOF'
#include "lql_internal.h"
void *bad_cli_null_receiver_allocator(void) { return lql_allocator_from_receiver(NULL); }
void cli_private_exec(void) { (void)lql_eval_query_file_spooled_matches; }
void *bad_clql_default_allocator(void) { return lql_allocator_default(); }
EOF

cat >"$tmp/examples/global_query.c" <<'EOF'
void consumer_global_query(void) { (void)lql_version(); }
EOF

cat >"$tmp/tests/private_operation.c" <<'EOF'
void test_private_operation(void) { (void)lql_mutate_json_impl(0, 0, 0, 0, 0, 0); }
void test_private_spooled_operation(void) { (void)lql_project_spooled(0, 0, 0, 0, 0, 0); }
EOF

cat >"$tmp/parity/sdk_liblql.go" <<'EOF'
static int liblql_matches_json(const char *expr) { return expr != 0; }
static int liblql_read_tmp(void *tmp) { return tmp != 0; }
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
expect_diagnostic "$out" "receiver memory wrappers must not be public SDK methods"
expect_diagnostic "$out" "project runtime must not use null receiver allocator fallback"
expect_diagnostic "$out" "receiver allocator accessor must not fall back to default allocator"
expect_diagnostic "$out" "cleanup paths must use explicit receiver/handle allocators"
expect_diagnostic "$out" "selector internals must be receiver-owned at subsystem boundaries"
expect_diagnostic "$out" "receiver-owned operations must not fall back to handle-stored allocators"
expect_diagnostic "$out" "receiver-owned child handles must not store allocators"
expect_diagnostic "$out" "receiver-owned cleanup helpers must take lql \*self"
expect_diagnostic "$out" "receiver-owned parser boundary helpers must derive allocators"
expect_diagnostic "$out" "receiver-owned runtime helpers must take receiver runtime context"
expect_diagnostic "$out" "lonejson runtimes must use receiver allocator bridge"
expect_diagnostic "$out" "private receiver implementations must not be called with NULL receivers"
expect_diagnostic "$out" "receiver methods must be file-local methods"
expect_diagnostic "$out" "receiver operation internals must not be shared"
expect_diagnostic "$out" "README documents forbidden operation/cleanup wrapper"
expect_diagnostic "$out" "Lua facade must use public liblql APIs only"
expect_diagnostic "$out" "clql must route execution through receiver methods"
expect_diagnostic "$out" "clql glue allocation must use the active receiver allocator"
expect_diagnostic "$out" "clql glue allocation must use private receiver allocator helpers"
expect_diagnostic "$out" "project-owned consumers must query version/capabilities through the receiver"
expect_diagnostic "$out" "tests and examples must exercise operations through receiver methods"
expect_diagnostic "$out" "parity C operation helpers must receive lql \*ctx first"

rm -rf "$tmp/src" "$tmp/tests" "$tmp/parity" "$tmp/lua" "$tmp/examples" \
  "$tmp/bench"
mkdir -p "$tmp/src" "$tmp/tests" "$tmp/parity" "$tmp/lua" "$tmp/examples" \
  "$tmp/bench"
printf '%s\n' '# fixture' >"$tmp/README.md"
write_clean_header

run_expect_ok
