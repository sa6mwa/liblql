#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
"$root/scripts/cpkt-toolchains.sh" ensure all
"$root/scripts/cpkt-aflpp.sh" ensure
