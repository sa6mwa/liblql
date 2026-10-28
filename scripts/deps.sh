#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case ${LIBLQL_TOOLCHAIN_OVERRIDE:-} in
  1) exit 0 ;;
  "") ;;
  *) printf '%s\n' 'deps: LIBLQL_TOOLCHAIN_OVERRIDE must be exactly 1' >&2; exit 1 ;;
esac
bash "$root/scripts/require-host-bash.sh" >/dev/null
if [ -n "${CPKT_TOOLCHAIN_CACHE:-}" ]; then
  cmake -DLQL_CACHE_PATH="$CPKT_TOOLCHAIN_CACHE" -DLQL_CACHE_KIND=toolchains \
    -P "$root/cmake/LqlCachePaths.cmake"
fi
case ${1:-all} in
  all)
    "$root/scripts/cpkt-toolchains.sh" ensure all
    "$root/scripts/cpkt-aflpp.sh" ensure
    ;;
  fuzz) "$root/scripts/cpkt-aflpp.sh" ensure ;;
  *) "$root/scripts/cpkt-toolchains.sh" ensure "$1" ;;
esac
