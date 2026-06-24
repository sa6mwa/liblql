#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
version=0.33.0
base_url=https://github.com/sa6mwa/lonejson/releases/download/v${version}
targets="x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin"

sha_for() {
  case "$1" in
    # SHA-256 values are from the GitHub release asset digests for v0.33.0.
    aarch64-linux-gnu) printf '%s\n' f4a94c0d611588d89961f134a082fa7dc83201401ea50351992ae7d8ff55e138 ;;
    aarch64-linux-musl) printf '%s\n' b390ef98920231a2dbb1e652106f4aa4cb0468c4f893bd02c8424303873d39eb ;;
    arm64-apple-darwin) printf '%s\n' f535263387e4f802b4b5789452a1d54ab5fc3c07e5da32a973117badc3d3239c ;;
    armhf-linux-gnu) printf '%s\n' 0ed0bbdcc4be19ace7095f4ba044e22f350a36f65e311d6a382916b618ad8132 ;;
    armhf-linux-musl) printf '%s\n' e0a2287c36c7fa8350fdda5a6b7707464426c8aa774f247e6344b032a834e449 ;;
    x86_64-linux-gnu) printf '%s\n' 3ea89642564f69be0ea9b427bf9dd31b0a2f70f7bb9912037712ee58f4e4e0fc ;;
    x86_64-linux-musl) printf '%s\n' 882a64453796ecbaafe610c0b4da4581de4fa29a960863470c43db4eb524db70 ;;
    *) return 1 ;;
  esac
}

fetch_one() {
  target=$1
  archive="liblonejson-${version}-${target}.tar.gz"
  cache="${root}/.cache/downloads/${archive}"
  dest="${root}/.cache/deps/${target}"
  sdk="${dest}/lonejson"
  stamp="${sdk}/.lql-dep-stamp"
  expected=$(sha_for "$target")

  if [ -f "$stamp" ] && grep -qx "lonejson ${version} ${target} ${expected}" "$stamp"; then
    printf 'deps: %s already ready\n' "$target"
    return 0
  fi
  mkdir -p "${root}/.cache/downloads" "$dest"
  if [ ! -f "$cache" ]; then
    curl -fL --retry 3 -o "$cache" "${base_url}/${archive}"
  fi
  actual=$(sha256sum "$cache" | awk '{print $1}')
  if [ "$actual" != "$expected" ]; then
    printf 'checksum mismatch for %s\nexpected=%s\nactual=%s\n' "$archive" "$expected" "$actual" >&2
    exit 1
  fi
  rm -rf "$sdk" "${dest}/liblonejson-${version}-${target}"
  tar -xzf "$cache" -C "$dest"
  mv "${dest}/liblonejson-${version}-${target}" "$sdk"
  printf 'lonejson %s %s %s\n' "$version" "$target" "$expected" >"$stamp"
  printf 'deps: installed %s\n' "$target"
}

arg=${1:-x86_64-linux-gnu}
if [ "$arg" = all ]; then
  for target in $targets; do
    fetch_one "$target"
  done
else
  fetch_one "$arg"
fi
