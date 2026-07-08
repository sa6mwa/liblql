#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
version=0.39.0
base_url=https://github.com/sa6mwa/lonejson/releases/download/v${version}
targets="x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin"

sha_for() {
  case "$1" in
    # SHA-256 values are from the GitHub release asset digests for v0.39.0.
    aarch64-linux-gnu) printf '%s\n' add5b64f8d2bf98368e202ec3e915d934bb55609155ecf5821f3d4c1cbd79d8b ;;
    aarch64-linux-musl) printf '%s\n' ebefdd7939910779442cce41cccd5f861c5299d46b8693173a423bd8e8e40389 ;;
    arm64-apple-darwin) printf '%s\n' 0d7721ee174ca7567af2ac9d65df5e058dd44fb62e3539ffab94f278fbaf2f14 ;;
    armhf-linux-gnu) printf '%s\n' 6833292468d7d2c2f7de03c8bbc24b1a45dce518fd424145705ba1b232051b0a ;;
    armhf-linux-musl) printf '%s\n' 3d9dd35409e9b9484bd2f123f472f2203950553bb1339ef4c9a4c8ac99eec83f ;;
    x86_64-linux-gnu) printf '%s\n' a33248ddb83f97dcbca626f7bedb4739518f1e8819bb2c42f1e4755aaa1fda00 ;;
    x86_64-linux-musl) printf '%s\n' e778933a6f0d121cfed6a1c653a0a9a9b54caf8c64ac1ae90ee16ce78b8a76a0 ;;
    *) return 1 ;;
  esac
}

fetch_archive() {
  name=$1
  version_value=$2
  target=$3
  url=$4
  expected=$5
  install_name=${6:-$name}

  archive="${name}-${version_value}-${target}.tar.gz"
  cache="${root}/.cache/downloads/${archive}"
  dest="${root}/.cache/deps/${target}"
  sdk="${dest}/${install_name}"
  stamp="${sdk}/.lql-dep-stamp"

  if [ -f "$stamp" ] && grep -qx "${install_name} ${version_value} ${target} ${expected}" "$stamp"; then
    printf 'deps: %s %s already ready\n' "$install_name" "$target"
    return 0
  fi
  mkdir -p "${root}/.cache/downloads" "$dest"
  if [ ! -f "$cache" ]; then
    curl -fL --retry 3 -o "$cache" "${url}/${archive}"
  fi
  actual=$(sha256sum "$cache" | awk '{print $1}')
  if [ "$actual" != "$expected" ]; then
    printf 'checksum mismatch for %s\nexpected=%s\nactual=%s\n' "$archive" "$expected" "$actual" >&2
    exit 1
  fi
  rm -rf "$sdk" "${dest}/${name}-${version_value}-${target}"
  tar -xzf "$cache" -C "$dest"
  mv "${dest}/${name}-${version_value}-${target}" "$sdk"
  printf '%s %s %s %s\n' "$install_name" "$version_value" "$target" "$expected" >"$stamp"
  printf 'deps: installed %s %s\n' "$install_name" "$target"
}

fetch_one() {
  target=$1
  fetch_archive liblonejson "$version" "$target" "$base_url" "$(sha_for "$target")" lonejson
}

arg=${1:-x86_64-linux-gnu}
if [ "$arg" = all ]; then
  for target in $targets; do
    fetch_one "$target"
  done
else
  fetch_one "$arg"
fi
