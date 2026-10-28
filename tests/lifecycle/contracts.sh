#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
cd "$root"
work=$(mktemp -d "$root/build/tmp/lifecycle-contracts.XXXXXXXX")
trap 'sh "$root/scripts/remove_path.sh" "$work"' EXIT
fail(){ printf 'lifecycle contracts: %s\n' "$*" >&2; exit 1; }
# Reject unsafe output overrides and traversal without creating or deleting them.
for path in / "$HOME" "$root/README.md" "$root/build/../../outside" ''; do
  if sh scripts/check_owned_path.sh "$path" >"$work/path.log" 2>&1; then fail "accepted unsafe output: $path"; fi
done
mkdir -p "$work/checkout/scripts" "$work/checkout/build" "$work/sibling"
mkdir -p "$work/checkout/cmake"
cp cmake/LqlCachePaths.cmake "$work/checkout/cmake/"
cp scripts/{check_owned_path,remove_path}.sh "$work/checkout/scripts/"
touch "$work/checkout/source" "$work/sibling/keep"
if sh "$work/checkout/scripts/remove_path.sh" "$work/checkout/source"; then fail 'deleted source through cleanup'; fi
[[ -f "$work/checkout/source" ]] || fail 'source disappeared'
ln -s "$work/sibling" "$work/checkout/build/borrowed"
if sh "$work/checkout/scripts/remove_path.sh" "$work/checkout/build/borrowed/keep"; then fail 'accepted borrowed output'; fi
[[ -f "$work/sibling/keep" ]] || fail 'borrowed output disappeared'
for kind in deps toolchains; do
  cmake -DLQL_CACHE_PATH="$work/checkout/.cache/cold" -DLQL_CACHE_KIND="$kind" \
    -P "$work/checkout/cmake/LqlCachePaths.cmake"
  [[ ! -e "$work/checkout/.cache" ]] || fail 'cache inspection provisioned state'
  for path in relative "$work/sibling" "$work/checkout/build/borrowed/cold"; do
    if cmake -DLQL_CACHE_PATH="$path" -DLQL_CACHE_KIND="$kind" \
      -P "$work/checkout/cmake/LqlCachePaths.cmake" >"$work/cache-path.log" 2>&1; then fail "accepted borrowed cache: $path"; fi
  done
done
# A symlink replacing an entire owned root is still borrowed state.
ln -s "$work/sibling" "$work/checkout/.cache"
if cmake -DLQL_CACHE_PATH="$work/checkout/.cache/cold" -DLQL_CACHE_KIND=deps \
  -P "$work/checkout/cmake/LqlCachePaths.cmake" >"$work/cache-path.log" 2>&1; then fail 'accepted symlinked cache root'; fi
rm "$work/checkout/.cache"
# Required heap checks fail on absent inputs, before attempting instrumentation.
mkdir -p "$work/bin"
printf '#!/bin/sh\nexit 91\n' >"$work/bin/valgrind"; chmod +x "$work/bin/valgrind"
if PATH="$work/bin:$PATH" LQL_DIRECT_BENCH_PATH="$work/missing" sh scripts/check_direct_live_heap.sh >"$work/heap.log" 2>&1; then fail 'heap gate skipped missing binary'; fi
grep -F 'benchmark binary is unavailable' "$work/heap.log" >/dev/null
# Release preconditions run without creating tags, signing keys or Git fixtures.
cp scripts/check_release_commit.sh "$work/checkout/scripts/"
cat >"$work/bin/git" <<'GIT'
#!/bin/sh
case "$1" in
 symbolic-ref) case "$*" in *refs/remotes/origin/HEAD*) echo origin/trunk;; *) echo "${FIXTURE_BRANCH:-trunk}";; esac ;;
 diff-index) [ "${FIXTURE_DIRTY:-0}" = 0 ] ;;
 describe) echo v0.0.1 ;;
 cat-file) echo "${FIXTURE_TAG_TYPE:-commit}" ;;
 verify-commit) [ "${FIXTURE_SIGNATURE:-valid}" = valid ] ;;
 *) exit 92 ;;
esac
GIT
chmod +x "$work/bin/git"
PATH="$work/bin:$PATH" sh "$work/checkout/scripts/check_release_commit.sh" >"$work/release.log" 2>&1
for setting in FIXTURE_BRANCH=feature FIXTURE_DIRTY=1 FIXTURE_TAG_TYPE=tag FIXTURE_SIGNATURE=invalid; do
 if env PATH="$work/bin:$PATH" "$setting" sh "$work/checkout/scripts/check_release_commit.sh" >"$work/release.log" 2>&1; then fail "accepted invalid release: $setting"; fi
done
# Exercise actual archive entrypoints under differing umasks and timestamps.
for round in one two; do
 (
  if [[ "$round" = one ]]; then umask 022; else umask 077; fi
  mkdir -p "$work/$round/payload/nested"
  printf payload >"$work/$round/payload/nested/data"
  printf '#!/bin/sh\nexit 0\n' >"$work/$round/payload/run"; chmod u+x "$work/$round/payload/run"
  ln -s nested/data "$work/$round/payload/link"
  touch -t 203001010000 "$work/$round/payload/nested/data"
  sh scripts/create_tar_gz.sh "$work/$round" "$work/$round.tar.gz" payload
  LQL_DIST_DIR="$work/$round/dist" LQL_LUA_PACKAGE_BUILD_DIR="$work/$round/lua" sh scripts/package_lua.sh >"$work/$round.log" 2>&1
 )
done
cmp "$work/one.tar.gz" "$work/two.tar.gz" || fail 'tar bytes depend on staging metadata'
for archive in "$work/one/dist/"*; do cmp "$archive" "$work/two/dist/${archive##*/}" || fail 'Lua artifact is not deterministic'; done
# Target identity comes from artifact bytes as well as the chosen toolchain.
reader=$(command -v readelf)
cat >"$work/checkout/build/CMakeCache.txt" <<CACHE
CMAKE_C_COMPILER:FILEPATH=/usr/bin/cc
CMAKE_READELF:FILEPATH=$reader
LQL_TARGET_ID:STRING=aarch64-linux-gnu
CACHE
if scripts/check_artifact_architecture.sh /bin/sh aarch64-linux-gnu "$work/checkout/build" >"$work/arch.log" 2>&1; then fail 'accepted native bytes as aarch64'; fi
grep -F 'expected AArch64/ELF64' "$work/arch.log" >/dev/null
printf 'lifecycle contracts: output ownership, release preconditions, required inputs, archive bytes and target identity passed\n'
