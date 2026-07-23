#!/bin/sh
set -eu

. scripts/direct_parity_common.sh

# Keep the hard performance gate representative rather than exhaustive.  The
# broad SDK parity matrix owns semantic coverage; this gate samples the rows
# most likely to regress speed, RSS, source-range replay, mutation, file-backed
# values, recursive selectors, and large-input streaming behavior.
direct_parity_init 'direct perf gate' build/direct-perf-gate.jsonl 2

sh scripts/ensure_direct_parity_fixtures.sh

direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open '/status="open"' decision_only_selector /id

# The 129-clause distinct miss is a semantic stress case for the spooled
# compatibility scanner above the machine-word term width.  It remains in the
# broad SDK parity matrix; it is intentionally not a hard perf sentinel because
# the required replay makes it close to Go and too noisy for a release gate.
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  contains_payload_cde 'contains{f=/payload,v=cde}' plus_value_source_selector /id
direct_parity_run_row build/direct-probe/scalar-100k.ndjson scalar_100k \
  code_eq_one '/code=1' plus_value_source_selector /id
direct_parity_run_row build/direct-probe/temporal-100k.ndjson temporal_100k \
  date_window 'date{field=/timestamp,after=2026-03-05T10:28:21Z,before=2026-03-05T10:30:00Z}' decision_only_selector /id
direct_parity_run_row build/direct-probe/project-mutate-status-100k.ndjson project_mutate_status_100k \
  project_mutation_status '/status="open"' project_mutate_file_selector /id
direct_parity_run_row build/direct-probe/nested-set-status-100k.ndjson nested_set_status_100k \
  eq_status_open_nested_set '/status="open"' mutate_file_selector /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open_file_backed_text '/status="open"' mutate_file_backed_text /id
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  eq_status_open_file_backed_base64 '/status="open"' mutate_file_backed_base64 /id
# Keep a multi-clause AND hard sentinel, but use the status fixture where C and
# Go timing is stable.  The broader semantic matrix retains the realworld
# multi-clause row; it is intentionally not a hard timing row because repeated
# runs hover around the 1.0x cutoff.
direct_parity_run_row build/direct-probe/status-100k.ndjson status_100k \
  and_status_open_region_west '/status="open",/region="us-west"' decision_only_selector /id
direct_parity_run_row build/direct-probe/realworld-100k.ndjson realworld_100k \
  realworld_recursive_nested_eq_sparse '/.../hash="c5d2460186f7233c927e7db2dcc703c0a3a8e0d5f0d8a3c5b4f1e2d3c4b5a697"' decision_only_selector /id
direct_parity_run_row build/direct-probe/large-4x25m.ndjson large_4x25m \
  eq_status_open_top_set '/status="open"' mutate_source_selector /id
direct_parity_run_row build/direct-probe/large-4x25m.ndjson large_4x25m \
  eq_status_open '/status="open"' project_source_selector /id

direct_parity_validate
direct_parity_expect_records 48
