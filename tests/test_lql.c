#include "lql/lql.h"
#include "lql/version.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static int read_tmpfile(FILE *fp, char *buf, size_t cap, size_t *out_len);

static void expect_public_utility_api(void) {
  lql_error error;
  lql_selector *selector;
  char *copy;
  int matched;
  lql_status st;

  lql_error_init(&error);
  if (error.code != LQL_STATUS_OK || error.message[0] != '\0') {
    printf("error init mismatch\n");
    ++failures;
  }
  lql_error_init(NULL);
  if (strcmp(lql_status_string(LQL_STATUS_OK), "ok") != 0 ||
      strcmp(lql_status_string(LQL_STATUS_INVALID_ARGUMENT),
             "invalid argument") != 0 ||
      strcmp(lql_status_string(LQL_STATUS_NO_MEMORY), "out of memory") != 0 ||
      strcmp(lql_status_string(LQL_STATUS_PARSE_ERROR), "parse error") != 0 ||
      strcmp(lql_status_string(LQL_STATUS_JSON_ERROR), "json error") != 0 ||
      strcmp(lql_status_string(LQL_STATUS_UNSUPPORTED), "unsupported") != 0 ||
      strcmp(lql_status_string(LQL_STATUS_STOP), "stop") != 0 ||
      strcmp(lql_status_string((lql_status)999), "unknown") != 0) {
    printf("status string mapping mismatch\n");
    ++failures;
  }

  copy = lql_strdup("hello");
  if (copy == NULL || strcmp(copy, "hello") != 0) {
    printf("lql_strdup copy mismatch\n");
    ++failures;
  }
  lql_free(copy);
  if (lql_strdup(NULL) != NULL) {
    printf("lql_strdup NULL mismatch\n");
    ++failures;
  }
  lql_free(NULL);

  lql_error_init(&error);
  st = lql_selector_parse("/status=open", NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "out selector required") != 0) {
    printf("selector parse NULL out mismatch: %s\n", error.message);
    ++failures;
  }

  if (!lql_selector_is_empty(NULL)) {
    printf("NULL selector should be empty\n");
    ++failures;
  }
  selector = NULL;
  lql_error_init(&error);
  st = lql_selector_parse("", &selector, &error);
  if (st != LQL_STATUS_OK || !lql_selector_is_empty(selector)) {
    printf("empty selector parse mismatch: %s\n", error.message);
    ++failures;
  } else {
    matched = 0;
    st = lql_matches_json(selector, "{\"anything\":true}",
                          strlen("{\"anything\":true}"), &matched, &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("empty selector match-all mismatch: %s\n", error.message);
      ++failures;
    }
  }
  lql_selector_free(selector);

  selector = NULL;
  lql_error_init(&error);
  st = lql_selector_parse("/status=open", &selector, &error);
  if (st != LQL_STATUS_OK || lql_selector_is_empty(selector)) {
    printf("non-empty selector state mismatch: %s\n", error.message);
    ++failures;
  }
  lql_selector_free(selector);

  matched = 1;
  lql_error_init(&error);
  st = lql_matches_json(NULL, NULL, 0u, &matched, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "json and out_matched are required") != 0) {
    printf("matches_json invalid json mismatch: %s\n", error.message);
    ++failures;
  }
  lql_error_init(&error);
  st = lql_matches_json(NULL, "{}", strlen("{}"), NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "json and out_matched are required") != 0) {
    printf("matches_json invalid out mismatch: %s\n", error.message);
    ++failures;
  }
}

static void expect_version_api(void) {
  lql_capabilities caps;

  if (strcmp(lql_version(), LQL_VERSION) != 0) {
    printf("version API mismatch: %s != %s\n", lql_version(), LQL_VERSION);
    ++failures;
  }
  if (strchr(LQL_VERSION, '.') == NULL) {
    printf("version macro is not dotted semver: %s\n", LQL_VERSION);
    ++failures;
  }
  memset(&caps, 0, sizeof(caps));
  lql_capabilities_get(&caps);
  if (!caps.selector_parse || !caps.matches_json ||
      !caps.file_decision_stream || !caps.file_match_stream ||
      !caps.source_decision_stream || !caps.seekable_range_payloads ||
      !caps.source_spooled_match_stream || !caps.spooled_payloads ||
      !caps.projection_file_range || !caps.projection_buffered_json ||
      !caps.compact_file_range || !caps.compact_buffered_json ||
      !caps.mutation_parse || !caps.mutation_file_range ||
      !caps.mutation_buffered_json || !caps.mutation_file_values) {
    printf("capability query omitted an implemented public surface\n");
    ++failures;
  }
  lql_capabilities_get(NULL);
}

typedef struct stream_seen {
  int calls;
  int matched;
  lql_uint64 offsets[4];
  lql_uint64 sizes[4];
  int stop_after_first;
} stream_seen;

typedef struct payload_seen {
  int calls;
  int stop_after_first;
  lql_uint64 offsets[4];
  lql_uint64 sizes[4];
  FILE *out;
} payload_seen;

typedef struct chunk_reader {
  const char *data;
  size_t len;
  size_t offset;
  size_t chunk_size;
  int calls;
} chunk_reader;

static lql_read_result read_chunk(void *user, unsigned char *buffer,
                                  size_t capacity) {
  chunk_reader *reader;
  lql_read_result result;
  size_t remaining;
  size_t want;

  reader = (chunk_reader *)user;
  memset(&result, 0, sizeof(result));
  ++reader->calls;
  if (reader->offset >= reader->len) {
    result.eof = 1;
    return result;
  }
  remaining = reader->len - reader->offset;
  want = remaining;
  if (want > reader->chunk_size) {
    want = reader->chunk_size;
  }
  if (want > capacity) {
    want = capacity;
  }
  memcpy(buffer, reader->data + reader->offset, want);
  reader->offset += want;
  result.bytes_read = want;
  if (reader->offset >= reader->len) {
    result.eof = 1;
  }
  return result;
}

static lql_status record_decision(void *user,
                                  const lql_query_decision *decision) {
  stream_seen *seen = (stream_seen *)user;
  if (seen->calls < 4) {
    seen->offsets[seen->calls] = decision->offset;
    seen->sizes[seen->calls] = decision->size;
  }
  if (decision->matched) {
    ++seen->matched;
  }
  ++seen->calls;
  if (seen->stop_after_first) {
    return LQL_STATUS_STOP;
  }
  return LQL_STATUS_OK;
}

static lql_status record_payload(void *user, const lql_query_match *match) {
  payload_seen *seen = (payload_seen *)user;
  lql_error error;
  lql_status st;

  if (seen->calls < 4) {
    seen->offsets[seen->calls] = match->payload.offset;
    seen->sizes[seen->calls] = match->payload.size;
  }
  if (!match->decision.matched ||
      match->payload.kind != LQL_PAYLOAD_SEEKABLE_RANGE ||
      match->payload.source == NULL ||
      match->payload.offset != match->decision.offset ||
      match->payload.size != match->decision.size ||
      match->payload.index != match->decision.index) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_error_init(&error);
  st = lql_payload_write_json(&match->payload, seen->out, &error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  ++seen->calls;
  if (seen->stop_after_first) {
    return LQL_STATUS_STOP;
  }
  return LQL_STATUS_OK;
}

static lql_status record_spooled_payload(void *user,
                                         const lql_query_match *match) {
  payload_seen *seen = (payload_seen *)user;
  lql_error error;
  lql_status st;

  if (seen->calls < 4) {
    seen->offsets[seen->calls] = match->payload.offset;
    seen->sizes[seen->calls] = match->payload.size;
  }
  if (!match->decision.matched || match->payload.kind != LQL_PAYLOAD_SPOOLED ||
      match->payload.spooled == NULL || match->payload.source != NULL ||
      match->payload.offset != match->decision.offset ||
      match->payload.size != match->decision.size ||
      match->payload.index != match->decision.index) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_error_init(&error);
  st = lql_payload_write_json(&match->payload, seen->out, &error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  ++seen->calls;
  if (seen->stop_after_first) {
    return LQL_STATUS_STOP;
  }
  return LQL_STATUS_OK;
}

static void expect_match(const char *expr, const char *json, int want) {
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int got;

  lql_error_init(&error);
  st = lql_selector_parse(expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("parse failed for %s: %s\n", expr, error.message);
    ++failures;
    return;
  }
  st = lql_matches_json(selector, json, strlen(json), &got, &error);
  if (st != LQL_STATUS_OK) {
    printf("eval failed for %s: %s\n", expr, error.message);
    ++failures;
  } else if (got != want) {
    printf("match mismatch for %s: got %d want %d\n", expr, got, want);
    ++failures;
  }
  lql_selector_free(selector);
}

static void expect_parse_error(const char *expr) {
  lql_selector *selector;
  lql_error error;
  lql_status st;

  selector = NULL;
  lql_error_init(&error);
  st = lql_selector_parse(expr, &selector, &error);
  if (st == LQL_STATUS_OK) {
    printf("parse unexpectedly succeeded for %s\n", expr);
    lql_selector_free(selector);
    ++failures;
  }
}

static void expect_match_or(const char *expr, const char *json, int want) {
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int got;

  lql_error_init(&error);
  st = lql_selector_parse_or(expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("parse-or failed for %s: %s\n", expr, error.message);
    ++failures;
    return;
  }
  st = lql_matches_json(selector, json, strlen(json), &got, &error);
  if (st != LQL_STATUS_OK) {
    printf("eval-or failed for %s: %s\n", expr, error.message);
    ++failures;
  } else if (got != want) {
    printf("or match mismatch for %s: got %d want %d\n", expr, got, want);
    ++failures;
  }
  lql_selector_free(selector);
}

static void expect_stream_file(void) {
  static const char input[] =
      "{\"status\":\"open\"}\n{\"status\":\"closed\"}\n";
  FILE *fp;
  lql_selector *selector;
  lql_query_result result;
  stream_seen seen;
  lql_error error;
  lql_status st;

  memset(&seen, 0, sizeof(seen));
  lql_error_init(&error);
  st = lql_selector_parse("/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  fp = tmpfile();
  if (fp == NULL) {
    printf("tmpfile failed\n");
    lql_selector_free(selector);
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("tmpfile write/seek failed\n");
    fclose(fp);
    lql_selector_free(selector);
    ++failures;
    return;
  }
  memset(&result, 0, sizeof(result));
  st = lql_query_file_decisions(selector, fp, record_decision, &seen, &result,
                                &error);
  fclose(fp);
  lql_selector_free(selector);
  if (st != LQL_STATUS_OK) {
    printf("stream query failed: %s\n", error.message);
    ++failures;
    return;
  }
  if (seen.calls != 2 || seen.matched != 1 ||
      result.candidates_seen != (lql_uint64)2 ||
      result.candidates_matched != (lql_uint64)1 ||
      result.bytes_read != (lql_uint64)38) {
    printf("stream counts mismatch calls=%d matched=%d\n", seen.calls,
           seen.matched);
    ++failures;
  }
  if (seen.offsets[0] != (lql_uint64)0 || seen.sizes[0] != (lql_uint64)17 ||
      seen.offsets[1] != (lql_uint64)18 || seen.sizes[1] != (lql_uint64)19) {
    printf("stream ranges mismatch\n");
    ++failures;
  }
}

static void expect_source_stream(void) {
  static const char input[] =
      "{\"status\":\"closed\"}\n{\"status\":\"open\"}\n{\"status\":\"done\"}\n";
  lql_selector *selector;
  lql_query_options options;
  lql_query_result result;
  stream_seen seen;
  chunk_reader reader;
  lql_error error;
  lql_status st;

  memset(&seen, 0, sizeof(seen));
  memset(&reader, 0, sizeof(reader));
  memset(&options, 0, sizeof(options));
  memset(&result, 0, sizeof(result));
  reader.data = input;
  reader.len = strlen(input);
  reader.chunk_size = 5u;
  options.max_candidates = 2u;
  lql_error_init(&error);
  st = lql_selector_parse("/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("source stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  st = lql_query_source_decisions_with_options(selector, read_chunk, &reader,
                                               &options, record_decision, &seen,
                                               &result, &error);
  lql_selector_free(selector);
  if (st != LQL_STATUS_OK) {
    printf("source stream query failed: %s\n", error.message);
    ++failures;
    return;
  }
  if (reader.calls <= 1) {
    printf("source stream did not use chunked reads\n");
    ++failures;
  }
  if (seen.calls != 2 || seen.matched != 1 ||
      result.candidates_seen != (lql_uint64)2 ||
      result.candidates_matched != (lql_uint64)1 || !result.stopped_early ||
      result.stop_reason != LQL_QUERY_STOP_CANDIDATE_LIMIT) {
    printf("source stream counts mismatch calls=%d matched=%d stop=%d\n",
           seen.calls, seen.matched, (int)result.stop_reason);
    ++failures;
  }
  if (seen.offsets[0] != (lql_uint64)0 || seen.sizes[0] != (lql_uint64)19 ||
      seen.offsets[1] != (lql_uint64)20 || seen.sizes[1] != (lql_uint64)17) {
    printf("source stream ranges mismatch\n");
    ++failures;
  }
}

static void expect_source_spooled_payload_api(void) {
  static const char input[] =
      "{\"status\":\"closed\",\"id\":\"a\"}\n{\"status\":\"open\",\"id\":\"b\"}"
      "\n{\"status\":\"open\",\"id\":\"c\"}\n";
  lql_selector *selector;
  lql_query_options options;
  lql_query_result result;
  payload_seen seen;
  chunk_reader reader;
  lql_error error;
  lql_status st;
  char buf[256];
  size_t len;

  memset(&seen, 0, sizeof(seen));
  memset(&reader, 0, sizeof(reader));
  memset(&options, 0, sizeof(options));
  memset(&result, 0, sizeof(result));
  reader.data = input;
  reader.len = strlen(input);
  reader.chunk_size = 7u;
  seen.stop_after_first = 1;
  seen.out = tmpfile();
  if (seen.out == NULL) {
    printf("source spooled payload tmpfile failed\n");
    ++failures;
    return;
  }
  lql_error_init(&error);
  st = lql_selector_parse("/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("source spooled payload parse failed: %s\n", error.message);
    fclose(seen.out);
    ++failures;
    return;
  }
  st = lql_query_source_spooled_matches_with_options(
      selector, read_chunk, &reader, &options, record_spooled_payload, &seen,
      &result, &error);
  lql_selector_free(selector);
  if (st != LQL_STATUS_OK) {
    printf("source spooled payload query failed: %s\n", error.message);
    fclose(seen.out);
    ++failures;
    return;
  }
  if (seen.calls != 1 || result.candidates_seen != (lql_uint64)2 ||
      result.candidates_matched != (lql_uint64)1 || !result.stopped_early ||
      result.stop_reason != LQL_QUERY_STOP_CALLBACK) {
    printf("source spooled payload counts mismatch calls=%d seen=%lu "
           "matched=%lu stop=%d\n",
           seen.calls, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched, (int)result.stop_reason);
    ++failures;
  }
  if (seen.offsets[0] != (lql_uint64)29 || seen.sizes[0] != (lql_uint64)26) {
    printf("source spooled payload range mismatch\n");
    ++failures;
  }
  if (!read_tmpfile(seen.out, buf, sizeof(buf), &len)) {
    printf("source spooled payload output read failed\n");
    ++failures;
  } else if (strcmp(buf, "{\"status\":\"open\",\"id\":\"b\"}") != 0) {
    printf("source spooled payload output mismatch: %s\n", buf);
    ++failures;
  }
  fclose(seen.out);
}

static void expect_stream_array_items(void) {
  static const char input[] =
      "[{\"status\":\"open\"}, {\"status\":\"closed\"}]";
  FILE *fp;
  lql_selector *selector;
  lql_query_result result;
  stream_seen seen;
  lql_error error;
  lql_status st;

  memset(&seen, 0, sizeof(seen));
  lql_error_init(&error);
  st = lql_selector_parse("/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("array stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  fp = tmpfile();
  if (fp == NULL) {
    printf("array tmpfile failed\n");
    lql_selector_free(selector);
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("array tmpfile write/seek failed\n");
    fclose(fp);
    lql_selector_free(selector);
    ++failures;
    return;
  }
  memset(&result, 0, sizeof(result));
  st = lql_query_file_decisions(selector, fp, record_decision, &seen, &result,
                                &error);
  fclose(fp);
  lql_selector_free(selector);
  if (st != LQL_STATUS_OK) {
    printf("array stream query failed: %s\n", error.message);
    ++failures;
    return;
  }
  if (seen.calls != 2 || seen.matched != 1 ||
      result.candidates_seen != (lql_uint64)2 ||
      result.candidates_matched != (lql_uint64)1 ||
      result.bytes_read != (lql_uint64)40) {
    printf("array stream counts mismatch calls=%d matched=%d\n", seen.calls,
           seen.matched);
    ++failures;
  }
  if (seen.offsets[0] != (lql_uint64)1 || seen.sizes[0] != (lql_uint64)17 ||
      seen.offsets[1] != (lql_uint64)20 || seen.sizes[1] != (lql_uint64)19) {
    printf("array stream ranges mismatch\n");
    ++failures;
  }
}

static void expect_stream_stop_controls(void) {
  static const char input[] =
      "{\"status\":\"open\"}\n{\"status\":\"open\"}\n{\"status\":\"closed\"}\n";
  FILE *fp;
  lql_selector *selector;
  lql_query_options options;
  lql_query_result result;
  stream_seen seen;
  lql_error error;
  lql_status st;

  lql_error_init(&error);
  st = lql_selector_parse("/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("stop controls parse failed: %s\n", error.message);
    ++failures;
    return;
  }

#define RUN_STOP_CASE(label, setup_options, setup_seen, want_calls,            \
                      want_matched, want_reason)                               \
  do {                                                                         \
    fp = tmpfile();                                                            \
    if (fp == NULL) {                                                          \
      printf(label " tmpfile failed\n");                                       \
      ++failures;                                                              \
      break;                                                                   \
    }                                                                          \
    if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||               \
        fseek(fp, 0L, SEEK_SET) != 0) {                                        \
      printf(label " tmpfile write/seek failed\n");                            \
      fclose(fp);                                                              \
      ++failures;                                                              \
      break;                                                                   \
    }                                                                          \
    memset(&options, 0, sizeof(options));                                      \
    memset(&seen, 0, sizeof(seen));                                            \
    memset(&result, 0, sizeof(result));                                        \
    setup_options;                                                             \
    setup_seen;                                                                \
    st = lql_query_file_decisions_with_options(                                \
        selector, fp, &options, record_decision, &seen, &result, &error);      \
    fclose(fp);                                                                \
    if (st != LQL_STATUS_OK) {                                                 \
      printf(label " query failed: %s\n", error.message);                      \
      ++failures;                                                              \
      break;                                                                   \
    }                                                                          \
    if (seen.calls != (want_calls) || seen.matched != (want_matched) ||        \
        !result.stopped_early || result.stop_reason != (want_reason)) {        \
      printf(label                                                             \
             " stop mismatch calls=%d matched=%d stopped=%d reason=%d\n",      \
             seen.calls, seen.matched, result.stopped_early,                   \
             (int)result.stop_reason);                                         \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

  RUN_STOP_CASE("max matches", options.max_matches = 1u, (void)0, 1, 1,
                LQL_QUERY_STOP_MATCH_LIMIT);
  RUN_STOP_CASE("max candidates", options.max_candidates = 2u, (void)0, 2, 2,
                LQL_QUERY_STOP_CANDIDATE_LIMIT);
  RUN_STOP_CASE("max bytes", options.max_bytes_read = 17u, (void)0, 1, 1,
                LQL_QUERY_STOP_BYTE_LIMIT);
  RUN_STOP_CASE("callback stop", (void)0, seen.stop_after_first = 1, 1, 1,
                LQL_QUERY_STOP_CALLBACK);

#undef RUN_STOP_CASE

  lql_selector_free(selector);
}

static void expect_stream_error_api(void) {
  static const char malformed[] = "{\"status\":\"open\"}\n{\"status\":";
  FILE *source;
  FILE *out;
  lql_selector *selector;
  lql_payload payload;
  stream_seen seen;
  payload_seen payload_seen_value;
  chunk_reader reader;
  lql_error error;
  lql_status st;

  selector = NULL;
  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("stream error tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }

  st = lql_selector_parse("/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("stream error selector parse failed: %s\n", error.message);
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  if (fwrite(malformed, 1u, strlen(malformed), source) != strlen(malformed) ||
      fseek(source, 0L, SEEK_SET) != 0) {
    printf("stream error malformed source setup failed\n");
    lql_selector_free(selector);
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  memset(&seen, 0, sizeof(seen));
  lql_error_init(&error);
  st = lql_query_file_decisions(selector, source, record_decision, &seen, NULL,
                                &error);
  if (st != LQL_STATUS_JSON_ERROR) {
    printf("malformed file decision stream status mismatch: %s\n",
           error.message);
    ++failures;
  }
  if (fseek(source, 0L, SEEK_SET) != 0) {
    printf("stream error malformed source rewind failed\n");
    ++failures;
  } else {
    memset(&payload_seen_value, 0, sizeof(payload_seen_value));
    payload_seen_value.out = out;
    lql_error_init(&error);
    st = lql_query_file_matches(selector, source, record_payload,
                                &payload_seen_value, NULL, &error);
    if (st != LQL_STATUS_JSON_ERROR) {
      printf("malformed file match stream status mismatch: %s\n",
             error.message);
      ++failures;
    }
  }
  memset(&seen, 0, sizeof(seen));
  memset(&reader, 0, sizeof(reader));
  reader.data = malformed;
  reader.len = strlen(malformed);
  reader.chunk_size = 5u;
  lql_error_init(&error);
  st = lql_query_source_decisions(selector, read_chunk, &reader,
                                  record_decision, &seen, NULL, &error);
  if (st != LQL_STATUS_JSON_ERROR) {
    printf("malformed source decision stream status mismatch: %s\n",
           error.message);
    ++failures;
  }
  memset(&payload_seen_value, 0, sizeof(payload_seen_value));
  memset(&reader, 0, sizeof(reader));
  reader.data = malformed;
  reader.len = strlen(malformed);
  reader.chunk_size = 5u;
  payload_seen_value.out = out;
  lql_error_init(&error);
  st = lql_query_source_spooled_matches(selector, read_chunk, &reader,
                                        record_spooled_payload,
                                        &payload_seen_value, NULL, &error);
  if (st != LQL_STATUS_JSON_ERROR) {
    printf("malformed source spooled stream status mismatch: %s\n",
           error.message);
    ++failures;
  }
  lql_selector_free(selector);

  lql_error_init(&error);
  st =
      lql_query_file_decisions(NULL, NULL, record_decision, NULL, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "file and on_decision are required") != 0) {
    printf("file decisions NULL file mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = lql_query_file_decisions(NULL, source, NULL, NULL, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "file and on_decision are required") != 0) {
    printf("file decisions NULL callback mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = lql_query_source_decisions(NULL, NULL, NULL, record_decision, NULL, NULL,
                                  &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "read and on_decision are required") != 0) {
    printf("source decisions NULL read mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = lql_query_source_decisions(NULL, read_chunk, NULL, NULL, NULL, NULL,
                                  &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "read and on_decision are required") != 0) {
    printf("source decisions NULL callback mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = lql_query_source_spooled_matches(NULL, NULL, NULL, record_payload, NULL,
                                        NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "read and on_match are required") != 0) {
    printf("source spooled matches NULL read mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = lql_query_source_spooled_matches(NULL, read_chunk, NULL, NULL, NULL,
                                        NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "read and on_match are required") != 0) {
    printf("source spooled matches NULL callback mismatch: %s\n",
           error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = lql_query_file_matches(NULL, NULL, record_payload, NULL, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "file and on_match are required") != 0) {
    printf("file matches NULL file mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = lql_query_file_matches(NULL, source, NULL, NULL, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "file and on_match are required") != 0) {
    printf("file matches NULL callback mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = lql_payload_write_json(NULL, out, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "payload and output file are required") != 0) {
    printf("payload write NULL payload mismatch: %s\n", error.message);
    ++failures;
  }

  memset(&payload, 0, sizeof(payload));
  payload.kind = LQL_PAYLOAD_NONE;
  lql_error_init(&error);
  st = lql_payload_write_json(&payload, out, &error);
  if (st != LQL_STATUS_UNSUPPORTED ||
      strcmp(error.message, "payload is not a seekable source range") != 0) {
    printf("payload write unsupported kind mismatch: %s\n", error.message);
    ++failures;
  }

  payload.kind = LQL_PAYLOAD_SEEKABLE_RANGE;
  payload.source = NULL;
  lql_error_init(&error);
  st = lql_payload_write_json(&payload, out, &error);
  if (st != LQL_STATUS_UNSUPPORTED ||
      strcmp(error.message, "payload is not a seekable source range") != 0) {
    printf("payload write missing source mismatch: %s\n", error.message);
    ++failures;
  }

  payload.kind = LQL_PAYLOAD_SEEKABLE_RANGE;
  payload.source = source;
  lql_error_init(&error);
  st = lql_payload_write_json(&payload, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "payload and output file are required") != 0) {
    printf("payload write NULL out mismatch: %s\n", error.message);
    ++failures;
  }

  fclose(source);
  fclose(out);
}

static int read_tmpfile(FILE *fp, char *buf, size_t cap, size_t *out_len) {
  long end;
  size_t got;
  if (fseek(fp, 0L, SEEK_END) != 0) {
    return 0;
  }
  end = ftell(fp);
  if (end < 0 || (size_t)end + 1u > cap) {
    return 0;
  }
  if (fseek(fp, 0L, SEEK_SET) != 0) {
    return 0;
  }
  got = fread(buf, 1u, (size_t)end, fp);
  if (got != (size_t)end) {
    return 0;
  }
  buf[got] = '\0';
  *out_len = got;
  return 1;
}

static void expect_seekable_payload_api(void) {
  static const char input[] =
      "{\"status\":\"open\",\"id\":1}\n{\"status\":\"closed\",\"id\":2}\n"
      "{\"status\":\"open\",\"id\":3}\n";
  FILE *fp;
  FILE *out;
  lql_selector *selector;
  lql_query_result result;
  payload_seen seen;
  lql_query_options options;
  lql_error error;
  lql_status st;
  char buf[128];
  size_t len;

  fp = tmpfile();
  out = tmpfile();
  if (fp == NULL || out == NULL) {
    printf("payload tmpfile failed\n");
    if (fp != NULL) {
      fclose(fp);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("payload input write failed\n");
    fclose(fp);
    fclose(out);
    ++failures;
    return;
  }
  lql_error_init(&error);
  selector = NULL;
  st = lql_selector_parse("/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("payload parse failed: %s\n", error.message);
    fclose(fp);
    fclose(out);
    ++failures;
    return;
  }
  memset(&seen, 0, sizeof(seen));
  seen.out = out;
  memset(&result, 0, sizeof(result));
  st = lql_query_file_matches(selector, fp, record_payload, &seen, &result,
                              &error);
  if (st != LQL_STATUS_OK) {
    printf("payload query failed: %s\n", error.message);
    fclose(fp);
    fclose(out);
    lql_selector_free(selector);
    ++failures;
    return;
  }
  if (seen.calls != 2 || result.candidates_seen != (lql_uint64)3 ||
      result.candidates_matched != (lql_uint64)2 ||
      result.bytes_read != (lql_uint64)77) {
    printf("payload counts mismatch calls=%d seen=%lu matched=%lu\n",
           seen.calls, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched);
    ++failures;
  }
  if (seen.offsets[0] != (lql_uint64)0 || seen.sizes[0] != (lql_uint64)24 ||
      seen.offsets[1] != (lql_uint64)52 || seen.sizes[1] != (lql_uint64)24) {
    printf("payload ranges mismatch\n");
    ++failures;
  }
  if (!read_tmpfile(out, buf, sizeof(buf), &len)) {
    printf("payload output read failed\n");
    ++failures;
  } else if (strcmp(buf, "{\"status\":\"open\",\"id\":1}"
                         "{\"status\":\"open\",\"id\":3}") != 0) {
    printf("payload output mismatch: %s\n", buf);
    ++failures;
  }

  if (fseek(fp, 0L, SEEK_SET) != 0) {
    printf("payload rewind failed\n");
    ++failures;
  } else {
    memset(&seen, 0, sizeof(seen));
    memset(&options, 0, sizeof(options));
    memset(&result, 0, sizeof(result));
    seen.out = out;
    seen.stop_after_first = 1;
    st = lql_query_file_matches_with_options(
        selector, fp, &options, record_payload, &seen, &result, &error);
    if (st != LQL_STATUS_OK) {
      printf("payload stop query failed: %s\n", error.message);
      ++failures;
    } else if (seen.calls != 1 || !result.stopped_early ||
               result.stop_reason != LQL_QUERY_STOP_CALLBACK) {
      printf("payload stop mismatch calls=%d stopped=%d reason=%d\n",
             seen.calls, result.stopped_early, (int)result.stop_reason);
      ++failures;
    }
  }

  lql_selector_free(selector);
  fclose(fp);
  fclose(out);
}

static void expect_projection_api(void) {
  static const char first[] =
      "{\"id\":\"a\",\"count\":1,\"nested\":{\"x\":true},\"items\":[{\"sku\":"
      "\"A\"},{\"sku\":\"B\"}]}";
  static const char second[] = "{\"id\":\"b\"}";
  const char *fields[4];
  const char *missing[1];
  const char *invalid[1];
  const char *root[1];
  const char *index[1];
  const char *conflict[2];
  const char *root_field[1];
  FILE *source;
  FILE *out;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  int found;
  char buf[128];
  size_t len;

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("projection tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite(first, 1u, strlen(first), source) != strlen(first) ||
      fputc('\n', source) == EOF ||
      fwrite(second, 1u, strlen(second), source) != strlen(second)) {
    printf("projection source write failed\n");
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }

  fields[0] = "/id";
  fields[1] = "/nested/x";
  fields[2] = "/nested/x";
  fields[3] = "/items/1/sku";
  projection = NULL;
  lql_error_init(&error);
  st = lql_projection_parse(fields, 4u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("projection parse failed: %s\n", error.message);
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  found = 0;
  st = lql_project_file_range(projection, source, 0u, (lql_uint64)strlen(first),
                              out, &found, &error);
  if (st != LQL_STATUS_OK) {
    printf("projection range failed: %s\n", error.message);
    ++failures;
  } else if (!found) {
    printf("projection expected found\n");
    ++failures;
  } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
             strcmp(buf, "{\"id\":\"a\",\"nested\":{\"x\":true},\"items\":["
                         "null,{\"sku\":\"B\"}]}") != 0) {
    printf("projection output mismatch: %s\n", buf);
    ++failures;
  }
  lql_projection_free(projection);
  fclose(out);

  out = tmpfile();
  root_field[0] = "/id";
  projection = NULL;
  lql_error_init(&error);
  st = lql_projection_parse(root_field, 1u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("root field projection parse failed: %s\n", error.message);
    ++failures;
  } else {
    found = 0;
    st = lql_project_file_range(
        projection, source, (lql_uint64)(strlen(first) + 1u),
        (lql_uint64)strlen(second), out, &found, &error);
    if (st != LQL_STATUS_OK) {
      printf("second object projection failed: %s\n", error.message);
      ++failures;
    }
  }
  lql_projection_free(projection);
  fclose(out);

  out = tmpfile();
  missing[0] = "/missing";
  projection = NULL;
  lql_error_init(&error);
  st = lql_projection_parse(missing, 1u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("missing projection parse failed: %s\n", error.message);
    ++failures;
  } else {
    found = 1;
    st = lql_project_file_range(projection, source, 0u,
                                (lql_uint64)strlen(first), out, &found, &error);
    if (st != LQL_STATUS_OK) {
      printf("missing projection range failed: %s\n", error.message);
      ++failures;
    } else if (found) {
      printf("missing projection unexpectedly found\n");
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) || len != 0u) {
      printf("missing projection wrote output\n");
      ++failures;
    }
  }
  lql_projection_free(projection);
  fclose(out);

  invalid[0] = "/items/999999999999999999999999/sku";
  projection = NULL;
  lql_error_init(&error);
  st = lql_projection_parse(invalid, 1u, &projection, &error);
  if (st == LQL_STATUS_OK) {
    printf("oversized array projection path parsed\n");
    lql_projection_free(projection);
    ++failures;
  }
  root[0] = "/";
  projection = NULL;
  st = lql_projection_parse(root, 1u, &projection, &error);
  if (st == LQL_STATUS_OK) {
    printf("root projection path parsed\n");
    lql_projection_free(projection);
    ++failures;
  }
  index[0] = "/0/id";
  projection = NULL;
  st = lql_projection_parse(index, 1u, &projection, &error);
  if (st == LQL_STATUS_OK) {
    printf("leading index projection path parsed\n");
    lql_projection_free(projection);
    ++failures;
  }
  conflict[0] = "/nested";
  conflict[1] = "/nested/x";
  projection = NULL;
  st = lql_projection_parse(conflict, 2u, &projection, &error);
  if (st == LQL_STATUS_OK) {
    printf("conflicting projection paths parsed\n");
    lql_projection_free(projection);
    ++failures;
  }

  out = tmpfile();
  root_field[0] = "/id";
  projection = NULL;
  lql_error_init(&error);
  st = lql_projection_parse(root_field, 1u, &projection, &error);
  if (st != LQL_STATUS_OK || out == NULL) {
    printf("large-offset projection setup failed\n");
    ++failures;
  } else {
    found = 1;
    st = lql_project_file_range(projection, source, ~(lql_uint64)0,
                                (lql_uint64)strlen(first), out, &found, &error);
    if (st != LQL_STATUS_JSON_ERROR || found) {
      printf("large-offset projection unexpectedly succeeded\n");
      ++failures;
    }
  }
  lql_projection_free(projection);
  if (out != NULL) {
    fclose(out);
  }
  fclose(source);

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("projection scalar tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite("7", 1u, 1u, source) != 1u) {
    printf("projection scalar write failed\n");
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  root_field[0] = "/id";
  projection = NULL;
  lql_error_init(&error);
  st = lql_projection_parse(root_field, 1u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("scalar projection parse failed: %s\n", error.message);
    ++failures;
  } else {
    found = 0;
    st =
        lql_project_file_range(projection, source, 0u, 1u, out, &found, &error);
    if (st == LQL_STATUS_OK) {
      printf("scalar projection unexpectedly succeeded\n");
      ++failures;
    }
  }
  lql_projection_free(projection);
  fclose(source);
  fclose(out);
}

static void expect_buffered_projection_api(void) {
  static const char doc[] =
      "{\"id\":\"a\",\"count\":1,\"nested\":{\"x\":true},\"items\":[{\"sku\":"
      "\"A\"},{\"sku\":\"B\"}]}";
  const char *fields[3];
  const char *missing[1];
  FILE *out;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  int found;
  char buf[128];
  size_t len;

  out = tmpfile();
  if (out == NULL) {
    printf("buffered projection tmpfile failed\n");
    ++failures;
    return;
  }
  fields[0] = "/id";
  fields[1] = "/nested/x";
  fields[2] = "/items/1/sku";
  projection = NULL;
  lql_error_init(&error);
  st = lql_projection_parse(fields, 3u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("buffered projection parse failed: %s\n", error.message);
    fclose(out);
    ++failures;
    return;
  }
  found = 0;
  st = lql_project_json(projection, doc, strlen(doc), out, &found, &error);
  if (st != LQL_STATUS_OK) {
    printf("buffered projection failed: %s\n", error.message);
    ++failures;
  } else if (!found) {
    printf("buffered projection expected found\n");
    ++failures;
  } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
             strcmp(buf, "{\"id\":\"a\",\"nested\":{\"x\":true},\"items\":["
                         "null,{\"sku\":\"B\"}]}") != 0) {
    printf("buffered projection output mismatch: %s\n", buf);
    ++failures;
  }
  lql_projection_free(projection);
  fclose(out);

  out = tmpfile();
  missing[0] = "/missing";
  projection = NULL;
  lql_error_init(&error);
  st = lql_projection_parse(missing, 1u, &projection, &error);
  if (st != LQL_STATUS_OK || out == NULL) {
    printf("buffered missing projection setup failed\n");
    ++failures;
  } else {
    found = 1;
    st = lql_project_json(projection, doc, strlen(doc), out, &found, &error);
    if (st != LQL_STATUS_OK) {
      printf("buffered missing projection failed: %s\n", error.message);
      ++failures;
    } else if (found) {
      printf("buffered missing projection unexpectedly found\n");
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) || len != 0u) {
      printf("buffered missing projection wrote output\n");
      ++failures;
    }
  }
  lql_projection_free(projection);
  if (out != NULL) {
    fclose(out);
  }
}

static void expect_projection_compact_error_api(void) {
  const char *field;
  FILE *out;
  FILE *source;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  int found;

  out = tmpfile();
  source = tmpfile();
  if (out == NULL || source == NULL) {
    printf("projection/compact error tmpfile failed\n");
    if (out != NULL) {
      fclose(out);
    }
    if (source != NULL) {
      fclose(source);
    }
    ++failures;
    return;
  }

  projection = (lql_projection *)1;
  lql_error_init(&error);
  st = lql_projection_parse(NULL, 0u, &projection, &error);
  if (st != LQL_STATUS_PARSE_ERROR || projection != NULL ||
      strcmp(error.message, "projection fields required") != 0) {
    printf("projection parse empty-field error mismatch: %s\n", error.message);
    ++failures;
  }

  field = "/id";
  lql_error_init(&error);
  st = lql_projection_parse(&field, 1u, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "out projection required") != 0) {
    printf("projection parse NULL out error mismatch: %s\n", error.message);
    ++failures;
  }

  projection = NULL;
  lql_error_init(&error);
  st = lql_projection_parse(&field, 1u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("projection/compact error projection setup failed: %s\n",
           error.message);
    ++failures;
  } else {
    found = 1;
    lql_error_init(&error);
    st = lql_project_json(projection, NULL, 0u, out, &found, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT || found ||
        strcmp(error.message, "json is required") != 0) {
      printf("project_json NULL json error mismatch: %s\n", error.message);
      ++failures;
    }

    found = 1;
    lql_error_init(&error);
    st = lql_project_json(NULL, "{}", strlen("{}"), out, &found, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT || found ||
        strcmp(error.message,
               "projection, reader, out, and out_found are required") != 0) {
      printf("project_json NULL projection error mismatch: %s\n",
             error.message);
      ++failures;
    }

    found = 1;
    lql_error_init(&error);
    st = lql_project_file_range(projection, NULL, 0u, 2u, out, &found, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT || found ||
        strcmp(error.message, "projection file is required") != 0) {
      printf("project_file_range NULL file error mismatch: %s\n",
             error.message);
      ++failures;
    }
  }
  lql_projection_free(projection);

  lql_error_init(&error);
  st = lql_compact_file_range(NULL, 0u, 0u, out, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "file and out are required") != 0) {
    printf("compact_file_range NULL file error mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = lql_compact_file_range(source, 0u, 0u, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "file and out are required") != 0) {
    printf("compact_file_range NULL out error mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = lql_compact_json(NULL, 0u, out, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "json and out are required") != 0) {
    printf("compact_json NULL json error mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = lql_compact_json("{}", strlen("{}"), NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "json and out are required") != 0) {
    printf("compact_json NULL out error mismatch: %s\n", error.message);
    ++failures;
  }

  fclose(out);
  fclose(source);
}

static void expect_compact_api(void) {
  FILE *source;
  FILE *out;
  lql_error error;
  lql_status st;
  char buf[256];
  size_t len;
  static const char first[] = "{\n  \"id\" : \"a\",\n  \"items\" : [ 1, 2 ]\n}";
  static const char second[] = "{ \"id\" : \"b\" }";

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("compact tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite(first, 1u, strlen(first), source) != strlen(first) ||
      fputc('\n', source) == EOF ||
      fwrite(second, 1u, strlen(second), source) != strlen(second)) {
    printf("compact source write failed\n");
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  lql_error_init(&error);
  st = lql_compact_file_range(source, 0u, (lql_uint64)strlen(first), out,
                              &error);
  if (st != LQL_STATUS_OK) {
    printf("compact file range failed: %s\n", error.message);
    ++failures;
  } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
             strcmp(buf, "{\"id\":\"a\",\"items\":[1,2]}") != 0) {
    printf("compact file output mismatch: %s\n", buf);
    ++failures;
  }
  fclose(out);

  out = tmpfile();
  lql_error_init(&error);
  st = lql_compact_json("{ \"ok\" : true }", strlen("{ \"ok\" : true }"), out,
                        &error);
  if (st != LQL_STATUS_OK) {
    printf("compact buffer failed: %s\n", error.message);
    ++failures;
  } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
             strcmp(buf, "{\"ok\":true}") != 0) {
    printf("compact buffer output mismatch: %s\n", buf);
    ++failures;
  }
  fclose(out);

  out = tmpfile();
  lql_error_init(&error);
  st = lql_compact_file_range(source, 0u, (lql_uint64)3, out, &error);
  if (st != LQL_STATUS_JSON_ERROR) {
    printf("compact invalid range unexpectedly succeeded\n");
    ++failures;
  }
  fclose(source);
  fclose(out);
}

static void expect_mutation_plan_api(void) {
  lql_mutation_plan *plan;
  lql_mutation_parse_options options;
  lql_error error;
  lql_status st;
  const char *valid[6];
  const char *wildcards[3];
  const char *file_backed[2];
  const char *invalid_default[8];
  const char *invalid_file_options[4];
  const char *blank;
  size_t i;

  valid[0] = "/state/progress=ready";
  valid[1] = "/state/metrics++";
  valid[2] = "/state/details{/owner=\"alice\",/note=\"hi, world\"}";
  valid[3] = "/state/metrics=+3";
  valid[4] = "time:/state/updated=NOW";
  valid[5] = "rm:/state/legacy";
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(valid, 6u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else if (lql_mutation_plan_count(plan) != 7u) {
    printf("mutation plan count mismatch: %lu\n",
           (unsigned long)lql_mutation_plan_count(plan));
    ++failures;
  }
  lql_mutation_plan_free(plan);

  wildcards[0] = "/items/*/status=ready";
  wildcards[1] = "/groups/.../sku=ok";
  wildcards[2] = "/records[]/count=+1";
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(wildcards, 3u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("wildcard mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else if (lql_mutation_plan_count(plan) != 3u) {
    printf("wildcard mutation plan count mismatch: %lu\n",
           (unsigned long)lql_mutation_plan_count(plan));
    ++failures;
  }
  lql_mutation_plan_free(plan);

  invalid_default[0] = "badexpr";
  invalid_default[1] = "/";
  invalid_default[2] = "/count=+0";
  invalid_default[3] = "time:/state/updated=tomorrowish";
  invalid_default[4] = "file:/payload=blob.txt";
  invalid_default[5] = "time:/state/updated=2025-01-01";
  invalid_default[6] = "textfile:/payload=blob.txt";
  invalid_default[7] = "base64file:/payload=blob.bin";
  for (i = 0u; i < sizeof(invalid_default) / sizeof(invalid_default[0]); ++i) {
    plan = NULL;
    lql_error_init(&error);
    st = lql_mutation_plan_parse(&invalid_default[i], 1u, &plan, &error);
    if (st != LQL_STATUS_PARSE_ERROR || plan != NULL) {
      printf("invalid mutation parse mismatch: %s status=%s error=%s\n",
             invalid_default[i], lql_status_string(st), error.message);
      ++failures;
    }
    lql_mutation_plan_free(plan);
  }

  blank = "";
  plan = (lql_mutation_plan *)1;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(&blank, 1u, &plan, &error);
  if (st != LQL_STATUS_PARSE_ERROR || plan != NULL ||
      strcmp(error.message, "no valid field mutations parsed") != 0) {
    printf("blank mutation parse mismatch: %s\n", error.message);
    ++failures;
  }

  file_backed[0] = "textfile:/payload=blob.txt";
  file_backed[1] = "base64file:/encoded=blob.bin";
  memset(&options, 0, sizeof(options));
  options.enable_file_values = 1;
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse_with_options(file_backed, 1u, &options, &plan,
                                            &error);
  if (st != LQL_STATUS_PARSE_ERROR || plan != NULL) {
    printf("relative file-backed mutation status mismatch: %s\n",
           error.message);
    ++failures;
  }
  lql_mutation_plan_free(plan);

  options.file_value_base_dir = "/tmp";
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse_with_options(file_backed, 2u, &options, &plan,
                                            &error);
  if (st != LQL_STATUS_OK) {
    printf("enabled file-backed mutation parse failed: %s\n", error.message);
    ++failures;
  } else if (lql_mutation_plan_count(plan) != 2u) {
    printf("enabled file-backed mutation count mismatch: %lu\n",
           (unsigned long)lql_mutation_plan_count(plan));
    ++failures;
  }
  lql_mutation_plan_free(plan);

  invalid_file_options[0] = "file:/payload++";
  invalid_file_options[1] = "file:rm:/payload=blob.txt";
  invalid_file_options[2] = "file:time:/payload=blob.txt";
  invalid_file_options[3] = "textfile:/payload=blob.txt";
  for (i = 0u;
       i < sizeof(invalid_file_options) / sizeof(invalid_file_options[0]);
       ++i) {
    memset(&options, 0, sizeof(options));
    options.enable_file_values = 1;
    if (i < 3u) {
      options.file_value_base_dir = "/tmp";
    }
    plan = NULL;
    lql_error_init(&error);
    st = lql_mutation_plan_parse_with_options(&invalid_file_options[i], 1u,
                                              &options, &plan, &error);
    if (st != LQL_STATUS_PARSE_ERROR || plan != NULL) {
      printf("invalid file-backed mutation parsed: %s status=%s error=%s\n",
             invalid_file_options[i], lql_status_string(st), error.message);
      ++failures;
    }
    lql_mutation_plan_free(plan);
  }
}

static void expect_mutation_error_api(void) {
  FILE *source;
  FILE *out;
  lql_mutation_plan *plan;
  lql_error error;
  lql_status st;
  const char *expr;

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("mutation error tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }

  if (lql_mutation_plan_count(NULL) != 0u) {
    printf("NULL mutation plan count mismatch\n");
    ++failures;
  }
  lql_mutation_plan_free(NULL);

  expr = "/status=done";
  lql_error_init(&error);
  st = lql_mutation_plan_parse(&expr, 1u, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "out is required") != 0) {
    printf("mutation parse NULL out mismatch: %s\n", error.message);
    ++failures;
  }

  plan = (lql_mutation_plan *)1;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(NULL, 0u, &plan, &error);
  if (st != LQL_STATUS_PARSE_ERROR || plan != NULL ||
      strcmp(error.message, "no field mutations provided") != 0) {
    printf("mutation parse empty input mismatch: %s\n", error.message);
    ++failures;
  }

  expr = NULL;
  plan = (lql_mutation_plan *)1;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(&expr, 1u, &plan, &error);
  if (st != LQL_STATUS_PARSE_ERROR || plan != NULL ||
      strcmp(error.message, "no valid field mutations parsed") != 0) {
    printf("mutation parse NULL expression mismatch: %s\n", error.message);
    ++failures;
  }

  expr = "/status=done";
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(&expr, 1u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("mutation error plan setup failed: %s\n", error.message);
    ++failures;
  } else {
    lql_error_init(&error);
    st = lql_mutate_file_range_root_fields(NULL, source, 0u, 2u, out, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, file, and out are required") != 0) {
      printf("root mutation NULL plan mismatch: %s\n", error.message);
      ++failures;
    }

    lql_error_init(&error);
    st = lql_mutate_file_range_root_fields(plan, NULL, 0u, 2u, out, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, file, and out are required") != 0) {
      printf("root mutation NULL file mismatch: %s\n", error.message);
      ++failures;
    }

    lql_error_init(&error);
    st = lql_mutate_file_range_paths(plan, source, 0u, 2u, NULL, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, file, and out are required") != 0) {
      printf("path mutation NULL out mismatch: %s\n", error.message);
      ++failures;
    }

    lql_error_init(&error);
    st = lql_mutate_json(NULL, "{}", strlen("{}"), out, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, json, and out are required") != 0) {
      printf("json mutation NULL plan mismatch: %s\n", error.message);
      ++failures;
    }

    lql_error_init(&error);
    st = lql_mutate_json(plan, NULL, 0u, out, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, json, and out are required") != 0) {
      printf("json mutation NULL json mismatch: %s\n", error.message);
      ++failures;
    }

    lql_error_init(&error);
    st = lql_mutate_json(plan, "{}", strlen("{}"), NULL, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, json, and out are required") != 0) {
      printf("json mutation NULL out mismatch: %s\n", error.message);
      ++failures;
    }
  }
  lql_mutation_plan_free(plan);
  fclose(source);
  fclose(out);
}

static void expect_root_field_mutation_api(void) {
  FILE *source;
  FILE *out;
  FILE *payload;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *exprs[5];
  const char *file_exprs[4];
  lql_mutation_parse_options options;
  char buf[256];
  size_t len;
  static const char doc[] = "{\"status\":\"open\",\"count\":1,\"old\":true}";

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("mutation tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite(doc, 1u, strlen(doc), source) != strlen(doc)) {
    printf("mutation source write failed\n");
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  exprs[0] = "/status=done";
  exprs[1] = "/count++";
  exprs[2] = "rm:/old";
  exprs[3] = "/missing=value";
  exprs[4] = "/quoted_number=\"2\"";
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(exprs, 5u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("root mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = lql_mutate_file_range_root_fields(
        plan, source, 0u, (lql_uint64)strlen(doc), out, &error);
    if (st != LQL_STATUS_OK) {
      printf("root mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"status\":\"done\",\"count\":2,\"missing\":\"value\","
                      "\"quoted_number\":2}") != 0) {
      printf("root mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  lql_mutation_plan_free(plan);
  fclose(out);

  payload = fopen("lql-test-payload.txt", "wb");
  if (payload == NULL) {
    printf("file-backed mutation payload write failed\n");
    fclose(source);
    ++failures;
    return;
  }
  if (fwrite("hi", 1u, 2u, payload) != 2u || fclose(payload) != 0) {
    printf("file-backed mutation payload write failed\n");
    fclose(source);
    ++failures;
    return;
  }
  payload = fopen("lql-test-payload.bin", "wb");
  if (payload == NULL) {
    printf("file-backed mutation binary payload write failed\n");
    fclose(source);
    ++failures;
    return;
  }
  if (fwrite("\000\001\002a", 1u, 4u, payload) != 4u || fclose(payload) != 0) {
    printf("file-backed mutation binary payload write failed\n");
    fclose(source);
    ++failures;
    return;
  }
  out = tmpfile();
  file_exprs[0] = "textfile:/text_payload=lql-test-payload.txt";
  file_exprs[1] = "base64file:/bin_payload=lql-test-payload.txt";
  file_exprs[2] = "file:/auto_text=lql-test-payload.txt";
  file_exprs[3] = "file:/auto_bin=lql-test-payload.bin";
  memset(&options, 0, sizeof(options));
  options.enable_file_values = 1;
  options.file_value_base_dir = ".";
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse_with_options(file_exprs, 4u, &options, &plan,
                                            &error);
  if (st != LQL_STATUS_OK) {
    printf("file-backed root mutation parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = lql_mutate_file_range_root_fields(
        plan, source, 0u, (lql_uint64)strlen(doc), out, &error);
    if (st != LQL_STATUS_OK) {
      printf("file-backed root mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"status\":\"open\",\"count\":1,\"old\":true,"
                      "\"text_payload\":\"hi\",\"bin_payload\":\"aGk=\","
                      "\"auto_text\":\"hi\",\"auto_bin\":\"AAECYQ==\"}") != 0) {
      printf("file-backed root mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  lql_mutation_plan_free(plan);
  fclose(out);
  remove("lql-test-payload.txt");
  remove("lql-test-payload.bin");

  out = tmpfile();
  exprs[0] = "/nested/status=done";
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(exprs, 1u, &plan, &error);
  if (st == LQL_STATUS_OK) {
    st = lql_mutate_file_range_root_fields(
        plan, source, 0u, (lql_uint64)strlen(doc), out, &error);
    if (st != LQL_STATUS_UNSUPPORTED) {
      printf("nested root mutation unexpectedly supported\n");
      ++failures;
    }
  }
  lql_mutation_plan_free(plan);
  fclose(source);
  fclose(out);
}

static void expect_path_mutation_api(void) {
  FILE *source;
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *exprs[7];
  char buf[512];
  size_t len;
  static const char doc[] =
      "{\"state\":{\"status\":\"open\",\"count\":1,\"old\":true},\"id\":\"a\"}";

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("path mutation tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite(doc, 1u, strlen(doc), source) != strlen(doc)) {
    printf("path mutation source write failed\n");
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  exprs[0] = "/state/status=done";
  exprs[1] = "/state/count++";
  exprs[2] = "rm:/state/old";
  exprs[3] = "/state/missing=value";
  exprs[4] = "/added/nested=ok";
  exprs[5] = "/added/other=2";
  exprs[6] = "time:/state/updated=2025-01-02T03:04:05.123456789+02:30";
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(exprs, 7u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("path mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = lql_mutate_file_range_paths(plan, source, 0u, (lql_uint64)strlen(doc),
                                     out, &error);
    if (st != LQL_STATUS_OK) {
      printf("path mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf, "{\"state\":{\"status\":\"done\",\"count\":2,"
                           "\"missing\":\"value\",\"updated\":\"2025-01-"
                           "02T00:34:05.123456789Z\"},\"id\":\"a\","
                           "\"added\":{\"nested\":\"ok\",\"other\":2}}") != 0) {
      printf("path mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  lql_mutation_plan_free(plan);
  fclose(source);
  fclose(out);
}

static void expect_buffered_mutation_api(void) {
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *exprs[3];
  char buf[256];
  size_t len;
  static const char doc[] =
      "{\"state\":{\"status\":\"open\",\"count\":1,\"old\":true},\"id\":\"a\"}";

  out = tmpfile();
  if (out == NULL) {
    printf("buffered mutation tmpfile failed\n");
    ++failures;
    return;
  }
  exprs[0] = "/state/status=done";
  exprs[1] = "/state/count++";
  exprs[2] = "rm:/state/old";
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(exprs, 3u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("buffered mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = lql_mutate_json(plan, doc, strlen(doc), out, &error);
    if (st != LQL_STATUS_OK) {
      printf("buffered mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"state\":{\"status\":\"done\",\"count\":2},\"id\":"
                      "\"a\"}") != 0) {
      printf("buffered mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  lql_mutation_plan_free(plan);
  fclose(out);
}

static void expect_array_element_mutation_api(void) {
  FILE *source;
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *exprs[4];
  char buf[512];
  size_t len;
  static const char doc[] = "{\"items\":[1,2,{\"status\":\"old\"}],\"other\":["
                            "\"x\"]}";

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("array mutation tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite(doc, 1u, strlen(doc), source) != strlen(doc)) {
    printf("array mutation source write failed\n");
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  exprs[0] = "/items/0=ready";
  exprs[1] = "/items/1++";
  exprs[2] = "rm:/items/2";
  exprs[3] = "/other/0=done";
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(exprs, 4u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("array mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = lql_mutate_file_range_paths(plan, source, 0u, (lql_uint64)strlen(doc),
                                     out, &error);
    if (st != LQL_STATUS_OK) {
      printf("array mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"items\":{\"0\":\"ready\",\"1\":1},\"other\":{\"0\":"
                      "\"done\"}}") != 0) {
      printf("array mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  lql_mutation_plan_free(plan);
  fclose(source);
  fclose(out);
}

static void expect_wildcard_mutation_api(void) {
  FILE *source;
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *exprs[3];
  char buf[512];
  size_t len;
  static const char doc[] =
      "{\"items\":[{\"status\":\"old\"},{\"status\":\"new\"}],\"labels\":{"
      "\"env\":\"prod\",\"tier\":\"edge\"},\"numeric_object\":{\"0\":{"
      "\"status\":"
      "\"unchanged\"}}}";

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("wildcard mutation tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite(doc, 1u, strlen(doc), source) != strlen(doc)) {
    printf("wildcard mutation source write failed\n");
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  exprs[0] = "/items[]/status=ready";
  exprs[1] = "/labels/*=tagged";
  exprs[2] = "/numeric_object[]/status=bad";
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(exprs, 3u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("wildcard mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = lql_mutate_file_range_paths(plan, source, 0u, (lql_uint64)strlen(doc),
                                     out, &error);
    if (st != LQL_STATUS_OK) {
      printf("wildcard mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"items\":[{\"status\":\"ready\"},{\"status\":"
                      "\"ready\"}],\"labels\":{\"env\":\"tagged\",\"tier\":"
                      "\"tagged\"},\"numeric_object\":{\"0\":{\"status\":"
                      "\"unchanged\"}}}") != 0) {
      printf("wildcard mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  lql_mutation_plan_free(plan);
  fclose(source);
  fclose(out);
}

static void expect_recursive_mutation_api(void) {
  FILE *source;
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *exprs[4];
  char buf[1024];
  size_t len;
  static const char doc[] =
      "{\"items\":[{\"status\":\"old\"}],\"boxes\":{\"a\":{\"status\":\"old\"}}"
      ","
      "\"groups\":[{\"items\":[{\"sku\":\"A\",\"count\":1,\"drop\":true}]}]}";

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("recursive mutation tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite(doc, 1u, strlen(doc), source) != strlen(doc)) {
    printf("recursive mutation source write failed\n");
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  exprs[0] = "/items/**/status=ready";
  exprs[1] = "/boxes/**/status=ready";
  exprs[2] = "/groups/.../sku=Z";
  exprs[3] = "/groups/.../count=+2";
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(exprs, 4u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("recursive mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = lql_mutate_file_range_paths(plan, source, 0u, (lql_uint64)strlen(doc),
                                     out, &error);
    if (st != LQL_STATUS_OK) {
      printf("recursive mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"items\":[{\"status\":\"ready\"}],\"boxes\":{\"a\":{"
                      "\"status\":\"ready\"}},\"groups\":[{\"items\":[{"
                      "\"sku\":\"Z\",\"count\":3,\"drop\":true}]}]}") != 0) {
      printf("recursive mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  lql_mutation_plan_free(plan);
  fclose(source);
  fclose(out);
}

static void expect_array_wildcard_value_mutation_api(void) {
  FILE *source;
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *exprs[5];
  char buf[1024];
  size_t len;
  static const char doc[] =
      "{\"nums\":[1,2],\"words\":[\"a\",\"b\"],\"drops\":[true,false],"
      "\"objects\":[{\"a\":1}],\"groups\":[{\"items\":[{\"count\":1}]}]}";

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("array wildcard value mutation tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite(doc, 1u, strlen(doc), source) != strlen(doc)) {
    printf("array wildcard value mutation source write failed\n");
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  exprs[0] = "/nums[]=+2";
  exprs[1] = "/words[]=ready";
  exprs[2] = "rm:/drops[]";
  exprs[3] = "/objects[]=done";
  exprs[4] = "/groups/.../count=+2";
  plan = NULL;
  lql_error_init(&error);
  st = lql_mutation_plan_parse(exprs, 5u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("array wildcard value mutation plan parse failed: %s\n",
           error.message);
    ++failures;
  } else {
    st = lql_mutate_file_range_paths(plan, source, 0u, (lql_uint64)strlen(doc),
                                     out, &error);
    if (st != LQL_STATUS_OK) {
      printf("array wildcard value mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf, "{\"nums\":[3,4],\"words\":[\"ready\",\"ready\"],"
                           "\"drops\":[null,null],\"objects\":[\"done\"],"
                           "\"groups\":[{\"items\":[{\"count\":3}]}]}") != 0) {
      printf("array wildcard value mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  lql_mutation_plan_free(plan);
  fclose(source);
  fclose(out);
}

typedef void (*sdk_parity_test_fn)(void);

typedef struct sdk_parity_requirement {
  const char *surface;
  const char *requirement;
  sdk_parity_test_fn test;
} sdk_parity_requirement;

static void expect_selector_match_api(void);
static void expect_selector_parse_error_api(void);

static void expect_sdk_parity_manifest(void) {
  static const sdk_parity_requirement manifest[] = {
      {"utility",
       "status, error, ownership, selector emptiness, and invalid "
       "argument helpers",
       expect_public_utility_api},
      {"version", "version and capability public API", expect_version_api},
      {"selector",
       "scalar, string, numeric, temporal, path, wildcard, "
       "existence, and logical matching",
       expect_selector_match_api},
      {"selector", "OR parse/evaluation public API", expect_selector_match_api},
      {"selector", "parse-error invariants", expect_selector_parse_error_api},
      {"streaming", "seekable FILE decision streams", expect_stream_file},
      {"streaming", "callback-source decision streams", expect_source_stream},
      {"streaming", "callback-source spooled matched payloads",
       expect_source_spooled_payload_api},
      {"streaming", "top-level array candidate streams",
       expect_stream_array_items},
      {"streaming", "stream stop controls", expect_stream_stop_controls},
      {"streaming", "query and payload public API error contracts",
       expect_stream_error_api},
      {"streaming", "seekable matched payload ranges",
       expect_seekable_payload_api},
      {"projection", "seekable file-range projection", expect_projection_api},
      {"projection", "caller-buffered JSON projection",
       expect_buffered_projection_api},
      {"projection", "projection and compact public API error contracts",
       expect_projection_compact_error_api},
      {"compact", "seekable and buffered JSON compaction", expect_compact_api},
      {"mutation", "mutation parse/plan public API success and parse errors",
       expect_mutation_plan_api},
      {"mutation", "mutation public API error contracts",
       expect_mutation_error_api},
      {"mutation", "root field mutation over seekable ranges",
       expect_root_field_mutation_api},
      {"mutation", "nested path mutation over seekable ranges",
       expect_path_mutation_api},
      {"mutation", "caller-buffered JSON mutation",
       expect_buffered_mutation_api},
      {"mutation", "concrete array element mutation",
       expect_array_element_mutation_api},
      {"mutation", "wildcard path mutation", expect_wildcard_mutation_api},
      {"mutation", "one-child and recursive path mutation",
       expect_recursive_mutation_api},
      {"mutation", "array wildcard value mutation",
       expect_array_wildcard_value_mutation_api},
  };
  size_t i;
  int selector_cases;

  selector_cases = 0;
  for (i = 0u; i < sizeof(manifest) / sizeof(manifest[0]); ++i) {
    if (manifest[i].surface == NULL || manifest[i].surface[0] == '\0' ||
        manifest[i].requirement == NULL || manifest[i].requirement[0] == '\0') {
      printf("SDK parity manifest has an empty entry at %lu\n",
             (unsigned long)i);
      ++failures;
    }
    if (manifest[i].test == NULL) {
      printf("SDK parity manifest entry lacks a C unit function: %s/%s\n",
             manifest[i].surface, manifest[i].requirement);
      ++failures;
    }
    if (strcmp(manifest[i].surface, "selector") == 0) {
      ++selector_cases;
    }
  }
  if (selector_cases != 3) {
    printf("SDK parity manifest selector accounting mismatch: %d\n",
           selector_cases);
    ++failures;
  }
}

static void expect_selector_match_api(void) {
  expect_match("/status=\"open\"", "{\"status\":\"open\"}", 1);
  expect_match("/status=\"closed\"", "{\"status\":\"open\"}", 0);
  expect_match("eq{field=/status,field=/status,value=open,value=open}",
               "{\"status\":\"open\"}", 1);
  expect_match("/progress>=50", "{\"progress\":72}", 1);
  expect_match("/progress<50", "{\"progress\":72}", 0);
  expect_match("/timestamp=\"2025-01-01\"",
               "{\"timestamp\":\"2025-01-01T15:00:00Z\"}", 1);
  expect_match("/timestamp=\"2025-01-01\"",
               "{\"timestamp\":\"2025-01-02T00:00:00Z\"}", 0);
  expect_match("/timestamp!=2025-01-01",
               "{\"timestamp\":\"2025-01-01T15:00:00Z\"}", 0);
  expect_match("/timestamp!=2025-01-01", "{\"status\":\"open\"}", 1);
  expect_match("/timestamp>=2026-03-05T10:28:21Z",
               "{\"timestamp\":\"2026-03-05T11:28:21+01:00\"}", 1);
  expect_match("/timestamp>=2026-03-05T10:28:21",
               "{\"timestamp\":\"2026-03-05T11:28:21+01:00\"}", 1);
  expect_match("/timestamp>=2026-03-05T10:28:21",
               "{\"timestamp\":\"2026-03-05T10:28:20Z\"}", 0);
  expect_match("range{field=/progress,gt=10,lte=20}", "{\"progress\":11}", 1);
  expect_match("range{field=/progress,gt=10,lte=20}", "{\"progress\":10}", 0);
  expect_match("range{field=/progress,gt=10,lte=20}", "{\"progress\":20}", 1);
  expect_match("range{field=/progress,gt=10,lte=20}", "{\"progress\":21}", 0);
  expect_match("range{field=/"
               "timestamp,gte=2026-03-05T10:28:21Z,lt=2026-03-05T10:30:00Z}",
               "{\"timestamp\":\"2026-03-05T10:29:00Z\"}", 1);
  expect_match("range{field=/"
               "timestamp,gte=2026-03-05T10:28:21Z,lt=2026-03-05T10:30:00Z}",
               "{\"timestamp\":\"2026-03-05T10:30:00Z\"}", 0);
  expect_match("date{field=/timestamp,after=2025-01-01,before=2025-01-03}",
               "{\"timestamp\":\"2025-01-02T06:00:00Z\"}", 1);
  expect_match("date{field=/timestamp,after=2025-01-01,before=2025-01-03}",
               "{\"timestamp\":\"2025-01-03T00:00:00Z\"}", 0);
  expect_match("date{f=/timestamp,a=2025-01-01,b=2025-01-03}",
               "{\"timestamp\":\"2025-01-02T06:00:00Z\"}", 1);
  expect_match("date{field=/timestamp,value=2025-01-01}",
               "{\"timestamp\":\"2025-01-01T23:59:59Z\"}", 1);
  expect_match("date{field=/timestamp,value=2025-01-01}",
               "{\"timestamp\":\"2025-01-02T00:00:00Z\"}", 0);
  expect_match("date{field=/timestamp,since=2025-01-01}",
               "{\"timestamp\":\"2025-01-02T00:00:00Z\"}", 1);
  expect_match("date{field=/timestamp,since=now}",
               "{\"timestamp\":\"2099-01-01T00:00:00Z\"}", 1);
  expect_match("date{field=/timestamp,since=now}",
               "{\"timestamp\":\"1970-01-01T00:00:00Z\"}", 0);
  expect_match("date{field=/timestamp,since=TODAY}",
               "{\"timestamp\":\"2099-01-01T00:00:00Z\"}", 1);
  expect_match("date{field=/timestamp,since=yesterday}",
               "{\"timestamp\":\"1970-01-01T00:00:00Z\"}", 0);
  expect_match("date{f=/"
               "timestamp,after=2026-03-05T10:28:21.123,before=2026-03-05T10:"
               "28:21.123456790}",
               "{\"timestamp\":\"2026-03-05T10:28:21.123456789Z\"}", 1);
  expect_match("date{f=/"
               "timestamp,after=2026-03-05T10:28:21.123,before=2026-03-05T10:"
               "28:21.123456790}",
               "{\"timestamp\":\"2026-03-05T10:28:21.123+01:00\"}", 0);
  expect_match("contains{field=/message,value=timeout}",
               "{\"message\":\"upstream timeout\"}", 1);
  expect_match("contains{field=/metadata}", "{\"metadata\":{\"etag\":\"x\"}}",
               1);
  expect_match("contains{field=/missing}", "{\"metadata\":{\"etag\":\"x\"}}",
               0);
  expect_match("contains{field=/metadata,value=\"\"}",
               "{\"metadata\":{\"etag\":\"x\"}}", 0);
  expect_match("contains{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("icontains{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("prefix{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("iprefix{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("contains{f=/*}", "{\"status\":\"open\"}", 1);
  expect_match("icontains{f=/...,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("not.icontains{f=/,v=\"\"}", "{\"status\":\"open\"}", 0);
  expect_match("contains{field=/message,value=TIMEOUT,ignoreCase=true}",
               "{\"message\":\"upstream timeout\"}", 1);
  expect_match("contains{field=/message,value=TIMEOUT,ic=f}",
               "{\"message\":\"upstream timeout\"}", 0);
  expect_match("contains{field=/message,any=timeout|degraded}",
               "{\"message\":\"upstream timeout\"}", 1);
  expect_match("contains{field=/message,any=missing|degraded}",
               "{\"message\":\"upstream timeout\"}", 0);
  expect_match("icontains{field=/message,value=TIMEOUT}",
               "{\"message\":\"upstream timeout\"}", 1);
  expect_match("icontains{f=/message,a=TIMEOUT|DEGRADED}",
               "{\"message\":\"upstream timeout\"}", 1);
  expect_match("prefix{field=/service,value=auth}",
               "{\"service\":\"auth-api\"}", 1);
  expect_match("prefix{field=/metadata}", "{\"metadata\":{\"etag\":\"x\"}}", 1);
  expect_match("iprefix{field=/metadata}", "{\"metadata\":{\"etag\":\"x\"}}",
               1);
  expect_match("prefix{field=/metadata,value=\"\"}",
               "{\"metadata\":{\"etag\":\"x\"}}", 0);
  expect_match("prefix{field=/service,value=AUTH,ic=t}",
               "{\"service\":\"auth-api\"}", 1);
  expect_match("in{field=/env,any=prod|stage}", "{\"env\":\"prod\"}", 1);
  expect_match("in{field=/env,any=prod|stage}", "{\"env\":\"dev\"}", 0);
  expect_match("contains{f=/hello/*}", "{\"hello\":{\"name\":\"alice\"}}", 1);
  expect_match("contains{f=/hello/[]}", "{\"hello\":{\"0\":\"alice\"}}", 0);
  expect_match("contains{f=/arrays/[]/id}", "{\"arrays\":[{\"id\":1}]}", 1);
  expect_match("contains{f=/arrays/*/id}", "{\"arrays\":[{\"id\":1}]}", 0);
  expect_match("contains{f=/items[]/sku}", "{\"items\":[{\"sku\":\"a\"}]}", 1);
  expect_match("contains{f=/items/**/sku}", "{\"items\":[{\"sku\":\"a\"}]}", 1);
  expect_match("contains{f=/groups/.../sku}",
               "{\"groups\":[{\"items\":[{\"sku\":\"b\"}]}]}", 1);
  expect_match("exists{/metadata/etag}", "{\"metadata\":{\"etag\":\"x\"}}", 1);
  expect_match("exists{/metadata}", "{\"metadata\":{\"etag\":\"x\"}}", 1);
  expect_match("/metadata=\"\"", "{\"metadata\":{\"etag\":\"x\"}}", 0);
  expect_match("/items/0/sku=\"a\"", "{\"items\":[{\"sku\":\"a\"}]}", 1);
  expect_match("/status=\"open\",/progress>=50",
               "{\"status\":\"open\",\"progress\":72}", 1);
  expect_match("/status=\"open\",/progress>=50",
               "{\"status\":\"open\",\"progress\":4}", 0);
  expect_match_or("/status=\"open\",/progress>=50",
                  "{\"status\":\"closed\",\"progress\":72}", 1);
  expect_match_or("/status=\"open\",/progress>=50",
                  "{\"status\":\"closed\",\"progress\":4}", 0);
  expect_match("or.eq{field=/msg,value=warn},or.eq{field=/msg,value=timeout}",
               "{\"msg\":\"timeout\"}", 1);
  expect_match("or.eq{field=/msg,value=warn},or.eq{field=/msg,value=timeout}",
               "{\"msg\":\"ok\"}", 0);
  expect_match(
      "and.eq{field=/status,value=open},and.range{field=/progress,gte=50}",
      "{\"status\":\"open\",\"progress\":72}", 1);
  expect_match(
      "and.eq{field=/status,value=open},and.range{field=/progress,gte=50}",
      "{\"status\":\"open\",\"progress\":4}", 0);
  expect_match(
      "and.0.eq{field=/status,value=open},and.0.range{field=/progress,gte=50}",
      "{\"status\":\"open\",\"progress\":72}", 1);
  expect_match(
      "and.0.eq{field=/status,value=open},and.0.range{field=/progress,gte=50}",
      "{\"status\":\"open\",\"progress\":4}", 0);
  expect_match(
      "or.0.eq{field=/status,value=open},or.0.range{field=/progress,gte=50}",
      "{\"status\":\"open\",\"progress\":72}", 1);
  expect_match(
      "or.0.eq{field=/status,value=open},or.0.range{field=/progress,gte=50}",
      "{\"status\":\"closed\",\"progress\":72}", 0);
  expect_match("not.eq{field=/status,value=closed}", "{\"status\":\"open\"}",
               1);
  expect_match("not.eq{field=/status,value=closed}", "{\"status\":\"closed\"}",
               0);
}

static void expect_selector_parse_error_api(void) {
  expect_parse_error("contains{field=/message,value=timeout,any=error}");
  expect_parse_error("contains{field=/message,any=}");
  expect_parse_error("contains{field=/message,any=||}");
  expect_parse_error("contains{field=/message,value=timeout,value=error}");
  expect_parse_error("contains{field=/message,value=timeout,ignoreCase=maybe}");
  expect_parse_error("eq{field=/status,f=/other,value=open}");
  expect_parse_error("eq{field=/status,value=open,foo=bar}");
  expect_parse_error("eq{field=/status,value=open,ignoreCase=true}");
  expect_parse_error(
      "or.0.eq{field=/status,value=open},or.0.eq{field=/status,value=closed}");
  expect_parse_error("and.0.eq{field=/status,value=open},and.0.eq{field=/"
                     "status,value=closed}");
  expect_parse_error("range{field=/progress,gte=10,gte=20}");
  expect_parse_error("range{field=/progress,gte=10,foo=bar}");
  expect_parse_error("range{field=/progress,gte=10,lt=2025-01-01}");
  expect_parse_error("range{field=/timestamp,gte=yesterday}");
  expect_parse_error("/timestamp>=yesterday");
  expect_parse_error("date{field=/timestamp,value=2025-01-01 00:00:00}");
  expect_parse_error("date{field=/timestamp,after=2025-01-01,foo=bar}");
  expect_parse_error("date{field=/timestamp,since=yesterday,after=2025-01-01}");
  expect_parse_error("date{field=/timestamp,after=2025-01-01,gt=2025-01-02}");
  expect_parse_error("date{field=/timestamp,before=2025-01-03,lt=2025-01-02}");
  expect_parse_error("date{after=2025-01-01}");
  expect_parse_error("date{field=/timestamp,since=tomorrowish}");
  expect_parse_error("prefix{field=/service,any=auth|edge}");
  expect_parse_error("in{field=/env}");
  expect_parse_error("in{field=/env,any=prod|stage,a=dev}");
  expect_parse_error("in{field=/env,any=prod|stage,foo=bar}");
  expect_parse_error("range{field=/progress}");
}

int main(void) {
  expect_sdk_parity_manifest();
  expect_public_utility_api();
  expect_selector_match_api();
  expect_selector_parse_error_api();
  expect_version_api();
  expect_stream_file();
  expect_source_stream();
  expect_source_spooled_payload_api();
  expect_stream_array_items();
  expect_stream_stop_controls();
  expect_stream_error_api();
  expect_seekable_payload_api();
  expect_projection_api();
  expect_buffered_projection_api();
  expect_projection_compact_error_api();
  expect_compact_api();
  expect_mutation_plan_api();
  expect_mutation_error_api();
  expect_root_field_mutation_api();
  expect_path_mutation_api();
  expect_buffered_mutation_api();
  expect_array_element_mutation_api();
  expect_wildcard_mutation_api();
  expect_recursive_mutation_api();
  expect_array_wildcard_value_mutation_api();
  return failures == 0 ? 0 : 1;
}
