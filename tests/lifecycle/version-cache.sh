#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
work=$(mktemp -d "$root/build/tmp/version-cache.XXXXXXXX")
trap 'sh "$root/scripts/remove_path.sh" "$work"' EXIT
fixture=$work/fixture
mkdir -p "$fixture/cmake"
cp "$root/cmake/LqlCachePaths.cmake" "$fixture/cmake/"
source "$root/scripts/fixture_cache_env.sh"
export XDG_CACHE_HOME=$work/xdg
global_root=$XDG_CACHE_HOME/cpkt
global_deps=$global_root/deps/${work##*/}
global_tools=$global_root/toolchains/${work##*/}
fail() { printf 'version cache: %s\n' "$*" >&2; exit 1; }

check() {
  local name=$1 deps=$2 tools=$3 expected_deps=$4 expected_tools=$5
  (export CPKT_DEPENDENCY_CACHE="$deps" CPKT_TOOLCHAIN_CACHE="$tools"
   lql_redirect_fixture_caches "$root" "$fixture"
   [[ "$CPKT_DEPENDENCY_CACHE" == "$expected_deps" &&
      "$CPKT_TOOLCHAIN_CACHE" == "$expected_tools" ]]
  ) >"$work/$name.log" 2>&1 || {
      cat "$work/$name.log"; fail "$name contract failed";
    }
  grep -Fx "version fixture: CPKT_DEPENDENCY_CACHE=$expected_deps" "$work/$name.log"
  grep -Fx "version fixture: CPKT_TOOLCHAIN_CACHE=$expected_tools" "$work/$name.log"
  # This is version/source metadata, not tool preparation. Both the original
  # and redirected cold caches must stay absent, including global selections.
  for path in "$deps" "$tools" "$expected_deps" "$expected_tools"; do
    [[ ! -e "$path" ]] || fail "version check provisioned cache: $path"
  done
}

case ${1:-} in
  local)
    check local "$root/.cache/${work##*/}-deps" "$work/tools" \
      "$fixture/.cache/${work##*/}-deps" "$fixture/${work#"$root/"}/tools" ;;
  local-spaces)
    check local-spaces "$work/deps with spaces" "$root/.cache/${work##*/} tools" \
      "$fixture/${work#"$root/"}/deps with spaces" "$fixture/.cache/${work##*/} tools" ;;
  global) check global "$global_deps" "$global_tools" "$global_deps" "$global_tools" ;;
  mixed) check mixed "$global_deps" "$work/tools" "$global_deps" "$fixture/${work#"$root/"}/tools" ;;
  explicit-only)
    unset HOME XDG_CACHE_HOME
    check explicit-only "$work/deps" "$work/tools" \
      "$fixture/${work#"$root/"}/deps" "$fixture/${work#"$root/"}/tools" ;;
  *) fail 'expected local, local-spaces, global, mixed or explicit-only case' ;;
esac
printf 'version cache: %s passed\n' "$1"
