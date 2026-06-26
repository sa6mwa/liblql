#!/bin/sh
set -eu

check_release_surface() {
  makefile=$1
  release_script=$2
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
  if ! grep -F 'make print-release-assets' "$makefile" >/dev/null; then
    printf 'release surface: make help must advertise make print-release-assets\n' >&2
    exit 1
  fi
  if ! grep -Eq '^print-release-assets:' "$makefile" ||
     ! grep -F './scripts/package.sh print-release-assets' "$makefile" \
       >/dev/null; then
    printf 'release surface: print-release-assets must use checksum manifest helper\n' >&2
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
    if ! grep -F 'CMAKE_BUILD_WITH_INSTALL_RPATH ON' "$cmakelists" \
      >/dev/null; then
      printf 'release surface: Darwin builds must use install RPATH at build time\n' >&2
      exit 1
    fi
    if ! grep -F 'INSTALL_RPATH "@loader_path"' "$cmakelists" >/dev/null ||
       ! grep -F 'INSTALL_RPATH "@loader_path/../lib"' "$cmakelists" \
         >/dev/null; then
      printf 'release surface: Darwin runtime paths must be loader-relative\n' >&2
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
}

if [ "${1:-}" = "--fixtures" ]; then
  tmp=${TMPDIR:-/tmp}/liblql-release-surface-fixtures.$$
  trap 'rm -rf "$tmp"' EXIT HUP INT TERM
  mkdir -p "$tmp"
  makefile="$tmp/Makefile"
  release_script="$tmp/release_gate.sh"
  cmakelists="$tmp/CMakeLists.txt"

  cat >"$makefile" <<'EOF'
help:
	@printf '%s\n' 'make release' 'make bench-1g-check' 'make print-release-assets'
prerelease-hardening: prerelease bench-1g-check release-matrix
release:
	@./scripts/release_gate.sh
print-release-assets:
	@./scripts/package.sh print-release-assets
EOF
  cat >"$release_script" <<'EOF'
#!/bin/sh
scripts/clean.sh
make test-all
make bench-check
make bench-memory-check
make release-matrix
EOF
  cat >"$cmakelists" <<'EOF'
set(LQL_PROJECT_WARNINGS -Wall -Wextra -Wpedantic -Werror)
if(LQL_TARGET_OS STREQUAL "darwin")
  set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
endif()
function(lql_apply_project_warnings target)
endfunction()
add_library(lql_common OBJECT src/lql.c)
lql_apply_project_warnings(lql_common)
add_library(lql_shared SHARED src/lql.c)
set_target_properties(lql_shared PROPERTIES INSTALL_RPATH "@loader_path")
add_executable(clql src/clql.c)
set_target_properties(clql PROPERTIES INSTALL_RPATH "@loader_path/../lib")
lql_apply_project_warnings(clql)
EOF
  check_release_surface "$makefile" "$release_script" "$cmakelists"

  sed 's/ -Werror//' "$cmakelists" >"$tmp/no-werror.cmake"
  if (check_release_surface "$makefile" "$release_script" \
    "$tmp/no-werror.cmake" >/dev/null 2>&1); then
    printf 'release surface fixture: expected missing -Werror to fail\n' >&2
    exit 1
  fi

  sed '/lql_apply_project_warnings(clql)/d' "$cmakelists" \
    >"$tmp/missing-target-warning.cmake"
  if (check_release_surface "$makefile" "$release_script" \
    "$tmp/missing-target-warning.cmake" >/dev/null 2>&1); then
    printf 'release surface fixture: expected missing target warning policy to fail\n' >&2
    exit 1
  fi

  sed '/CMAKE_BUILD_WITH_INSTALL_RPATH/d' "$cmakelists" \
    >"$tmp/missing-darwin-build-rpath.cmake"
  if (check_release_surface "$makefile" "$release_script" \
    "$tmp/missing-darwin-build-rpath.cmake" >/dev/null 2>&1); then
    printf 'release surface fixture: expected missing Darwin build RPATH to fail\n' >&2
    exit 1
  fi

  exit 0
fi

makefile=${1:?usage: check_make_release_target.sh MAKEFILE RELEASE_SCRIPT [CMAKELISTS]}
release_script=${2:?usage: check_make_release_target.sh MAKEFILE RELEASE_SCRIPT [CMAKELISTS]}
cmakelists=${3:-}

check_release_surface "$makefile" "$release_script" "$cmakelists"
