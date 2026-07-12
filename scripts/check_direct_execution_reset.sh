#!/bin/sh
set -eu

legacy_pattern='lonejson_candidate_run|source_candidate_run|candidate[_ -](transform|action)|transform[_ -]stage|mutation_source_candidates|mutate_(file_range|source).*candidates|lql_(eval|project|mutation)_methods_install|query_(file|source|json)|payload_(write|project)|project_(file|source|json)|compact_(file|source|json)|matches_json|field_segment|predicate_depth|observer_|hit_(index|count)|contains_lps|match_sticky'

matches=$(git grep -n -E "$legacy_pattern" -- \
  ':!docs/liblql-direct-execution-spec.md' \
  ':!docs/liblql-direct-execution-handover.md' \
  ':!reference/**' \
  ':!scripts/check_direct_execution_reset.sh' \
  ':!scripts/check_direct_live_heap.sh' \
  ':!scripts/check_scanner_parity_smoke.sh' \
  ':!tools/lql_direct_bench.c' || true)
if [ -n "$matches" ]; then
  printf '%s\n' 'removed execution architecture remains:' >&2
  printf '%s\n' "$matches" >&2
  exit 1
fi

for path in \
  src/lql_eval.c \
  src/lql_project.c \
  src/lql_mutation.c \
  src/clql.c \
  bench \
  lua \
  parity; do
  if git ls-files --error-unmatch "$path" >/dev/null 2>&1; then
    printf '%s\n' "removed path remains tracked: $path" >&2
    exit 1
  fi
done
