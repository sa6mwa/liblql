#!/bin/sh
set -eu

resolve_git_version() {
  tags=$(git tag --points-at HEAD --list 'v[0-9]*.[0-9]*.[0-9]*' 2>/dev/null || true)
  resolved=
  for tag in $tags; do
    if ! printf '%s\n' "$tag" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$'; then
      continue
    fi
    tag_type=$(git cat-file -t "refs/tags/$tag" 2>/dev/null || true)
    if [ "$tag_type" != commit ]; then
      continue
    fi
    version=${tag#v}
    if [ -n "$resolved" ] && [ "$resolved" != "$version" ]; then
      printf 'release version: multiple lightweight release tags point at HEAD: %s and %s\n' \
        "$resolved" "$version" >&2
      exit 1
    fi
    resolved=$version
  done
  if [ -n "$resolved" ]; then
    printf '%s\n' "$resolved"
    return 0
  fi
  return 1
}

if [ "${LQL_VERSION_OVERRIDE:-}" ]; then
  printf '%s\n' "$LQL_VERSION_OVERRIDE"
  exit 0
fi
if resolve_git_version; then
  exit 0
fi
if [ ! -d .git ] && [ -f VERSION ]; then
  sed -n '1p' VERSION
else
  printf '0.0.0\n'
fi
