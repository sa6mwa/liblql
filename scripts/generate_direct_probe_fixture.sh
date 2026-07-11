#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
  printf '%s\n' "usage: $0 OUTPUT_PATH RECORD_COUNT" >&2
  exit 2
fi

case "$2" in
  ''|*[!0-9]*)
    printf '%s\n' 'record count must be a positive integer' >&2
    exit 2
    ;;
esac

if [ "$2" -eq 0 ]; then
  printf '%s\n' 'record count must be a positive integer' >&2
  exit 2
fi

mkdir -p "$(dirname "$1")"
awk -v count="$2" 'BEGIN {
  for (i = 0; i < count; ++i) {
    status = (i % 4 == 0) ? "open" : "closed"
    region = (i % 3 == 0) ? "us-west" : "eu-north"
    printf "{\"id\":\"id-%d\",\"status\":\"%s\",\"region\":\"%s\",\"payload\":\"abcdefghijklmnopqrstuvwx\"}\n", i, status, region
  }
}' > "$1"
