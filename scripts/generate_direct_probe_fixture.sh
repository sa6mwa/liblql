#!/bin/sh
set -eu

if [ "$#" -ne 2 ] && [ "$#" -ne 3 ] && [ "$#" -ne 4 ]; then
  printf '%s\n' "usage: $0 OUTPUT_PATH RECORD_COUNT [PAYLOAD_BYTES] [SHAPE]" >&2
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
  status|statusws|scalar|nested|nestedprojection|indexed|recursive|temporal|arrayscalar|arrayexists|rangecode|realworld|lockd)
    ;;
  *)
    printf '%s\n' 'fixture shape must be status, statusws, scalar, nested, nestedprojection, indexed, recursive, temporal, arrayscalar, arrayexists, rangecode, realworld, or lockd' >&2
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
    } else if (shape == "nestedprojection") {
      status = (i % 4 == 0) ? "open" : "closed"
      state = (i % 3 == 0) ? "active" : "idle"
      printf "{\"id\":\"id-%d\",\"status\":\"%s\",\"meta\":{\"state\":\"%s\",\"code\":%d,\"count\":%d},\"payload\":\"", i, status, state, i % 8, i % 16
    } else if (shape == "indexed") {
      sku = (i % 4 == 0) ? "B" : "A"
      printf "{\"id\":\"id-%d\",\"items\":[{\"sku\":\"A\"},{\"sku\":\"%s\"}],\"payload\":\"", i, sku
    } else if (shape == "recursive") {
      sku = (i % 4 == 0) ? "needle" : "other"
      printf "{\"id\":\"id-%d\",\"tree\":{\"branch\":{\"deep\":{\"sku\":\"%s\"}}},\"payload\":\"", i, sku
    } else if (shape == "temporal") {
      timestamp = (i % 4 == 0) ? "2026-03-05T10:29:00Z" : "2026-03-05T11:28:21Z"
      printf "{\"id\":\"id-%d\",\"timestamp\":\"%s\",\"payload\":\"", i, timestamp
    } else if (shape == "arrayscalar") {
      value = (i % 4 == 0) ? "B" : "C"
      printf "{\"id\":\"id-%d\",\"values\":[\"A\",\"%s\"],\"payload\":\"", i, value
    } else if (shape == "arrayexists") {
      if (i % 4 == 0) {
        printf "{\"id\":\"id-%d\",\"values\":[\"A\",\"B\"],\"payload\":\"", i
      } else {
        printf "{\"id\":\"id-%d\",\"values\":[\"A\"],\"payload\":\"", i
      }
    } else if (shape == "rangecode") {
      code = (i % 4 == 0) ? 1 : ((i % 4 == 1) ? 0 : 2)
      printf "{\"id\":\"id-%d\",\"code\":%d,\"payload\":\"", i, code
    } else if (shape == "realworld") {
      event = (i % 16 == 0) ? "session_sync" : ((i % 3 == 0) ? "tabs_update" : "heartbeat")
      component = (i % 2 == 0) ? "edge" : "core"
      active_idx = (i % 16 == 0) ? 0 : 1
      tab_count = (i % 16 == 0) ? 1 : 3
      code = (i % 16 == 0) ? 11 : (i % 9)
      hash = (i % 16 == 0) ? "c5d2460186f7233c927e7db2dcc703c0a3a8e0d5f0d8a3c5b4f1e2d3c4b5a697" : "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
      target_sid = (i % 16 == 0) ? "sid-0a3f-target" : "sid-other"
      printf "{\"id\":\"id-%d\",\"event\":\"%s\",\"component\":\"%s\",\"active_idx\":%d,\"tab_count\":%d,\"code\":%d,\"query\":{\"hash\":\"%s\",\"nested\":{\"event\":\"%s\"}},\"session_ids\":[\"sid-%d\",\"%s\"],\"payload\":\"", i, event, component, active_idx, tab_count, code, hash, event, i, target_sid
    } else if (shape == "lockd") {
      event = (i % 4 == 0) ? "session_sync" : ((i % 4 == 1) ? "tabs_update" : ((i % 4 == 2) ? "lock_request" : "heartbeat"))
      op = (i % 3 == 0) ? "write" : ((i % 3 == 1) ? "delete" : "read")
      active = (i % 2 == 0) ? "true" : "false"
      printf "{\"event\":\"%s\",\"op\":\"%s\",\"session_id\":\"session-%d\",\"lockd\":{\"key\":\"browser/session/%d\",\"owner\":\"pid-%d\",\"tab\":{\"id\":\"tab-%d\",\"url\":\"https://example.test/%d\",\"active\":%s}},\"timestamp\":\"2026-03-05T11:28:21Z\",\"payload\":\"", event, op, i, i, 1000 + i, i, i, active
    } else if (shape == "statusws") {
      printf " { \"id\" : \"id-%d\" , \"status\" : \"%s\" , \"region\" : \"%s\" , \"payload\" : \"", i, status, region
    } else {
      printf "{\"id\":\"id-%d\",\"status\":\"%s\",\"region\":\"%s\",\"payload\":\"", i, status, region
    }
    for (j = 0; j < payload_bytes; ++j) {
      printf "x"
    }
    if (shape == "statusws") {
      printf "\" }\n"
    } else {
      printf "\"}\n"
    }
  }
}' > "$1"
