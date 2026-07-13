#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/liblql-dependency-cache.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
test_script=$root/tests/dependency_cache_config.cmake

env CPKT_DEPENDENCY_CACHE="$tmp/environment" XDG_CACHE_HOME="$tmp/xdg" \
  HOME="$tmp/home" cmake -DCPKT_DEPENDENCY_CACHE="$tmp/explicit" \
  -DEXPECTED_CPKT_DEPENDENCY_CACHE="$tmp/explicit" -P "$test_script" >/dev/null
env CPKT_DEPENDENCY_CACHE="$tmp/environment" XDG_CACHE_HOME="$tmp/xdg" \
  HOME="$tmp/home" cmake -DEXPECTED_CPKT_DEPENDENCY_CACHE="$tmp/environment" \
  -P "$test_script" >/dev/null
env -u CPKT_DEPENDENCY_CACHE XDG_CACHE_HOME="$tmp/xdg" HOME="$tmp/home" \
  cmake -DEXPECTED_CPKT_DEPENDENCY_CACHE="$tmp/xdg/c.pkt.systems/deps" \
  -P "$test_script" >/dev/null
env -u CPKT_DEPENDENCY_CACHE -u XDG_CACHE_HOME HOME="$tmp/home" \
  cmake -DEXPECTED_CPKT_DEPENDENCY_CACHE="$tmp/home/.cache/c.pkt.systems/deps" \
  -P "$test_script" >/dev/null

if env -u CPKT_DEPENDENCY_CACHE -u XDG_CACHE_HOME -u HOME cmake \
  -P "$test_script" >/dev/null 2>&1; then
  printf '%s\n' 'dependency cache config: accepted no cache-root source' >&2
  exit 1
fi

if env CPKT_DEPENDENCY_CACHE="$tmp/environment" cmake \
  -DCPKT_DEPENDENCY_CACHE= -P "$test_script" >/dev/null 2>&1; then
  printf '%s\n' 'dependency cache config: accepted an explicitly empty cache root' >&2
  exit 1
fi

printf '%s\n' 'dependency cache config: precedence and failure contract passed'
