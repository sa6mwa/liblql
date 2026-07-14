#!/usr/bin/env bash
set -euo pipefail

skill_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
resolver="$skill_dir/scripts/cpkt-aflpp.sh"
fail() { printf 'test-cpkt-aflpp-resolver: %s\n' "$*" >&2; exit 1; }

[[ -x "$resolver" ]] || fail 'resolver is not executable'
bash -n "$resolver"
grep -Fq 'version=5.02c' "$resolver" || fail 'AFL++ version is not pinned'
grep -Fq 'archive_sha256=' "$resolver" || fail 'AFL++ checksum is not pinned'
grep -Fq 'cpkt-toolchains.sh' "$resolver" || fail 'resolver does not use the embedded Bootlin resolver'
grep -Fq 'install_cleanup_trap -rf "$tmp"' "$resolver" || fail 'resolver does not clean failed staging state'
grep -Fq 'with_cache_lock "$c/locks/aflplusplus-${version}-x86_64-linux-gnu.lock" ensure_locked' "$resolver" || fail 'resolver does not serialize shared AFL++ publication'
grep -Fq 'collection_id()' "$resolver" || fail 'resolver does not key AFL++ caches by Bootlin collection identity'
grep -Fq '.cpkt-aflpp-revision-$revision-$id' "$resolver" || fail 'resolver readiness marker is not tied to the Bootlin collection identity'
grep -Fq 'ready "$r" "$id" && return' "$resolver" || fail 'resolver does not recheck collection-specific AFL++ readiness under the lock'
grep -Fq '"-DAFL_PATH=\"$helper\""' "$resolver" || fail 'resolver does not preserve AFL++ cache paths as one compiler argument'

fake_bin=$(mktemp -d "${TMPDIR:-/tmp}/cpkt-aflpp-test.XXXXXX")
trap 'rm -rf "$fake_bin"' EXIT HUP INT TERM
printf '#!/bin/sh\nprintf "aarch64\\n"\n' > "$fake_bin/uname"
chmod +x "$fake_bin/uname"
if PATH="$fake_bin:$PATH" "$resolver" ensure >/dev/null 2>&1; then
  fail 'resolver accepted a non-native host'
fi

if env -u HOME -u XDG_CACHE_HOME -u CPKT_TOOLCHAIN_CACHE "$resolver" discover >/dev/null 2>&1; then
  fail 'resolver accepted a missing cache root'
fi

if "$resolver" ensure extra >/dev/null 2>&1; then
  fail 'resolver accepted an invalid command shape'
fi

printf 'AFL++ resolver tests passed\n'
