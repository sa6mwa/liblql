#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
  printf 'usage: %s <version> <output-rockspec>\n' "$0" >&2
  exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=$1
output=$2
source_url=${LQL_LUA_SOURCE_URL:-"https://github.com/sa6mwa/liblql/releases/download/v${version}/liblql-lua-${version}.tar.gz"}

case "$source_url" in
  file://*)
    if [ "${LQL_ALLOW_LOCAL_LUA_SOURCE_URL:-0}" != "1" ]; then
      printf 'render-release-rockspec: refusing local source URL for release rockspec: %s\n' "$source_url" >&2
      exit 1
    fi
    ;;
esac

mkdir -p "$(dirname "$output")"
sed \
  -e "s#@VERSION@#$version#g" \
  -e "s#@LUA_SOURCE_URL@#$source_url#g" \
  "$root/liblql.rockspec.in" >"$output"
