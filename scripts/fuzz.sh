#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
cmake --preset fuzz
cmake --build --preset fuzz --target lql_json_fuzz
LQL_JSON_FUZZ_PATH=build/fuzz/lql_json_fuzz sh scripts/check_fuzz_smoke.sh
