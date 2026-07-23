#!/bin/sh
set -eu

smoke=scripts/check_direct_parity_smoke.sh
matrix=scripts/check_direct_parity_matrix.sh
perf=scripts/check_direct_perf_gate.sh
common=scripts/direct_parity_common.sh

require_fixed() {
  file=$1
  needle=$2
  message=$3
  if ! grep -F -- "$needle" "$file" >/dev/null; then
    printf 'SDK parity contract: %s\n' "$message" >&2
    exit 1
  fi
}

require_fixed "$smoke" 'LQL_DIRECT_PARITY_MIN_C_GO_SPEEDUP=' \
  'sdk-parity-gate must not make every semantic row a one-sample timing gate'
require_fixed "$smoke" 'direct_parity_validate' \
  'sdk-parity-gate must validate counters, unsupported rows, and RSS policy'
require_fixed "$common" '--require-counter-parity' \
  'shared SDK parity validation must compare Go/C semantic counters independently of timing/RSS gates'
require_fixed "$smoke" 'direct_parity_expect_records 88' \
  'sdk-parity-gate record count must be explicit'
require_fixed "$smoke" 'direct_parity_expect_failure text' \
  'sdk-parity-gate must include text failure parity rows'
require_fixed "$smoke" 'direct_parity_expect_failure json' \
  'sdk-parity-gate must include selector JSON failure parity rows'

require_fixed "$matrix" 'LQL_DIRECT_PARITY_MIN_C_GO_SPEEDUP=' \
  'direct-parity-matrix must remain semantic-only for timing'
require_fixed "$matrix" 'LQL_DIRECT_PARITY_REQUIRE_C_RSS_BELOW_GO=0' \
  'direct-parity-matrix must not duplicate hard RSS timing policy'
require_fixed "$matrix" 'wide_129_distinct_miss' \
  'direct-parity-matrix must keep the >64-term SDK selector stress row'
require_fixed "$matrix" 'realworld_multi_clause_and' \
  'direct-parity-matrix must keep the broad realworld multi-clause semantic row'
require_fixed "$matrix" 'direct_parity_expect_records 320' \
  'direct-parity-matrix record count must be explicit'

require_fixed "$matrix" 'sdk_json_eq_status_open_source' \
  'selector AST JSON must be covered with selected-value callbacks'
require_fixed "$matrix" 'sdk_json_eq_status_open_project' \
  'selector AST JSON must be covered with projection'
require_fixed "$matrix" 'sdk_json_eq_status_open_mutate' \
  'selector AST JSON must be covered with mutation'
require_fixed "$matrix" 'sdk_json_project_mutation_status' \
  'selector AST JSON must be covered with project+mutation'
require_fixed "$matrix" 'sdk_json_date_window_matrix' \
  'selector AST JSON must be covered with temporal terms'
require_fixed "$matrix" 'sdk_json_array_scalar_wildcard_eq' \
  'selector AST JSON must be covered with array wildcard terms'
require_fixed "$matrix" 'large_4x25m' \
  'direct-parity-matrix must keep large-input SDK rows'

if grep -F 'LQL_DIRECT_PARITY_MIN_C_GO_SPEEDUP=' "$perf" >/dev/null; then
  printf '%s\n' \
    'SDK parity contract: direct-perf-gate must keep the default hard speed threshold' >&2
  exit 1
fi
if grep -F 'realworld_multi_clause_and' "$perf" >/dev/null; then
  printf '%s\n' \
    'SDK parity contract: noisy realworld semantic row must stay out of the hard perf gate' >&2
  exit 1
fi
require_fixed "$perf" 'and_status_open_region_west' \
  'direct-perf-gate must keep a stable multi-clause AND hard timing row'
require_fixed "$perf" 'direct_parity_expect_records 48' \
  'direct-perf-gate record count must be explicit'

printf '%s\n' 'SDK parity contract passed'
