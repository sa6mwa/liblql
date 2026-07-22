#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

tmp=build/tar-portability
fakebin=$tmp/bin
base=$tmp/input
archive=$tmp/out.tar.gz
real_tar=$(command -v tar)

sh scripts/remove_path.sh "$tmp"
mkdir -p "$fakebin" "$base/root/nested"
printf '%s\n' payload >"$base/root/file.txt"
printf '%s\n' nested >"$base/root/nested/file.txt"

cat >"$fakebin/tar" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "--version" ]]; then
  printf '%s\n' 'bsdtar 3.7.0 - libarchive'
  exit 0
fi
args=()
saw_bsd_owner=0
saw_file_list=0
while (($#)); do
  case "$1" in
    --sort=* | --owner=* | --group=* | --numeric-owner)
      printf 'fake tar: rejected GNU-only option %s\n' "$1" >&2
      exit 99
      ;;
    --uid | --gid | --uname | --gname)
      saw_bsd_owner=1
      shift 2
      ;;
    --uid=* | --gid=* | --uname=* | --gname=*)
      saw_bsd_owner=1
      shift
      ;;
    -T)
      saw_file_list=1
      args+=("$1" "$2")
      shift 2
      ;;
    *)
      args+=("$1")
      shift
      ;;
  esac
done
if [[ "${LQL_EXPECT_BSD_FLAGS:-}" == 1 && "$saw_bsd_owner" != 1 ]]; then
  printf 'fake tar: missing BSD ownership normalization flags\n' >&2
  exit 98
fi
if [[ "${LQL_EXPECT_SORTED_LIST:-}" == 1 && "$saw_file_list" != 1 ]]; then
  printf 'fake tar: missing sorted file list\n' >&2
  exit 97
fi
exec "$LQL_REAL_TAR" "${args[@]}"
EOF
chmod +x "$fakebin/tar"

LQL_EXPECT_BSD_FLAGS=1 LQL_EXPECT_SORTED_LIST=1 \
  LQL_REAL_TAR=$real_tar PATH=$fakebin:$PATH \
  sh scripts/create_tar_gz.sh "$base" "$archive" root

if [ ! -f "$archive" ]; then
  printf 'tar portability: archive was not created\n' >&2
  exit 1
fi
if ! tar -xOf "$archive" root/file.txt | grep -Fx payload >/dev/null; then
  printf 'tar portability: archive payload was not readable\n' >&2
  exit 1
fi
if tar -tzf "$archive" | sort | uniq -d | grep . >/dev/null; then
  printf 'tar portability: archive contains duplicate members\n' >&2
  exit 1
fi
first_hash=$(sha256sum "$archive" | sed 's/ .*//')

touch "$base/root/file.txt"
LQL_EXPECT_BSD_FLAGS=1 LQL_EXPECT_SORTED_LIST=1 \
  LQL_REAL_TAR=$real_tar PATH=$fakebin:$PATH \
  sh scripts/create_tar_gz.sh "$base" "$archive" root
second_hash=$(sha256sum "$archive" | sed 's/ .*//')
if [ "$first_hash" != "$second_hash" ]; then
  printf 'tar portability: repeated archive hash changed\n' >&2
  exit 1
fi

printf 'tar portability tests passed\n'
