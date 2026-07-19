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
mkdir -p "$fakebin" "$base/root"
printf '%s\n' payload >"$base/root/file.txt"

cat >"$fakebin/tar" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "--version" ]; then
  printf '%s\n' 'bsdtar 3.7.0 - libarchive'
  exit 0
fi
for arg in "$@"; do
  case "$arg" in
    --sort=* | --owner=* | --group=* | --numeric-owner)
      printf 'fake tar: rejected GNU-only option %s\n' "$arg" >&2
      exit 99
      ;;
  esac
done
exec "$LQL_REAL_TAR" "$@"
EOF
chmod +x "$fakebin/tar"

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
first_hash=$(sha256sum "$archive" | sed 's/ .*//')

touch "$base/root/file.txt"
LQL_REAL_TAR=$real_tar PATH=$fakebin:$PATH \
  sh scripts/create_tar_gz.sh "$base" "$archive" root
second_hash=$(sha256sum "$archive" | sed 's/ .*//')
if [ "$first_hash" != "$second_hash" ]; then
  printf 'tar portability: repeated archive hash changed\n' >&2
  exit 1
fi

printf 'tar portability tests passed\n'
