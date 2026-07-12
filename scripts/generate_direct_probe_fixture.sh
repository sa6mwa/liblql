#!/bin/sh
set -eu

if [ "$#" -ne 2 ] && [ "$#" -ne 3 ] && [ "$#" -ne 4 ]; then
  printf '%s\n' "usage: $0 OUTPUT_PATH RECORD_COUNT [PAYLOAD_BYTES] [status|scalar]" >&2
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
shape=${4:-status}

case "$payload_bytes" in
  ''|*[!0-9]*)
    printf '%s\n' 'payload bytes must be a non-negative integer' >&2
    exit 2
    ;;
esac

case "$shape" in
  status|scalar|nested)
    ;;
  *)
    printf '%s\n' 'fixture shape must be status, scalar, or nested' >&2
    exit 2
    ;;
esac

mkdir -p "$(dirname "$1")"
awk -v count="$2" -v payload_bytes="$payload_bytes" -v shape="$shape" 'BEGIN {
  for (i = 0; i < count; ++i) {
    status = (i % 4 == 0) ? "open" : "closed"
    region = (i % 3 == 0) ? "us-west" : "eu-north"
    if (shape == "scalar") {
      code = (i % 4 == 0) ? 1 : 2
      enabled = (i % 2 == 0) ? "true" : "false"
      empty = (i % 3 == 0) ? "null" : "false"
      printf "{\"id\":\"id-%d\",\"code\":%d,\"enabled\":%s,\"empty\":%s,\"payload\":\"", i, code, enabled, empty
    } else if (shape == "nested") {
      state = (i % 4 == 0) ? "open" : "closed"
      printf "{\"id\":\"id-%d\",\"meta\":{\"state\":\"%s\",\"code\":%d},\"payload\":\"", i, state, i % 8
    } else {
      printf "{\"id\":\"id-%d\",\"status\":\"%s\",\"region\":\"%s\",\"payload\":\"", i, status, region
    }
    for (j = 0; j < payload_bytes; ++j) {
      printf "x"
    }
    printf "\"}\n"
  }
}' > "$1"
