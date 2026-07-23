#!/bin/sh
set -eu

. scripts/direct_parity_common.sh

# This is the canonical public-SDK language parity gate.  It enforces Go/C
# counters, output invariants, unsupported-row failures, and C RSS below Go.
# Hard speed thresholds live in direct-perf-gate, where representative timing
# rows use stable sampling instead of making every semantic row a timing gate.
LQL_DIRECT_PARITY_MIN_C_GO_SPEEDUP=
direct_parity_init 'liblql SDK parity gate' build/sdk-parity-gate.jsonl 1

sh scripts/ensure_direct_parity_fixtures.sh

direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  sdk_text_eq_status_open 'eq{field=/status,value=open}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  sdk_text_contains_payload 'contains{field=/payload,value=cde}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  sdk_text_in_status 'in{field=/status,any=open|closed}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  sdk_text_or_status 'or.0.eq{field=/status,value=open},or.1.eq{field=/status,value=closed}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  sdk_text_not_status 'not.eq{field=/status,value=closed}' decision_only_selector /id
direct_parity_run_row build/direct-probe/scalar-100k.ndjson scalar_100k \
  sdk_text_range_code 'range{field=/code,gte=1,lte=1}' decision_only_selector /id
direct_parity_run_json_row build/direct-probe/status-100k.ndjson status_100k \
  sdk_json_eq_status_open '{"eq":{"field":"/status","value":"open"}}' decision_only_selector /id
direct_parity_run_json_row build/direct-probe/status-100k.ndjson status_100k \
  sdk_json_contains_any_payload '{"contains":{"field":"/payload","any":["cde","__nope__"]}}' decision_only_selector /id
direct_parity_run_json_row build/direct-probe/status-100k.ndjson status_100k \
  sdk_json_or_status '{"or":[{"eq":{"field":"/status","value":"open"}},{"eq":{"field":"/status","value":"closed"}}]}' decision_only_selector /id
direct_parity_run_json_row build/direct-probe/status-100k.ndjson status_100k \
  sdk_json_and_not_status '{"and":[{"eq":{"field":"/region","value":"us-west"}},{"not":{"eq":{"field":"/status","value":"closed"}}}]}' decision_only_selector /id
direct_parity_run_json_row build/direct-probe/scalar-100k.ndjson scalar_100k \
  sdk_json_range_code '{"range":{"field":"/code","gte":1,"lte":1}}' decision_only_selector /id
direct_parity_run_json_row build/direct-probe/temporal-100k.ndjson temporal_100k \
  sdk_json_date_window '{"date":{"field":"/timestamp","after":"2026-03-05T10:28:21Z","before":"2026-03-05T10:30:00Z"}}' decision_only_selector /id
direct_parity_expect_failure text build/direct-probe/status-100k.ndjson status_100k \
  sdk_invalid_text_bare_eq 'eq{field=status,value=open}' decision_only_selector /id
direct_parity_expect_failure text build/direct-probe/status-100k.ndjson status_100k \
  sdk_invalid_text_bare_contains 'contains{field=payload,value=cde}' decision_only_selector /id
direct_parity_expect_failure json build/direct-probe/status-100k.ndjson status_100k \
  sdk_invalid_json_bare_eq '{"eq":{"field":"status","value":"open"}}' decision_only_selector /id
direct_parity_expect_failure json build/direct-probe/scalar-100k.ndjson scalar_100k \
  sdk_invalid_json_bare_range '{"range":{"field":"code","gte":1}}' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' plus_value_source_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' project_file_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open_top_set '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/nested-set-status-100k.ndjson nested_set_status_100k \
  eq_status_open_nested_set '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/top-increment-code-100k.ndjson top_increment_code_100k \
  eq_code_one_top_increment '/code=1' mutate_source_selector /id
direct_parity_run_row build/direct-probe/project-mutate-status-100k.ndjson project_mutate_status_100k \
  project_mutation_status '/status="open"' project_mutate_file_selector /id
direct_parity_run_row build/direct-probe/temporal-100k.ndjson temporal_100k \
  date_window 'date{field=/timestamp,after=2026-03-05T10:28:21Z,before=2026-03-05T10:30:00Z}' decision_only_selector /id
direct_parity_run_row build/direct-probe/array-scalar-100k.ndjson array_scalar_100k \
  array_scalar_indexed_eq '/values/1="B"' decision_only_selector /id
direct_parity_run_row build/direct-probe/large-4x25m.ndjson large_4x25m \
  eq_status_open_top_set '/status="open"' mutate_file_selector /id

direct_parity_validate
direct_parity_expect_records 88
