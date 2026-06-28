#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
version=0.35.2
base_url=https://github.com/sa6mwa/lonejson/releases/download/v${version}
targets="x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin"

sha_for() {
  case "$1" in
    # SHA-256 values are from the GitHub release asset digests for v0.35.2.
    aarch64-linux-gnu) printf '%s\n' 5095cd5c7df634dee5ea41b442ed22229a249d22c6defac00e4d04023c70dec4 ;;
    aarch64-linux-musl) printf '%s\n' e743f9b11688ef254fc58af43571d1bd694fdfda9dd32fc3b9721a039fb04b16 ;;
    arm64-apple-darwin) printf '%s\n' cbc0b1965661da56162cb14832be03de5f466521448fb5df4b38383bd7b0c98a ;;
    armhf-linux-gnu) printf '%s\n' 3c73be06a49b0706bb23eea08b8858f3b5ecfe5cff185a16f7b91247b4cd89ec ;;
    armhf-linux-musl) printf '%s\n' b018782ec76b8829caaec6eec0a09eb8c26471f07f1c6ba939648c803a543dd1 ;;
    x86_64-linux-gnu) printf '%s\n' 775f306a604cb8b3731d899f849d5eeeb4a2f561ccb3de4ea3cd5725514e7bc9 ;;
    x86_64-linux-musl) printf '%s\n' 2d8224eb436be0d1c85c77cc2c223f57ac0bde68feb91c4c2ef3b01f9f57875d ;;
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
