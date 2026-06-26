#!/bin/sh
set -eu

header=${1:?usage: check_public_api_style.sh HEADER [SHARED_LIBRARY] [SOURCE_ROOT]}
shared=${2:-}
source_root=${3:-}

allowed_exports='
lql_capabilities_get
lql_error_init
lql_new
lql_status_string
lql_version
'

forbidden='
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
