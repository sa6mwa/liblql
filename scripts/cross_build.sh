#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec "$root/scripts/run_linux_release_matrix.sh" "$@"
