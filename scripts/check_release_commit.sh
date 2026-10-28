#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
cd "$root"
fail() { printf 'release commit: %s\n' "$*" >&2; exit 1; }
branch=$(git symbolic-ref --quiet --short HEAD) || fail 'check out the default branch before release'
default=$(git symbolic-ref --quiet --short refs/remotes/origin/HEAD) || default=origin/trunk
default=${default#origin/}
[ "$branch" = "$default" ] || fail "release must run on $default, not $branch"
git diff-index --quiet HEAD -- || fail 'tracked worktree must be clean'
version=$(git describe --exact-match --tags --match 'v[0-9]*.[0-9]*.[0-9]*' HEAD) ||
  fail 'HEAD requires an exact lightweight vX.Y.Z release tag'
printf '%s\n' "$version" | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' >/dev/null || fail 'release tag must be vX.Y.Z'
[ "$(git cat-file -t "refs/tags/$version")" = commit ] || fail 'release tag must be lightweight'
git verify-commit HEAD || fail 'HEAD must have a verified signature before release'
printf 'release commit: verified %s on %s\n' "$version" "$branch"
