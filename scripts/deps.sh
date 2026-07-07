#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
version=0.37.0
base_url=https://github.com/sa6mwa/lonejson/releases/download/v${version}
targets="x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin"

sha_for() {
  case "$1" in
    # SHA-256 values are from the GitHub release asset digests for v0.37.0.
    aarch64-linux-gnu) printf '%s\n' fd1a1ab60c7b4ffe64bc59240a66b4b598daea766fa4e4e84d368ad538bc5f39 ;;
    aarch64-linux-musl) printf '%s\n' d0a1c4053ebd2f2fa7c7edbcb4411e77aef96dac7f73a0be6a81bb215d4958d9 ;;
    arm64-apple-darwin) printf '%s\n' d385f28c00bd25bfc5388ffa8e146e98831eebeab7f060e85b33a8960d7e4d60 ;;
    armhf-linux-gnu) printf '%s\n' 6ebea5309d6026dbb9c0151562e48809f3abf9eb28a7bff553b2c00038a9ab54 ;;
    armhf-linux-musl) printf '%s\n' 7c934e3aa1efcb184ddb1854e2932ace35ff173c29c9d5601ef40e1add6575cb ;;
    x86_64-linux-gnu) printf '%s\n' 6245a791c0970e109bcea100c2c1f0962565ab21d7886d2d46eecc1f47f06c0a ;;
    x86_64-linux-musl) printf '%s\n' c5ff2e49ee3bd40e7aef96b9719aa013ed53f80fe4894eaad3c4707e77842904 ;;
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
