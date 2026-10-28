#!/usr/bin/env bash
# Exercise version/tag/index behavior in an owned Git fixture, never in the
# developer's real index or tag namespace.
set -euo pipefail
# The fixture selects its own tagged/untagged/override cases. Candidate version
# settings, including Make command-line exports, must not alter those inputs.
unset LQL_VERSION_OVERRIDE MAKEFLAGS MAKEOVERRIDES
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
fixture=$root/build/lifecycle-version-contract-worktree
sh "$root/scripts/remove_path.sh" "$fixture"
mkdir -p "$fixture"
list=$root/build/lifecycle-version-contract-files
cd "$root"
git ls-files -z | while IFS= read -r -d '' path; do
  [[ ! -f "$path" && ! -L "$path" ]] || printf '%s\0' "$path"
done >"$list"
tar -cf - --null -T "$list" | tar -xf - -C "$fixture"
# A synthetic checkout owns its local caches. Global selections remain shared;
# never inherit a path into the caller's checkout-local state.
source "$root/scripts/fixture_cache_env.sh"
lql_redirect_fixture_caches "$root" "$fixture"
git -C "$fixture" init -q -b trunk
git -C "$fixture" config user.name 'liblql version fixture'
git -C "$fixture" config user.email 'fixture@invalid'
git -C "$fixture" add -A
git -C "$fixture" -c core.hooksPath=/dev/null -c commit.gpgSign=false \
  commit -qm 'test(version): seed isolated source fixture'
exec bash "$fixture/tests/lifecycle/version-contract.sh"
