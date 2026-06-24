#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
rm -rf "${root}/build" "${root}/dist" "${root}/.cache"
