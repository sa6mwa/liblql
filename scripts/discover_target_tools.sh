#!/bin/sh
set -eu

exec python3 "$(dirname "$0")/discover_target_tools.py" "$@"
