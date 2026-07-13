#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
sh scripts/test_discover_target_tools.sh
sh scripts/check_darwin_linker_route.sh
