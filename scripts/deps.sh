#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
version=0.38.0
base_url=https://github.com/sa6mwa/lonejson/releases/download/v${version}
cpkt_version=0.7.0
cpkt_base_url=https://github.com/sa6mwa/c.pkt.systems/releases/download/v${cpkt_version}
targets="x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin"

sha_for() {
  case "$1" in
    # SHA-256 values are from the GitHub release asset digests for v0.38.0.
    aarch64-linux-gnu) printf '%s\n' da833da56dd61293caed0429bbadfdf1aa8c8b0d3f1a5c2363aa9bab157063a2 ;;
    aarch64-linux-musl) printf '%s\n' fc78193a5f7c63d86b7d2998c9f529410e00cf5dc9b2208cfff356a4d623e0a9 ;;
    arm64-apple-darwin) printf '%s\n' fc824e3a82a19af03845a410299def07df250736f16d424dd12492b3e22d54c8 ;;
    armhf-linux-gnu) printf '%s\n' 5a27a529a78e66fd3f260670b6a698d193f017b694140b673f1b7585baf58f6c ;;
    armhf-linux-musl) printf '%s\n' 51659d634ec5e90e1a9beb3864608fedc95c513f1270526e0674cd33f9a17b1a ;;
    x86_64-linux-gnu) printf '%s\n' fd71697e28964da924bbdd8442aa4dd3326537aa89eb52f3d2ee9b0a86f24cfd ;;
    x86_64-linux-musl) printf '%s\n' eb34aec71c8ecd69a10e349f0fb1cf99fc4a6ccdf015d5c0ba82c0593623e639 ;;
    *) return 1 ;;
  esac
}

cpkt_sha_for() {
  case "$1" in
    # SHA-256 values are from the GitHub release asset digests for v0.7.0.
    aarch64-linux-gnu) printf '%s\n' b0cd27ef2939d3538b324793f0718d6b444cd487df6dfb8835ae15c170d51f42 ;;
    aarch64-linux-musl) printf '%s\n' 7705dda7f17e40afbbfb81f6404715a2064afcfadc009317a7d41863d93960e8 ;;
    arm64-apple-darwin) printf '%s\n' c581c88541ae0d445e546700bfb8721a47708a63a82fa1df152964ededc6a02d ;;
    armhf-linux-gnu) printf '%s\n' 04765c76cec60db56c6efed263dcdd7b8a0929b4eac679b693b62cc5b47b4090 ;;
    armhf-linux-musl) printf '%s\n' dea4ca0fc75517e007484786f3c0168bdc10b6b5eb658808c45032d922d83516 ;;
    x86_64-linux-gnu) printf '%s\n' 35e50e02ca4b0f7ba7ff0e3683c1c19b1ae07aa0c47b349e52025e45e0e35b28 ;;
    x86_64-linux-musl) printf '%s\n' 4340cba25a7d44810b167ad955ce18ee4f370c1f54914a16bb462463e13051a1 ;;
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
  fetch_archive c.pkt.systems "$cpkt_version" "$target" "$cpkt_base_url" "$(cpkt_sha_for "$target")"
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
