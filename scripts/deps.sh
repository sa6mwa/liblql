#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
version=0.40.0
base_url=https://github.com/sa6mwa/lonejson/releases/download/v${version}
targets="x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin"

sha_for() {
  case "$1" in
    # SHA-256 values are from the GitHub release checksum manifest for v0.40.0.
    aarch64-linux-gnu) printf '%s\n' 05fd295ed245a3ec59188093a13c3bcb2736b71a3cb4656f65a838d6f4a07142 ;;
    aarch64-linux-musl) printf '%s\n' 44b58b62ec0ccae5a8d6f6235fc2050afe67cad57b321fc8483f16673184dc45 ;;
    arm64-apple-darwin) printf '%s\n' 358da5a367963cd13ac30cdd00cc2a1d10ba40dedb7b45de5090d1e5e812ed1f ;;
    armhf-linux-gnu) printf '%s\n' 7296a1707bf96d0a3004f5d7293a6ba9cc6023d8664b7779fb6dddc87be4ec05 ;;
    armhf-linux-musl) printf '%s\n' 9e436f7f118cd73b55c048ca8a235e707f8446e8abfb3cccf19c48cfefddc056 ;;
    x86_64-linux-gnu) printf '%s\n' e7b9af3810fe07b25d42afee126554c3939f940def7f79ceb652e14e494957b8 ;;
    x86_64-linux-musl) printf '%s\n' 3491e3017895b6b88187d702ea3ba26051cf0f8b76fd3ed769f49a56f6b9dd4b ;;
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
