#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "lql/lql.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct payload_counts {
  lql_uint64 payloads;
  lql_uint64 payload_bytes;
  FILE *sink;
} payload_counts;

typedef struct file_reader {
  FILE *file;
} file_reader;

static lql_read_result read_file_chunk(void *user, unsigned char *buffer,
                                       size_t capacity) {
  file_reader *reader;
  lql_read_result result;

  reader = (file_reader *)user;
  memset(&result, 0, sizeof(result));
  result.bytes_read = fread(buffer, 1u, capacity, reader->file);
  if (result.bytes_read < capacity) {
    if (ferror(reader->file)) {
      result.error_code = 1;
    } else {
      result.eof = 1;
    }
  }
  return result;
}

static lql_status observe_decision(void *user,
                                   const lql_query_decision *decision) {
  (void)user;
  (void)decision;
  return LQL_STATUS_OK;
}

static void print_u64(lql_uint64 value) {
  char buf[32];
  size_t len;

  len = 0u;
  if (value == 0u) {
    fputc('0', stdout);
    return;
  }
  while (value != 0u && len < sizeof(buf)) {
    buf[len++] = (char)('0' + (int)(value % 10u));
    value /= 10u;
  }
  while (len != 0u) {
    fputc(buf[--len], stdout);
  }
}

static lql_status count_payload(void *user, const lql_query_match *match) {
  payload_counts *counts;
  lql_status st;
  lql_error error;

  counts = (payload_counts *)user;
  if (match->payload.kind != LQL_PAYLOAD_SEEKABLE_RANGE ||
      match->payload.size != match->decision.size ||
      match->payload.offset != match->decision.offset) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_error_init(&error);
  st = lql_payload_write_json(&match->payload, counts->sink, &error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  counts->payloads++;
  counts->payload_bytes += match->payload.size;
  return LQL_STATUS_OK;
}

static lql_status count_spooled_payload(void *user,
                                        const lql_query_match *match) {
  payload_counts *counts;
  lql_status st;
  lql_error error;

  counts = (payload_counts *)user;
  if (match->payload.kind != LQL_PAYLOAD_SPOOLED ||
      match->payload.size != match->decision.size ||
      match->payload.offset != match->decision.offset) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_error_init(&error);
  st = lql_payload_write_json(&match->payload, counts->sink, &error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  counts->payloads++;
  counts->payload_bytes += match->payload.size;
  return LQL_STATUS_OK;
}

int main(int argc, char **argv) {
  const char *mode;
  const char *expr;
  const char *fixture_path;
  FILE *fixture;
  FILE *sink;
  lql_selector *selector;
  lql_query_result result;
  lql_error error;
  lql_status st;
  payload_counts counts;
  file_reader reader;

  if (argc != 4) {
    fprintf(stderr, "usage: lql_payload_bench MODE SELECTOR FIXTURE\n");
    return 2;
  }
  mode = argv[1];
  expr = argv[2];
  fixture_path = argv[3];

  lql_error_init(&error);
  selector = NULL;
  st = lql_selector_parse(expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "lql_payload_bench: parse selector: %s\n", error.message);
    return 1;
  }

  fixture = fopen(fixture_path, "rb");
  if (fixture == NULL) {
    fprintf(stderr, "lql_payload_bench: failed to open fixture\n");
    lql_selector_free(selector);
    return 1;
  }

  memset(&counts, 0, sizeof(counts));
  memset(&result, 0, sizeof(result));
  if (strcmp(mode, "decision_only_plan") == 0) {
    st = lql_query_file_decisions(selector, fixture, observe_decision, NULL,
                                  &result, &error);
  } else if (strcmp(mode, "plus_value_selector") == 0 ||
             strcmp(mode, "plus_value_plan") == 0) {
    sink = fopen("/dev/null", "wb");
    if (sink == NULL) {
      fprintf(stderr, "lql_payload_bench: failed to open /dev/null\n");
      fclose(fixture);
      lql_selector_free(selector);
      return 1;
    }
    counts.sink = sink;
    st = lql_query_file_matches(selector, fixture, count_payload, &counts,
                                &result, &error);
    fclose(sink);
  } else if (strcmp(mode, "plus_value_openjson_selector") == 0 ||
             strcmp(mode, "plus_value_openjson_plan") == 0) {
    sink = fopen("/dev/null", "wb");
    if (sink == NULL) {
      fprintf(stderr, "lql_payload_bench: failed to open /dev/null\n");
      fclose(fixture);
      lql_selector_free(selector);
      return 1;
    }
    counts.sink = sink;
    reader.file = fixture;
    st = lql_query_source_spooled_matches(selector, read_file_chunk, &reader,
                                          count_spooled_payload, &counts,
                                          &result, &error);
    fclose(sink);
  } else {
    fprintf(stderr, "lql_payload_bench: unsupported mode: %s\n", mode);
    fclose(fixture);
    lql_selector_free(selector);
    return 2;
  }
  fclose(fixture);
  lql_selector_free(selector);

  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "lql_payload_bench: query mode %s: %s\n", mode,
            error.message);
    return 1;
  }

  fputs("candidates=", stdout);
  print_u64(result.candidates_seen);
  fputs(" matches=", stdout);
  print_u64(result.candidates_matched);
  fputs(" payloads=", stdout);
  print_u64(counts.payloads);
  fputs(" payload_bytes=", stdout);
  print_u64(counts.payload_bytes);
  fputc('\n', stdout);
  return 0;
}
