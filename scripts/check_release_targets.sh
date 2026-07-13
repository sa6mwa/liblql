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

require_line '^release-pipeline: test-all release-matrix$' \
  'release-pipeline must run ordinary gates before release-matrix'
require_line '^prerelease: release-pipeline$' \
  'prerelease must share release-pipeline'
require_line '^release:$' \
  'release target is missing'

release_line=$(grep -n '^release:$' "$makefile" | cut -d: -f1 | head -1)
clean_line=$((release_line + 1))
pipeline_line=$((release_line + 2))
clean_cmd=$(sed -n "${clean_line}p" "$makefile")
pipeline_cmd=$(sed -n "${pipeline_line}p" "$makefile")
tab=$(printf '\t')

if [ "$clean_cmd" != "${tab}@\$(MAKE) clean" ]; then
  printf 'release target check: release must clean first\n' >&2
  exit 1
fi

if [ "$pipeline_cmd" != "${tab}@\$(MAKE) release-pipeline" ]; then
  printf 'release target check: release must invoke shared release-pipeline after clean\n' >&2
  exit 1
fi

printf 'release target check passed\n'
