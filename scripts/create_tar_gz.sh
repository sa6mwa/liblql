#!/bin/sh
set -eu

base_dir=$1
archive=$2
root_name=$3

if tar --version 2>/dev/null | grep 'GNU tar' >/dev/null 2>&1; then
  tar -C "$base_dir" --sort=name --owner=0 --group=0 --numeric-owner \
    -czf "$archive" "$root_name"
else
  tar -C "$base_dir" -czf "$archive" "$root_name"
fi
