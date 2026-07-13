#!/bin/sh
set -eu

ensure_fixture() {
  fixture=$1
  count=$2
  payload_bytes=$3
  shape=$4

  if [ ! -f "$fixture" ]; then
    sh scripts/generate_direct_probe_fixture.sh \
      "$fixture" "$count" "$payload_bytes" "$shape"
  fi
}

ensure_fixture build/direct-probe/status-100k.ndjson 100000 24 status
ensure_fixture build/direct-probe/status-whitespace-100k.ndjson 100000 24 statusws
ensure_fixture build/direct-probe/scalar-100k.ndjson 100000 24 scalar
ensure_fixture build/direct-probe/recursive-10k.ndjson 10000 24 recursive
ensure_fixture build/direct-probe/indexed-10k.ndjson 10000 24 indexed
ensure_fixture build/direct-probe/realworld-100k.ndjson 100000 64 realworld
ensure_fixture build/direct-probe/lockd-100k.ndjson 100000 64 lockd
ensure_fixture build/direct-probe/large-4x25m.ndjson 4 25000000 status
ensure_fixture build/direct-probe/large-100m.ndjson 1 100000000 status

ensure_fixture build/direct-probe/top-set-status-100k.ndjson 100000 24 status
ensure_fixture build/direct-probe/top-remove-status-100k.ndjson 100000 24 status
ensure_fixture build/direct-probe/top-increment-code-100k.ndjson 100000 24 scalar
ensure_fixture build/direct-probe/top-multi-code-100k.ndjson 100000 24 scalar

ensure_fixture build/direct-probe/nested-set-status-100k.ndjson 100000 24 nestedprojection
ensure_fixture build/direct-probe/nested-increment-100k.ndjson 100000 24 nestedprojection
ensure_fixture build/direct-probe/nested-remove-status-100k.ndjson 100000 24 nestedprojection
ensure_fixture build/direct-probe/same-top-nested-increment-100k.ndjson 100000 24 nestedprojection
ensure_fixture build/direct-probe/mixed-nested-code-100k.ndjson 100000 24 scalar

ensure_fixture build/direct-probe/project-mutate-status-100k.ndjson 100000 24 status
ensure_fixture build/direct-probe/temporal-100k.ndjson 100000 24 temporal
ensure_fixture build/direct-probe/array-scalar-100k.ndjson 100000 24 arrayscalar
ensure_fixture build/direct-probe/array-exists-100k.ndjson 100000 24 arrayexists
ensure_fixture build/direct-probe/range-code-100k.ndjson 100000 24 rangecode
