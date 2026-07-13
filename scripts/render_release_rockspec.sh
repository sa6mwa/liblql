#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
  printf 'usage: %s <version> <source-url> <source-dir> <output>\n' "$0" >&2
  exit 2
fi

version=$1
source_url=$2
source_dir=$3
output=$4
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

mkdir -p "$(dirname "$output")"
sed \
  -e "s|@LQL_ROCK_VERSION@|$version-1|g" \
  -e "s|@LQL_ROCK_SOURCE_URL@|$source_url|g" \
  -e "s|@LQL_ROCK_SOURCE_DIR@|$source_dir|g" \
  "$root/liblql-dev-1.rockspec.in" >"$output"
