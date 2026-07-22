#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${LQL_LUA_PRIVACY_REGRESSION_DIR:-"$root/build/lua-privacy-regression"}
project=liblql

artifact_version() {
  manifest=$(find "$1" -maxdepth 1 -name "$project-*-CHECKSUMS" -type f | sort | sed -n '1p')
  basename "$manifest" | sed "s/^$project-//;s/-CHECKSUMS\$//"
}

refresh_checksums() {
  dist=$1
  version=$2
  (
    cd "$dist"
    sha256sum \
      "$project-lua-$version.tar.gz" \
      "$project-$version-1.rockspec" \
      "$project-$version-1.src.rock" >"$project-$version-CHECKSUMS"
  )
}

expect_verify_failure() {
  name=$1
  dist=$2
  if LQL_DIST_DIR="$dist" sh "$root/scripts/package-verify.sh" lua >/dev/null 2>&1; then
    printf 'lua privacy regression: expected verification failure for %s\n' "$name" >&2
    exit 1
  fi
  printf 'lua privacy regression: %s rejected as expected\n' "$name"
}

build_case() {
  name=$1
  dist=$work/$name/dist
  sh "$root/scripts/remove_path.sh" "$work/$name"
  mkdir -p "$dist"
  LQL_DIST_DIR="$dist" sh "$root/scripts/package_lua.sh" >/dev/null
  version=$(artifact_version "$dist")
  printf '%s\n' "$version"
}

version=$(build_case rockspec-home-file-url)
dist=$work/rockspec-home-file-url/dist
printf '%s\n' "-- file://${HOME:-/home}/liblql-leak" >>"$dist/$project-$version-1.rockspec"
refresh_checksums "$dist" "$version"
expect_verify_failure rockspec-home-file-url "$dist"

version=$(build_case lua-source-repo-file-url)
dist=$work/lua-source-repo-file-url/dist
mutate=$work/lua-source-repo-file-url/mutate
mkdir -p "$mutate"
tar -xzf "$dist/$project-lua-$version.tar.gz" -C "$mutate"
printf 'file://%s/lua/lql_core.c\n' "$root" >"$mutate/$project-lua-$version/LEAK"
tar -C "$mutate" --sort=name --owner=0 --group=0 --numeric-owner \
  -czf "$dist/$project-lua-$version.tar.gz" "$project-lua-$version"
refresh_checksums "$dist" "$version"
expect_verify_failure lua-source-repo-file-url "$dist"

version=$(build_case lua-source-package-manager-temp)
dist=$work/lua-source-package-manager-temp/dist
mutate=$work/lua-source-package-manager-temp/mutate
mkdir -p "$mutate"
tar -xzf "$dist/$project-lua-$version.tar.gz" -C "$mutate"
printf '/tmp/luarocks-build-liblql\n' >"$mutate/$project-lua-$version/LEAK"
tar -C "$mutate" --sort=name --owner=0 --group=0 --numeric-owner \
  -czf "$dist/$project-lua-$version.tar.gz" "$project-lua-$version"
refresh_checksums "$dist" "$version"
expect_verify_failure lua-source-package-manager-temp "$dist"

version=$(build_case src-rock-live-worktree-path)
dist=$work/src-rock-live-worktree-path/dist
mutate=$work/src-rock-live-worktree-path/mutate
mkdir -p "$mutate"
unzip -q "$dist/$project-$version-1.src.rock" -d "$mutate"
printf '%s/lua/lql_core.c\n' "$root" >"$mutate/LEAK"
(
  cd "$mutate"
  zip -X -q "$dist/$project-$version-1.src.rock" \
    "$project-$version-1.rockspec" "$project-lua-$version.tar.gz" LEAK
)
refresh_checksums "$dist" "$version"
expect_verify_failure src-rock-live-worktree-path "$dist"
