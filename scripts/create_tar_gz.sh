#!/bin/sh
set -eu

base_dir=$1
archive=$2
root_name=$3
tmp_tar=$archive.tmp.tar
touch_stamp=197001010000.00
tar_version=$(tar --version 2>/dev/null || true)

normalize_mtime() {
  path=$1
  if touch -h -t "$touch_stamp" "$path" 2>/dev/null; then
    find "$path" -exec touch -h -t "$touch_stamp" {} \;
  else
    find "$path" ! -type l -exec touch -t "$touch_stamp" {} \;
  fi
}

sh scripts/remove_path.sh "$tmp_tar" "$archive"
normalize_mtime "$base_dir/$root_name"

if printf '%s\n' "$tar_version" | grep 'GNU tar' >/dev/null 2>&1; then
  tar -C "$base_dir" --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime='UTC 1970-01-01' -cf "$tmp_tar" "$root_name"
elif printf '%s\n' "$tar_version" | grep -Ei 'bsdtar|libarchive' \
    >/dev/null 2>&1; then
  tar -C "$base_dir" --uid 0 --gid 0 --uname root --gname root \
    -cf "$tmp_tar" "$root_name"
else
  printf 'create tar: GNU tar or bsdtar/libarchive is required for deterministic archives\n' >&2
  exit 1
fi
gzip -n -c "$tmp_tar" >"$archive"
sh scripts/remove_path.sh "$tmp_tar"
