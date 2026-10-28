#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
work=$(mktemp -d "$TMPDIR/lifecycle-integration.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
fail() { printf 'lifecycle integration: %s\n' "$*" >&2; exit 1; }

# Configuration must report missing preparation without creating a cache or
# attempting provisioning, including when the helper path contains spaces.
mkdir -p "$work/helpers with spaces"
cat > "$work/helpers with spaces/cpkt-toolchains.sh" <<'RESOLVER'
#!/bin/sh
printf '%s %s\n' "$1" "$2" >> "$TEST_RESOLVER_LOG"
[ "$1" = discover ] || exit 91
printf 'status=missing\n'
RESOLVER
chmod +x "$work/helpers with spaces/cpkt-toolchains.sh"
for target in x86_64-linux-gnu aarch64-linux-musl; do
  if env -u LIBLQL_TOOLCHAIN_OVERRIDE TEST_RESOLVER_LOG="$work/resolver.log" \
    cmake -S "$root" -B "$work/$target" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$root/cmake/cpkt-toolchain.cmake" \
      -DCPKT_LIFECYCLE_SCRIPTS_DIR="$work/helpers with spaces" \
      -DLQL_TARGET_ID="$target" > "$work/configure.log" 2>&1; then
    fail 'configuration accepted missing tools'
  fi
  grep -Fq "ensure $target" "$work/configure.log" || fail 'missing preparation instruction'
done
printf 'discover x86_64-linux-gnu\ndiscover aarch64-linux-musl\n' > "$work/expected.log"
cmp "$work/expected.log" "$work/resolver.log" || fail 'configuration provisioned or selected wrong target'
if env -u LIBLQL_TOOLCHAIN_OVERRIDE CPKT_TOOLCHAIN_CACHE="$work/missing-cache" \
  cmake -S "$root" -B "$work/missing" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/cpkt-toolchain.cmake" \
    > "$work/missing.log" 2>&1; then
  fail 'configuration accepted missing cache'
fi
[[ ! -e "$work/missing-cache" ]] || fail 'configuration created toolchain cache'

# Validate registered CTest properties, including missing and invalid metadata.
mkdir -p "$work/metadata"
cat > "$work/metadata/CMakeLists.txt" <<'PROJECT'
cmake_minimum_required(VERSION 3.20)
project(metadata_fixture NONE)
enable_testing()
if(NOT TEST_EMPTY)
  add_test(NAME fixture COMMAND "${CMAKE_COMMAND}" -E true)
  if(DEFINED TEST_TIMEOUT)
    set_tests_properties(fixture PROPERTIES TIMEOUT "${TEST_TIMEOUT}")
  endif()
  set_tests_properties(fixture PROPERTIES LABELS "${TEST_LABELS}")
endif()
include("${TEST_CONTRACT}")
lql_verify_test_contract()
PROJECT
for metadata in valid missing-timeout long-timeout no-local unknown-label empty; do
  metadata_args=(-DTEST_LABELS=unit\;local -DTEST_TIMEOUT=10)
  case "$metadata" in
    missing-timeout) metadata_args=(-DTEST_LABELS=local) ;;
    long-timeout) metadata_args+=(-DTEST_TIMEOUT=61) ;;
    no-local) metadata_args+=(-DTEST_LABELS=unit) ;;
    unknown-label) metadata_args+=(-DTEST_LABELS=local\;unknown) ;;
    empty) metadata_args+=(-DTEST_EMPTY=ON) ;;
  esac
  if cmake -S "$work/metadata" -B "$work/metadata-$metadata" \
    -DTEST_CONTRACT="$root/cmake/LqlTestContract.cmake" "${metadata_args[@]}" \
    > "$work/metadata-$metadata.log" 2>&1; then
    [[ "$metadata" == valid ]] || fail "accepted $metadata CTest metadata"
  else
    [[ "$metadata" != valid ]] || { cat "$work/metadata-valid.log"; fail 'rejected valid CTest metadata'; }
  fi
done

# Exercise actual cleanup in an isolated checkout-shaped fixture. Local hidden
# outputs are disposable; source files and the unrelated cache survive.
mkdir -p "$work/checkout/scripts" "$work/checkout/build/.hidden" \
  "$work/checkout/dist/.hidden" "$work/checkout/.cache/.hidden" "$work/global-cache"
cp "$root/scripts/clean.sh" "$root/scripts/remove_path.sh" "$root/scripts/check_owned_path.sh" "$work/checkout/scripts/"
touch "$work/checkout/source.c" "$work/global-cache/keep"
sh "$work/checkout/scripts/clean.sh" dist
[[ ! -e "$work/checkout/dist" && -d "$work/checkout/build/.hidden" &&
   -d "$work/checkout/.cache/.hidden" ]] || fail 'scoped clean removed unrelated outputs'
sh "$work/checkout/scripts/clean.sh"
[[ ! -e "$work/checkout/build" && ! -e "$work/checkout/.cache" &&
   -f "$work/checkout/source.c" && -f "$work/global-cache/keep" ]] || fail 'full clean violated ownership'
printf 'lifecycle integration passed\n'
