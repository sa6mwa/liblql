#!/bin/sh
set -eu

header=${1:?usage: check_public_api_style.sh HEADER [SHARED_LIBRARY] [SOURCE_ROOT]}
shared=${2:-}
source_root=${3:-}

allowed_exports='
lql_error_init
lql_new
lql_status_string
'

forbidden='
lql_capabilities_get
lql_version
lql_selector_parse
lql_selector_parse_or
lql_selector_free
lql_selector_destroy
lql_selector_is_empty
lql_matches_json
lql_query_file_decisions
lql_query_file_decisions_with_options
lql_query_source_decisions
lql_query_source_decisions_with_options
lql_query_source_spooled_matches
lql_query_source_spooled_matches_with_options
lql_query_file_matches
lql_query_file_matches_with_options
lql_payload_write_json
lql_payload_write_json_sink
lql_payload_project_json
lql_projection_parse
lql_projection_free
lql_projection_destroy
lql_project_file_range
lql_project_source
lql_project_json
lql_compact_file_range
lql_compact_source
lql_compact_json
lql_mutation_plan_parse
lql_mutation_plan_parse_with_options
lql_mutation_plan_count
lql_mutation_plan_free
lql_mutation_plan_destroy
lql_mutate_file_range_root_fields
lql_mutate_file_range_paths
lql_mutate_file_range_candidates
lql_mutate_file_range_projected_candidates
lql_mutate_source_paths
lql_mutate_source_candidates
lql_mutate_source_projected_candidates
lql_mutate_json
lql_destroy
lql_free
lql_alloc
lql_calloc
lql_realloc
lql_dealloc
lql_strdup
'

failed=0
for symbol in $forbidden; do
  if grep -Eq "^[[:space:]]*([_A-Za-z][_A-Za-z0-9]*[[:space:]]+)+${symbol}[[:space:]]*\\(" "$header"; then
    printf 'public API style: forbidden free-operation/cleanup prototype in %s: %s\n' "$header" "$symbol" >&2
    failed=1
  fi
  if grep -Eq "^[[:space:]]*#define[[:space:]]+${symbol}[[:space:]]*\\(" "$header"; then
    printf 'public API style: forbidden public free-operation/cleanup macro wrapper in %s: %s\n' "$header" "$symbol" >&2
    failed=1
  fi
done

if grep -Eq "^[[:space:]]*void[[:space:]]+\\(\\*[A-Za-z0-9_]+_free\\)[[:space:]]*\\(" "$header"; then
  printf 'public API style: forbidden receiver cleanup field named *_free in %s\n' "$header" >&2
  failed=1
fi

receiver_doc_hits=$(
  awk '
    /^struct lql[[:space:]]*\{/ { in_receiver = 1; prev = ""; next }
    in_receiver && /^};/ { in_receiver = 0; next }
    in_receiver {
      if ($0 ~ /^[[:space:]]*$/) {
        next
      }
      if ($0 ~ /^[[:space:]]*(void[[:space:]]+\*impl;|[A-Za-z_][A-Za-z0-9_[:space:]*]*\(\*[A-Za-z_][A-Za-z0-9_]*\)[[:space:]]*\()/ &&
          prev !~ /^[[:space:]]*\/\*/) {
        print FILENAME ":" FNR ":" $0
      }
      prev = $0
    }
  ' "$header"
)
if [ -n "$receiver_doc_hits" ]; then
  printf 'public API style: undocumented receiver field in %s\n' "$header" >&2
  printf '%s\n' "$receiver_doc_hits" >&2
  failed=1
fi

if [ -n "$shared" ] && [ -f "$shared" ]; then
  if command -v nm >/dev/null 2>&1; then
    symbols=$(nm -D --defined-only "$shared" 2>/dev/null | awk '{print $3}' || true)
    if [ -z "$symbols" ]; then
      symbols=$(nm -gU "$shared" 2>/dev/null | awk '{print $3}' || true)
    fi
    for symbol in $forbidden; do
      if printf '%s\n' "$symbols" | grep -Eq "^_?${symbol}$"; then
        printf 'public API style: forbidden exported operation/cleanup symbol in %s: %s\n' "$shared" "$symbol" >&2
        failed=1
      fi
    done
    exported_lql=$(
      printf '%s\n' "$symbols" |
        sed -n 's/^_\(lql_.*\)$/\1/p; /^lql_/p' |
        sort -u
    )
    for symbol in $exported_lql; do
      if ! printf '%s\n' "$allowed_exports" | grep -Fxq "$symbol"; then
        printf 'public API style: unexpected exported liblql symbol in %s: %s\n' "$shared" "$symbol" >&2
        failed=1
      fi
    done
  fi
fi

if [ -n "$source_root" ] && [ -d "$source_root" ]; then
  if [ -f "$source_root/README.md" ]; then
    for symbol in $forbidden; do
      if grep -F "\`$symbol()\`" "$source_root/README.md" >/dev/null; then
        printf 'public API style: README documents forbidden operation/cleanup wrapper: %s\n' "$symbol" >&2
        failed=1
      fi
    done
  fi

  for symbol in $forbidden; do
    if grep -REn \
      "^[[:space:]]*#define[[:space:]]+${symbol}[[:space:]]*\\(" \
      "$source_root/src" "$source_root/tests" "$source_root/parity" \
      "$source_root/lua" "$source_root/examples" "$source_root/bench" \
      2>/dev/null; then
      printf 'public API style: forbidden free-operation/cleanup macro wrapper: %s\n' "$symbol" >&2
      failed=1
    fi
    if grep -REn \
      "^[[:space:]]*static[[:space:]][^(;]*[[:space:]*]${symbol}[[:space:]]*\\(" \
      "$source_root/src" "$source_root/tests" "$source_root/parity" \
      "$source_root/lua" "$source_root/examples" "$source_root/bench" \
      2>/dev/null; then
      printf 'public API style: forbidden static free-operation/cleanup wrapper: %s\n' "$symbol" >&2
      failed=1
    fi
  done

  receiver_wrapper_hits=$(
    grep -REn \
      '^[[:space:]]*static[[:space:]][^(;]*[[:space:]*]receiver_(selector|matches|query|payload|projection|project|compact|mutation|mutate)_' \
      "$source_root/src" 2>/dev/null || true
  )
  if [ -n "$receiver_wrapper_hits" ]; then
    printf 'public API style: forbidden static receiver operation wrapper\n' >&2
    printf '%s\n' "$receiver_wrapper_hits" >&2
    failed=1
  fi

  free_helper_hits=$(
    grep -REn \
      '^[[:space:]]*static[[:space:]][^(;]*[[:space:]*]([A-Za-z0-9_]+_free|free_[A-Za-z0-9_]+)[[:space:]]*\(' \
      "$source_root/src" "$source_root/tests" "$source_root/parity" \
      "$source_root/lua" "$source_root/examples" "$source_root/bench" \
      2>/dev/null || true
  )
  if [ -n "$free_helper_hits" ]; then
    printf 'public API style: forbidden project-owned free cleanup helper\n' >&2
    printf '%s\n' "$free_helper_hits" >&2
    failed=1
  fi

  alloc_hits=$(
    find "$source_root/src" "$source_root/tests" "$source_root/lua" \
      "$source_root/examples" "$source_root/bench" \
      -type f \( -name '*.c' -o -name '*.h' \) \
      ! -path "$source_root/src/lql_allocator.c" \
      -exec grep -HEn '(^|[^_[:alnum:]>])(malloc|calloc|realloc|free|strdup)[[:space:]]*\(' {} + \
      2>/dev/null || true
  )
  if [ -n "$alloc_hits" ]; then
    printf 'public API style: direct C runtime allocation outside src/lql_allocator.c\n' >&2
    printf '%s\n' "$alloc_hits" >&2
    failed=1
  fi

  allocator_wrapper_hits=$(
    grep -REn \
      '(^|[^_[:alnum:]])(lql_alloc|lql_calloc|lql_realloc|lql_dealloc|lql_strdup)[[:space:]]*\(' \
      "$source_root/src" "$source_root/tests" "$source_root/parity" \
      "$source_root/lua" "$source_root/examples" "$source_root/bench" \
      2>/dev/null || true
  )
  if [ -n "$allocator_wrapper_hits" ]; then
    printf 'public API style: forbidden liblql allocator wrapper call\n' >&2
    printf '%s\n' "$allocator_wrapper_hits" >&2
    failed=1
  fi

  allocator_macro_hits=$(
    grep -REn \
      '(^|[^_[:alnum:]])LQL_ALLOCATOR_(ALLOC|CALLOC|REALLOC|DESTROY|STRDUP)[[:space:]]*\(' \
      "$source_root/src" "$source_root/tests" "$source_root/parity" \
      "$source_root/lua" "$source_root/examples" "$source_root/bench" \
      2>/dev/null || true
  )
  if [ -n "$allocator_macro_hits" ]; then
    printf 'public API style: forbidden liblql allocator macro wrapper call\n' >&2
    printf '%s\n' "$allocator_macro_hits" >&2
    failed=1
  fi

  library_default_allocator_hits=$(
    grep -En 'lql_allocator_default[[:space:]]*\(' \
      "$source_root/src/lql_selector.c" \
      "$source_root/src/lql_project.c" \
      "$source_root/src/lql_eval.c" \
      "$source_root/src/lql_mutation.c" 2>/dev/null || true
  )
  if [ -n "$library_default_allocator_hits" ]; then
    printf 'public API style: library core must use receiver/handle allocators\n' >&2
    printf '%s\n' "$library_default_allocator_hits" >&2
    failed=1
  fi

  null_receiver_allocator_hits=$(
    grep -REn 'lql_allocator_from_receiver[[:space:]]*\([[:space:]]*NULL[[:space:]]*\)' \
      "$source_root/src/lql.c" \
      "$source_root/src/lql_selector.c" \
      "$source_root/src/lql_project.c" \
      "$source_root/src/lql_eval.c" \
      "$source_root/src/lql_mutation.c" \
      "$source_root/src/clql.c" \
      "$source_root/lua" \
      "$source_root/examples" \
      "$source_root/bench" 2>/dev/null || true
  )
  if [ -n "$null_receiver_allocator_hits" ]; then
    printf 'public API style: project runtime must not use null receiver allocator fallback\n' >&2
    printf '%s\n' "$null_receiver_allocator_hits" >&2
    failed=1
  fi

  cleanup_default_allocator_hits=$(
    grep -En 'allocator[[:space:]]*=[[:space:]]*lql_allocator_default[[:space:]]*\(' \
      "$source_root/src/lql.c" \
      "$source_root/src/lql_selector.c" \
      "$source_root/src/lql_project.c" \
      "$source_root/src/lql_eval.c" \
      "$source_root/src/lql_mutation.c" 2>/dev/null || true
  )
  if [ -n "$cleanup_default_allocator_hits" ]; then
    printf 'public API style: cleanup paths must use explicit receiver/handle allocators\n' >&2
    printf '%s\n' "$cleanup_default_allocator_hits" >&2
    failed=1
  fi

  selector_allocator_entry_hits=$(
    grep -REn \
      'lql_(parse_selector_internal|node_cleanup)[^(]*\([[:space:]]*lql_allocator[[:space:]*]|lql_(parse_selector_internal|node_cleanup)[[:space:]]*\([[:space:]]*allocator' \
      "$source_root/src/lql.c" \
      "$source_root/src/lql_selector.c" \
      "$source_root/src/lql_internal.h" 2>/dev/null || true
  )
  if [ -n "$selector_allocator_entry_hits" ]; then
    printf 'public API style: selector internals must be receiver-owned at subsystem boundaries\n' >&2
    printf '%s\n' "$selector_allocator_entry_hits" >&2
    failed=1
  fi

  receiver_allocator_fallback_hits=$(
    grep -REn \
      '(eval_selector_allocator|projection_allocator)[[:space:]]*\(|->[[:space:]]*allocator[[:space:]]*!=[[:space:]]*NULL[[:space:]]*\?' \
      "$source_root/src/lql.c" \
      "$source_root/src/lql_selector.c" \
      "$source_root/src/lql_project.c" \
      "$source_root/src/lql_eval.c" \
      "$source_root/src/lql_mutation.c" \
      "$source_root/src/lql_internal.h" 2>/dev/null || true
  )
  if [ -n "$receiver_allocator_fallback_hits" ]; then
    printf 'public API style: receiver-owned operations must not fall back to handle-stored allocators\n' >&2
    printf '%s\n' "$receiver_allocator_fallback_hits" >&2
    failed=1
  fi

  lonejson_default_allocator_hits=$(
    grep -En 'lonejson_new[[:space:]]*\([[:space:]]*NULL[[:space:]]*,' \
      "$source_root/src/lql_selector.c" \
      "$source_root/src/lql_project.c" \
      "$source_root/src/lql_eval.c" \
      "$source_root/src/lql_mutation.c" 2>/dev/null || true
  )
  if [ -n "$lonejson_default_allocator_hits" ]; then
    printf 'public API style: lonejson runtimes must use receiver allocator bridge\n' >&2
    printf '%s\n' "$lonejson_default_allocator_hits" >&2
    failed=1
  fi

  null_private_receiver_hits=$(
    grep -REn 'lql_[A-Za-z0-9_]+_impl[[:space:]]*\([[:space:]]*NULL[[:space:]]*,' \
      "$source_root/src/lql.c" \
      "$source_root/src/lql_selector.c" \
      "$source_root/src/lql_project.c" \
      "$source_root/src/lql_eval.c" \
      "$source_root/src/lql_mutation.c" 2>/dev/null || true
  )
  if [ -n "$null_private_receiver_hits" ]; then
    printf 'public API style: private receiver implementations must not be called with NULL receivers\n' >&2
    printf '%s\n' "$null_private_receiver_hits" >&2
    failed=1
  fi

  receiver_impl_symbol_hits=$(
    grep -REn 'lql_[A-Za-z0-9_]+_impl[[:space:]]*\(' \
      "$source_root/src/lql.c" \
      "$source_root/src/lql_selector.c" \
      "$source_root/src/lql_project.c" \
      "$source_root/src/lql_eval.c" \
      "$source_root/src/lql_mutation.c" \
      "$source_root/src/lql_internal.h" 2>/dev/null || true
  )
  if [ -n "$receiver_impl_symbol_hits" ]; then
    printf 'public API style: receiver methods must be file-local methods, not private *_impl operation symbols\n' >&2
    printf '%s\n' "$receiver_impl_symbol_hits" >&2
    failed=1
  fi

  cli_receiver_bypass_hits=$(
    grep -En \
      'lql_eval_query_|lql_[A-Za-z0-9_]+_impl[[:space:]]*\(' \
      "$source_root/src/clql.c" 2>/dev/null || true
  )
  if [ -n "$cli_receiver_bypass_hits" ]; then
    printf 'public API style: clql must route execution through receiver methods\n' >&2
    printf '%s\n' "$cli_receiver_bypass_hits" >&2
    failed=1
  fi

  cli_allocator_owner_hits=$(
    grep -En \
      'lql_allocator_from_receiver[[:space:]]*\([[:space:]]*clql_ctx[[:space:]]*\)' \
      "$source_root/src/clql.c" 2>/dev/null || true
  )
  if [ -n "$cli_allocator_owner_hits" ]; then
    printf 'public API style: clql glue allocation must not depend on liblql receiver ownership\n' >&2
    printf '%s\n' "$cli_allocator_owner_hits" >&2
    failed=1
  fi

  consumer_global_query_hits=$(
    grep -REn \
      'lql_(version|capabilities_get)[[:space:]]*\(' \
      "$source_root/src/clql.c" \
      "$source_root/lua" \
      "$source_root/examples" \
      "$source_root/tests/header_smoke.c" \
      "$source_root/tests/header_smoke.cpp" \
      "$source_root/scripts/package.sh" 2>/dev/null || true
  )
  if [ -n "$consumer_global_query_hits" ]; then
    printf 'public API style: project-owned consumers must query version/capabilities through the receiver\n' >&2
    printf '%s\n' "$consumer_global_query_hits" >&2
    failed=1
  fi

  test_receiver_bypass_hits=$(
    grep -REn \
      'lql_parse_selector_internal[[:space:]]*\(|lql_eval_query_|lql_[A-Za-z0-9_]+_impl[[:space:]]*\(' \
      "$source_root/tests" "$source_root/examples" "$source_root/bench" \
      2>/dev/null || true
  )
  if [ -n "$test_receiver_bypass_hits" ]; then
    printf 'public API style: tests and examples must exercise operations through receiver methods\n' >&2
    printf '%s\n' "$test_receiver_bypass_hits" >&2
    failed=1
  fi

  lua_private_hits=$(
    grep -REn \
      'lql_internal\.h|LQL_INTERNAL_SYMBOL|lql_[A-Za-z0-9_]+_impl[[:space:]]*\(' \
      "$source_root/lua" 2>/dev/null || true
  )
  if [ -n "$lua_private_hits" ]; then
    printf 'public API style: Lua facade must use public liblql APIs only\n' >&2
    printf '%s\n' "$lua_private_hits" >&2
    failed=1
  fi
fi

exit "$failed"
