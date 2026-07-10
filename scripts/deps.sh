#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
version=0.41.0
base_url=https://github.com/sa6mwa/lonejson/releases/download/v${version}
targets="x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin"

sha_for() {
  case "$1" in
    # SHA-256 values are from the GitHub release checksum manifest for v0.41.0.
    aarch64-linux-gnu) printf '%s\n' 94b9e5eebba46b8c411ac2b5b3f3e029fdcf7b0c57aba673dad10f3a6cd84721 ;;
    aarch64-linux-musl) printf '%s\n' 61d7078ff9efba013a05da1d96c0847029d45848cebace197e27358b7e9582e6 ;;
    arm64-apple-darwin) printf '%s\n' a5221bf60e749eb533854cbe9f6143947415222c1d0fc253b08e4cfc7fb5ed67 ;;
    armhf-linux-gnu) printf '%s\n' d6fcfa25f93bcd1381351c3e369282cc3375ecf45831bfe6a8b82298fc03ec98 ;;
    armhf-linux-musl) printf '%s\n' fc77ebb256581f22a05889110159c7c6a1593bf46556efcc766cfc5899f5c993 ;;
    x86_64-linux-gnu) printf '%s\n' bcd352c413db4186d516b43040c57dc2a2060e2acf38d7d91eca6609043d54a8 ;;
    x86_64-linux-musl) printf '%s\n' 182a948f752b24a508e08756db9813f4caad8690be762a2a62284d97599fdeed ;;
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
