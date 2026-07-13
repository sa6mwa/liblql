#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
"$root/scripts/cpkt-toolchains.sh" discover arm64-apple-darwin |
  grep '^status=ready$' >/dev/null
