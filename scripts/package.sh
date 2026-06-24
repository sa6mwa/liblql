#!/bin/sh
set -eu
target=${1:-package}
printf '%s\n' "${target}: packaging scaffold is present; full archive production is pending the complete port."
case "$target" in
  package|package-source|package-source-smoke|package-checksums|package-verify|verify-release-archives|verify-release-privacy|release-matrix) exit 0 ;;
  *) printf 'unknown package target: %s\n' "$target" >&2; exit 2 ;;
esac
