#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
sh scripts/check_owned_path.sh "$root/build"
# Make owns native debug tests and the release build. Only the two distinct
# musl test configurations need preparation here.
sh scripts/deps.sh x86_64-linux-musl >/dev/null
ctest --test-dir build/release -R '^lql\.(thread_stack\.|workspace_alloc)' \
  --stop-on-failure --output-on-failure --no-tests=error
for preset in musl-debug musl-release; do
  cmake --preset "$preset"
  cmake --build "build/$preset" --target lql_thread_stack_test lql_workspace_alloc_test
  ctest --test-dir "build/$preset" -R '^lql\.(thread_stack\.|workspace_alloc)' \
    --stop-on-failure --output-on-failure --no-tests=error
done
