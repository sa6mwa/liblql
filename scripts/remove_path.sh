#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
sh "$root/scripts/check_owned_path.sh" "$@"

for path in "$@"; do
  if [ ! -e "$path" ] && [ ! -L "$path" ]; then
    continue
  fi
  if [ -d "$path" ] && [ ! -L "$path" ]; then
    find "$path" ! -type d -exec rm -- {} \;
    find "$path" -depth -type d -exec rmdir -- {} \;
  else
    rm -- "$path"
  fi
done
