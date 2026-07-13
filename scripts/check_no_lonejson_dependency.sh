#!/bin/sh
set -eu

source_matches=$(git grep -n -E '#[[:space:]]*include[[:space:]]*[<"].*lonejson|lonejson_' -- \
  CMakeLists.txt include src tests tools || true)
if [ -n "$source_matches" ]; then
  printf '%s\n' 'LoneJSON runtime/source reference found:' >&2
  printf '%s\n' "$source_matches" >&2
  exit 1
fi

check_pair() {
  shared=$1
  static=$2

  if [ ! -f "$shared" ]; then
    printf 'missing shared liblql artifact: %s\n' "$shared" >&2
    exit 1
  fi
  if [ ! -f "$static" ]; then
    printf 'missing static liblql artifact: %s\n' "$static" >&2
    exit 1
  fi

  if command -v readelf >/dev/null 2>&1; then
    if readelf -d "$shared" | grep -i 'lonejson' >/dev/null 2>&1; then
      printf 'shared liblql links LoneJSON: %s\n' "$shared" >&2
      readelf -d "$shared" | grep -i 'lonejson' >&2
      exit 1
    fi
  fi

  if nm -D "$shared" | grep -i 'lonejson' >/dev/null 2>&1; then
    printf 'shared liblql exports or imports LoneJSON symbols: %s\n' "$shared" >&2
    nm -D "$shared" | grep -i 'lonejson' >&2
    exit 1
  fi

  if nm -g "$static" | grep -i 'lonejson' >/dev/null 2>&1; then
    printf 'static liblql contains LoneJSON symbols: %s\n' "$static" >&2
    nm -g "$static" | grep -i 'lonejson' >&2
    exit 1
  fi

  printf 'no LoneJSON runtime dependency: %s %s\n' "$shared" "$static"
}

if [ "${LQL_SHARED_PATH+x}" = x ] || [ "${LQL_STATIC_PATH+x}" = x ]; then
  if [ "${LQL_SHARED_PATH+x}" != x ] || [ "${LQL_STATIC_PATH+x}" != x ]; then
    printf '%s\n' \
      'LQL_SHARED_PATH and LQL_STATIC_PATH must be set together' >&2
    exit 1
  fi
  check_pair "$LQL_SHARED_PATH" "$LQL_STATIC_PATH"
else
  check_pair build/debug/liblql.so.0 build/debug/liblql.a
  check_pair build/release/liblql.so.0 build/release/liblql.a
fi
