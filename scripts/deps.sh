#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
version=0.35.1
base_url=https://github.com/sa6mwa/lonejson/releases/download/v${version}
targets="x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin"

sha_for() {
  case "$1" in
    # SHA-256 values are from the GitHub release asset digests for v0.35.1.
    aarch64-linux-gnu) printf '%s\n' 1dfbb8876f7902bdf8fa14196ccd7aa224e83b85be7fc2a21c6cd84ecc10ec63 ;;
    aarch64-linux-musl) printf '%s\n' bc21ed9137444ce3a7ce7d71950cf743b6aa41233a6320495f1398565fb7b047 ;;
    arm64-apple-darwin) printf '%s\n' fbab83594d492f77d7d6ed681ddb0c7441597ec13fe923b20c0ef05322a884fd ;;
    armhf-linux-gnu) printf '%s\n' 50a51e303151fea42ab27f80b5bd551296d499c9d15f95284ded741acaa5c96e ;;
    armhf-linux-musl) printf '%s\n' 7746c362b38daf5dccc039b5a54743c45f68fd8ca97bec8e57c389d750a45be0 ;;
    x86_64-linux-gnu) printf '%s\n' 525c4194a0fe9acf713e55dcd798be64134b8c39260826de67f5162965448b11 ;;
    x86_64-linux-musl) printf '%s\n' 890a9b0c098a757a04b84e181a21e18062a99e0bcd267e26795a855aa5b66f7b ;;
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
