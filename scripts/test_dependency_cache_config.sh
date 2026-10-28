#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/build/tmp"
tmp=$(mktemp -d "$root/build/tmp/liblql-dependency-cache.XXXXXX")
trap 'sh "$root/scripts/remove_path.sh" "$tmp"' EXIT HUP INT TERM
test_script=$root/tests/dependency_cache_config.cmake
retry_test_script=$root/tests/dependency_cache_retry.cmake
retry_server=$root/tests/dependency_cache_retry_server.py
retry_fixture=$root/tests/fixtures/dependency-cache-retry-payload.txt

env CPKT_DEPENDENCY_CACHE="$tmp/environment" XDG_CACHE_HOME="$tmp/xdg" \
  HOME="$tmp/home" cmake -DCPKT_DEPENDENCY_CACHE="$tmp/explicit" \
  -DEXPECTED_CPKT_DEPENDENCY_CACHE="$tmp/explicit" -P "$test_script" >/dev/null
env CPKT_DEPENDENCY_CACHE="$tmp/environment" XDG_CACHE_HOME="$tmp/xdg" \
  HOME="$tmp/home" cmake -DEXPECTED_CPKT_DEPENDENCY_CACHE="$tmp/environment" \
  -P "$test_script" >/dev/null
env -u CPKT_DEPENDENCY_CACHE XDG_CACHE_HOME="$tmp/xdg" HOME="$tmp/home" \
  cmake -DEXPECTED_CPKT_DEPENDENCY_CACHE="$tmp/xdg/cpkt/deps" \
  -P "$test_script" >/dev/null
env -u CPKT_DEPENDENCY_CACHE -u XDG_CACHE_HOME HOME="$tmp/home" \
  cmake -DEXPECTED_CPKT_DEPENDENCY_CACHE="$tmp/home/.cache/cpkt/deps" \
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

server_log=$tmp/retry-server.log
python3 "$retry_server" "$retry_fixture" >"$server_log" 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || :; wait "$server_pid" 2>/dev/null || :; sh "$root/scripts/remove_path.sh" "$tmp"' EXIT HUP INT TERM

retry_port=
retry_wait=0
while [ "$retry_wait" -lt 50 ]; do
  retry_port=$(sed -n 's/^PORT //p' "$server_log" | sed -n '1p')
  if [ -n "$retry_port" ]; then
    break
  fi
  retry_wait=$((retry_wait + 1))
  sleep 1
done
if [ -z "$retry_port" ]; then
  printf '%s\n' 'dependency cache retry: fixture server did not report a port' >&2
  sed -n '1,120p' "$server_log" >&2
  exit 1
fi

cmake -DCPKT_DEPENDENCY_CACHE="$tmp/retry-cache" \
  -DRETRY_ARCHIVE_URL="http://127.0.0.1:$retry_port/archive.tar.gz" \
  -DRETRY_ARCHIVE_SOURCE="$retry_fixture" -P "$retry_test_script" >/dev/null

# Hits and renamed basenames reuse the digest without any HTTP requests.
for name in archive.tar.gz renamed.tar.gz; do
  cmake -DCPKT_DEPENDENCY_CACHE="$tmp/retry-cache" \
    -DARCHIVE_NAME="$name" \
    -DRETRY_ARCHIVE_URL="http://127.0.0.1:$retry_port/archive.tar.gz" \
    -DRETRY_ARCHIVE_SOURCE="$retry_fixture" -P "$retry_test_script" >/dev/null
done
[ "$(grep -c '^REQUEST ' "$server_log")" = 2 ]
digest=$(sha256sum "$retry_fixture" | awk '{print $1}')
archive_dir=$tmp/retry-cache/archives/sha256/$digest
rm "$archive_dir/renamed.tar.gz"
printf corrupt >"$archive_dir/archive.tar.gz"
cmake -DCPKT_DEPENDENCY_CACHE="$tmp/retry-cache" \
  -DRETRY_ARCHIVE_URL="http://127.0.0.1:$retry_port/archive.tar.gz" \
  -DRETRY_ARCHIVE_SOURCE="$retry_fixture" -P "$retry_test_script" >/dev/null
[ "$(grep -c '^REQUEST ' "$server_log")" = 3 ]
wrong_digest=0000000000000000000000000000000000000000000000000000000000000000
if cmake -DCPKT_DEPENDENCY_CACHE="$tmp/retry-cache" \
  -DEXPECTED_SHA256="$wrong_digest" \
  -DRETRY_ARCHIVE_URL="http://127.0.0.1:$retry_port/archive.tar.gz" \
  -DRETRY_ARCHIVE_SOURCE="$retry_fixture" -P "$retry_test_script" \
  >"$tmp/wrong-digest.log" 2>&1; then
  printf '%s\n' 'dependency cache: accepted wrong checksum' >&2; exit 1
fi
grep 'checksum mismatch' "$tmp/wrong-digest.log" >/dev/null
[ ! -e "$tmp/retry-cache/archives/sha256/$wrong_digest/archive.tar.gz" ]
[ ! -e "$tmp/retry-cache/archives/sha256/$wrong_digest/archive.tar.gz.tmp" ]
[ "$(grep -c '^REQUEST ' "$server_log")" = 6 ]
curl -fsS "http://127.0.0.1:$retry_port/shutdown" >/dev/null
if ! wait "$server_pid"; then
  printf '%s\n' 'dependency cache retry: fixture server failed' >&2
  sed -n '1,120p' "$server_log" >&2
  exit 1
fi
server_pid=
if ! grep -Fx 'REQUEST 6' "$server_log" >/dev/null; then
  printf '%s\n' 'dependency cache retry: expected one transient failure followed by a retry' >&2
  sed -n '1,120p' "$server_log" >&2
  exit 1
fi

printf '%s\n' 'dependency cache: precedence, retry, digest hits, renamed hits, corruption and checksum rejection passed'
