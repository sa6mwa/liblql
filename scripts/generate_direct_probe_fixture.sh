#!/bin/sh
set -eu

if [ "$#" -ne 2 ] && [ "$#" -ne 3 ]; then
  printf '%s\n' "usage: $0 OUTPUT_PATH RECORD_COUNT [PAYLOAD_BYTES]" >&2
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

payload_bytes=${3:-24}

case "$payload_bytes" in
  ''|*[!0-9]*)
    printf '%s\n' 'payload bytes must be a non-negative integer' >&2
    exit 2
    ;;
esac

mkdir -p "$(dirname "$1")"
awk -v count="$2" -v payload_bytes="$payload_bytes" 'BEGIN {
  for (i = 0; i < count; ++i) {
    status = (i % 4 == 0) ? "open" : "closed"
    region = (i % 3 == 0) ? "us-west" : "eu-north"
    printf "{\"id\":\"id-%d\",\"status\":\"%s\",\"region\":\"%s\",\"payload\":\"", i, status, region
    for (j = 0; j < payload_bytes; ++j) {
      printf "x"
    }
    printf "\"}\n"
  }
}' > "$1"
