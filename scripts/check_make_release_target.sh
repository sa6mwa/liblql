#!/bin/sh
set -eu

makefile=${1:?usage: check_make_release_target.sh MAKEFILE RELEASE_SCRIPT}
release_script=${2:?usage: check_make_release_target.sh MAKEFILE RELEASE_SCRIPT}

if ! grep -Eq '^release:' "$makefile"; then
  printf 'release surface: Makefile is missing release target\n' >&2
  exit 1
fi
if grep -F 'release requires explicit engineer-controlled tag/publish flow' \
  "$makefile" >/dev/null; then
  printf 'release surface: release target is still a placeholder\n' >&2
  exit 1
fi
if ! grep -F './scripts/release_gate.sh' "$makefile" >/dev/null; then
  printf 'release surface: release target must call scripts/release_gate.sh\n' >&2
  exit 1
fi
if ! grep -F 'make release' "$makefile" >/dev/null; then
  printf 'release surface: make help must advertise make release\n' >&2
  exit 1
fi

for required in 'scripts/clean.sh' 'test-all' 'bench-check' \
  'bench-memory-check' 'release-matrix'; do
  if ! grep -F "$required" "$release_script" >/dev/null; then
    printf 'release surface: release gate is missing %s\n' "$required" >&2
    exit 1
  fi
done
