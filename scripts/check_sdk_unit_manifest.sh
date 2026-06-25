#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  printf 'usage: %s tests/test_lql.c\n' "$0" >&2
  exit 2
fi

test_file=$1
if [ ! -f "$test_file" ]; then
  printf 'SDK unit manifest check: missing test file: %s\n' "$test_file" >&2
  exit 2
fi

tmp_dir=${TMPDIR:-/tmp}/liblql-sdk-manifest.$$
mkdir -p "$tmp_dir"
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

functions_file=$tmp_dir/functions
manifest_file=$tmp_dir/manifest
manifest_all_file=$tmp_dir/manifest-all
main_file=$tmp_dir/main
main_all_file=$tmp_dir/main-all

sed -n 's/^static void \(expect_[A-Za-z0-9_]*\)(void) {$/\1/p' "$test_file" |
  grep -v '^expect_sdk_parity_manifest$' |
  sort > "$functions_file"

awk '
  /static void expect_sdk_parity_manifest\(void\)/ { in_manifest = 1 }
  in_manifest && /};/ { in_manifest = 0 }
  in_manifest {
    while (match($0, /expect_[A-Za-z0-9_]*/)) {
      print substr($0, RSTART, RLENGTH)
      $0 = substr($0, RSTART + RLENGTH)
    }
  }
' "$test_file" | grep -v '^expect_sdk_parity_manifest$' | sort > "$manifest_all_file"
sort -u "$manifest_all_file" > "$manifest_file"

awk '
  /^int main\(void\)/ { in_main = 1 }
  in_main && /^}/ { in_main = 0 }
  in_main {
    while (match($0, /expect_[A-Za-z0-9_]*/)) {
      print substr($0, RSTART, RLENGTH)
      $0 = substr($0, RSTART + RLENGTH)
    }
  }
' "$test_file" | grep -v '^expect_sdk_parity_manifest$' | sort > "$main_all_file"
sort -u "$main_all_file" > "$main_file"

if [ ! -s "$functions_file" ]; then
  printf 'SDK unit manifest check: no SDK unit functions found\n' >&2
  exit 1
fi

if [ "$(wc -l < "$manifest_all_file" | tr -d ' ')" != \
     "$(wc -l < "$manifest_file" | tr -d ' ')" ]; then
  printf 'SDK unit manifest check: manifest lists one or more SDK unit functions more than once\n' >&2
  printf '%s\n' '--- duplicate manifest entries ---' >&2
  sort "$manifest_all_file" | uniq -d >&2
  exit 1
fi

if ! cmp -s "$functions_file" "$manifest_file"; then
  printf 'SDK unit manifest check: manifest does not match SDK unit functions\n' >&2
  printf '%s\n' '--- expected manifest entries ---' >&2
  cat "$functions_file" >&2
  printf '%s\n' '--- actual manifest entries ---' >&2
  cat "$manifest_file" >&2
  exit 1
fi

if [ "$(wc -l < "$main_all_file" | tr -d ' ')" != \
     "$(wc -l < "$main_file" | tr -d ' ')" ]; then
  printf 'SDK unit manifest check: main calls one or more SDK unit functions more than once\n' >&2
  printf '%s\n' '--- duplicate main calls ---' >&2
  sort "$main_all_file" | uniq -d >&2
  exit 1
fi

if ! cmp -s "$functions_file" "$main_file"; then
  printf 'SDK unit manifest check: main does not call every SDK unit function\n' >&2
  printf '%s\n' '--- expected main calls ---' >&2
  cat "$functions_file" >&2
  printf '%s\n' '--- actual main calls ---' >&2
  cat "$main_file" >&2
  exit 1
fi
