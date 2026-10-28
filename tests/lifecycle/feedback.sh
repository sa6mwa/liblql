#!/usr/bin/env bash
# Exercise the real Make graph with cheap stand-ins for tools and costly gates.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
work=$(mktemp -d "$root/build/tmp/feedback.XXXXXXXX")
trap 'sh "$root/scripts/remove_path.sh" "$work"' EXIT
checkout=$work/checkout
mkdir -p "$checkout/scripts" "$checkout/bin" "$checkout/reference/go-benchmark"
cp "$root/Makefile" "$checkout/"
export TRACE=$work/trace
fail() { printf 'feedback contract: %s\n' "$*" >&2; exit 1; }
cat >"$work/step" <<'STEP'
#!/bin/sh
name=${0##*/}
printf '%s %s\n' "$name" "$*" >>"$TRACE"
if [ "${FAIL_STEP:-}" = "$name" ]; then exit 63; fi
STEP
for script in check_owned_path.sh build.sh deps.sh check_lua_symbols.sh \
  check_development_runtime.sh check_valgrind.sh check_direct_parity_smoke.sh \
  check_direct_perf_gate.sh check_direct_parity_matrix.sh package.sh \
  package_clql.sh package-verify.sh run_linux_release_matrix.sh; do
  cp "$work/step" "$checkout/scripts/$script"
  chmod +x "$checkout/scripts/$script"
done
cp "$root/scripts/check_thread_stack.sh" "$checkout/scripts/"
for tool in cmake ctest go; do
  cp "$work/step" "$checkout/bin/$tool"
  chmod +x "$checkout/bin/$tool"
done
export PATH="$checkout/bin:$PATH"
gates='ctest-contract test thread-stack-check development-runtime-check valgrind sdk-parity-gate direct-perf-gate package-verify'
run_graph() {
  : >"$TRACE"
  make --no-print-directory -C "$checkout" prerelease TEST_ALL_GATES="$gates" \
    >"$work/make.log" 2>&1
}
count() { awk -v step="$1" '$1 == step { n++ } END { print n+0 }' "$TRACE"; }
once() { [[ $(count "$1") == 1 ]] || fail "$1 did not run exactly once"; }
run_graph || { cat "$work/make.log"; fail 'prerelease graph failed'; }
for step in build.sh check_development_runtime.sh check_valgrind.sh \
  check_direct_parity_smoke.sh check_direct_perf_gate.sh \
  check_direct_parity_matrix.sh package.sh package_clql.sh package-verify.sh \
  run_linux_release_matrix.sh; do
  once "$step"
done
[[ $(grep -Fc 'cmake --preset release' "$TRACE") == 1 ]] || fail 'release build configured twice'
[[ $(grep -Fc 'cmake --install build/release --prefix build/lua-sdk' "$TRACE") == 1 ]] || fail 'Lua SDK installed twice'
[[ $(count go) == 2 ]] || fail 'benchmark tools were rebuilt per parity gate'
[[ $(count ctest) == 4 ]] || fail 'native debug/release and musl debug/release coverage changed'
grep -F 'ctest --preset debug ' "$TRACE" >/dev/null || fail 'native debug tests omitted'
for preset in release musl-debug musl-release; do
  [[ $(grep -Fc "ctest --test-dir build/$preset " "$TRACE") == 1 ]] || fail "$preset tests duplicated or omitted"
done
previous=0
for step in check_direct_parity_smoke.sh check_direct_perf_gate.sh package-verify.sh \
  check_direct_parity_matrix.sh run_linux_release_matrix.sh; do
  current=$(awk -v step="$step" '$1 == step { print NR }' "$TRACE")
  (( current > previous )) || fail 'release proof stages ran out of order'
  previous=$current
done
for step in check_direct_parity_smoke.sh check_direct_parity_matrix.sh; do
  export FAIL_STEP=$step
  if run_graph; then fail "accepted failed gate $step"; fi
  once "$step"
  [[ $(count run_linux_release_matrix.sh) == 0 ]] || fail 'packaged after a failed gate'
done
unset FAIL_STEP

# Keep every heap scenario, the reused pass, and the eight-repeat sentinel.
mkdir -p "$checkout/build/direct-probe"
for fixture in status-100k large-4x25m project-mutate-status-100k realworld-100k large-100m; do
  : >"$checkout/build/direct-probe/$fixture.ndjson"
done
cp "$work/step" "$checkout/scripts/ensure_direct_parity_fixtures.sh"
cp "$root/scripts/remove_path.sh" "$checkout/scripts/"
cp "$root/scripts/check_direct_live_heap.sh" "$checkout/scripts/"
cp "$work/step" "$checkout/bin/bench"
chmod +x "$checkout/bin/bench"
cat >"$checkout/bin/valgrind" <<'MASSIF'
#!/bin/sh
printf 'massif %s %s\n' "$LQL_BENCH_SAMPLES" "$*" >>"$TRACE"
for arg; do
  case "$arg" in
    --massif-out-file=*) printf 'mem_heap_B=%s\n' "${MASSIF_PEAK:-4096}" >"${arg#*=}" ;;
  esac
done
MASSIF
chmod +x "$checkout/bin/valgrind"
export LQL_DIRECT_BENCH_PATH=$checkout/bin/bench
: >"$TRACE"
(cd "$checkout" && sh scripts/check_direct_live_heap.sh) >"$work/heap.log" 2>&1 || {
  cat "$work/heap.log"; fail 'heap inventory failed';
}
[[ $(count massif) == 24 ]] || fail 'heap cases omitted'
[[ $(awk '$1 == "massif" && $2 == 1 { n++ } END { print n+0 }' "$TRACE") == 23 ]] || fail 'ordinary heap sample policy changed'
[[ $(awk '$1 == "massif" && $2 == 8 { n++ } END { print n+0 }' "$TRACE") == 1 ]] || fail 'retention sentinel omitted'
[[ $(grep -c -- '--submode steady_state' "$TRACE") == 24 ]] || fail 'cold/reused heap coverage omitted'
: >"$TRACE"
if (cd "$checkout" && MASSIF_PEAK=262145 sh scripts/check_direct_live_heap.sh) >"$work/heap-fail.log" 2>&1; then
  fail 'accepted peak above 256 KiB'
fi
grep -F 'direct live heap exceeded' "$work/heap-fail.log" >/dev/null || fail 'heap failure was not actionable'
[[ $(count massif) == 1 ]] || fail 'heap gate continued after failure'
printf 'feedback contract: prerequisite reuse, serial stages, failure stops and all 24 heap cases passed\n'
