#!/bin/sh
set -eu
if [ "${LQL_VERSION_OVERRIDE:-}" ]; then
  printf '%s\n' "$LQL_VERSION_OVERRIDE"
  exit 0
fi
if git describe --tags --exact-match --match 'v[0-9]*.[0-9]*.[0-9]*' >/tmp/lql-version.$$ 2>/dev/null; then
  sed 's/^v//' /tmp/lql-version.$$
  rm -f /tmp/lql-version.$$
  exit 0
fi
rm -f /tmp/lql-version.$$
if [ ! -d .git ] && [ -f VERSION ]; then
  sed -n '1p' VERSION
else
  printf '0.0.0\n'
fi
