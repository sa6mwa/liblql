#!/bin/sh
set -eu

. scripts/direct_parity_common.sh

direct_parity_init 'direct parity smoke' build/direct-parity-smoke.jsonl 5

sh scripts/ensure_direct_parity_fixtures.sh

direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' decision_only_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' plus_value_source_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' project_file_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open_top_set '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/nested-set-status-100k.ndjson nested_set_status_100k \
  eq_status_open_nested_set '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open_file_backed_text '/status="open"' mutate_file_backed_text /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open_file_backed_base64 '/status="open"' mutate_file_backed_base64 /id
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
direct_parity_expect_records 48
