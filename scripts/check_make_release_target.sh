#!/bin/sh
set -eu

makefile=${1:?usage: check_make_release_target.sh MAKEFILE RELEASE_SCRIPT [CMAKELISTS]}
release_script=${2:?usage: check_make_release_target.sh MAKEFILE RELEASE_SCRIPT [CMAKELISTS]}
cmakelists=${3:-}

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
if ! grep -F 'make bench-1g-check' "$makefile" >/dev/null; then
  printf 'release surface: make help must advertise make bench-1g-check\n' >&2
  exit 1
fi
if ! grep -Eq '^prerelease-hardening:.*bench-1g-check' "$makefile"; then
  printf 'release surface: prerelease-hardening must include bench-1g-check\n' >&2
  exit 1
fi

for required in 'scripts/clean.sh' 'test-all' 'bench-check' \
  'bench-memory-check' 'release-matrix'; do
  if ! grep -F "$required" "$release_script" >/dev/null; then
    printf 'release surface: release gate is missing %s\n' "$required" >&2
    exit 1
  fi
done

if [ -n "$cmakelists" ]; then
  if [ ! -f "$cmakelists" ]; then
    printf 'release surface: CMakeLists not found: %s\n' "$cmakelists" >&2
    exit 1
  fi
  if ! grep -F -- '-Werror' "$cmakelists" >/dev/null; then
    printf 'release surface: project warning flags must include -Werror\n' >&2
    exit 1
  fi
  for target in lql_common clql lql_payload_bench lql_match_example \
    lql_lua_core test_lql lql_handle_allocator_test lql_fuzz_smoke; do
    if grep -Eq "add_(library|executable)\\(${target}([[:space:]]|\\))" \
      "$cmakelists" &&
      ! grep -F "lql_apply_project_warnings(${target})" "$cmakelists" \
        >/dev/null; then
      printf 'release surface: project target lacks warning-as-error policy: %s\n' "$target" >&2
      exit 1
    fi
  done
fi
