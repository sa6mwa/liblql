#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
version=0.35.0
base_url=https://github.com/sa6mwa/lonejson/releases/download/v${version}
targets="x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin"

sha_for() {
  case "$1" in
    # SHA-256 values are from the GitHub release asset digests for v0.35.0.
    aarch64-linux-gnu) printf '%s\n' f69a39fc80ac9d0d40c6b0d6c2eec46d5c00aeff1696f983526cbba7d0e6f6db ;;
    aarch64-linux-musl) printf '%s\n' dd16709052eeff1797e775907db554e7805c9d1bed655a07e44094108d6b68c2 ;;
    arm64-apple-darwin) printf '%s\n' 6cbee12c36e43eda14ae5a96265067a99a2a3dd36fe1baffd5fd35e3a8583370 ;;
    armhf-linux-gnu) printf '%s\n' 6eb99f6683283b872245608488125f9ac2b05673e9416791fb2ff89d5319601a ;;
    armhf-linux-musl) printf '%s\n' 4ac1e5d844de02548d6e23f06dacc8be44eded0a8f686c7600cc8ee718f648e2 ;;
    x86_64-linux-gnu) printf '%s\n' f328684b58c89a3c04d76af919e59ae9b31ee56e0d14d01eaa59fb8126aaec85 ;;
    x86_64-linux-musl) printf '%s\n' 5216796e1192cc7aa18b59f809380ee2631e270fce6d1149b6fa5d2e9c95ad7d ;;
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
