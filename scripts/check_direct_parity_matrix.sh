#!/bin/sh
set -eu

. scripts/direct_parity_common.sh

# The release perf gate is a hard C > Go contract.  Use enough steady-state
# samples to dampen scheduler noise without weakening the 1.0x threshold.
direct_parity_init 'direct parity matrix' build/direct-parity-matrix.jsonl 30

sh scripts/ensure_direct_parity_fixtures.sh

direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  ne_status_open '/status!="open"' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-whitespace-100k.ndjson status_whitespace_100k \
  eq_status_open '/status="open"' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' reuse_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' reparse_selector_each_run /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' decision_only_source_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  prefix_status_open 'prefix{field=/status,value=op}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  iprefix_status_open 'iprefix{field=/status,value=OP}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  contains_payload_cde 'contains{f=/payload,v=cde}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  icontains_payload_cde 'icontains{f=/payload,v=CDE}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_payload_exists 'eq{field=/payload}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  contains_payload_exists 'contains{field=/payload}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  icontains_payload_exists 'icontains{field=/payload}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  prefix_payload_exists 'prefix{field=/payload}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  iprefix_payload_exists 'iprefix{field=/payload}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  in_status_open_closed 'in{field=/status,any=open|closed}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  in_status_open_pending 'in{field=/status,any=open|pending}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  and_status_open_region_west '/status="open",/region="us-west"' decision_only_selector /id

# Above the scanner's machine-word batch width, validate a genuinely distinct
# miss rather than only the duplicate-alias fast path.
wide_miss_selector=''
wide_index=0
while [ "$wide_index" -lt 129 ]; do
  if [ -n "$wide_miss_selector" ]; then
    wide_miss_selector="$wide_miss_selector,"
  fi
  wide_miss_selector="${wide_miss_selector}/status=\"open-${wide_index}\""
  wide_index=$((wide_index + 1))
done
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  wide_129_distinct_miss "$wide_miss_selector" decision_only_selector /id

direct_parity_run_row build/direct-probe/scalar-100k.ndjson scalar_100k \
  code_eq_one '/code=1' decision_only_selector /id
direct_parity_run_row build/direct-probe/scalar-100k.ndjson scalar_100k \
  bool_enabled_true '/enabled=true' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' plus_value_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' plus_value_source_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' plus_value_openjson_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  ne_status_open '/status!="open"' plus_value_source_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  prefix_status_open 'prefix{field=/status,value=op}' plus_value_source_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  contains_payload_cde 'contains{f=/payload,v=cde}' plus_value_source_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  icontains_payload_cde 'icontains{f=/payload,v=CDE}' plus_value_source_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  in_status_open_pending 'in{field=/status,any=open|pending}' plus_value_source_selector /id
direct_parity_run_row build/direct-probe/scalar-100k.ndjson scalar_100k \
  code_eq_one '/code=1' plus_value_source_selector /id
direct_parity_run_row build/direct-probe/scalar-100k.ndjson scalar_100k \
  bool_enabled_true '/enabled=true' plus_value_source_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' project_file_selector /id
direct_parity_run_row build/direct-probe/status-whitespace-100k.ndjson status_whitespace_100k \
  eq_status_open '/status="open"' project_file_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' project_source_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open_top_set '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/status-whitespace-100k.ndjson status_whitespace_100k \
  eq_status_open_top_set '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open_top_set '/status="open"' mutate_source_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open_file_backed_text '/status="open"' mutate_file_backed_text /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open_file_backed_base64 '/status="open"' mutate_file_backed_base64 /id

direct_parity_run_row build/direct-probe/top-set-status-100k.ndjson top_set_status_100k \
  eq_status_open_top_set_multi '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/top-remove-status-100k.ndjson top_remove_status_100k \
  eq_status_open_top_remove '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/top-increment-code-100k.ndjson top_increment_code_100k \
  eq_code_one_top_increment '/code=1' mutate_source_selector /id
direct_parity_run_row build/direct-probe/top-increment-code-100k.ndjson top_increment_code_100k \
  eq_code_one_top_increment_multi '/code=1' mutate_file_selector /id
direct_parity_run_row build/direct-probe/top-multi-code-100k.ndjson top_multi_code_100k \
  eq_code_one_top_multi '/code=1' mutate_file_selector /id

direct_parity_run_row build/direct-probe/nested-set-status-100k.ndjson nested_set_status_100k \
  eq_status_open_nested_set '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/nested-increment-100k.ndjson nested_increment_100k \
  eq_status_open_nested_increment '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/nested-remove-status-100k.ndjson nested_remove_status_100k \
  eq_status_open_nested_remove '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/same-top-nested-increment-100k.ndjson same_top_nested_increment_100k \
  eq_status_open_same_top_nested_increment '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/mixed-nested-code-100k.ndjson mixed_nested_code_100k \
  eq_code_one_mixed_nested_multi '/code=1' mutate_file_selector /id

direct_parity_run_row build/direct-probe/project-mutate-status-100k.ndjson project_mutate_status_100k \
  project_mutation_status '/status="open"' project_mutate_file_selector /id
direct_parity_run_row build/direct-probe/temporal-100k.ndjson temporal_100k \
  date_window 'date{field=/timestamp,after=2026-03-05T10:28:21Z,before=2026-03-05T10:30:00Z}' decision_only_selector /id
direct_parity_run_row build/direct-probe/array-scalar-100k.ndjson array_scalar_100k \
  array_scalar_indexed_eq '/values/1="B"' decision_only_selector /id
direct_parity_run_row build/direct-probe/array-scalar-100k.ndjson array_scalar_100k \
  array_scalar_wildcard_eq '/values/[]="B"' decision_only_selector /id
direct_parity_run_row build/direct-probe/array-scalar-100k.ndjson array_scalar_100k \
  array_scalar_exists_wildcard 'exists{/values/[]}' decision_only_selector /id
direct_parity_run_row build/direct-probe/array-exists-100k.ndjson array_exists_100k \
  array_exists_indexed 'exists{/values/1}' decision_only_selector /id
# Do not add exists{/values/[]} on array-exists-100k until the Go long-stream
# oracle anomaly is resolved. C and Go agree on the array-scalar wildcard-exists
# fixture above, but the pinned Go benchmark reports one fewer match on
# array-exists-100k.ndjson.
direct_parity_run_row build/direct-probe/range-code-100k.ndjson range_code_100k \
  range_code_eq_one 'range{field=/code,gte=1,lte=1}' decision_only_selector /id
direct_parity_run_row build/direct-probe/recursive-10k.ndjson recursive_10k \
  recursive_eq '/.../sku="needle"' decision_only_selector /id
direct_parity_run_row build/direct-probe/indexed-10k.ndjson indexed_10k \
  indexed_eq '/items/1/sku="B"' decision_only_selector /id
direct_parity_run_row build/direct-probe/realworld-100k.ndjson realworld_100k \
  realworld_eq_sparse '/event="session_sync"' decision_only_selector /id
direct_parity_run_row build/direct-probe/realworld-100k.ndjson realworld_100k \
  realworld_eq_dense '/component="edge"' decision_only_selector /id
direct_parity_run_row build/direct-probe/realworld-100k.ndjson realworld_100k \
  realworld_nested_eq_sparse '/query/hash="c5d2460186f7233c927e7db2dcc703c0a3a8e0d5f0d8a3c5b4f1e2d3c4b5a697"' decision_only_selector /id
direct_parity_run_row build/direct-probe/realworld-100k.ndjson realworld_100k \
  realworld_contains_event_sparse 'contains{field=/event,value=sync}' decision_only_selector /id
direct_parity_run_row build/direct-probe/realworld-100k.ndjson realworld_100k \
  realworld_multi_clause_and '/component="edge",/event="session_sync",/active_idx=0,/tab_count=1,/code>=10' decision_only_selector /id
direct_parity_run_row build/direct-probe/realworld-100k.ndjson realworld_100k \
  realworld_array_eq_sparse '/session_ids[]="sid-0a3f-target"' decision_only_selector /id
direct_parity_run_row build/direct-probe/realworld-100k.ndjson realworld_100k \
  realworld_recursive_nested_eq_sparse '/.../hash="c5d2460186f7233c927e7db2dcc703c0a3a8e0d5f0d8a3c5b4f1e2d3c4b5a697"' decision_only_selector /id
direct_parity_run_row build/direct-probe/realworld-100k.ndjson realworld_100k \
  realworld_icontains_component_dense 'icontains{field=/component,value=EDGE}' decision_only_selector /id
direct_parity_run_row build/direct-probe/realworld-100k.ndjson realworld_100k \
  realworld_contains_any_event_sparse 'contains{field=/event,any=sync|__nope__}' decision_only_selector /id
direct_parity_run_row build/direct-probe/lockd-100k.ndjson lockd_100k \
  lockd_session_sync '/event="session_sync"' decision_only_selector /id
direct_parity_run_row build/direct-probe/lockd-100k.ndjson lockd_100k \
  lockd_tabs_update '/event="tabs_update"' decision_only_selector /id
direct_parity_run_row build/direct-probe/lockd-100k.ndjson lockd_100k \
  lockd_write_event '/op="write"' decision_only_selector /id
direct_parity_run_row build/direct-probe/large-4x25m.ndjson large_4x25m \
  eq_status_open_top_set '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/large-4x25m.ndjson large_4x25m \
  eq_status_open_top_set '/status="open"' mutate_source_selector /id
direct_parity_run_row build/direct-probe/large-4x25m.ndjson large_4x25m \
  eq_status_open '/status="open"' project_source_selector /id
direct_parity_run_row build/direct-probe/large-4x25m.ndjson large_4x25m \
  eq_status_open_top_set '/status="open"' project_mutate_file_selector /id

direct_parity_validate
direct_parity_expect_records 296
