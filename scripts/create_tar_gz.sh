#!/bin/sh
set -eu

base_dir=$1
archive=$2
root_name=$3
tmp_tar=$archive.tmp.tar
touch_stamp=197001010000.00

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

if tar --version 2>/dev/null | grep 'GNU tar' >/dev/null 2>&1; then
  tar -C "$base_dir" --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime='UTC 1970-01-01' -cf "$tmp_tar" "$root_name"
else
  tar -C "$base_dir" -cf "$tmp_tar" "$root_name"
fi
gzip -n -c "$tmp_tar" >"$archive"
sh scripts/remove_path.sh "$tmp_tar"
