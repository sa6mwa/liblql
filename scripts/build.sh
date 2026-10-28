#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd -- "$root"
sh scripts/deps.sh x86_64-linux-gnu >/dev/null
cmake --preset debug
cmake --build --preset debug
