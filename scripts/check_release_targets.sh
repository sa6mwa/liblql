#!/bin/sh
set -eu

makefile=Makefile

require_line() {
  pattern=$1
  message=$2
  if ! grep -E "$pattern" "$makefile" >/dev/null; then
    printf 'release target check: %s\n' "$message" >&2
    exit 1
  fi
}

require_line '^release-pipeline:$' \
  'release-pipeline target is missing'
require_line '^prerelease: release-pipeline$' \
  'prerelease must share release-pipeline'
require_line '^lifecycle-version-contract:$' \
  'lifecycle-version-contract target is missing'
require_line '^prerelease-hardening: prerelease$' \
  'prerelease-hardening must preserve the shared prerelease graph'
require_line '^package-checksums:$' \
  'package-checksums target is missing'
require_line '^verify-release-archives:$' \
  'verify-release-archives target is missing'
require_line '^verify-release-privacy:$' \
  'verify-release-privacy target is missing'
require_line '^clean-dist:$' \
  'clean-dist target is missing'
require_line '^direct-parity-matrix: direct-bench$' \
  'direct-parity-matrix must be the public parity matrix target'
require_line '^bench-gate: direct-parity-matrix$' \
  'bench-gate must enforce the accepted direct parity matrix'
require_line '^release:$' \
  'release target is missing'

old_phase=scanner
if grep -E "(^|[^[:alnum:]_-])(${old_phase}-parity|${old_phase}-profile|debug-${old_phase}|release-${old_phase})([^[:alnum:]_-]|$)" "$makefile" >/dev/null; then
  printf 'release target check: legacy public lifecycle target remains\n' >&2
  exit 1
fi

release_line=$(grep -n '^release:$' "$makefile" | cut -d: -f1 | head -1)
pipeline_target_line=$(grep -n '^release-pipeline:$' "$makefile" |
  cut -d: -f1 | head -1)
pipeline_test_line=$((pipeline_target_line + 1))
pipeline_matrix_line=$((pipeline_target_line + 2))
version_line=$((release_line + 1))
clean_line=$((release_line + 2))
pipeline_line=$((release_line + 3))
version_cmd=$(sed -n "${version_line}p" "$makefile")
clean_cmd=$(sed -n "${clean_line}p" "$makefile")
pipeline_cmd=$(sed -n "${pipeline_line}p" "$makefile")
tab=$(printf '\t')
pipeline_test_cmd=$(sed -n "${pipeline_test_line}p" "$makefile")
pipeline_matrix_cmd=$(sed -n "${pipeline_matrix_line}p" "$makefile")

if [ "$pipeline_test_cmd" != "${tab}@\$(MAKE) --no-print-directory -f Makefile test-all" ]; then
  printf 'release target check: release-pipeline must run test-all first\n' >&2
  exit 1
fi

if [ "$pipeline_matrix_cmd" != "${tab}@\$(MAKE) --no-print-directory -f Makefile release-matrix" ]; then
  printf 'release target check: release-pipeline must run release-matrix after test-all\n' >&2
  exit 1
fi

if [ "$version_cmd" != "${tab}@\$(MAKE) lifecycle-version-contract" ]; then
  printf 'release target check: release must run lifecycle-version-contract first\n' >&2
  exit 1
fi

if [ "$clean_cmd" != "${tab}@\$(MAKE) clean" ]; then
  printf 'release target check: release must clean after version contract\n' >&2
  exit 1
fi

if [ "$pipeline_cmd" != "${tab}@\$(MAKE) release-pipeline" ]; then
  printf 'release target check: release must invoke shared release-pipeline after clean\n' >&2
  exit 1
fi

printf 'release target check passed\n'
