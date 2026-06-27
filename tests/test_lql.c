#include "lql/lql.h"
#include "lql/version.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static lql *test_ctx = NULL;

static int read_tmpfile(FILE *fp, char *buf, size_t cap, size_t *out_len);
static int append_literal(char *buf, size_t cap, size_t *pos,
                          const char *text);
static int append_repeated(char *buf, size_t cap, size_t *pos, char ch,
                           size_t count);
static lql_string_view view_from_cstr(const char *text);
static int view_equals(lql_string_view view, const char *text);

static int append_literal(char *buf, size_t cap, size_t *pos,
                          const char *text) {
  size_t len;
  len = strlen(text);
  if (*pos + len >= cap) {
    return 0;
  }
  memcpy(buf + *pos, text, len);
  *pos += len;
  buf[*pos] = '\0';
  return 1;
}

static int append_repeated(char *buf, size_t cap, size_t *pos, char ch,
                           size_t count) {
  if (*pos + count >= cap) {
    return 0;
  }
  memset(buf + *pos, ch, count);
  *pos += count;
  buf[*pos] = '\0';
  return 1;
}

static lql_string_view view_from_cstr(const char *text) {
  lql_string_view view;
  view.data = text;
  view.len = text == NULL ? 0u : strlen(text);
  return view;
}

static int view_equals(lql_string_view view, const char *text) {
  size_t len;
  if (text == NULL) {
    return view.data == NULL && view.len == 0u;
  }
  len = strlen(text);
  return view.data != NULL && view.len == len &&
         memcmp(view.data, text, len) == 0;
}

static void expect_receiver_api(void) {
  lql *ctx;
  lql_selector *selector;
  lql_capabilities caps;
  lql_error error;
  int matched;
  lql_status st;

  lql_error_init(&error);
  st = lql_new(NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "out lql required") != 0) {
    printf("receiver constructor invalid argument mismatch: %s\n",
           error.message);
    ++failures;
  }

  ctx = NULL;
  lql_error_init(&error);
  st = lql_new(&ctx, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("receiver constructor mismatch: %s\n", error.message);
    ++failures;
    return;
  }
  if (ctx->impl == NULL) {
    printf("receiver private implementation missing\n");
    ++failures;
  }
  if (ctx->version == NULL || ctx->capabilities_get == NULL ||
      ctx->selector_parse == NULL || ctx->selector_parse_or == NULL ||
      ctx->selector_parse_json == NULL ||
      ctx->selector_destroy == NULL || ctx->selector_is_empty == NULL ||
      ctx->selector_capabilities_get == NULL ||
      ctx->selector_execution_traits_get == NULL ||
      ctx->selector_root == NULL ||
      ctx->selector_node_child_count == NULL ||
      ctx->selector_node_child == NULL ||
      ctx->selector_node_string_term == NULL ||
      ctx->selector_node_string_term_any == NULL ||
      ctx->selector_node_range_term == NULL ||
      ctx->selector_node_date_term == NULL ||
      ctx->selector_node_in_term == NULL ||
      ctx->selector_node_in_term_any == NULL ||
      ctx->selector_node_exists_path == NULL ||
      ctx->selector_write_json == NULL ||
      ctx->selector_build_all == NULL ||
      ctx->selector_build_compound == NULL ||
      ctx->selector_build_not == NULL ||
      ctx->selector_build_string == NULL ||
      ctx->selector_build_range == NULL ||
      ctx->selector_build_date == NULL ||
      ctx->selector_build_in == NULL ||
      ctx->selector_build_exists == NULL ||
      ctx->matches_json == NULL || ctx->query_file_decisions == NULL ||
      ctx->query_file_decisions_with_options == NULL ||
      ctx->query_source_decisions == NULL ||
      ctx->query_source_decisions_with_options == NULL ||
      ctx->query_file_matches == NULL ||
      ctx->query_file_matches_with_options == NULL ||
      ctx->query_source_spooled_matches == NULL ||
      ctx->query_source_spooled_matches_with_options == NULL ||
      ctx->payload_write_json == NULL || ctx->payload_write_json_sink == NULL ||
      ctx->payload_project_json == NULL || ctx->projection_parse == NULL ||
      ctx->projection_destroy == NULL || ctx->project_file_range == NULL ||
      ctx->project_source == NULL || ctx->project_json == NULL ||
      ctx->compact_file_range == NULL || ctx->compact_source == NULL ||
      ctx->compact_json == NULL || ctx->mutation_plan_parse == NULL ||
      ctx->mutation_plan_parse_with_options == NULL ||
      ctx->mutation_plan_count == NULL || ctx->mutation_plan_destroy == NULL ||
      ctx->mutate_file_range_root_fields == NULL ||
      ctx->mutate_file_range_paths == NULL ||
      ctx->mutate_file_range_candidates == NULL ||
      ctx->mutate_file_range_candidates_with_options == NULL ||
      ctx->mutate_file_range_projected_candidates == NULL ||
      ctx->mutate_file_range_projected_candidates_with_options == NULL ||
      ctx->mutate_source_paths == NULL ||
      ctx->mutate_source_candidates == NULL ||
      ctx->mutate_source_candidates_with_options == NULL ||
      ctx->mutate_source_projected_candidates == NULL ||
      ctx->mutate_source_projected_candidates_with_options == NULL ||
      ctx->mutate_json == NULL || ctx->destroy == NULL) {
    printf("receiver method table missing required methods\n");
    ++failures;
  }
  memset(&caps, 0, sizeof(caps));
  ctx->capabilities_get(ctx, &caps);
  if (strcmp(ctx->version(ctx), LQL_VERSION) != 0 || !caps.selector_parse ||
      !caps.selector_inspection) {
    printf("receiver version/capability mismatch\n");
    ++failures;
  }

  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("receiver selector parse mismatch: %s\n", error.message);
    ++failures;
  } else {
    matched = 0;
    st = ctx->matches_json(ctx, selector, "{\"status\":\"open\"}",
                           strlen("{\"status\":\"open\"}"), &matched, &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("receiver matches_json mismatch: %s\n", error.message);
      ++failures;
    }
  }
  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
}

static void expect_public_utility_api(void) {
  lql_error error;
  lql_selector *selector;
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

  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, "/status=open", NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "out selector required") != 0) {
    printf("selector parse NULL out mismatch: %s\n", error.message);
    ++failures;
  }

  if (!test_ctx->selector_is_empty(test_ctx, NULL)) {
    printf("NULL selector should be empty\n");
    ++failures;
  }
  selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, "", &selector, &error);
  if (st != LQL_STATUS_OK || !test_ctx->selector_is_empty(test_ctx, selector)) {
    printf("empty selector parse mismatch: %s\n", error.message);
    ++failures;
  } else {
    matched = 0;
    st =
        test_ctx->matches_json(test_ctx, selector, "{\"anything\":true}",
                               strlen("{\"anything\":true}"), &matched, &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("empty selector match-all mismatch: %s\n", error.message);
      ++failures;
    }
  }
  test_ctx->selector_destroy(test_ctx, selector);
  test_ctx->selector_destroy(test_ctx, NULL);
  test_ctx->projection_destroy(test_ctx, NULL);
  test_ctx->mutation_plan_destroy(test_ctx, NULL);

  selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, "/status=open", &selector, &error);
  if (st != LQL_STATUS_OK || test_ctx->selector_is_empty(test_ctx, selector)) {
    printf("non-empty selector state mismatch: %s\n", error.message);
    ++failures;
  }
  test_ctx->selector_destroy(test_ctx, selector);

  matched = 1;
  lql_error_init(&error);
  st = test_ctx->matches_json(test_ctx, NULL, NULL, 0u, &matched, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "json and out_matched are required") != 0) {
    printf("matches_json invalid json mismatch: %s\n", error.message);
    ++failures;
  }
  lql_error_init(&error);
  st = test_ctx->matches_json(test_ctx, NULL, "{}", strlen("{}"), NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "json and out_matched are required") != 0) {
    printf("matches_json invalid out mismatch: %s\n", error.message);
    ++failures;
  }
}

static void expect_version_api(void) {
  lql_capabilities caps;

  if (strcmp(test_ctx->version(test_ctx), LQL_VERSION) != 0) {
    printf("receiver version mismatch: %s != %s\n",
           test_ctx->version(test_ctx), LQL_VERSION);
    ++failures;
  }
  if (strchr(LQL_VERSION, '.') == NULL) {
    printf("version macro is not dotted semver: %s\n", LQL_VERSION);
    ++failures;
  }
  memset(&caps, 0, sizeof(caps));
  test_ctx->capabilities_get(test_ctx, &caps);
  if (!caps.selector_parse || !caps.matches_json ||
      !caps.file_decision_stream || !caps.file_match_stream ||
      !caps.source_decision_stream || !caps.seekable_range_payloads ||
      !caps.source_spooled_match_stream || !caps.spooled_payloads ||
      !caps.payload_sink_write || !caps.payload_projection ||
      !caps.projection_file_range || !caps.projection_source ||
      !caps.projection_buffered_json || !caps.compact_file_range ||
      !caps.compact_source || !caps.compact_buffered_json ||
      !caps.mutation_parse || !caps.mutation_file_range ||
      !caps.mutation_file_range_candidates || !caps.mutation_source ||
      !caps.mutation_source_candidates ||
      !caps.mutation_file_range_projected_candidates ||
      !caps.mutation_source_projected_candidates ||
      !caps.mutation_buffered_json || !caps.mutation_file_values) {
    printf("capability query omitted an implemented public surface\n");
    ++failures;
  }
  test_ctx->capabilities_get(test_ctx, NULL);
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

typedef struct memory_sink {
  char data[256];
  size_t len;
  int calls;
  int fail_after_first;
  lql_status fail_status;
} memory_sink;

typedef struct chunk_reader {
  const char *data;
  size_t len;
  size_t offset;
  size_t chunk_size;
  int calls;
} chunk_reader;

typedef struct fail_after_reader {
  const char *data;
  size_t len;
  size_t offset;
  size_t chunk_size;
  size_t fail_offset;
  int calls;
} fail_after_reader;

static int query_result_is_zero(const lql_query_result *result) {
  return result != NULL && result->candidates_seen == 0u &&
         result->candidates_matched == 0u && result->bytes_read == 0u &&
         !result->stopped_early && result->stop_reason == LQL_QUERY_STOP_NONE;
}

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

static lql_read_result read_until_offset_then_fail(void *user,
                                                   unsigned char *buffer,
                                                   size_t capacity) {
  fail_after_reader *reader;
  lql_read_result result;
  size_t remaining;
  size_t want;

  reader = (fail_after_reader *)user;
  memset(&result, 0, sizeof(result));
  ++reader->calls;
  if (reader->offset >= reader->fail_offset) {
    result.error_code = 9;
    return result;
  }
  if (reader->offset >= reader->len) {
    result.eof = 1;
    return result;
  }
  remaining = reader->len - reader->offset;
  want = remaining;
  if (reader->offset + want > reader->fail_offset) {
    want = reader->fail_offset - reader->offset;
  }
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

static lql_read_result read_fail_once(void *user, unsigned char *buffer,
                                      size_t capacity) {
  lql_read_result result;

  (void)user;
  (void)buffer;
  (void)capacity;
  memset(&result, 0, sizeof(result));
  result.error_code = 7;
  return result;
}

static lql_read_result read_over_capacity(void *user, unsigned char *buffer,
                                          size_t capacity) {
  lql_read_result result;

  (void)user;
  if (capacity != 0u) {
    buffer[0] = '{';
  }
  memset(&result, 0, sizeof(result));
  result.bytes_read = capacity == (size_t)-1 ? capacity : capacity + 1u;
  return result;
}

static lql_status write_memory_sink(void *user, const void *data, size_t len) {
  memory_sink *sink;

  sink = (memory_sink *)user;
  ++sink->calls;
  if (sink->fail_after_first && sink->calls == 1) {
    return sink->fail_status == LQL_STATUS_OK ? LQL_STATUS_STOP
                                              : sink->fail_status;
  }
  if (sink->len + len >= sizeof(sink->data)) {
    return LQL_STATUS_NO_MEMORY;
  }
  memcpy(sink->data + sink->len, data, len);
  sink->len += len;
  sink->data[sink->len] = '\0';
  return LQL_STATUS_OK;
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

static lql_status fail_decision_callback(void *user,
                                         const lql_query_decision *decision) {
  stream_seen *seen = (stream_seen *)user;
  if (seen != NULL) {
    ++seen->calls;
    if (decision->matched) {
      ++seen->matched;
    }
  }
  return LQL_STATUS_UNSUPPORTED;
}

static lql_status fail_match_callback(void *user,
                                      const lql_query_match *match) {
  payload_seen *seen = (payload_seen *)user;
  if (seen != NULL) {
    if (seen->calls < 4) {
      seen->offsets[seen->calls] = match->payload.offset;
      seen->sizes[seen->calls] = match->payload.size;
    }
    ++seen->calls;
  }
  return LQL_STATUS_UNSUPPORTED;
}

static lql_status record_spooled_payload(void *user,
                                         const lql_query_match *match);

static void expect_output_state_contract_api(void) {
  static const char stream[] = "{\"status\":\"open\"}\n";
  const char *bad_projection[1];
  const char *bad_mutation[1];
  FILE *source;
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation_plan *plan;
  lql_error error;
  lql_status st;
  stream_seen seen;
  payload_seen payload_seen_value;
  chunk_reader reader;
  fail_after_reader fail_reader;
  lql_query_result result;
  int found;

  selector = (lql_selector *)1;
  lql_error_init(&error);
  st = test_ctx->selector_parse(
      test_ctx, "eq{field=/status,value=open,foo=bar}", &selector, &error);
  if (st != LQL_STATUS_PARSE_ERROR || selector != NULL) {
    printf("selector parse failure output state mismatch: status=%s out=%p\n",
           lql_status_string(st), (void *)selector);
    ++failures;
  }

  bad_projection[0] = "missing-leading-slash";
  projection = (lql_projection *)1;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, bad_projection, 1u, &projection,
                                  &error);
  if (st != LQL_STATUS_PARSE_ERROR || projection != NULL) {
    printf("projection parse failure output state mismatch: status=%s out=%p\n",
           lql_status_string(st), (void *)projection);
    ++failures;
  }

  bad_mutation[0] = "rm:";
  plan = (lql_mutation_plan *)1;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, bad_mutation, 1u, &plan, &error);
  if (st != LQL_STATUS_PARSE_ERROR || plan != NULL) {
    printf("mutation parse failure output state mismatch: status=%s out=%p\n",
           lql_status_string(st), (void *)plan);
    ++failures;
  }

  found = 7;
  lql_error_init(&error);
  st = test_ctx->project_json(test_ctx, NULL, "{}", strlen("{}"), NULL, &found,
                              &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT || found != 0) {
    printf("projection invalid argument found-state mismatch: status=%s "
           "found=%d\n",
           lql_status_string(st), found);
    ++failures;
  }

  selector = NULL;
  lql_error_init(&error);
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("callback failure selector parse mismatch: %s\n", error.message);
    ++failures;
    return;
  }
  source = tmpfile();
  if (source == NULL) {
    printf("callback failure tmpfile failed\n");
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (fwrite(stream, 1u, strlen(stream), source) != strlen(stream) ||
      fseek(source, 0L, SEEK_SET) != 0) {
    printf("callback failure stream setup failed\n");
    fclose(source);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  memset(&seen, 0, sizeof(seen));
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_file_decisions(test_ctx, selector, source,
                                      fail_decision_callback, &seen, &result,
                                      &error);
  if (st != LQL_STATUS_UNSUPPORTED ||
      strcmp(error.message, "query decision callback failed") != 0 ||
      seen.calls != 1 || seen.matched != 1 || result.candidates_seen != 1u ||
      result.candidates_matched != 1u) {
    printf("decision callback failure propagation mismatch: status=%s calls=%d "
           "matched=%d seen=%lu result_matched=%lu error=%s\n",
           lql_status_string(st), seen.calls, seen.matched,
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched, error.message);
    ++failures;
  }

  if (fseek(source, 0L, SEEK_SET) != 0) {
    printf("match callback failure source rewind failed\n");
    ++failures;
  } else {
    memset(&payload_seen_value, 0, sizeof(payload_seen_value));
    memset(&result, 0, sizeof(result));
    lql_error_init(&error);
    st = test_ctx->query_file_matches(test_ctx, selector, source,
                                      fail_match_callback, &payload_seen_value,
                                      &result, &error);
    if (st != LQL_STATUS_UNSUPPORTED ||
        strcmp(error.message, "query match callback failed") != 0 ||
        payload_seen_value.calls != 1 || payload_seen_value.offsets[0] != 0u ||
        payload_seen_value.sizes[0] != (lql_uint64)strlen(stream) - 1u ||
        result.candidates_seen != 1u || result.candidates_matched != 1u) {
      printf("seekable match callback failure mismatch: status=%s calls=%d "
             "seen=%lu matched=%lu error=%s\n",
             lql_status_string(st), payload_seen_value.calls,
             (unsigned long)result.candidates_seen,
             (unsigned long)result.candidates_matched, error.message);
      ++failures;
    }
  }

  memset(&reader, 0, sizeof(reader));
  memset(&payload_seen_value, 0, sizeof(payload_seen_value));
  memset(&result, 0, sizeof(result));
  reader.data = stream;
  reader.len = strlen(stream);
  reader.chunk_size = 3u;
  lql_error_init(&error);
  st = test_ctx->query_source_spooled_matches(
      test_ctx, selector, read_chunk, &reader, fail_match_callback,
      &payload_seen_value, &result, &error);
  if (st != LQL_STATUS_UNSUPPORTED ||
      strcmp(error.message, "query match callback failed") != 0 ||
      payload_seen_value.calls != 1 || payload_seen_value.offsets[0] != 0u ||
      payload_seen_value.sizes[0] != (lql_uint64)strlen(stream) - 1u ||
      result.candidates_seen != 1u || result.candidates_matched != 1u ||
      reader.calls <= 1) {
    printf("spooled match callback failure mismatch: status=%s calls=%d "
           "seen=%lu matched=%lu reads=%d error=%s\n",
           lql_status_string(st), payload_seen_value.calls,
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched, reader.calls,
           error.message);
    ++failures;
  }

  memset(&fail_reader, 0, sizeof(fail_reader));
  memset(&seen, 0, sizeof(seen));
  memset(&result, 0, sizeof(result));
  fail_reader.data = stream;
  fail_reader.len = strlen(stream);
  fail_reader.chunk_size = 5u;
  fail_reader.fail_offset = strlen(stream);
  lql_error_init(&error);
  st = test_ctx->query_source_decisions(
      test_ctx, selector, read_until_offset_then_fail, &fail_reader,
      record_decision, &seen, &result, &error);
  if (st != LQL_STATUS_JSON_ERROR ||
      strcmp(error.message, "query source reader failed") != 0 ||
      seen.calls != 1 || seen.matched != 1 || result.candidates_seen != 1u ||
      result.candidates_matched != 1u ||
      result.bytes_read != (lql_uint64)strlen(stream)) {
    printf("source decision reader failure partial result mismatch: status=%s "
           "calls=%d matched=%d seen=%lu result_matched=%lu bytes=%lu "
           "error=%s\n",
           lql_status_string(st), seen.calls, seen.matched,
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched,
           (unsigned long)result.bytes_read, error.message);
    ++failures;
  }

  memset(&seen, 0, sizeof(seen));
  memset(&result, 0x5a, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_source_decisions(test_ctx, selector, read_over_capacity,
                                        NULL, record_decision, &seen, &result,
                                        &error);
  if (st != LQL_STATUS_JSON_ERROR ||
      strcmp(error.message, "query source reader failed") != 0 ||
      seen.calls != 0 || !query_result_is_zero(&result)) {
    printf("source decision over-capacity read mismatch: status=%s calls=%d "
           "seen=%lu matched=%lu bytes=%lu error=%s\n",
           lql_status_string(st), seen.calls,
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched,
           (unsigned long)result.bytes_read, error.message);
    ++failures;
  }

  memset(&fail_reader, 0, sizeof(fail_reader));
  memset(&payload_seen_value, 0, sizeof(payload_seen_value));
  memset(&result, 0, sizeof(result));
  fail_reader.data = stream;
  fail_reader.len = strlen(stream);
  fail_reader.chunk_size = 5u;
  fail_reader.fail_offset = strlen(stream);
  payload_seen_value.out = tmpfile();
  if (payload_seen_value.out == NULL) {
    printf("source spooled reader failure tmpfile failed\n");
    ++failures;
  } else {
    lql_error_init(&error);
    st = test_ctx->query_source_spooled_matches(
        test_ctx, selector, read_until_offset_then_fail, &fail_reader,
        record_spooled_payload, &payload_seen_value, &result, &error);
    if (st != LQL_STATUS_JSON_ERROR ||
        strcmp(error.message, "query source reader failed") != 0 ||
        payload_seen_value.calls != 1 || result.candidates_seen != 1u ||
        result.candidates_matched != 1u ||
        result.bytes_read != (lql_uint64)strlen(stream)) {
      printf("source spooled reader failure partial result mismatch: status=%s "
             "calls=%d seen=%lu matched=%lu bytes=%lu error=%s\n",
             lql_status_string(st), payload_seen_value.calls,
             (unsigned long)result.candidates_seen,
             (unsigned long)result.candidates_matched,
             (unsigned long)result.bytes_read, error.message);
      ++failures;
    }
    fclose(payload_seen_value.out);
  }

  memset(&payload_seen_value, 0, sizeof(payload_seen_value));
  memset(&result, 0x5a, sizeof(result));
  payload_seen_value.out = tmpfile();
  if (payload_seen_value.out == NULL) {
    printf("source spooled over-capacity tmpfile failed\n");
    ++failures;
  } else {
    lql_error_init(&error);
    st = test_ctx->query_source_spooled_matches(
        test_ctx, selector, read_over_capacity, NULL, record_spooled_payload,
        &payload_seen_value, &result, &error);
    if (st != LQL_STATUS_JSON_ERROR ||
        strcmp(error.message, "query source reader failed") != 0 ||
        payload_seen_value.calls != 0 || !query_result_is_zero(&result)) {
      printf("source spooled over-capacity read mismatch: status=%s calls=%d "
             "seen=%lu matched=%lu bytes=%lu error=%s\n",
             lql_status_string(st), payload_seen_value.calls,
             (unsigned long)result.candidates_seen,
             (unsigned long)result.candidates_matched,
             (unsigned long)result.bytes_read, error.message);
      ++failures;
    }
    fclose(payload_seen_value.out);
  }
  fclose(source);
  test_ctx->selector_destroy(test_ctx, selector);
}

static void expect_handle_ownership_contract_api(void) {
  const char *field;
  const char *mutation;
  const char *null_mutation[1];
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation_plan *plan;
  lql_error error;
  lql_status st;

  selector = (lql_selector *)1;
  st = test_ctx->selector_parse_or(
      test_ctx, "eq{field=/status,value=open,unknown=true}", &selector, NULL);
  if (st != LQL_STATUS_PARSE_ERROR || selector != NULL) {
    printf("selector_parse_or optional-error failure state mismatch: "
           "status=%s out=%p\n",
           lql_status_string(st), (void *)selector);
    ++failures;
  }

  selector = (lql_selector *)1;
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, NULL, &selector, &error);
  if (st != LQL_STATUS_OK || selector == NULL ||
      !test_ctx->selector_is_empty(test_ctx, selector)) {
    printf("NULL selector expression ownership mismatch: status=%s error=%s\n",
           lql_status_string(st), error.message);
    ++failures;
  }
  test_ctx->selector_destroy(test_ctx, selector);
  test_ctx->selector_destroy(test_ctx, NULL);

  field = NULL;
  projection = (lql_projection *)1;
  st = test_ctx->projection_parse(test_ctx, &field, 1u, &projection, NULL);
  if (st != LQL_STATUS_PARSE_ERROR || projection != NULL) {
    printf("projection optional-error failure state mismatch: status=%s "
           "out=%p\n",
           lql_status_string(st), (void *)projection);
    ++failures;
  }

  field = "/id";
  projection = NULL;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, &field, 1u, &projection, &error);
  if (st != LQL_STATUS_OK || projection == NULL) {
    printf("projection ownership parse mismatch: status=%s error=%s\n",
           lql_status_string(st), error.message);
    ++failures;
  }
  test_ctx->projection_destroy(test_ctx, projection);
  test_ctx->projection_destroy(test_ctx, NULL);

  null_mutation[0] = NULL;
  plan = (lql_mutation_plan *)1;
  st = test_ctx->mutation_plan_parse(test_ctx, null_mutation, 1u, &plan, NULL);
  if (st != LQL_STATUS_PARSE_ERROR || plan != NULL) {
    printf("mutation optional-error failure state mismatch: status=%s out=%p\n",
           lql_status_string(st), (void *)plan);
    ++failures;
  }

  mutation = "/status=closed";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, &mutation, 1u, &plan, &error);
  if (st != LQL_STATUS_OK || plan == NULL ||
      test_ctx->mutation_plan_count(test_ctx, plan) != 1u) {
    printf("mutation ownership parse mismatch: status=%s count=%lu error=%s\n",
           lql_status_string(st),
           (unsigned long)test_ctx->mutation_plan_count(test_ctx, plan),
           error.message);
    ++failures;
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  if (test_ctx->mutation_plan_count(test_ctx, NULL) != 0u) {
    printf("NULL mutation plan count ownership mismatch\n");
    ++failures;
  }
  test_ctx->mutation_plan_destroy(test_ctx, NULL);
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
  st = test_ctx->payload_write_json(test_ctx, &match->payload, seen->out,
                                    &error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  ++seen->calls;
  if (seen->stop_after_first) {
    return LQL_STATUS_STOP;
  }
  return LQL_STATUS_OK;
}

static lql_status record_payload_sink(void *user,
                                      const lql_query_match *match) {
  memory_sink *sink = (memory_sink *)user;
  lql_error error;
  lql_status st;

  if (!match->decision.matched ||
      match->payload.kind != LQL_PAYLOAD_SEEKABLE_RANGE ||
      match->payload.source == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_error_init(&error);
  st = test_ctx->payload_write_json_sink(test_ctx, &match->payload,
                                         write_memory_sink, sink, &error);
  return st;
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
  st = test_ctx->payload_write_json(test_ctx, &match->payload, seen->out,
                                    &error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  ++seen->calls;
  if (seen->stop_after_first) {
    return LQL_STATUS_STOP;
  }
  return LQL_STATUS_OK;
}

static lql_status record_spooled_payload_sink(void *user,
                                              const lql_query_match *match) {
  memory_sink *sink = (memory_sink *)user;
  lql_error error;
  lql_status st;

  if (!match->decision.matched || match->payload.kind != LQL_PAYLOAD_SPOOLED ||
      match->payload.spooled == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_error_init(&error);
  st = test_ctx->payload_write_json_sink(test_ctx, &match->payload,
                                         write_memory_sink, sink, &error);
  return st;
}

typedef struct projected_payload_seen {
  FILE *out;
  const lql_projection *projection;
  int calls;
} projected_payload_seen;

static lql_status
record_spooled_payload_projection(void *user, const lql_query_match *match) {
  projected_payload_seen *seen = (projected_payload_seen *)user;
  lql_error error;
  lql_status st;
  int found;

  if (!match->decision.matched || match->payload.kind != LQL_PAYLOAD_SPOOLED ||
      match->payload.spooled == NULL || seen->projection == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  found = 0;
  lql_error_init(&error);
  st = test_ctx->payload_project_json(
      test_ctx, &match->payload, seen->projection, seen->out, &found, &error);
  if (st != LQL_STATUS_OK || !found) {
    return st == LQL_STATUS_OK ? LQL_STATUS_INVALID_ARGUMENT : st;
  }
  ++seen->calls;
  return LQL_STATUS_STOP;
}

static lql_status
record_seekable_payload_projection(void *user, const lql_query_match *match) {
  projected_payload_seen *seen = (projected_payload_seen *)user;
  lql_error error;
  lql_status st;
  int found;

  if (!match->decision.matched ||
      match->payload.kind != LQL_PAYLOAD_SEEKABLE_RANGE ||
      match->payload.source == NULL || seen->projection == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  found = 0;
  lql_error_init(&error);
  st = test_ctx->payload_project_json(
      test_ctx, &match->payload, seen->projection, seen->out, &found, &error);
  if (st != LQL_STATUS_OK || !found) {
    return st == LQL_STATUS_OK ? LQL_STATUS_INVALID_ARGUMENT : st;
  }
  ++seen->calls;
  return LQL_STATUS_OK;
}

static void expect_match(const char *expr, const char *json, int want) {
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int got;

  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("parse failed for %s: %s\n", expr, error.message);
    ++failures;
    return;
  }
  st = test_ctx->matches_json(test_ctx, selector, json, strlen(json), &got,
                              &error);
  if (st != LQL_STATUS_OK) {
    printf("eval failed for %s: %s\n", expr, error.message);
    ++failures;
  } else if (got != want) {
    printf("match mismatch for %s: got %d want %d\n", expr, got, want);
    ++failures;
  }
  test_ctx->selector_destroy(test_ctx, selector);
}

static void expect_parse_error(const char *expr) {
  lql_selector *selector;
  lql_error error;
  lql_status st;

  selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, expr, &selector, &error);
  if (st == LQL_STATUS_OK) {
    printf("parse unexpectedly succeeded for %s\n", expr);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
  }
}

static void expect_parse_or_error(const char *expr) {
  lql_selector *selector;
  lql_error error;
  lql_status st;

  selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse_or(test_ctx, expr, &selector, &error);
  if (st == LQL_STATUS_OK) {
    printf("parse-or unexpectedly succeeded for %s\n", expr);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
  }
}

static void expect_match_or(const char *expr, const char *json, int want) {
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int got;

  lql_error_init(&error);
  st = test_ctx->selector_parse_or(test_ctx, expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("parse-or failed for %s: %s\n", expr, error.message);
    ++failures;
    return;
  }
  st = test_ctx->matches_json(test_ctx, selector, json, strlen(json), &got,
                              &error);
  if (st != LQL_STATUS_OK) {
    printf("eval-or failed for %s: %s\n", expr, error.message);
    ++failures;
  } else if (got != want) {
    printf("or match mismatch for %s: got %d want %d\n", expr, got, want);
    ++failures;
  }
  test_ctx->selector_destroy(test_ctx, selector);
}

static void expect_selector_equivalent_forms(const char *name,
                                             const char *const *exprs,
                                             size_t expr_count,
                                             const char *matching_json,
                                             const char *rejecting_json) {
  size_t i;

  if (expr_count == 0u) {
    printf("selector equivalence case %s has no expressions\n", name);
    ++failures;
    return;
  }
  for (i = 0u; i < expr_count; ++i) {
    expect_match(exprs[i], matching_json, 1);
    expect_match(exprs[i], rejecting_json, 0);
  }
}

static void expect_selector_equivalent_match_result(const char *name,
                                                    const char *left_expr,
                                                    const char *right_expr,
                                                    const char *json) {
  lql_selector *left;
  lql_selector *right;
  lql_error error;
  lql_status st;
  int left_matched;
  int right_matched;

  left = NULL;
  right = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, left_expr, &left, &error);
  if (st != LQL_STATUS_OK) {
    printf("parse failed for %s left expr %s: %s\n", name, left_expr,
           error.message);
    ++failures;
    return;
  }
  st = test_ctx->selector_parse(test_ctx, right_expr, &right, &error);
  if (st != LQL_STATUS_OK) {
    printf("parse failed for %s right expr %s: %s\n", name, right_expr,
           error.message);
    test_ctx->selector_destroy(test_ctx, left);
    ++failures;
    return;
  }
  left_matched = 0;
  right_matched = 0;
  st = test_ctx->matches_json(test_ctx, left, json, strlen(json), &left_matched,
                              &error);
  if (st != LQL_STATUS_OK) {
    printf("eval failed for %s left expr %s: %s\n", name, left_expr,
           error.message);
    ++failures;
  }
  st = test_ctx->matches_json(test_ctx, right, json, strlen(json),
                              &right_matched, &error);
  if (st != LQL_STATUS_OK) {
    printf("eval failed for %s right expr %s: %s\n", name, right_expr,
           error.message);
    ++failures;
  }
  if (left_matched != right_matched) {
    printf("selector equivalence mismatch for %s: left=%d right=%d json=%s\n",
           name, left_matched, right_matched, json);
    ++failures;
  }
  test_ctx->selector_destroy(test_ctx, right);
  test_ctx->selector_destroy(test_ctx, left);
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
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  fp = tmpfile();
  if (fp == NULL) {
    printf("tmpfile failed\n");
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("tmpfile write/seek failed\n");
    fclose(fp);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  memset(&result, 0, sizeof(result));
  st = test_ctx->query_file_decisions(test_ctx, selector, fp, record_decision,
                                      &seen, &result, &error);
  fclose(fp);
  test_ctx->selector_destroy(test_ctx, selector);
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

static void expect_stream_numeric_path_segments(void) {
  static const char input[] =
      "{\"voucher\":{\"lines\":{\"10\":{\"amount\":3500,\"status\":\"open\"}}}}"
      "\n"
      "{\"voucher\":{\"lines\":[{\"amount\":0},{\"amount\":1},{\"amount\":2},"
      "{\"amount\":3},{\"amount\":4},{\"amount\":5},{\"amount\":6},"
      "{\"amount\":7},{\"amount\":8},{\"amount\":9},{\"amount\":3600,"
      "\"status\":\"closed\"}]}}\n"
      "{\"batches\":[{\"lines\":{\"10\":{\"amount\":4100,\"status\":\"open\"}}}"
      "]}"
      "\n"
      "{\"voucher\":{\"lines\":{\"10\":{\"amount\":1200,"
      "\"status\":\"processing\"}}}}\n";
  FILE *fp;
  lql_selector *selector;
  lql_query_result result;
  stream_seen seen;
  lql_error error;
  lql_status st;

  memset(&seen, 0, sizeof(seen));
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, "/voucher/lines/10/amount>=3000",
                                &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("numeric path stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  fp = tmpfile();
  if (fp == NULL) {
    printf("numeric path stream tmpfile failed\n");
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("numeric path stream write/seek failed\n");
    fclose(fp);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  st = test_ctx->query_file_decisions(test_ctx, selector, fp, record_decision,
                                      &seen, &result, &error);
  fclose(fp);
  test_ctx->selector_destroy(test_ctx, selector);
  if (st != LQL_STATUS_OK) {
    printf("numeric path stream query failed: %s\n", error.message);
    ++failures;
    return;
  }
  if (seen.calls != 4 || seen.matched != 2 ||
      result.candidates_seen != (lql_uint64)4 ||
      result.candidates_matched != (lql_uint64)2 ||
      result.bytes_read != (lql_uint64)strlen(input)) {
    printf("numeric path stream counts mismatch calls=%d matched=%d "
           "seen=%lu matched_result=%lu bytes=%lu want_bytes=%lu\n",
           seen.calls, seen.matched, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched,
           (unsigned long)result.bytes_read, (unsigned long)strlen(input));
    ++failures;
  }
}

static void expect_stream_mixed_scalar_candidates(void) {
  static const char input[] = "\"x\"\n{\"id\":\"x\"}\n123\n";
  static const char sized_first[] = "{\"a\":1}";
  static const char sized_second[] = "{\"b\":[1, 2]}";
  static const char sized_input[] = " \t{\"a\":1}\n\n  {\"b\":[1, 2]} \n";
  FILE *fp;
  FILE *out;
  lql_selector *selector;
  lql_query_result result;
  stream_seen seen;
  payload_seen payloads;
  chunk_reader reader;
  lql_error error;
  lql_status st;
  char buf[128];
  size_t len;

  out = NULL;
  memset(&seen, 0, sizeof(seen));
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, "/id=\"x\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("mixed scalar stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  fp = tmpfile();
  if (fp == NULL) {
    printf("mixed scalar stream tmpfile failed\n");
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("mixed scalar stream write/seek failed\n");
    fclose(fp);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  memset(&result, 0, sizeof(result));
  st = test_ctx->query_file_decisions(test_ctx, selector, fp, record_decision,
                                      &seen, &result, &error);
  fclose(fp);
  if (st != LQL_STATUS_OK) {
    printf("mixed scalar stream query failed: %s\n", error.message);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (seen.calls != 3 || seen.matched != 1 ||
      result.candidates_seen != (lql_uint64)3 ||
      result.candidates_matched != (lql_uint64)1 ||
      result.bytes_read != (lql_uint64)19) {
    printf("mixed scalar stream counts mismatch calls=%d matched=%d "
           "bytes=%lu\n",
           seen.calls, seen.matched, (unsigned long)result.bytes_read);
    ++failures;
  }
  if (seen.offsets[0] != (lql_uint64)0 || seen.sizes[0] != (lql_uint64)3 ||
      seen.offsets[1] != (lql_uint64)4 || seen.sizes[1] != (lql_uint64)10 ||
      seen.offsets[2] != (lql_uint64)15 || seen.sizes[2] != (lql_uint64)3) {
    printf("mixed scalar stream ranges mismatch\n");
    ++failures;
  }

  memset(&seen, 0, sizeof(seen));
  memset(&reader, 0, sizeof(reader));
  memset(&result, 0, sizeof(result));
  reader.data = input;
  reader.len = strlen(input);
  reader.chunk_size = 2u;
  lql_error_init(&error);
  st = test_ctx->query_source_decisions(test_ctx, selector, read_chunk,
                                        &reader, record_decision, &seen,
                                        &result, &error);
  if (st != LQL_STATUS_OK) {
    printf("mixed scalar source stream query failed: %s\n", error.message);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (seen.calls != 3 || seen.matched != 1 ||
      result.candidates_seen != (lql_uint64)3 ||
      result.candidates_matched != (lql_uint64)1 ||
      result.bytes_read != (lql_uint64)19) {
    printf("mixed scalar source stream counts mismatch calls=%d matched=%d "
           "bytes=%lu\n",
           seen.calls, seen.matched, (unsigned long)result.bytes_read);
    ++failures;
  }
  if (seen.offsets[0] != (lql_uint64)0 || seen.sizes[0] != (lql_uint64)3 ||
      seen.offsets[1] != (lql_uint64)4 || seen.sizes[1] != (lql_uint64)10 ||
      seen.offsets[2] != (lql_uint64)15 || seen.sizes[2] != (lql_uint64)3) {
    printf("mixed scalar source stream ranges mismatch\n");
    ++failures;
  }

  fp = tmpfile();
  if (fp == NULL) {
    printf("candidate size tmpfile failed\n");
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (fwrite(sized_input, 1u, strlen(sized_input), fp) !=
          strlen(sized_input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("candidate size write/seek failed\n");
    fclose(fp);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  memset(&seen, 0, sizeof(seen));
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_file_decisions(test_ctx, NULL, fp, record_decision,
                                      &seen, &result, &error);
  fclose(fp);
  if (st != LQL_STATUS_OK || seen.calls != 2 ||
      result.candidates_seen != (lql_uint64)2 ||
      seen.offsets[0] != (lql_uint64)2 ||
      seen.sizes[0] != (lql_uint64)strlen(sized_first) ||
      seen.offsets[1] != (lql_uint64)(2u + strlen(sized_first) + 4u) ||
      seen.sizes[1] != (lql_uint64)strlen(sized_second)) {
    printf("candidate size file contract mismatch: status=%s calls=%d "
           "off0=%lu size0=%lu off1=%lu size1=%lu error=%s\n",
           lql_status_string(st), seen.calls, (unsigned long)seen.offsets[0],
           (unsigned long)seen.sizes[0], (unsigned long)seen.offsets[1],
           (unsigned long)seen.sizes[1], error.message);
    ++failures;
  }

  memset(&seen, 0, sizeof(seen));
  memset(&reader, 0, sizeof(reader));
  memset(&result, 0, sizeof(result));
  reader.data = sized_input;
  reader.len = strlen(sized_input);
  reader.chunk_size = 3u;
  lql_error_init(&error);
  st = test_ctx->query_source_decisions(test_ctx, NULL, read_chunk, &reader,
                                        record_decision, &seen, &result,
                                        &error);
  if (st != LQL_STATUS_OK || seen.calls != 2 ||
      result.candidates_seen != (lql_uint64)2 ||
      seen.offsets[0] != (lql_uint64)2 ||
      seen.sizes[0] != (lql_uint64)strlen(sized_first) ||
      seen.offsets[1] != (lql_uint64)(2u + strlen(sized_first) + 4u) ||
      seen.sizes[1] != (lql_uint64)strlen(sized_second) ||
      reader.calls <= 1) {
    printf("candidate size source contract mismatch: status=%s calls=%d "
           "reads=%d off0=%lu size0=%lu off1=%lu size1=%lu error=%s\n",
           lql_status_string(st), seen.calls, reader.calls,
           (unsigned long)seen.offsets[0], (unsigned long)seen.sizes[0],
           (unsigned long)seen.offsets[1], (unsigned long)seen.sizes[1],
           error.message);
    ++failures;
  }

  fp = tmpfile();
  out = tmpfile();
  if (fp == NULL || out == NULL) {
    printf("mixed scalar payload tmpfile failed\n");
    if (fp != NULL) {
      fclose(fp);
    }
    if (out != NULL) {
      fclose(out);
    }
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("mixed scalar payload write/seek failed\n");
    fclose(fp);
    fclose(out);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  memset(&payloads, 0, sizeof(payloads));
  memset(&result, 0, sizeof(result));
  payloads.out = out;
  lql_error_init(&error);
  st = test_ctx->query_file_matches(test_ctx, selector, fp, record_payload,
                                    &payloads, &result, &error);
  fclose(fp);
  if (st != LQL_STATUS_OK) {
    printf("mixed scalar payload query failed: %s\n", error.message);
    fclose(out);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (payloads.calls != 1 || result.candidates_seen != (lql_uint64)3 ||
      result.candidates_matched != (lql_uint64)1 ||
      result.bytes_read != (lql_uint64)19) {
    printf("mixed scalar payload counts mismatch calls=%d seen=%lu "
           "matched=%lu bytes=%lu\n",
           payloads.calls, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched,
           (unsigned long)result.bytes_read);
    ++failures;
  }
  if (payloads.offsets[0] != (lql_uint64)4 ||
      payloads.sizes[0] != (lql_uint64)10) {
    printf("mixed scalar payload range mismatch\n");
    ++failures;
  }
  if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
      strcmp(buf, "{\"id\":\"x\"}") != 0) {
    printf("mixed scalar payload output mismatch: %s\n", buf);
    ++failures;
  }
  fclose(out);
  test_ctx->selector_destroy(test_ctx, selector);
}

static void expect_stream_escaped_json_pointer_segments(void) {
  static const char input[] =
      "{\"a/b\":{\"~key\":\"ready\"}}\n"
      "{\"a/b\":{\"~key\":\"old\"}}\n"
      "{\"a\":{\"b\":{\"~key\":\"ready\"}}}\n";
  FILE *fp;
  lql_selector *selector;
  lql_query_result result;
  stream_seen seen;
  lql_error error;
  lql_status st;

  memset(&seen, 0, sizeof(seen));
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, "/a~1b/~0key=\"ready\"", &selector,
                                &error);
  if (st != LQL_STATUS_OK) {
    printf("escaped pointer stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  fp = tmpfile();
  if (fp == NULL) {
    printf("escaped pointer stream tmpfile failed\n");
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("escaped pointer stream write/seek failed\n");
    fclose(fp);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  st = test_ctx->query_file_decisions(test_ctx, selector, fp, record_decision,
                                      &seen, &result, &error);
  fclose(fp);
  test_ctx->selector_destroy(test_ctx, selector);
  if (st != LQL_STATUS_OK || seen.calls != 3 || seen.matched != 1 ||
      result.candidates_seen != (lql_uint64)3 ||
      result.candidates_matched != (lql_uint64)1) {
    printf("escaped pointer stream mismatch: status=%s calls=%d matched=%d "
           "seen=%lu result_matched=%lu error=%s\n",
           lql_status_string(st), seen.calls, seen.matched,
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched, error.message);
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
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("source stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  st = test_ctx->query_source_decisions_with_options(
      test_ctx, selector, read_chunk, &reader, &options, record_decision, &seen,
      &result, &error);
  test_ctx->selector_destroy(test_ctx, selector);
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

static void expect_stream_large_irrelevant_scalar_api(void) {
  static const char prefix[] = "{\"blob\":\"";
  static const char chunk[] =
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
  static const char suffix[] = "\",\"status\":\"open\"}\n";
  enum { repeat_count = 4096 };
  FILE *fp;
  lql_selector *selector;
  lql_query_result result;
  stream_seen seen;
  lql_error error;
  lql_status st;
  lql_uint64 expected_bytes;
  int i;

  fp = tmpfile();
  if (fp == NULL) {
    printf("large irrelevant scalar tmpfile failed\n");
    ++failures;
    return;
  }
  if (fwrite(prefix, 1u, strlen(prefix), fp) != strlen(prefix)) {
    printf("large irrelevant scalar prefix write failed\n");
    fclose(fp);
    ++failures;
    return;
  }
  for (i = 0; i < repeat_count; ++i) {
    if (fwrite(chunk, 1u, strlen(chunk), fp) != strlen(chunk)) {
      printf("large irrelevant scalar chunk write failed\n");
      fclose(fp);
      ++failures;
      return;
    }
  }
  if (fwrite(suffix, 1u, strlen(suffix), fp) != strlen(suffix) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("large irrelevant scalar suffix write/seek failed\n");
    fclose(fp);
    ++failures;
    return;
  }

  selector = NULL;
  lql_error_init(&error);
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("large irrelevant scalar selector parse failed: %s\n",
           error.message);
    fclose(fp);
    ++failures;
    return;
  }

  memset(&seen, 0, sizeof(seen));
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_file_decisions(test_ctx, selector, fp, record_decision,
                                      &seen, &result, &error);
  fclose(fp);
  test_ctx->selector_destroy(test_ctx, selector);
  expected_bytes = (lql_uint64)strlen(prefix) +
                   (lql_uint64)strlen(chunk) * (lql_uint64)repeat_count +
                   (lql_uint64)strlen(suffix);
  if (st != LQL_STATUS_OK || seen.calls != 1 || seen.matched != 1 ||
      result.candidates_seen != (lql_uint64)1 ||
      result.candidates_matched != (lql_uint64)1 ||
      result.bytes_read != expected_bytes ||
      seen.sizes[0] != expected_bytes - (lql_uint64)1) {
    printf("large irrelevant scalar stream mismatch: status=%s calls=%d "
           "matched=%d bytes=%lu size=%lu expected=%lu error=%s\n",
           lql_status_string(st), seen.calls, seen.matched,
           (unsigned long)result.bytes_read, (unsigned long)seen.sizes[0],
           (unsigned long)expected_bytes, error.message);
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
  projected_payload_seen projected;
  memory_sink sink;
  chunk_reader reader;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  const char *fields[1];
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
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("source spooled payload parse failed: %s\n", error.message);
    fclose(seen.out);
    ++failures;
    return;
  }
  st = test_ctx->query_source_spooled_matches_with_options(
      test_ctx, selector, read_chunk, &reader, &options, record_spooled_payload,
      &seen, &result, &error);
  test_ctx->selector_destroy(test_ctx, selector);
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

  fields[0] = "/id";
  projection = NULL;
  memset(&projected, 0, sizeof(projected));
  memset(&reader, 0, sizeof(reader));
  memset(&options, 0, sizeof(options));
  memset(&result, 0, sizeof(result));
  reader.data = input;
  reader.len = strlen(input);
  reader.chunk_size = 5u;
  projected.out = tmpfile();
  if (projected.out == NULL) {
    printf("source spooled payload projection tmpfile failed\n");
    ++failures;
    return;
  }
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, fields, 1u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("source spooled payload projection parse failed: %s\n",
           error.message);
    fclose(projected.out);
    ++failures;
    return;
  }
  projected.projection = projection;
  lql_error_init(&error);
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("source spooled payload projection selector failed: %s\n",
           error.message);
    test_ctx->projection_destroy(test_ctx, projection);
    fclose(projected.out);
    ++failures;
    return;
  }
  st = test_ctx->query_source_spooled_matches_with_options(
      test_ctx, selector, read_chunk, &reader, &options,
      record_spooled_payload_projection, &projected, &result, &error);
  test_ctx->selector_destroy(test_ctx, selector);
  test_ctx->projection_destroy(test_ctx, projection);
  if (st != LQL_STATUS_OK) {
    printf("source spooled payload projection query failed: %s\n",
           error.message);
    fclose(projected.out);
    ++failures;
    return;
  }
  if (projected.calls != 1 || result.candidates_seen != (lql_uint64)2 ||
      result.candidates_matched != (lql_uint64)1 || !result.stopped_early ||
      result.stop_reason != LQL_QUERY_STOP_CALLBACK) {
    printf("source spooled payload projection counts mismatch calls=%d "
           "seen=%lu matched=%lu stop=%d\n",
           projected.calls, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched, (int)result.stop_reason);
    ++failures;
  }
  if (!read_tmpfile(projected.out, buf, sizeof(buf), &len)) {
    printf("source spooled payload projection output read failed\n");
    ++failures;
  } else if (strcmp(buf, "{\"id\":\"b\"}") != 0) {
    printf("source spooled payload projection output mismatch: %s\n", buf);
    ++failures;
  }
  fclose(projected.out);

  memset(&sink, 0, sizeof(sink));
  memset(&reader, 0, sizeof(reader));
  memset(&options, 0, sizeof(options));
  memset(&result, 0, sizeof(result));
  reader.data = input;
  reader.len = strlen(input);
  reader.chunk_size = 7u;
  options.max_candidates = 1u;
  lql_error_init(&error);
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("source spooled payload candidate limit parse failed: %s\n",
           error.message);
    ++failures;
    return;
  }
  st = test_ctx->query_source_spooled_matches_with_options(
      test_ctx, selector, read_chunk, &reader, &options,
      record_spooled_payload_sink, &sink, &result, &error);
  test_ctx->selector_destroy(test_ctx, selector);
  if (st != LQL_STATUS_OK || sink.calls != 0 ||
      result.candidates_seen != (lql_uint64)1 ||
      result.candidates_matched != (lql_uint64)0 || !result.stopped_early ||
      result.stop_reason != LQL_QUERY_STOP_CANDIDATE_LIMIT ||
      result.bytes_read != (lql_uint64)28) {
    printf("source spooled payload candidate limit mismatch: status=%s "
           "calls=%d seen=%lu matched=%lu stop=%d bytes=%lu\n",
           lql_status_string(st), sink.calls,
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched, (int)result.stop_reason,
           (unsigned long)result.bytes_read);
    ++failures;
  }

  memset(&sink, 0, sizeof(sink));
  memset(&reader, 0, sizeof(reader));
  memset(&options, 0, sizeof(options));
  memset(&result, 0, sizeof(result));
  reader.data = input;
  reader.len = strlen(input);
  reader.chunk_size = 7u;
  options.max_bytes_read = 1u;
  lql_error_init(&error);
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("source spooled payload byte limit parse failed: %s\n",
           error.message);
    ++failures;
    return;
  }
  st = test_ctx->query_source_spooled_matches_with_options(
      test_ctx, selector, read_chunk, &reader, &options,
      record_spooled_payload_sink, &sink, &result, &error);
  test_ctx->selector_destroy(test_ctx, selector);
  if (st != LQL_STATUS_OK || sink.calls != 0 ||
      result.candidates_seen != (lql_uint64)1 ||
      result.candidates_matched != (lql_uint64)0 || !result.stopped_early ||
      result.stop_reason != LQL_QUERY_STOP_BYTE_LIMIT ||
      result.bytes_read != (lql_uint64)28) {
    printf("source spooled payload byte limit mismatch: status=%s "
           "calls=%d seen=%lu matched=%lu stop=%d bytes=%lu\n",
           lql_status_string(st), sink.calls,
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched, (int)result.stop_reason,
           (unsigned long)result.bytes_read);
    ++failures;
  }

  memset(&sink, 0, sizeof(sink));
  memset(&reader, 0, sizeof(reader));
  memset(&options, 0, sizeof(options));
  memset(&result, 0, sizeof(result));
  reader.data = input;
  reader.len = strlen(input);
  reader.chunk_size = 7u;
  options.max_matches = 1u;
  lql_error_init(&error);
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("source spooled payload sink parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  st = test_ctx->query_source_spooled_matches_with_options(
      test_ctx, selector, read_chunk, &reader, &options,
      record_spooled_payload_sink, &sink, &result, &error);
  test_ctx->selector_destroy(test_ctx, selector);
  if (st != LQL_STATUS_OK) {
    printf("source spooled payload sink query failed: %s\n", error.message);
    ++failures;
  } else if (strcmp(sink.data, "{\"status\":\"open\",\"id\":\"b\"}") != 0) {
    printf("source spooled payload sink output mismatch: %s\n", sink.data);
    ++failures;
  }

  sink.len = 0u;
  sink.calls = 0;
  sink.data[0] = '\0';
  memset(&reader, 0, sizeof(reader));
  memset(&options, 0, sizeof(options));
  memset(&result, 0, sizeof(result));
  reader.data = input;
  reader.len = strlen(input);
  reader.chunk_size = 7u;
  options.max_matches = 1u;
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, "/id=\"c\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("source spooled reusable sink parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  st = test_ctx->query_source_spooled_matches_with_options(
      test_ctx, selector, read_chunk, &reader, &options,
      record_spooled_payload_sink, &sink, &result, &error);
  test_ctx->selector_destroy(test_ctx, selector);
  if (st != LQL_STATUS_OK || sink.calls != 1 ||
      strcmp(sink.data, "{\"status\":\"open\",\"id\":\"c\"}") != 0 ||
      result.candidates_seen != (lql_uint64)3 ||
      result.candidates_matched != (lql_uint64)1 || !result.stopped_early ||
      result.stop_reason != LQL_QUERY_STOP_MATCH_LIMIT) {
    printf("source spooled reusable sink mismatch: status=%s calls=%d out=%s "
           "seen=%lu matched=%lu stopped=%d reason=%d error=%s\n",
           lql_status_string(st), sink.calls, sink.data,
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched, result.stopped_early,
           (int)result.stop_reason, error.message);
    ++failures;
  }

  memset(&sink, 0, sizeof(sink));
  memset(&reader, 0, sizeof(reader));
  memset(&options, 0, sizeof(options));
  memset(&result, 0, sizeof(result));
  reader.data = input;
  reader.len = strlen(input);
  reader.chunk_size = 7u;
  sink.fail_after_first = 1;
  sink.fail_status = LQL_STATUS_INVALID_ARGUMENT;
  lql_error_init(&error);
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("source spooled payload sink failure parse failed: %s\n",
           error.message);
    ++failures;
    return;
  }
  st = test_ctx->query_source_spooled_matches_with_options(
      test_ctx, selector, read_chunk, &reader, &options,
      record_spooled_payload_sink, &sink, &result, &error);
  test_ctx->selector_destroy(test_ctx, selector);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "query match callback failed") != 0 ||
      sink.calls != 1 || result.candidates_seen != (lql_uint64)2 ||
      result.candidates_matched != (lql_uint64)1 ||
      result.bytes_read != (lql_uint64)56) {
    printf("source spooled payload sink failure mismatch: status=%s "
           "error=%s calls=%d seen=%lu matched=%lu bytes=%lu\n",
           lql_status_string(st), error.message, sink.calls,
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched,
           (unsigned long)result.bytes_read);
    ++failures;
  }
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
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("array stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  fp = tmpfile();
  if (fp == NULL) {
    printf("array tmpfile failed\n");
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("array tmpfile write/seek failed\n");
    fclose(fp);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  memset(&result, 0, sizeof(result));
  st = test_ctx->query_file_decisions(test_ctx, selector, fp, record_decision,
                                      &seen, &result, &error);
  fclose(fp);
  test_ctx->selector_destroy(test_ctx, selector);
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

static void expect_stream_nested_array_items(void) {
  static const char input[] = "[{\"id\":\"a\"},[{\"id\":\"b\"}],{\"id\":\"c\"}]";
  FILE *fp;
  FILE *out;
  lql_selector *selector;
  lql_query_result result;
  stream_seen seen;
  payload_seen payload;
  memory_sink sink;
  chunk_reader reader;
  char buf[64];
  long end;
  size_t got;
  lql_error error;
  lql_status st;

  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, "/id=\"b\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("nested array stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  fp = tmpfile();
  if (fp == NULL) {
    printf("nested array decision tmpfile failed\n");
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("nested array decision tmpfile write/seek failed\n");
    fclose(fp);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  memset(&seen, 0, sizeof(seen));
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_file_decisions(test_ctx, selector, fp, record_decision,
                                      &seen, &result, &error);
  if (st != LQL_STATUS_OK) {
    printf("nested array file decision query failed: %s\n", error.message);
    fclose(fp);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (seen.calls != 3 || seen.matched != 1 ||
      result.candidates_seen != (lql_uint64)3 ||
      result.candidates_matched != (lql_uint64)1) {
    printf("nested array file decision mismatch calls=%d matched=%d seen=%lu "
           "matched_result=%lu\n",
           seen.calls, seen.matched, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched);
    ++failures;
  }
  memset(&seen, 0, sizeof(seen));
  memset(&reader, 0, sizeof(reader));
  memset(&result, 0, sizeof(result));
  reader.data = input;
  reader.len = strlen(input);
  reader.chunk_size = 5u;
  lql_error_init(&error);
  st = test_ctx->query_source_decisions(test_ctx, selector, read_chunk,
                                        &reader, record_decision, &seen,
                                        &result, &error);
  if (st != LQL_STATUS_OK) {
    printf("nested array source decision query failed: %s\n", error.message);
    fclose(fp);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (reader.calls <= 1 || seen.calls != 3 || seen.matched != 1 ||
      result.candidates_seen != (lql_uint64)3 ||
      result.candidates_matched != (lql_uint64)1) {
    printf("nested array source decision mismatch calls=%d matched=%d "
           "seen=%lu matched_result=%lu reader_calls=%d\n",
           seen.calls, seen.matched, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched, reader.calls);
    ++failures;
  }
  if (fseek(fp, 0L, SEEK_SET) != 0) {
    printf("nested array range payload seek failed\n");
    fclose(fp);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  memset(&sink, 0, sizeof(sink));
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_file_matches(test_ctx, selector, fp, record_payload_sink,
                                    &sink, &result, &error);
  fclose(fp);
  if (st != LQL_STATUS_OK) {
    printf("nested array file payload query failed: %s\n", error.message);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  if (result.candidates_seen != (lql_uint64)3 ||
      result.candidates_matched != (lql_uint64)1 ||
      strcmp(sink.data, "{\"id\":\"b\"}") != 0) {
    printf("nested array file payload mismatch seen=%lu matched=%lu "
           "payload=%s\n",
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched, sink.data);
    ++failures;
  }
  out = tmpfile();
  if (out == NULL) {
    printf("nested array payload tmpfile failed\n");
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  memset(&payload, 0, sizeof(payload));
  memset(&reader, 0, sizeof(reader));
  memset(&result, 0, sizeof(result));
  reader.data = input;
  reader.len = strlen(input);
  reader.chunk_size = 5u;
  payload.out = out;
  lql_error_init(&error);
  st = test_ctx->query_source_spooled_matches(
      test_ctx, selector, read_chunk, &reader, record_spooled_payload,
      &payload, &result, &error);
  test_ctx->selector_destroy(test_ctx, selector);
  if (st != LQL_STATUS_OK) {
    printf("nested array source payload query failed: %s\n", error.message);
    fclose(out);
    ++failures;
    return;
  }
  end = ftell(out);
  if (end < 0 || end >= (long)sizeof(buf) || fseek(out, 0L, SEEK_SET) != 0) {
    printf("nested array payload output sizing failed\n");
    fclose(out);
    ++failures;
    return;
  }
  got = fread(buf, 1u, (size_t)end, out);
  fclose(out);
  buf[got] = '\0';
  if (payload.calls != 1 || result.candidates_seen != (lql_uint64)3 ||
      result.candidates_matched != (lql_uint64)1 ||
      strcmp(buf, "{\"id\":\"b\"}") != 0) {
    printf("nested array payload mismatch calls=%d seen=%lu matched=%lu "
           "payload=%s\n",
           payload.calls, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched, buf);
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
  chunk_reader reader;
  lql_error error;
  lql_status st;

  lql_error_init(&error);
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
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
    st = test_ctx->query_file_decisions_with_options(                          \
        test_ctx, selector, fp, &options, record_decision, &seen, &result,     \
        &error);                                                               \
    fclose(fp);                                                                \
    if (st != LQL_STATUS_OK) {                                                 \
      printf(label " query failed: %s\n", error.message);                      \
      ++failures;                                                              \
      break;                                                                   \
    }                                                                          \
    if (seen.calls != (want_calls) || seen.matched != (want_matched) ||        \
        result.candidates_seen != (lql_uint64)(want_calls) ||                  \
        result.candidates_matched != (lql_uint64)(want_matched) ||             \
        !result.stopped_early || result.stop_reason != (want_reason)) {        \
      printf(label " stop mismatch calls=%d matched=%d result_seen=%lu "       \
                   "result_matched=%lu stopped=%d reason=%d\n",                \
             seen.calls, seen.matched, (unsigned long)result.candidates_seen,  \
             (unsigned long)result.candidates_matched, result.stopped_early,   \
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
  RUN_STOP_CASE("callback stop precedence", options.max_matches = 1u;
                options.max_candidates = 1u;
                options.max_bytes_read = 1u, seen.stop_after_first = 1, 1, 1,
                LQL_QUERY_STOP_CALLBACK);
  RUN_STOP_CASE("max matches precedence", options.max_matches = 1u;
                options.max_candidates = 1u;
                options.max_bytes_read = 1u, (void)0, 1, 1,
                LQL_QUERY_STOP_MATCH_LIMIT);
  RUN_STOP_CASE("max candidates precedence", options.max_candidates = 1u;
                options.max_bytes_read = 1u, (void)0, 1, 1,
                LQL_QUERY_STOP_CANDIDATE_LIMIT);

#undef RUN_STOP_CASE

#define RUN_SOURCE_STOP_CASE(label, setup_options, setup_seen, want_calls,     \
                             want_matched, want_reason)                        \
  do {                                                                         \
    memset(&options, 0, sizeof(options));                                      \
    memset(&seen, 0, sizeof(seen));                                            \
    memset(&reader, 0, sizeof(reader));                                        \
    memset(&result, 0, sizeof(result));                                        \
    reader.data = input;                                                       \
    reader.len = strlen(input);                                                \
    reader.chunk_size = 7u;                                                    \
    setup_options;                                                             \
    setup_seen;                                                                \
    st = test_ctx->query_source_decisions_with_options(                        \
        test_ctx, selector, read_chunk, &reader, &options, record_decision,    \
        &seen, &result, &error);                                               \
    if (st != LQL_STATUS_OK) {                                                 \
      printf(label " source query failed: %s\n", error.message);               \
      ++failures;                                                              \
      break;                                                                   \
    }                                                                          \
    if (seen.calls != (want_calls) || seen.matched != (want_matched) ||        \
        result.candidates_seen != (lql_uint64)(want_calls) ||                  \
        result.candidates_matched != (lql_uint64)(want_matched) ||             \
        !result.stopped_early || result.stop_reason != (want_reason)) {        \
      printf(label " source stop mismatch calls=%d matched=%d "                \
                   "result_seen=%lu result_matched=%lu stopped=%d "           \
                   "reason=%d\n",                                             \
             seen.calls, seen.matched, (unsigned long)result.candidates_seen,  \
             (unsigned long)result.candidates_matched, result.stopped_early,   \
             (int)result.stop_reason);                                         \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

  RUN_SOURCE_STOP_CASE("source max matches", options.max_matches = 1u,
                       (void)0, 1, 1, LQL_QUERY_STOP_MATCH_LIMIT);
  RUN_SOURCE_STOP_CASE("source max candidates", options.max_candidates = 2u,
                       (void)0, 2, 2, LQL_QUERY_STOP_CANDIDATE_LIMIT);
  RUN_SOURCE_STOP_CASE("source max bytes", options.max_bytes_read = 17u,
                       (void)0, 1, 1, LQL_QUERY_STOP_BYTE_LIMIT);
  RUN_SOURCE_STOP_CASE("source callback stop", (void)0,
                       seen.stop_after_first = 1, 1, 1,
                       LQL_QUERY_STOP_CALLBACK);
  RUN_SOURCE_STOP_CASE("source callback stop precedence",
                       options.max_matches = 1u;
                       options.max_candidates = 1u;
                       options.max_bytes_read = 1u,
                       seen.stop_after_first = 1, 1, 1,
                       LQL_QUERY_STOP_CALLBACK);
  RUN_SOURCE_STOP_CASE("source max matches precedence",
                       options.max_matches = 1u;
                       options.max_candidates = 1u;
                       options.max_bytes_read = 1u,
                       (void)0, 1, 1, LQL_QUERY_STOP_MATCH_LIMIT);
  RUN_SOURCE_STOP_CASE("source max candidates precedence",
                       options.max_candidates = 1u;
                       options.max_bytes_read = 1u,
                       (void)0, 1, 1, LQL_QUERY_STOP_CANDIDATE_LIMIT);

#undef RUN_SOURCE_STOP_CASE

  test_ctx->selector_destroy(test_ctx, selector);
}

static void expect_stream_error_api(void) {
  static const char malformed[] = "{\"status\":\"open\"}\n{\"status\":";
  FILE *source;
  FILE *out;
  lql_selector *selector;
  lql_projection *projection;
  lql_payload payload;
  stream_seen seen;
  payload_seen payload_seen_value;
  chunk_reader reader;
  lql_error error;
  lql_query_result result;
  lql_status st;
  const char *fields[1];
  lql_uint64 complete_candidate_bytes;

  selector = NULL;
  projection = NULL;
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

  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("stream error selector parse failed: %s\n", error.message);
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  fields[0] = "/status";
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, fields, 1u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("stream error projection parse failed: %s\n", error.message);
    test_ctx->selector_destroy(test_ctx, selector);
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  if (fwrite(malformed, 1u, strlen(malformed), source) != strlen(malformed) ||
      fseek(source, 0L, SEEK_SET) != 0) {
    printf("stream error malformed source setup failed\n");
    test_ctx->projection_destroy(test_ctx, projection);
    test_ctx->selector_destroy(test_ctx, selector);
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  complete_candidate_bytes = (lql_uint64)strlen("{\"status\":\"open\"}");
  memset(&seen, 0, sizeof(seen));
  memset(&result, 0x5a, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_file_decisions(test_ctx, selector, source,
                                      record_decision, &seen, &result, &error);
  if (st != LQL_STATUS_JSON_ERROR || seen.calls != 1 || seen.matched != 1 ||
      result.candidates_seen != 1u || result.candidates_matched != 1u ||
      result.bytes_read != complete_candidate_bytes || result.stopped_early) {
    printf("malformed file decision stream mismatch: status=%s calls=%d "
           "matched=%d seen=%lu result_matched=%lu bytes=%lu stopped=%d "
           "error=%s\n",
           lql_status_string(st), seen.calls, seen.matched,
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched,
           (unsigned long)result.bytes_read, result.stopped_early,
           error.message);
    ++failures;
  }
  if (fseek(source, 0L, SEEK_SET) != 0) {
    printf("stream error malformed source rewind failed\n");
    ++failures;
  } else {
    memset(&payload_seen_value, 0, sizeof(payload_seen_value));
    payload_seen_value.out = out;
    memset(&result, 0x5a, sizeof(result));
    lql_error_init(&error);
    st =
        test_ctx->query_file_matches(test_ctx, selector, source, record_payload,
                                     &payload_seen_value, &result, &error);
    if (st != LQL_STATUS_JSON_ERROR || payload_seen_value.calls != 1 ||
        result.candidates_seen != 1u || result.candidates_matched != 1u ||
        result.bytes_read != complete_candidate_bytes || result.stopped_early) {
      printf("malformed file match stream mismatch: status=%s calls=%d "
             "seen=%lu matched=%lu bytes=%lu stopped=%d error=%s\n",
             lql_status_string(st), payload_seen_value.calls,
             (unsigned long)result.candidates_seen,
             (unsigned long)result.candidates_matched,
             (unsigned long)result.bytes_read, result.stopped_early,
             error.message);
      ++failures;
    }
  }
  memset(&seen, 0, sizeof(seen));
  memset(&reader, 0, sizeof(reader));
  reader.data = malformed;
  reader.len = strlen(malformed);
  reader.chunk_size = 5u;
  memset(&result, 0x5a, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_source_decisions(test_ctx, selector, read_chunk, &reader,
                                        record_decision, &seen, &result,
                                        &error);
  if (st != LQL_STATUS_JSON_ERROR || seen.calls != 1 || seen.matched != 1 ||
      result.candidates_seen != 1u || result.candidates_matched != 1u ||
      result.bytes_read != complete_candidate_bytes || result.stopped_early) {
    printf("malformed source decision stream mismatch: status=%s calls=%d "
           "matched=%d seen=%lu result_matched=%lu bytes=%lu stopped=%d "
           "error=%s\n",
           lql_status_string(st), seen.calls, seen.matched,
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched,
           (unsigned long)result.bytes_read, result.stopped_early,
           error.message);
    ++failures;
  }
  memset(&payload_seen_value, 0, sizeof(payload_seen_value));
  memset(&reader, 0, sizeof(reader));
  reader.data = malformed;
  reader.len = strlen(malformed);
  reader.chunk_size = 5u;
  payload_seen_value.out = out;
  memset(&result, 0x5a, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_source_spooled_matches(
      test_ctx, selector, read_chunk, &reader, record_spooled_payload,
      &payload_seen_value, &result, &error);
  if (st != LQL_STATUS_JSON_ERROR || payload_seen_value.calls != 1 ||
      result.candidates_seen != 1u || result.candidates_matched != 1u ||
      result.bytes_read != complete_candidate_bytes || result.stopped_early) {
    printf("malformed source spooled stream mismatch: status=%s calls=%d "
           "seen=%lu matched=%lu bytes=%lu stopped=%d error=%s\n",
           lql_status_string(st), payload_seen_value.calls,
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched,
           (unsigned long)result.bytes_read, result.stopped_early,
           error.message);
    ++failures;
  }
  test_ctx->selector_destroy(test_ctx, selector);

  memset(&result, 0x5a, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_file_decisions(test_ctx, NULL, NULL, record_decision,
                                      NULL, &result, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "file and on_decision are required") != 0 ||
      !query_result_is_zero(&result)) {
    printf("file decisions NULL file mismatch: %s seen=%lu matched=%lu\n",
           error.message, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched);
    ++failures;
  }

  memset(&result, 0x5a, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_file_decisions(test_ctx, NULL, source, NULL, NULL,
                                      &result, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "file and on_decision are required") != 0 ||
      !query_result_is_zero(&result)) {
    printf("file decisions NULL callback mismatch: %s seen=%lu matched=%lu\n",
           error.message, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched);
    ++failures;
  }

  memset(&result, 0x5a, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_source_decisions(test_ctx, NULL, NULL, NULL,
                                        record_decision, NULL, &result, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "read and on_decision are required") != 0 ||
      !query_result_is_zero(&result)) {
    printf("source decisions NULL read mismatch: %s seen=%lu matched=%lu\n",
           error.message, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched);
    ++failures;
  }

  memset(&result, 0x5a, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_source_decisions(test_ctx, NULL, read_chunk, NULL, NULL,
                                        NULL, &result, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "read and on_decision are required") != 0 ||
      !query_result_is_zero(&result)) {
    printf("source decisions NULL callback mismatch: %s seen=%lu matched=%lu\n",
           error.message, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched);
    ++failures;
  }

  memset(&result, 0x5a, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_source_spooled_matches(
      test_ctx, NULL, NULL, NULL, record_payload, NULL, &result, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "read and on_match are required") != 0 ||
      !query_result_is_zero(&result)) {
    printf("source spooled matches NULL read mismatch: %s seen=%lu "
           "matched=%lu\n",
           error.message, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched);
    ++failures;
  }

  memset(&result, 0x5a, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_source_spooled_matches(test_ctx, NULL, read_chunk, NULL,
                                              NULL, NULL, &result, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "read and on_match are required") != 0 ||
      !query_result_is_zero(&result)) {
    printf("source spooled matches NULL callback mismatch: %s\n",
           error.message);
    ++failures;
  }

  memset(&result, 0x5a, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_file_matches(test_ctx, NULL, NULL, record_payload, NULL,
                                    &result, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "file and on_match are required") != 0 ||
      !query_result_is_zero(&result)) {
    printf("file matches NULL file mismatch: %s seen=%lu matched=%lu\n",
           error.message, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched);
    ++failures;
  }

  memset(&result, 0x5a, sizeof(result));
  lql_error_init(&error);
  st = test_ctx->query_file_matches(test_ctx, NULL, source, NULL, NULL, &result,
                                    &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "file and on_match are required") != 0 ||
      !query_result_is_zero(&result)) {
    printf("file matches NULL callback mismatch: %s seen=%lu matched=%lu\n",
           error.message, (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched);
    ++failures;
  }

  lql_error_init(&error);
  st = test_ctx->payload_write_json(test_ctx, NULL, out, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "payload and output file are required") != 0) {
    printf("payload write NULL payload mismatch: %s\n", error.message);
    ++failures;
  }

  memset(&payload, 0, sizeof(payload));
  payload.kind = LQL_PAYLOAD_NONE;
  lql_error_init(&error);
  st = test_ctx->payload_write_json(test_ctx, &payload, out, &error);
  if (st != LQL_STATUS_UNSUPPORTED ||
      strcmp(error.message, "payload is not a seekable source range") != 0) {
    printf("payload write unsupported kind mismatch: %s\n", error.message);
    ++failures;
  }

  payload.kind = LQL_PAYLOAD_SEEKABLE_RANGE;
  payload.source = NULL;
  lql_error_init(&error);
  st = test_ctx->payload_write_json(test_ctx, &payload, out, &error);
  if (st != LQL_STATUS_UNSUPPORTED ||
      strcmp(error.message, "payload is not a seekable source range") != 0) {
    printf("payload write missing source mismatch: %s\n", error.message);
    ++failures;
  }

  payload.kind = LQL_PAYLOAD_SEEKABLE_RANGE;
  payload.source = source;
  lql_error_init(&error);
  st = test_ctx->payload_write_json(test_ctx, &payload, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "payload and output file are required") != 0) {
    printf("payload write NULL out mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = test_ctx->payload_write_json_sink(test_ctx, NULL, write_memory_sink,
                                         NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "payload and write callback are required") != 0) {
    printf("payload sink NULL payload mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st =
      test_ctx->payload_write_json_sink(test_ctx, &payload, NULL, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "payload and write callback are required") != 0) {
    printf("payload sink NULL write mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = test_ctx->payload_project_json(test_ctx, NULL, projection, out, NULL,
                                      &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message,
             "payload, projection, and output file are required") != 0) {
    printf("payload project NULL payload mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = test_ctx->payload_project_json(test_ctx, &payload, NULL, out, NULL,
                                      &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message,
             "payload, projection, and output file are required") != 0) {
    printf("payload project NULL projection mismatch: %s\n", error.message);
    ++failures;
  }

  memset(&payload, 0, sizeof(payload));
  payload.kind = LQL_PAYLOAD_NONE;
  lql_error_init(&error);
  st = test_ctx->payload_project_json(test_ctx, &payload, projection, out, NULL,
                                      &error);
  if (st != LQL_STATUS_UNSUPPORTED ||
      strcmp(error.message, "payload cannot be projected") != 0) {
    printf("payload project unsupported kind mismatch: %s\n", error.message);
    ++failures;
  }

  test_ctx->projection_destroy(test_ctx, projection);
  fclose(source);
  fclose(out);
}

static void expect_stream_malformed_doc(lql_selector *selector,
                                        const char *label, const char *doc) {
  FILE *source;
  FILE *out;
  stream_seen seen;
  payload_seen payload_seen_value;
  chunk_reader reader;
  lql_error error;
  lql_status st;

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("stream malformed corpus tmpfile failed: %s\n", label);
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite(doc, 1u, strlen(doc), source) != strlen(doc) ||
      fseek(source, 0L, SEEK_SET) != 0) {
    printf("stream malformed corpus source setup failed: %s\n", label);
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }

  memset(&seen, 0, sizeof(seen));
  lql_error_init(&error);
  st = test_ctx->query_file_decisions(test_ctx, selector, source,
                                      record_decision, &seen, NULL, &error);
  if (st != LQL_STATUS_JSON_ERROR) {
    printf("stream malformed file decisions mismatch: %s status=%s error=%s\n",
           label, lql_status_string(st), error.message);
    ++failures;
  }

  if (fseek(source, 0L, SEEK_SET) != 0) {
    printf("stream malformed corpus source rewind failed: %s\n", label);
    ++failures;
  } else {
    memset(&payload_seen_value, 0, sizeof(payload_seen_value));
    payload_seen_value.out = out;
    lql_error_init(&error);
    st =
        test_ctx->query_file_matches(test_ctx, selector, source, record_payload,
                                     &payload_seen_value, NULL, &error);
    if (st != LQL_STATUS_JSON_ERROR) {
      printf("stream malformed file payload mismatch: %s status=%s error=%s\n",
             label, lql_status_string(st), error.message);
      ++failures;
    }
  }

  memset(&seen, 0, sizeof(seen));
  memset(&reader, 0, sizeof(reader));
  reader.data = doc;
  reader.len = strlen(doc);
  reader.chunk_size = 5u;
  lql_error_init(&error);
  st = test_ctx->query_source_decisions(test_ctx, selector, read_chunk, &reader,
                                        record_decision, &seen, NULL, &error);
  if (st != LQL_STATUS_JSON_ERROR) {
    printf("stream malformed source decisions mismatch: %s status=%s "
           "error=%s\n",
           label, lql_status_string(st), error.message);
    ++failures;
  }

  memset(&payload_seen_value, 0, sizeof(payload_seen_value));
  memset(&reader, 0, sizeof(reader));
  reader.data = doc;
  reader.len = strlen(doc);
  reader.chunk_size = 5u;
  payload_seen_value.out = out;
  lql_error_init(&error);
  st = test_ctx->query_source_spooled_matches(
      test_ctx, selector, read_chunk, &reader, record_spooled_payload,
      &payload_seen_value, NULL, &error);
  if (st != LQL_STATUS_JSON_ERROR) {
    printf("stream malformed source payload mismatch: %s status=%s error=%s\n",
           label, lql_status_string(st), error.message);
    ++failures;
  }

  fclose(source);
  fclose(out);
}

static void expect_stream_error_corpus_api(void) {
  static const struct {
    const char *name;
    const char *doc;
  } cases[] = {
      {"truncated object candidate", "{\"status\":\"open\"}\n{\"status\":"},
      {"truncated array stream", "[{\"status\":\"open\"},{\"status\":"},
      {"invalid literal", "{\"status\": tru}"}};
  lql_selector *selector;
  lql_error error;
  lql_status st;
  size_t i;

  selector = NULL;
  lql_error_init(&error);
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("stream malformed corpus selector parse failed: %s\n",
           error.message);
    ++failures;
    return;
  }
  for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    expect_stream_malformed_doc(selector, cases[i].name, cases[i].doc);
  }
  test_ctx->selector_destroy(test_ctx, selector);
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
  FILE *project_out;
  lql_selector *selector;
  lql_projection *projection;
  lql_query_result result;
  payload_seen seen;
  projected_payload_seen projected;
  memory_sink sink;
  lql_query_options options;
  lql_payload payload;
  lql_error error;
  lql_status st;
  static const char *projection_fields[] = {"/id"};
  char buf[128];
  size_t len;
  long pos;

  fp = tmpfile();
  out = tmpfile();
  project_out = tmpfile();
  if (fp == NULL || out == NULL || project_out == NULL) {
    printf("payload tmpfile failed\n");
    if (fp != NULL) {
      fclose(fp);
    }
    if (out != NULL) {
      fclose(out);
    }
    if (project_out != NULL) {
      fclose(project_out);
    }
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("payload input write failed\n");
    fclose(fp);
    fclose(out);
    fclose(project_out);
    ++failures;
    return;
  }
  lql_error_init(&error);
  selector = NULL;
  projection = NULL;
  st =
      test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("payload parse failed: %s\n", error.message);
    fclose(fp);
    fclose(out);
    fclose(project_out);
    ++failures;
    return;
  }
  st = test_ctx->projection_parse(test_ctx, projection_fields, 1u, &projection,
                                  &error);
  if (st != LQL_STATUS_OK) {
    printf("payload projection parse failed: %s\n", error.message);
    fclose(fp);
    fclose(out);
    fclose(project_out);
    test_ctx->selector_destroy(test_ctx, selector);
    ++failures;
    return;
  }
  memset(&seen, 0, sizeof(seen));
  seen.out = out;
  memset(&result, 0, sizeof(result));
  st = test_ctx->query_file_matches(test_ctx, selector, fp, record_payload,
                                    &seen, &result, &error);
  if (st != LQL_STATUS_OK) {
    printf("payload query failed: %s\n", error.message);
    fclose(fp);
    fclose(out);
    fclose(project_out);
    test_ctx->projection_destroy(test_ctx, projection);
    test_ctx->selector_destroy(test_ctx, selector);
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
    printf("payload sink rewind failed\n");
    ++failures;
  } else {
    memset(&sink, 0, sizeof(sink));
    memset(&result, 0, sizeof(result));
    st = test_ctx->query_file_matches(
        test_ctx, selector, fp, record_payload_sink, &sink, &result, &error);
    if (st != LQL_STATUS_OK) {
      printf("payload sink query failed: %s\n", error.message);
      ++failures;
    } else if (strcmp(sink.data, "{\"status\":\"open\",\"id\":1}"
                                 "{\"status\":\"open\",\"id\":3}") != 0) {
      printf("payload sink output mismatch: %s\n", sink.data);
      ++failures;
    }
  }

  if (fseek(fp, 0L, SEEK_SET) != 0) {
    printf("payload projection rewind failed\n");
    ++failures;
  } else {
    memset(&projected, 0, sizeof(projected));
    memset(&result, 0, sizeof(result));
    projected.out = project_out;
    projected.projection = projection;
    lql_error_init(&error);
    st = test_ctx->query_file_matches(
        test_ctx, selector, fp, record_seekable_payload_projection, &projected,
        &result, &error);
    if (st != LQL_STATUS_OK || projected.calls != 2 ||
        result.candidates_seen != (lql_uint64)3 ||
        result.candidates_matched != (lql_uint64)2) {
      printf("payload projection query mismatch: status=%s calls=%d seen=%lu "
             "matched=%lu error=%s\n",
             lql_status_string(st), projected.calls,
             (unsigned long)result.candidates_seen,
             (unsigned long)result.candidates_matched, error.message);
      ++failures;
    } else if (!read_tmpfile(project_out, buf, sizeof(buf), &len) ||
               strcmp(buf, "{\"id\":1}{\"id\":3}") != 0) {
      printf("payload projection output mismatch: %s\n", buf);
      ++failures;
    }
  }

  if (fseek(fp, 0L, SEEK_SET) != 0) {
    printf("payload max-match rewind failed\n");
    ++failures;
  } else {
    memset(&seen, 0, sizeof(seen));
    memset(&options, 0, sizeof(options));
    memset(&result, 0, sizeof(result));
    seen.out = out;
    options.max_matches = 1u;
    lql_error_init(&error);
    st = test_ctx->query_file_matches_with_options(
        test_ctx, selector, fp, &options, record_payload, &seen, &result,
        &error);
    if (st != LQL_STATUS_OK || seen.calls != 1 ||
        result.candidates_seen != (lql_uint64)1 ||
        result.candidates_matched != (lql_uint64)1 ||
        !result.stopped_early ||
        result.stop_reason != LQL_QUERY_STOP_MATCH_LIMIT) {
      printf("payload max-match stop mismatch: status=%s calls=%d seen=%lu "
             "matched=%lu stopped=%d reason=%d error=%s\n",
             lql_status_string(st), seen.calls,
             (unsigned long)result.candidates_seen,
             (unsigned long)result.candidates_matched, result.stopped_early,
             (int)result.stop_reason, error.message);
      ++failures;
    }
  }

  if (fseek(fp, 0L, SEEK_SET) != 0) {
    printf("payload max-candidate rewind failed\n");
    ++failures;
  } else {
    memset(&seen, 0, sizeof(seen));
    memset(&options, 0, sizeof(options));
    memset(&result, 0, sizeof(result));
    seen.out = out;
    options.max_candidates = 2u;
    lql_error_init(&error);
    st = test_ctx->query_file_matches_with_options(
        test_ctx, selector, fp, &options, record_payload, &seen, &result,
        &error);
    if (st != LQL_STATUS_OK || seen.calls != 1 ||
        result.candidates_seen != (lql_uint64)2 ||
        result.candidates_matched != (lql_uint64)1 ||
        !result.stopped_early ||
        result.stop_reason != LQL_QUERY_STOP_CANDIDATE_LIMIT) {
      printf("payload max-candidate stop mismatch: status=%s calls=%d "
             "seen=%lu matched=%lu stopped=%d reason=%d error=%s\n",
             lql_status_string(st), seen.calls,
             (unsigned long)result.candidates_seen,
             (unsigned long)result.candidates_matched, result.stopped_early,
             (int)result.stop_reason, error.message);
      ++failures;
    }
  }

  if (fseek(fp, 0L, SEEK_SET) != 0) {
    printf("payload max-byte rewind failed\n");
    ++failures;
  } else {
    memset(&seen, 0, sizeof(seen));
    memset(&options, 0, sizeof(options));
    memset(&result, 0, sizeof(result));
    seen.out = out;
    options.max_bytes_read = 1u;
    lql_error_init(&error);
    st = test_ctx->query_file_matches_with_options(
        test_ctx, selector, fp, &options, record_payload, &seen, &result,
        &error);
    if (st != LQL_STATUS_OK || seen.calls != 1 ||
        result.candidates_seen != (lql_uint64)1 ||
        result.candidates_matched != (lql_uint64)1 ||
        !result.stopped_early ||
        result.stop_reason != LQL_QUERY_STOP_BYTE_LIMIT ||
        result.bytes_read < (lql_uint64)24) {
      printf("payload max-byte stop mismatch: status=%s calls=%d seen=%lu "
             "matched=%lu bytes=%lu stopped=%d reason=%d error=%s\n",
             lql_status_string(st), seen.calls,
             (unsigned long)result.candidates_seen,
             (unsigned long)result.candidates_matched,
             (unsigned long)result.bytes_read, result.stopped_early,
             (int)result.stop_reason, error.message);
      ++failures;
    }
  }

  memset(&payload, 0, sizeof(payload));
  memset(&sink, 0, sizeof(sink));
  sink.fail_after_first = 1;
  payload.kind = LQL_PAYLOAD_SEEKABLE_RANGE;
  payload.source = fp;
  payload.offset = 0u;
  payload.size = 24u;
  if (fseek(fp, 7L, SEEK_SET) != 0) {
    printf("payload sink failure position setup failed\n");
    ++failures;
  } else {
    lql_error_init(&error);
    st = test_ctx->payload_write_json_sink(test_ctx, &payload,
                                           write_memory_sink, &sink, &error);
    if (st != LQL_STATUS_STOP ||
        strcmp(error.message, "payload sink write failed") != 0) {
      printf("payload sink failure mismatch: status=%s error=%s\n",
             lql_status_string(st), error.message);
      ++failures;
    }
    pos = ftell(fp);
    if (pos != 7L) {
      printf("payload sink failure did not restore source position: %ld\n",
             pos);
      ++failures;
    }
  }

  if (fseek(fp, 7L, SEEK_SET) != 0) {
    printf("payload position setup failed\n");
    ++failures;
  } else {
    memset(&sink, 0, sizeof(sink));
    payload.offset = 52u;
    payload.size = 24u;
    lql_error_init(&error);
    st = test_ctx->payload_write_json_sink(test_ctx, &payload,
                                           write_memory_sink, &sink, &error);
    pos = ftell(fp);
    if (st != LQL_STATUS_OK ||
        strcmp(sink.data, "{\"status\":\"open\",\"id\":3}") != 0 || pos != 7L) {
      printf("payload sink position preservation mismatch: status=%s pos=%ld "
             "out=%s error=%s\n",
             lql_status_string(st), pos, sink.data, error.message);
      ++failures;
    }
  }

  memset(&sink, 0, sizeof(sink));
  payload.offset = ~(lql_uint64)0;
  payload.size = 1u;
  if (fseek(fp, 7L, SEEK_SET) != 0) {
    printf("payload large-offset position setup failed\n");
    ++failures;
  }
  lql_error_init(&error);
  st = test_ctx->payload_write_json_sink(test_ctx, &payload, write_memory_sink,
                                         &sink, &error);
  pos = ftell(fp);
  if (st != LQL_STATUS_JSON_ERROR ||
      strcmp(error.message, "failed to write seekable payload range") != 0 ||
      sink.len != 0u || pos != 7L) {
    printf("payload large-offset failure mismatch: status=%s len=%lu "
           "pos=%ld error=%s\n",
           lql_status_string(st), (unsigned long)sink.len, pos, error.message);
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
    st = test_ctx->query_file_matches_with_options(test_ctx, selector, fp,
                                                   &options, record_payload,
                                                   &seen, &result, &error);
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

  test_ctx->projection_destroy(test_ctx, projection);
  test_ctx->selector_destroy(test_ctx, selector);
  fclose(fp);
  fclose(out);
  fclose(project_out);
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
  st = test_ctx->projection_parse(test_ctx, fields, 4u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("projection parse failed: %s\n", error.message);
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  found = 0;
  st = test_ctx->project_file_range(test_ctx, projection, source, 0u,
                                    (lql_uint64)strlen(first), out, &found,
                                    &error);
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
  test_ctx->projection_destroy(test_ctx, projection);
  fclose(out);

  out = tmpfile();
  root_field[0] = "/id";
  projection = NULL;
  lql_error_init(&error);
  st =
      test_ctx->projection_parse(test_ctx, root_field, 1u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("root field projection parse failed: %s\n", error.message);
    ++failures;
  } else {
    found = 0;
    st = test_ctx->project_file_range(
        test_ctx, projection, source, (lql_uint64)(strlen(first) + 1u),
        (lql_uint64)strlen(second), out, &found, &error);
    if (st != LQL_STATUS_OK) {
      printf("second object projection failed: %s\n", error.message);
      ++failures;
    }
  }
  test_ctx->projection_destroy(test_ctx, projection);
  fclose(out);

  out = tmpfile();
  missing[0] = "/missing";
  projection = NULL;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, missing, 1u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("missing projection parse failed: %s\n", error.message);
    ++failures;
  } else {
    found = 1;
    st = test_ctx->project_file_range(test_ctx, projection, source, 0u,
                                      (lql_uint64)strlen(first), out, &found,
                                      &error);
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
  test_ctx->projection_destroy(test_ctx, projection);
  fclose(out);

  invalid[0] = "/items/999999999999999999999999/sku";
  projection = NULL;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, invalid, 1u, &projection, &error);
  if (st == LQL_STATUS_OK) {
    printf("oversized array projection path parsed\n");
    test_ctx->projection_destroy(test_ctx, projection);
    ++failures;
  }
  root[0] = "/";
  projection = NULL;
  st = test_ctx->projection_parse(test_ctx, root, 1u, &projection, &error);
  if (st == LQL_STATUS_OK) {
    printf("root projection path parsed\n");
    test_ctx->projection_destroy(test_ctx, projection);
    ++failures;
  }
  index[0] = "/0/id";
  projection = NULL;
  st = test_ctx->projection_parse(test_ctx, index, 1u, &projection, &error);
  if (st == LQL_STATUS_OK) {
    printf("leading index projection path parsed\n");
    test_ctx->projection_destroy(test_ctx, projection);
    ++failures;
  }
  conflict[0] = "/nested";
  conflict[1] = "/nested/x";
  projection = NULL;
  st = test_ctx->projection_parse(test_ctx, conflict, 2u, &projection, &error);
  if (st == LQL_STATUS_OK) {
    printf("conflicting projection paths parsed\n");
    test_ctx->projection_destroy(test_ctx, projection);
    ++failures;
  }

  out = tmpfile();
  root_field[0] = "/id";
  projection = NULL;
  lql_error_init(&error);
  st =
      test_ctx->projection_parse(test_ctx, root_field, 1u, &projection, &error);
  if (st != LQL_STATUS_OK || out == NULL) {
    printf("large-offset projection setup failed\n");
    ++failures;
  } else {
    found = 1;
    st = test_ctx->project_file_range(test_ctx, projection, source,
                                      ~(lql_uint64)0, (lql_uint64)strlen(first),
                                      out, &found, &error);
    if (st != LQL_STATUS_JSON_ERROR || found) {
      printf("large-offset projection unexpectedly succeeded\n");
      ++failures;
    }
  }
  test_ctx->projection_destroy(test_ctx, projection);
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
  st =
      test_ctx->projection_parse(test_ctx, root_field, 1u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("scalar projection parse failed: %s\n", error.message);
    ++failures;
  } else {
    found = 0;
    st = test_ctx->project_file_range(test_ctx, projection, source, 0u, 1u, out,
                                      &found, &error);
    if (st == LQL_STATUS_OK) {
      printf("scalar projection unexpectedly succeeded\n");
      ++failures;
    }
  }
  test_ctx->projection_destroy(test_ctx, projection);
  fclose(source);
  fclose(out);
}

static void expect_projection_parse_normalization_api(void) {
  static const char doc[] =
      "{\"id\":\"a\",\"meta\":{\"trace\":9,\"ignore\":1},\"payload\":\"x\"}";
  const char *fields[4];
  const char *blank_fields[2];
  FILE *out;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  int found;
  char buf[128];
  size_t len;

  out = tmpfile();
  if (out == NULL) {
    printf("projection normalization tmpfile failed\n");
    ++failures;
    return;
  }
  fields[0] = " /id ";
  fields[1] = "";
  fields[2] = "/meta/trace";
  fields[3] = "/id";
  projection = NULL;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, fields, 4u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("projection normalization parse failed: %s\n", error.message);
    ++failures;
  } else {
    found = 0;
    st = test_ctx->project_json(test_ctx, projection, doc, strlen(doc), out,
                                &found, &error);
    if (st != LQL_STATUS_OK) {
      printf("projection normalization execution failed: %s\n", error.message);
      ++failures;
    } else if (!found) {
      printf("projection normalization expected found\n");
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf, "{\"id\":\"a\",\"meta\":{\"trace\":9}}") != 0) {
      printf("projection normalization output mismatch: %s\n", buf);
      ++failures;
    }
  }
  test_ctx->projection_destroy(test_ctx, projection);
  fclose(out);

  blank_fields[0] = "";
  blank_fields[1] = " ";
  projection = NULL;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, blank_fields, 2u, &projection,
                                  &error);
  if (st != LQL_STATUS_PARSE_ERROR || projection != NULL ||
      error.message[0] == '\0') {
    printf("blank projection path set mismatch: status=%s out=%p error=%s\n",
           lql_status_string(st), (void *)projection, error.message);
    ++failures;
    test_ctx->projection_destroy(test_ctx, projection);
  }
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
  st = test_ctx->projection_parse(test_ctx, fields, 3u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("buffered projection parse failed: %s\n", error.message);
    fclose(out);
    ++failures;
    return;
  }
  found = 0;
  st = test_ctx->project_json(test_ctx, projection, doc, strlen(doc), out,
                              &found, &error);
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
  test_ctx->projection_destroy(test_ctx, projection);
  fclose(out);

  out = tmpfile();
  missing[0] = "/missing";
  projection = NULL;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, missing, 1u, &projection, &error);
  if (st != LQL_STATUS_OK || out == NULL) {
    printf("buffered missing projection setup failed\n");
    ++failures;
  } else {
    found = 1;
    st = test_ctx->project_json(test_ctx, projection, doc, strlen(doc), out,
                                &found, &error);
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
  test_ctx->projection_destroy(test_ctx, projection);
  if (out != NULL) {
    fclose(out);
  }
}

static void expect_source_projection_api(void) {
  static const char doc[] =
      "{\"id\":\"a\",\"count\":1,\"nested\":{\"x\":true},\"items\":[{\"sku\":"
      "\"A\"},{\"sku\":\"B\"}]}";
  const char *fields[3];
  FILE *out;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  chunk_reader reader;
  int found;
  char buf[128];
  size_t len;

  out = tmpfile();
  if (out == NULL) {
    printf("source projection tmpfile failed\n");
    ++failures;
    return;
  }
  fields[0] = "/id";
  fields[1] = "/nested/x";
  fields[2] = "/items/1/sku";
  projection = NULL;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, fields, 3u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("source projection parse failed: %s\n", error.message);
    fclose(out);
    ++failures;
    return;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = doc;
  reader.len = strlen(doc);
  reader.chunk_size = 4u;
  found = 0;
  st = test_ctx->project_source(test_ctx, projection, read_chunk, &reader, out,
                                &found, &error);
  if (st != LQL_STATUS_OK) {
    printf("source projection failed: %s\n", error.message);
    ++failures;
  } else if (!found) {
    printf("source projection expected found\n");
    ++failures;
  } else if (reader.calls <= 1) {
    printf("source projection did not consume fragmented reads\n");
    ++failures;
  } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
             strcmp(buf, "{\"id\":\"a\",\"nested\":{\"x\":true},\"items\":["
                         "null,{\"sku\":\"B\"}]}") != 0) {
    printf("source projection output mismatch: %s\n", buf);
    ++failures;
  }

  found = 1;
  lql_error_init(&error);
  st = test_ctx->project_source(test_ctx, projection, NULL, &reader, out,
                                &found, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT || found ||
      strcmp(error.message, "projection source read callback is required") !=
          0) {
    printf("source projection NULL read mismatch: found=%d error=%s\n", found,
           error.message);
    ++failures;
  }
  found = 1;
  lql_error_init(&error);
  st = test_ctx->project_source(test_ctx, NULL, read_chunk, &reader, out,
                                &found, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT || found ||
      strcmp(error.message,
             "projection, reader, out, and out_found are required") != 0) {
    printf("source projection NULL projection mismatch: found=%d error=%s\n",
           found, error.message);
    ++failures;
  }
  found = 1;
  lql_error_init(&error);
  st = test_ctx->project_source(test_ctx, projection, read_chunk, &reader, NULL,
                                &found, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT || found ||
      strcmp(error.message,
             "projection, reader, out, and out_found are required") != 0) {
    printf("source projection NULL out mismatch: found=%d error=%s\n", found,
           error.message);
    ++failures;
  }
  lql_error_init(&error);
  st = test_ctx->project_source(test_ctx, projection, read_chunk, &reader, out,
                                NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message,
             "projection, reader, out, and out_found are required") != 0) {
    printf("source projection NULL found mismatch: %s\n", error.message);
    ++failures;
  }
  fclose(out);
  out = tmpfile();
  if (out == NULL) {
    printf("source projection read-error tmpfile failed\n");
    test_ctx->projection_destroy(test_ctx, projection);
    ++failures;
    return;
  }
  found = 1;
  lql_error_init(&error);
  st = test_ctx->project_source(test_ctx, projection, read_fail_once, NULL, out,
                                &found, &error);
  if (st != LQL_STATUS_JSON_ERROR || found ||
      strcmp(error.message, "projection source read failed") != 0) {
    printf("source projection read error mismatch: found=%d error=%s\n", found,
           error.message);
    ++failures;
  } else if (!read_tmpfile(out, buf, sizeof(buf), &len) || len != 0u) {
    printf("source projection read error wrote output: %s\n", buf);
    ++failures;
  }

  fclose(out);
  out = tmpfile();
  if (out == NULL) {
    printf("source projection over-capacity tmpfile failed\n");
    test_ctx->projection_destroy(test_ctx, projection);
    ++failures;
    return;
  }
  found = 1;
  lql_error_init(&error);
  st = test_ctx->project_source(test_ctx, projection, read_over_capacity, NULL,
                                out, &found, &error);
  if (st != LQL_STATUS_JSON_ERROR || found ||
      strcmp(error.message, "projection source read failed") != 0) {
    printf("source projection over-capacity mismatch: found=%d error=%s\n",
           found, error.message);
    ++failures;
  } else if (!read_tmpfile(out, buf, sizeof(buf), &len) || len != 0u) {
    printf("source projection over-capacity wrote output: %s\n", buf);
    ++failures;
  }

  test_ctx->projection_destroy(test_ctx, projection);
  fclose(out);
}

static void expect_projection_path_invariant_api(void) {
  static const char doc[] =
      "{\"id\":\"a\",\"meta\":{\"trace\":7},\"items\":[{\"sku\":\"A\"}]}";
  const char *duplicate[2];
  const char *conflict_parent_first[2];
  const char *conflict_child_first[2];
  FILE *out;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  int found;
  char buf[64];
  size_t len;

  out = tmpfile();
  if (out == NULL) {
    printf("projection invariant tmpfile failed\n");
    ++failures;
    return;
  }

  duplicate[0] = "/id";
  duplicate[1] = "/id";
  projection = NULL;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, duplicate, 2u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("duplicate projection path rejected: %s\n", error.message);
    ++failures;
  } else {
    found = 0;
    st = test_ctx->project_json(test_ctx, projection, doc, strlen(doc), out,
                                &found, &error);
    if (st != LQL_STATUS_OK) {
      printf("duplicate projection failed: %s\n", error.message);
      ++failures;
    } else if (!found) {
      printf("duplicate projection expected found\n");
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf, "{\"id\":\"a\"}") != 0) {
      printf("duplicate projection output mismatch: %s\n", buf);
      ++failures;
    }
  }
  test_ctx->projection_destroy(test_ctx, projection);
  fclose(out);

  conflict_parent_first[0] = "/meta";
  conflict_parent_first[1] = "/meta/trace";
  projection = NULL;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, conflict_parent_first, 2u,
                                  &projection, &error);
  if (st == LQL_STATUS_OK) {
    printf("parent-first projection conflict parsed\n");
    test_ctx->projection_destroy(test_ctx, projection);
    ++failures;
  }

  conflict_child_first[0] = "/items/0/sku";
  conflict_child_first[1] = "/items";
  projection = NULL;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, conflict_child_first, 2u,
                                  &projection, &error);
  if (st == LQL_STATUS_OK) {
    printf("child-first projection conflict parsed\n");
    test_ctx->projection_destroy(test_ctx, projection);
    ++failures;
  }
}

static void expect_projection_parse_error_corpus_api(void) {
  static const char *blank_fields[] = {"  ", "\t"};
  const char *field;
  lql_projection *projection;
  lql_error error;
  lql_status st;

  projection = (lql_projection *)1;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, NULL, 0u, &projection, &error);
  if (st != LQL_STATUS_PARSE_ERROR || projection != NULL) {
    printf("projection parse corpus empty field-set mismatch: status=%s "
           "error=%s\n",
           lql_status_string(st), error.message);
    ++failures;
  }

  projection = NULL;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, blank_fields, 2u, &projection,
                                  &error);
  if (st != LQL_STATUS_PARSE_ERROR || projection != NULL) {
    printf("projection parse corpus blank field-set mismatch: status=%s "
           "error=%s\n",
           lql_status_string(st), error.message);
    ++failures;
    test_ctx->projection_destroy(test_ctx, projection);
  }

#define EXPECT_PROJECTION_PARSE_ERROR(label, value)                            \
  do {                                                                         \
    field = (value);                                                           \
    projection = NULL;                                                         \
    lql_error_init(&error);                                                    \
    st =                                                                       \
        test_ctx->projection_parse(test_ctx, &field, 1u, &projection, &error); \
    if (st != LQL_STATUS_PARSE_ERROR || projection != NULL) {                  \
      printf("projection parse corpus mismatch: " label                        \
             " status=%s error=%s\n",                                          \
             lql_status_string(st), error.message);                            \
      ++failures;                                                              \
      test_ctx->projection_destroy(test_ctx, projection);                      \
    }                                                                          \
  } while (0)

  EXPECT_PROJECTION_PARSE_ERROR("root path", "/");
  EXPECT_PROJECTION_PARSE_ERROR("missing leading slash", "id");
  EXPECT_PROJECTION_PARSE_ERROR("leading array index", "/0/id");
  EXPECT_PROJECTION_PARSE_ERROR("oversized array index",
                                "/items/999999999999999999999999/sku");

#undef EXPECT_PROJECTION_PARSE_ERROR
}

static void expect_projection_compact_error_api(void) {
  static const char malformed[] = "{\"id\":";
  const char *field;
  FILE *out;
  FILE *source;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  int found;
  char buf[16];
  size_t len;

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
  st = test_ctx->projection_parse(test_ctx, NULL, 0u, &projection, &error);
  if (st != LQL_STATUS_PARSE_ERROR || projection != NULL ||
      strcmp(error.message, "projection fields required") != 0) {
    printf("projection parse empty-field error mismatch: %s\n", error.message);
    ++failures;
  }

  field = "/id";
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, &field, 1u, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "out projection required") != 0) {
    printf("projection parse NULL out error mismatch: %s\n", error.message);
    ++failures;
  }

  projection = NULL;
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, &field, 1u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("projection/compact error projection setup failed: %s\n",
           error.message);
    ++failures;
  } else {
    found = 1;
    lql_error_init(&error);
    st = test_ctx->project_json(test_ctx, projection, NULL, 0u, out, &found,
                                &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT || found ||
        strcmp(error.message, "json is required") != 0) {
      printf("project_json NULL json error mismatch: %s\n", error.message);
      ++failures;
    }

    found = 1;
    lql_error_init(&error);
    st = test_ctx->project_json(test_ctx, NULL, "{}", strlen("{}"), out, &found,
                                &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT || found ||
        strcmp(error.message,
               "projection, reader, out, and out_found are required") != 0) {
      printf("project_json NULL projection error mismatch: %s\n",
             error.message);
      ++failures;
    }

    found = 1;
    lql_error_init(&error);
    st = test_ctx->project_file_range(test_ctx, projection, NULL, 0u, 2u, out,
                                      &found, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT || found ||
        strcmp(error.message, "projection file is required") != 0) {
      printf("project_file_range NULL file error mismatch: %s\n",
             error.message);
      ++failures;
    }

    found = 1;
    lql_error_init(&error);
    st = test_ctx->project_json(test_ctx, projection, malformed,
                                strlen(malformed), out, &found, &error);
    if (st != LQL_STATUS_JSON_ERROR || found) {
      printf("project_json malformed input status mismatch: %s\n",
             error.message);
      ++failures;
    }

    if (fwrite(malformed, 1u, strlen(malformed), source) != strlen(malformed) ||
        fflush(source) != 0 || fseek(source, 0L, SEEK_SET) != 0) {
      printf("projection malformed file setup failed\n");
      ++failures;
    } else {
      found = 1;
      lql_error_init(&error);
      st = test_ctx->project_file_range(test_ctx, projection, source, 0u,
                                        (lql_uint64)strlen(malformed), out,
                                        &found, &error);
      if (st != LQL_STATUS_JSON_ERROR || found) {
        printf("project_file_range malformed input status mismatch: %s\n",
               error.message);
        ++failures;
      }
    }
  }
  test_ctx->projection_destroy(test_ctx, projection);

  lql_error_init(&error);
  st = test_ctx->compact_file_range(test_ctx, NULL, 0u, 0u, out, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "file and out are required") != 0) {
    printf("compact_file_range NULL file error mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = test_ctx->compact_file_range(test_ctx, source, 0u, 0u, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "file and out are required") != 0) {
    printf("compact_file_range NULL out error mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = test_ctx->compact_source(test_ctx, NULL, NULL, out, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "read and out are required") != 0) {
    printf("compact_source NULL read error mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = test_ctx->compact_source(test_ctx, read_chunk, NULL, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "read and out are required") != 0) {
    printf("compact_source NULL out error mismatch: %s\n", error.message);
    ++failures;
  }

  fclose(out);
  out = tmpfile();
  if (out == NULL) {
    printf("compact_source read-error tmpfile failed\n");
    fclose(source);
    ++failures;
    return;
  }
  lql_error_init(&error);
  st = test_ctx->compact_source(test_ctx, read_fail_once, NULL, out, &error);
  if (st != LQL_STATUS_JSON_ERROR ||
      strcmp(error.message, "compact source read failed") != 0) {
    printf("compact_source read error mismatch: %s\n", error.message);
    ++failures;
  } else {
    if (!read_tmpfile(out, buf, sizeof(buf), &len) || len != 0u) {
      printf("compact_source read error wrote output: %s\n", buf);
      ++failures;
    }
  }

  fclose(out);
  out = tmpfile();
  if (out == NULL) {
    printf("compact_source over-capacity tmpfile failed\n");
    fclose(source);
    ++failures;
    return;
  }
  lql_error_init(&error);
  st =
      test_ctx->compact_source(test_ctx, read_over_capacity, NULL, out, &error);
  if (st != LQL_STATUS_JSON_ERROR ||
      strcmp(error.message, "compact source read failed") != 0) {
    printf("compact_source over-capacity mismatch: %s\n", error.message);
    ++failures;
  } else if (!read_tmpfile(out, buf, sizeof(buf), &len) || len != 0u) {
    printf("compact_source over-capacity wrote output: %s\n", buf);
    ++failures;
  }

  lql_error_init(&error);
  st = test_ctx->compact_json(test_ctx, NULL, 0u, out, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "json and out are required") != 0) {
    printf("compact_json NULL json error mismatch: %s\n", error.message);
    ++failures;
  }

  lql_error_init(&error);
  st = test_ctx->compact_json(test_ctx, "{}", strlen("{}"), NULL, &error);
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
  chunk_reader reader;
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
  st = test_ctx->compact_file_range(test_ctx, source, 0u,
                                    (lql_uint64)strlen(first), out, &error);
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
  memset(&reader, 0, sizeof(reader));
  reader.data = first;
  reader.len = strlen(first);
  reader.chunk_size = 4u;
  lql_error_init(&error);
  st = test_ctx->compact_source(test_ctx, read_chunk, &reader, out, &error);
  if (st != LQL_STATUS_OK) {
    printf("compact source failed: %s\n", error.message);
    ++failures;
  } else if (reader.calls <= 1) {
    printf("compact source did not consume fragmented reads\n");
    ++failures;
  } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
             strcmp(buf, "{\"id\":\"a\",\"items\":[1,2]}") != 0) {
    printf("compact source output mismatch: %s\n", buf);
    ++failures;
  }
  fclose(out);

  out = tmpfile();
  lql_error_init(&error);
  st = test_ctx->compact_json(test_ctx, "{ \"ok\" : true }",
                              strlen("{ \"ok\" : true }"), out, &error);
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
  st = test_ctx->compact_file_range(test_ctx, source, 0u, (lql_uint64)3, out,
                                    &error);
  if (st != LQL_STATUS_JSON_ERROR) {
    printf("compact invalid range unexpectedly succeeded\n");
    ++failures;
  }
  fclose(source);
  fclose(out);
}

static void expect_compact_error_corpus_api(void) {
  static const struct {
    const char *name;
    const char *json;
  } cases[] = {{"truncated object", "{\"id\":"},
               {"trailing comma", "[1,]"},
               {"invalid literal", "{\"ok\":tru}"},
               {"trailing token", "{\"ok\":true} false"}};
  FILE *source;
  FILE *out;
  chunk_reader reader;
  lql_error error;
  lql_status st;
  size_t i;

  for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    out = tmpfile();
    if (out == NULL) {
      printf("compact error corpus output tmpfile failed: %s\n", cases[i].name);
      ++failures;
      continue;
    }
    lql_error_init(&error);
    st = test_ctx->compact_json(test_ctx, cases[i].json, strlen(cases[i].json),
                                out, &error);
    if (st != LQL_STATUS_JSON_ERROR) {
      printf("compact_json invalid corpus mismatch: %s status=%s error=%s\n",
             cases[i].name, lql_status_string(st), error.message);
      ++failures;
    }
    fclose(out);

    out = tmpfile();
    if (out == NULL) {
      printf("compact error corpus source output tmpfile failed: %s\n",
             cases[i].name);
      ++failures;
      continue;
    }
    memset(&reader, 0, sizeof(reader));
    reader.data = cases[i].json;
    reader.len = strlen(cases[i].json);
    reader.chunk_size = 5u;
    lql_error_init(&error);
    st = test_ctx->compact_source(test_ctx, read_chunk, &reader, out, &error);
    if (st != LQL_STATUS_JSON_ERROR) {
      printf("compact_source invalid corpus mismatch: %s status=%s error=%s\n",
             cases[i].name, lql_status_string(st), error.message);
      ++failures;
    }
    fclose(out);

    source = tmpfile();
    out = tmpfile();
    if (source == NULL || out == NULL) {
      printf("compact error corpus file tmpfile failed: %s\n", cases[i].name);
      if (source != NULL) {
        fclose(source);
      }
      if (out != NULL) {
        fclose(out);
      }
      ++failures;
      continue;
    }
    if (fwrite(cases[i].json, 1u, strlen(cases[i].json), source) !=
            strlen(cases[i].json) ||
        fseek(source, 0L, SEEK_SET) != 0) {
      printf("compact error corpus source setup failed: %s\n", cases[i].name);
      ++failures;
    } else {
      lql_error_init(&error);
      st = test_ctx->compact_file_range(
          test_ctx, source, 0u, (lql_uint64)strlen(cases[i].json), out, &error);
      if (st != LQL_STATUS_JSON_ERROR) {
        printf("compact_file_range invalid corpus mismatch: %s status=%s "
               "error=%s\n",
               cases[i].name, lql_status_string(st), error.message);
        ++failures;
      }
    }
    fclose(source);
    fclose(out);
  }
}

static void expect_mutation_plan_api(void) {
  lql_mutation_plan *plan;
  lql_mutation_parse_options options;
  lql_error error;
  lql_status st;
  const char *valid[11];
  const char *wildcards[3];
  const char *newline_separated;
  const char *multiline_brace;
  const char *file_backed[2];
  const char *invalid_default[9];
  const char *invalid_file_options[4];
  const char *blank;
  size_t i;

  valid[0] = "/state/progress=ready";
  valid[1] = "/state/metrics++";
  valid[2] = "/state/details{/owner=\"alice\",/note=\"hi, world\"}";
  valid[3] = "/state/metrics=+3";
  valid[4] = "time:/state/updated=NOW";
  valid[5] = "rm:/state/legacy";
  valid[6] = "/state/metrics--";
  valid[7] = "/state/metrics=-2";
  valid[8] = "remove:/state/remove_me";
  valid[9] = "delete:/state/delete_me";
  valid[10] = "del:/state/del_me";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, valid, 11u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else if (test_ctx->mutation_plan_count(test_ctx, plan) != 12u) {
    printf("mutation plan count mismatch: %lu\n",
           (unsigned long)test_ctx->mutation_plan_count(test_ctx, plan));
    ++failures;
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);

  newline_separated = "/state/status=ready\n/state/count=+2\nrm:/state/old";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, &newline_separated, 1u, &plan,
                                     &error);
  if (st != LQL_STATUS_OK) {
    printf("newline-separated mutation plan parse failed: %s\n",
           error.message);
    ++failures;
  } else if (test_ctx->mutation_plan_count(test_ctx, plan) != 3u) {
    printf("newline-separated mutation plan count mismatch: %lu\n",
           (unsigned long)test_ctx->mutation_plan_count(test_ctx, plan));
    ++failures;
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);

  multiline_brace =
      "/state/details{\n"
      "  /owner = \"alice\"\n"
      "  /note = \"hi, world\"\n"
      "}\n"
      "delete:/state/details/temporary\n"
      "/state/count=+4\n"
      "/state/count--";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, &multiline_brace, 1u, &plan,
                                     &error);
  if (st != LQL_STATUS_OK) {
    printf("multiline brace mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else if (test_ctx->mutation_plan_count(test_ctx, plan) != 5u) {
    printf("multiline brace mutation plan count mismatch: %lu\n",
           (unsigned long)test_ctx->mutation_plan_count(test_ctx, plan));
    ++failures;
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);

  wildcards[0] = "/items/*/status=ready";
  wildcards[1] = "/groups/.../sku=ok";
  wildcards[2] = "/records[]/count=+1";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, wildcards, 3u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("wildcard mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else if (test_ctx->mutation_plan_count(test_ctx, plan) != 3u) {
    printf("wildcard mutation plan count mismatch: %lu\n",
           (unsigned long)test_ctx->mutation_plan_count(test_ctx, plan));
    ++failures;
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);

  invalid_default[0] = "badexpr";
  invalid_default[1] = "/";
  invalid_default[2] = "/count=+0";
  invalid_default[3] = "time:/state/updated=tomorrowish";
  invalid_default[4] = "file:/payload=blob.txt";
  invalid_default[5] = "time:/state/updated=2025-01-01";
  invalid_default[6] = "textfile:/payload=blob.txt";
  invalid_default[7] = "base64file:/payload=blob.bin";
  invalid_default[8] = "/state/details{/owner=\"alice\"}}";
  for (i = 0u; i < sizeof(invalid_default) / sizeof(invalid_default[0]); ++i) {
    plan = NULL;
    lql_error_init(&error);
    st = test_ctx->mutation_plan_parse(test_ctx, &invalid_default[i], 1u, &plan,
                                       &error);
    if (st != LQL_STATUS_PARSE_ERROR || plan != NULL) {
      printf("invalid mutation parse mismatch: %s status=%s error=%s\n",
             invalid_default[i], lql_status_string(st), error.message);
      ++failures;
    }
    test_ctx->mutation_plan_destroy(test_ctx, plan);
  }

  blank = "";
  plan = (lql_mutation_plan *)1;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, &blank, 1u, &plan, &error);
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
  st = test_ctx->mutation_plan_parse_with_options(test_ctx, file_backed, 1u,
                                                  &options, &plan, &error);
  if (st != LQL_STATUS_PARSE_ERROR || plan != NULL) {
    printf("relative file-backed mutation status mismatch: %s\n",
           error.message);
    ++failures;
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);

  options.file_value_base_dir = "/tmp";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse_with_options(test_ctx, file_backed, 2u,
                                                  &options, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("enabled file-backed mutation parse failed: %s\n", error.message);
    ++failures;
  } else if (test_ctx->mutation_plan_count(test_ctx, plan) != 2u) {
    printf("enabled file-backed mutation count mismatch: %lu\n",
           (unsigned long)test_ctx->mutation_plan_count(test_ctx, plan));
    ++failures;
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);

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
    st = test_ctx->mutation_plan_parse_with_options(
        test_ctx, &invalid_file_options[i], 1u, &options, &plan, &error);
    if (st != LQL_STATUS_PARSE_ERROR || plan != NULL) {
      printf("invalid file-backed mutation parsed: %s status=%s error=%s\n",
             invalid_file_options[i], lql_status_string(st), error.message);
      ++failures;
    }
    test_ctx->mutation_plan_destroy(test_ctx, plan);
  }
}

static void expect_mutation_error_api(void) {
  static const char malformed[] = "{\"status\":\"open\",\"count\":";
  FILE *source;
  FILE *out;
  lql_mutation_plan *plan;
  lql_mutation_plan *nested_plan;
  lql_query_result result;
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

  if (test_ctx->mutation_plan_count(test_ctx, NULL) != 0u) {
    printf("NULL mutation plan count mismatch\n");
    ++failures;
  }
  test_ctx->mutation_plan_destroy(test_ctx, NULL);

  expr = "/status=done";
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, &expr, 1u, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "out is required") != 0) {
    printf("mutation parse NULL out mismatch: %s\n", error.message);
    ++failures;
  }

  plan = (lql_mutation_plan *)1;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, NULL, 0u, &plan, &error);
  if (st != LQL_STATUS_PARSE_ERROR || plan != NULL ||
      strcmp(error.message, "no field mutations provided") != 0) {
    printf("mutation parse empty input mismatch: %s\n", error.message);
    ++failures;
  }

  expr = NULL;
  plan = (lql_mutation_plan *)1;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, &expr, 1u, &plan, &error);
  if (st != LQL_STATUS_PARSE_ERROR || plan != NULL ||
      strcmp(error.message, "no valid field mutations parsed") != 0) {
    printf("mutation parse NULL expression mismatch: %s\n", error.message);
    ++failures;
  }

  nested_plan = NULL;
  expr = "/status=done";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, &expr, 1u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("mutation error plan setup failed: %s\n", error.message);
    ++failures;
  } else {
    lql_error_init(&error);
    st = test_ctx->mutate_file_range_root_fields(test_ctx, NULL, source, 0u, 2u,
                                                 out, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, file, and out are required") != 0) {
      printf("root mutation NULL plan mismatch: %s\n", error.message);
      ++failures;
    }

    lql_error_init(&error);
    st = test_ctx->mutate_file_range_root_fields(test_ctx, plan, NULL, 0u, 2u,
                                                 out, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, file, and out are required") != 0) {
      printf("root mutation NULL file mismatch: %s\n", error.message);
      ++failures;
    }

    lql_error_init(&error);
    st = test_ctx->mutate_file_range_paths(test_ctx, plan, source, 0u, 2u, NULL,
                                           &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, file, and out are required") != 0) {
      printf("path mutation NULL out mismatch: %s\n", error.message);
      ++failures;
    }

    expr = "/state/status=done";
    lql_error_init(&error);
    st = test_ctx->mutation_plan_parse(test_ctx, &expr, 1u, &nested_plan,
                                       &error);
    if (st != LQL_STATUS_OK) {
      printf("nested mutation plan setup failed: %s\n", error.message);
      ++failures;
    } else {
      lql_error_init(&error);
      st = test_ctx->mutate_file_range_root_fields(
          test_ctx, nested_plan, source, 0u, 2u, out, &error);
      if (st != LQL_STATUS_UNSUPPORTED ||
          strcmp(error.message,
                 "mutation plan requires unsupported non-root behavior") !=
              0) {
        printf("root mutation nested-plan unsupported mismatch: status=%s "
               "error=%s\n",
               lql_status_string(st), error.message);
        ++failures;
      }
    }
    test_ctx->mutation_plan_destroy(test_ctx, nested_plan);
    nested_plan = NULL;

    memset(&result, 0x5a, sizeof(result));
    lql_error_init(&error);
    st = test_ctx->mutate_file_range_candidates(
        test_ctx, NULL, NULL, source, 0u, 2u, out, 1, 0, &result, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, file, and out are required") != 0 ||
        !query_result_is_zero(&result)) {
      printf("candidate mutation NULL plan mismatch: %s\n", error.message);
      ++failures;
    }

    memset(&result, 0x5a, sizeof(result));
    lql_error_init(&error);
    st = test_ctx->mutate_source_candidates(test_ctx, NULL, NULL, read_chunk,
                                            NULL, out, 1, 0, &result, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, read, and out are required") != 0 ||
        !query_result_is_zero(&result)) {
      printf("source candidate mutation NULL plan mismatch: %s\n",
             error.message);
      ++failures;
    }

    memset(&result, 0x5a, sizeof(result));
    lql_error_init(&error);
    st = test_ctx->mutate_source_candidates_with_options(
        test_ctx, NULL, plan, NULL, NULL, out, 1, 0, NULL, &result, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, read, and out are required") != 0 ||
        !query_result_is_zero(&result)) {
      printf("source candidate mutation options NULL read mismatch: %s\n",
             error.message);
      ++failures;
    }

    lql_error_init(&error);
    st = test_ctx->mutate_json(test_ctx, NULL, "{}", strlen("{}"), out, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, json, and out are required") != 0) {
      printf("json mutation NULL plan mismatch: %s\n", error.message);
      ++failures;
    }

    lql_error_init(&error);
    st = test_ctx->mutate_json(test_ctx, plan, NULL, 0u, out, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, json, and out are required") != 0) {
      printf("json mutation NULL json mismatch: %s\n", error.message);
      ++failures;
    }

    lql_error_init(&error);
    st =
        test_ctx->mutate_json(test_ctx, plan, "{}", strlen("{}"), NULL, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, json, and out are required") != 0) {
      printf("json mutation NULL out mismatch: %s\n", error.message);
      ++failures;
    }

    lql_error_init(&error);
    st = test_ctx->mutate_json(test_ctx, plan, malformed, strlen(malformed),
                               out, &error);
    if (st != LQL_STATUS_JSON_ERROR) {
      printf("json mutation malformed input status mismatch: %s\n",
             error.message);
      ++failures;
    }
    {
      static const char *const non_object_roots[] = {"", "7", "null", "\"x\"",
                                                     "[{\"id\":\"a\"}]"};
      size_t i;
      for (i = 0u; i < sizeof(non_object_roots) / sizeof(non_object_roots[0]);
           ++i) {
        lql_error_init(&error);
        st = test_ctx->mutate_json(test_ctx, plan, non_object_roots[i],
                                   strlen(non_object_roots[i]), out, &error);
        if (st != LQL_STATUS_JSON_ERROR) {
          printf("json mutation accepted non-object root %s with status %s\n",
                 non_object_roots[i], lql_status_string(st));
          ++failures;
        }
      }
    }

    if (fseek(source, 0L, SEEK_SET) != 0 ||
        fwrite(malformed, 1u, strlen(malformed), source) != strlen(malformed) ||
        fflush(source) != 0 || fseek(source, 0L, SEEK_SET) != 0) {
      printf("mutation malformed file setup failed\n");
      ++failures;
    } else {
      lql_error_init(&error);
      st = test_ctx->mutate_file_range_paths(test_ctx, plan, source, 0u,
                                             (lql_uint64)strlen(malformed), out,
                                             &error);
      if (st != LQL_STATUS_JSON_ERROR) {
        printf("path mutation malformed input status mismatch: %s\n",
               error.message);
        ++failures;
      }
      lql_error_init(&error);
      st = test_ctx->mutate_file_range_root_fields(
          test_ctx, plan, source, 0u, (lql_uint64)strlen(malformed), out,
          &error);
      if (st != LQL_STATUS_JSON_ERROR) {
        printf("root mutation malformed input status mismatch: %s\n",
               error.message);
        ++failures;
      }
    }

    {
      static const char array_root[] = "[{\"id\":\"a\"}]";
      if (fseek(source, 0L, SEEK_SET) != 0 ||
          fwrite(array_root, 1u, strlen(array_root), source) !=
              strlen(array_root) ||
          fflush(source) != 0 || fseek(source, 0L, SEEK_SET) != 0) {
        printf("mutation non-object file setup failed\n");
        ++failures;
      } else {
        lql_error_init(&error);
        st = test_ctx->mutate_file_range_paths(
            test_ctx, plan, source, 0u, (lql_uint64)strlen(array_root), out,
            &error);
        if (st != LQL_STATUS_JSON_ERROR) {
          printf("path mutation accepted non-object root with status %s\n",
                 lql_status_string(st));
          ++failures;
        }
        lql_error_init(&error);
        st = test_ctx->mutate_file_range_root_fields(
            test_ctx, plan, source, 0u, (lql_uint64)strlen(array_root), out,
            &error);
        if (st != LQL_STATUS_JSON_ERROR) {
          printf("root mutation accepted non-object root with status %s\n",
                 lql_status_string(st));
          ++failures;
        }
      }
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
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
  const char *exprs[9];
  const char *file_exprs[4];
  lql_mutation_parse_options options;
  char buf[256];
  size_t len;
  static const char doc[] =
      "{\"status\":\"open\",\"count\":1,\"score\":5,\"old\":true,"
      "\"remove_me\":true,\"delete_me\":true,\"del_me\":true}";

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
  exprs[1] = "/count--";
  exprs[2] = "rm:/old";
  exprs[3] = "/missing=value";
  exprs[4] = "/quoted_number=\"2\"";
  exprs[5] = "/score=-2";
  exprs[6] = "remove:/remove_me";
  exprs[7] = "delete:/delete_me";
  exprs[8] = "del:/del_me";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 9u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("root mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_file_range_root_fields(
        test_ctx, plan, source, 0u, (lql_uint64)strlen(doc), out, &error);
    if (st != LQL_STATUS_OK) {
      printf("root mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf, "{\"status\":\"done\",\"count\":0,\"score\":3,"
                           "\"missing\":\"value\",\"quoted_number\":2}") != 0) {
      printf("root mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
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
  st = test_ctx->mutation_plan_parse_with_options(test_ctx, file_exprs, 4u,
                                                  &options, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("file-backed root mutation parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_file_range_root_fields(
        test_ctx, plan, source, 0u, (lql_uint64)strlen(doc), out, &error);
    if (st != LQL_STATUS_OK) {
      printf("file-backed root mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"status\":\"open\",\"count\":1,\"score\":5,"
                      "\"old\":true,\"remove_me\":true,\"delete_me\":true,"
                      "\"del_me\":true,\"text_payload\":\"hi\","
                      "\"bin_payload\":\"aGk=\","
                      "\"auto_text\":\"hi\",\"auto_bin\":\"AAECYQ==\"}") != 0) {
      printf("file-backed root mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  fclose(out);
  remove("lql-test-payload.txt");
  remove("lql-test-payload.bin");

  out = tmpfile();
  exprs[0] = "/nested/status=done";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 1u, &plan, &error);
  if (st == LQL_STATUS_OK) {
    st = test_ctx->mutate_file_range_root_fields(
        test_ctx, plan, source, 0u, (lql_uint64)strlen(doc), out, &error);
    if (st != LQL_STATUS_UNSUPPORTED) {
      printf("nested root mutation unexpectedly supported\n");
      ++failures;
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
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
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 7u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("path mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_file_range_paths(
        test_ctx, plan, source, 0u, (lql_uint64)strlen(doc), out, &error);
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
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  fclose(source);
  fclose(out);
}

static void expect_buffered_mutation_api(void) {
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *exprs[3];
  char buf[512];
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
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 3u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("buffered mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_json(test_ctx, plan, doc, strlen(doc), out, &error);
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
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  fclose(out);

  out = tmpfile();
  if (out == NULL) {
    printf("buffered numeric mutation tmpfile failed\n");
    ++failures;
    return;
  }
  exprs[0] = "/voucher/lines/10/amount=+2";
  exprs[1] = "/voucher/lines/10/status=patched";
  exprs[2] = "/voucher/.../10/code=patched";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 3u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("buffered numeric mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_json(
        test_ctx, plan,
        "{\"voucher\":{\"lines\":{\"10\":{\"amount\":5,\"status\":\"open\","
        "\"code\":\"before\"}}}}",
        strlen("{\"voucher\":{\"lines\":{\"10\":{\"amount\":5,\"status\":"
               "\"open\",\"code\":\"before\"}}}}"),
        out, &error);
    if (st != LQL_STATUS_OK) {
      printf("buffered numeric mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"voucher\":{\"lines\":{\"10\":{\"amount\":7,"
                      "\"status\":\"patched\",\"code\":\"patched\"}}}}") != 0) {
      printf("buffered numeric mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  fclose(out);

  out = tmpfile();
  if (out == NULL) {
    printf("buffered numeric create mutation tmpfile failed\n");
    ++failures;
    return;
  }
  exprs[0] = "/voucher/lines/10/status=created";
  exprs[1] = "/voucher/lines/10/amount=+3";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 2u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("buffered numeric create mutation plan parse failed: %s\n",
           error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_json(test_ctx, plan, "{\"voucher\":{\"lines\":{}}}",
                               strlen("{\"voucher\":{\"lines\":{}}}"), out,
                               &error);
    if (st != LQL_STATUS_OK) {
      printf("buffered numeric create mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"voucher\":{\"lines\":{\"10\":{\"status\":\"created\","
                      "\"amount\":3}}}}") != 0) {
      printf("buffered numeric create mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  fclose(out);
}

static void expect_buffered_wildcard_mutation_api(void) {
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *exprs[5];
  char buf[1024];
  size_t len;
  static const char recursive_doc[] =
      "{\"items\":[{\"status\":\"old\"}],\"boxes\":{\"a\":{\"status\":\"old\"}}"
      ","
      "\"groups\":[{\"items\":[{\"sku\":\"A\",\"count\":1,\"drop\":true}]}]}";
  static const char array_doc[] =
      "{\"nums\":[1,2],\"words\":[\"a\",\"b\"],\"drops\":[true,false],"
      "\"objects\":[{\"a\":1}],\"groups\":[{\"items\":[{\"count\":1}]}]}";

  out = tmpfile();
  if (out == NULL) {
    printf("buffered recursive wildcard mutation tmpfile failed\n");
    ++failures;
    return;
  }
  exprs[0] = "/items/**/status=ready";
  exprs[1] = "/boxes/**/status=ready";
  exprs[2] = "/groups/.../sku=Z";
  exprs[3] = "/groups/.../count=+2";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 4u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("buffered recursive wildcard mutation plan parse failed: %s\n",
           error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_json(test_ctx, plan, recursive_doc,
                               strlen(recursive_doc), out, &error);
    if (st != LQL_STATUS_OK) {
      printf("buffered recursive wildcard mutation failed: %s\n",
             error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"items\":[{\"status\":\"ready\"}],\"boxes\":{\"a\":{"
                      "\"status\":\"ready\"}},\"groups\":[{\"items\":[{"
                      "\"sku\":\"Z\",\"count\":3,\"drop\":true}]}]}") != 0) {
      printf("buffered recursive wildcard mutation output mismatch: %s\n",
             buf);
      ++failures;
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  fclose(out);

  out = tmpfile();
  if (out == NULL) {
    printf("buffered array wildcard mutation tmpfile failed\n");
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
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 5u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("buffered array wildcard mutation plan parse failed: %s\n",
           error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_json(test_ctx, plan, array_doc, strlen(array_doc),
                               out, &error);
    if (st != LQL_STATUS_OK) {
      printf("buffered array wildcard mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf, "{\"nums\":[3,4],\"words\":[\"ready\",\"ready\"],"
                           "\"drops\":[null,null],\"objects\":[\"done\"],"
                           "\"groups\":[{\"items\":[{\"count\":3}]}]}") != 0) {
      printf("buffered array wildcard mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  fclose(out);
}

static void expect_source_mutation_api(void) {
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *exprs[4];
  chunk_reader reader;
  char buf[256];
  size_t len;
  static const char doc[] =
      "{\"state\":{\"status\":\"open\",\"count\":1,\"old\":true},\"id\":\"a\"}";

  out = tmpfile();
  if (out == NULL) {
    printf("source mutation tmpfile failed\n");
    ++failures;
    return;
  }
  exprs[0] = "/state/status=done";
  exprs[1] = "/state/count++";
  exprs[2] = "rm:/state/old";
  exprs[3] = "/state/missing=value";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 4u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("source mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    memset(&reader, 0, sizeof(reader));
    reader.data = doc;
    reader.len = strlen(doc);
    reader.chunk_size = 3u;
    st = test_ctx->mutate_source_paths(test_ctx, plan, read_chunk, &reader, out,
                                       &error);
    if (st != LQL_STATUS_OK) {
      printf("source mutation failed: %s\n", error.message);
      ++failures;
    } else if (reader.calls <= 1) {
      printf("source mutation did not consume fragmented reads\n");
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf, "{\"state\":{\"status\":\"done\",\"count\":2,"
                           "\"missing\":\"value\"},\"id\":\"a\"}") != 0) {
      printf("source mutation output mismatch: %s\n", buf);
      ++failures;
    }

    lql_error_init(&error);
    st = test_ctx->mutate_source_paths(test_ctx, NULL, read_chunk, &reader, out,
                                       &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, read, and out are required") != 0) {
      printf("source mutation NULL plan mismatch: %s\n", error.message);
      ++failures;
    }
    lql_error_init(&error);
    st = test_ctx->mutate_source_paths(test_ctx, plan, NULL, &reader, out,
                                       &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, read, and out are required") != 0) {
      printf("source mutation NULL read mismatch: %s\n", error.message);
      ++failures;
    }
    lql_error_init(&error);
    st = test_ctx->mutate_source_paths(test_ctx, plan, read_chunk, &reader,
                                       NULL, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "plan, read, and out are required") != 0) {
      printf("source mutation NULL out mismatch: %s\n", error.message);
      ++failures;
    }

    memset(&reader, 0, sizeof(reader));
    reader.data = "[{\"id\":\"a\"}]";
    reader.len = strlen(reader.data);
    reader.chunk_size = 2u;
    lql_error_init(&error);
    st = test_ctx->mutate_source_paths(test_ctx, plan, read_chunk, &reader, out,
                                       &error);
    if (st != LQL_STATUS_JSON_ERROR) {
      printf("source mutation accepted non-object root with status %s\n",
             lql_status_string(st));
      ++failures;
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source mutation read-failure tmpfile failed\n");
      ++failures;
      test_ctx->mutation_plan_destroy(test_ctx, plan);
      return;
    }
    lql_error_init(&error);
    st = test_ctx->mutate_source_paths(test_ctx, plan, read_fail_once, NULL,
                                       out, &error);
    if (st != LQL_STATUS_JSON_ERROR ||
        strcmp(error.message, "mutation source read failed") != 0) {
      printf("source mutation read error mismatch: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) || len != 0u) {
      printf("source mutation read error wrote output: %s\n", buf);
      ++failures;
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source mutation over-capacity tmpfile failed\n");
      ++failures;
      test_ctx->mutation_plan_destroy(test_ctx, plan);
      return;
    }
    lql_error_init(&error);
    st = test_ctx->mutate_source_paths(test_ctx, plan, read_over_capacity, NULL,
                                       out, &error);
    if (st != LQL_STATUS_JSON_ERROR ||
        strcmp(error.message, "mutation source read failed") != 0) {
      printf("source mutation over-capacity mismatch: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) || len != 0u) {
      printf("source mutation over-capacity wrote output: %s\n", buf);
      ++failures;
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  fclose(out);
}

static void expect_file_range_candidate_mutation_api(void) {
  FILE *source;
  FILE *out;
  lql_error error;
  lql_status st;
  lql_selector *selector;
  lql_mutation_plan *plan;
  lql_query_result result;
  lql_query_options options;
  const char *expr;
  const char *mutation;
  char buf[512];
  size_t len;
  static const char doc[] = "[{\"id\":\"a\",\"status\":\"open\"},{\"id\":\"b\","
                            "\"status\":\"closed\"}]";

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("candidate mutation tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite(doc, 1u, strlen(doc), source) != strlen(doc) ||
      fflush(source) != 0 || fseek(source, 0L, SEEK_SET) != 0) {
    printf("candidate mutation source setup failed\n");
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }

  selector = NULL;
  plan = NULL;
  expr = "/status=\"open\"";
  mutation = "/status=done";
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("candidate mutation selector parse failed: %s\n", error.message);
    ++failures;
  }
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, &mutation, 1u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("candidate mutation plan parse failed: %s\n", error.message);
    ++failures;
  }
  if (selector != NULL && plan != NULL) {
    memset(&result, 0, sizeof(result));
    lql_error_init(&error);
    st = test_ctx->mutate_file_range_candidates(
        test_ctx, selector, plan, source, 0u, (lql_uint64)strlen(doc), out, 1,
        0, &result, &error);
    if (st != LQL_STATUS_OK) {
      printf("candidate mutation failed: %s\n", error.message);
      ++failures;
    } else if (result.candidates_seen != 2u ||
               result.candidates_matched != 1u || result.stopped_early ||
               result.bytes_read == 0u) {
      printf("candidate mutation result mismatch: seen=%lu matched=%lu "
             "stopped=%d bytes=%lu\n",
             (unsigned long)result.candidates_seen,
             (unsigned long)result.candidates_matched, result.stopped_early,
             (unsigned long)result.bytes_read);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf, "{\"id\":\"a\",\"status\":\"done\"}\n"
                           "{\"id\":\"b\",\"status\":\"closed\"}\n") != 0) {
      printf("candidate mutation output mismatch: %s\n", buf);
      ++failures;
    }

    if (fseek(source, 0L, SEEK_SET) != 0) {
      printf("candidate mutation source rewind failed\n");
      ++failures;
    } else {
      fclose(out);
      out = tmpfile();
      if (out == NULL) {
        printf("candidate mutation matches-only tmpfile failed\n");
        ++failures;
      } else {
        memset(&result, 0, sizeof(result));
        lql_error_init(&error);
        st = test_ctx->mutate_file_range_candidates(
            test_ctx, selector, plan, source, 0u, (lql_uint64)strlen(doc), out,
            1, 1, &result, &error);
        if (st != LQL_STATUS_OK) {
          printf("candidate mutation matches-only failed: %s\n", error.message);
          ++failures;
        } else if (result.candidates_seen != 2u ||
                   result.candidates_matched != 1u || result.stopped_early) {
          printf("candidate mutation matches-only result mismatch\n");
          ++failures;
        } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
                   strcmp(buf, "{\"id\":\"a\",\"status\":\"done\"}\n") != 0) {
          printf("candidate mutation matches-only output mismatch: %s\n", buf);
          ++failures;
        }
      }
    }

    if (fseek(source, 0L, SEEK_SET) != 0) {
      printf("candidate mutation match-all rewind failed\n");
      ++failures;
    } else {
      fclose(out);
      out = tmpfile();
      if (out == NULL) {
        printf("candidate mutation match-all tmpfile failed\n");
        ++failures;
      } else {
        memset(&result, 0, sizeof(result));
        lql_error_init(&error);
        st = test_ctx->mutate_file_range_candidates(
            test_ctx, NULL, plan, source, 0u, (lql_uint64)strlen(doc), out, 1,
            0, &result, &error);
        if (st != LQL_STATUS_OK) {
          printf("candidate mutation match-all failed: %s\n", error.message);
          ++failures;
        } else if (result.candidates_seen != 2u ||
                   result.candidates_matched != 2u || result.stopped_early) {
          printf("candidate mutation match-all result mismatch: seen=%lu "
                 "matched=%lu stopped=%d\n",
                 (unsigned long)result.candidates_seen,
                 (unsigned long)result.candidates_matched,
                 result.stopped_early);
          ++failures;
        } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
                   strcmp(buf, "{\"id\":\"a\",\"status\":\"done\"}\n"
                               "{\"id\":\"b\",\"status\":\"done\"}\n") != 0) {
          printf("candidate mutation match-all output mismatch: %s\n", buf);
          ++failures;
        }
      }
    }

    {
      FILE *limit_source;
      FILE *limit_out;
      static const char limit_doc[] =
          "{\"id\":\"a\",\"status\":\"open\"}\n"
          "{\"id\":\"b\",\"status\":\"open\"}\n"
          "{\"id\":\"c\",\"status\":\"closed\"}\n";
      limit_source = tmpfile();
      limit_out = tmpfile();
      if (limit_source == NULL || limit_out == NULL) {
        printf("candidate mutation options tmpfile failed\n");
        ++failures;
      } else if (fwrite(limit_doc, 1u, strlen(limit_doc), limit_source) !=
                     strlen(limit_doc) ||
                 fseek(limit_source, 0L, SEEK_SET) != 0) {
        printf("candidate mutation options source setup failed\n");
        ++failures;
      } else {
        memset(&options, 0, sizeof(options));
        options.max_matches = 1u;
        memset(&result, 0, sizeof(result));
        lql_error_init(&error);
        st = test_ctx->mutate_file_range_candidates_with_options(
            test_ctx, selector, plan, limit_source, 0u,
            (lql_uint64)strlen(limit_doc), limit_out, 1, 1, &options, &result,
            &error);
        if (st != LQL_STATUS_OK) {
          printf("candidate mutation options failed: %s\n", error.message);
          ++failures;
        } else if (result.candidates_seen != 1u ||
                   result.candidates_matched != 1u ||
                   !result.stopped_early ||
                   result.stop_reason != LQL_QUERY_STOP_MATCH_LIMIT) {
          printf("candidate mutation options result mismatch: seen=%lu "
                 "matched=%lu stopped=%d reason=%d\n",
                 (unsigned long)result.candidates_seen,
                 (unsigned long)result.candidates_matched,
                 result.stopped_early, (int)result.stop_reason);
          ++failures;
        } else if (!read_tmpfile(limit_out, buf, sizeof(buf), &len) ||
                   strcmp(buf, "{\"id\":\"a\",\"status\":\"done\"}\n") != 0) {
          printf("candidate mutation options output mismatch: %s\n", buf);
          ++failures;
        }
      }
      if (limit_source != NULL) {
        fclose(limit_source);
      }
      if (limit_out != NULL) {
        fclose(limit_out);
      }
    }

    {
      FILE *nested_source;
      FILE *nested_out;
      static const char nested_doc[] =
          "[{\"id\":\"a\",\"status\":\"open\"},[{\"id\":\"b\",\"status\":"
          "\"open\"}],{\"id\":\"c\",\"status\":\"closed\"}]";
      nested_source = tmpfile();
      nested_out = tmpfile();
      if (nested_source == NULL || nested_out == NULL) {
        printf("candidate mutation nested tmpfile failed\n");
        ++failures;
      } else if (fwrite(nested_doc, 1u, strlen(nested_doc), nested_source) !=
                     strlen(nested_doc) ||
                 fseek(nested_source, 0L, SEEK_SET) != 0) {
        printf("candidate mutation nested source setup failed\n");
        ++failures;
      } else {
        memset(&result, 0, sizeof(result));
        lql_error_init(&error);
        st = test_ctx->mutate_file_range_candidates(
            test_ctx, selector, plan, nested_source, 0u,
            (lql_uint64)strlen(nested_doc), nested_out, 1, 0, &result, &error);
        if (st != LQL_STATUS_OK) {
          printf("candidate mutation nested failed: %s\n", error.message);
          ++failures;
        } else if (result.candidates_seen != 3u ||
                   result.candidates_matched != 2u || result.stopped_early) {
          printf("candidate mutation nested result mismatch: seen=%lu "
                 "matched=%lu stopped=%d\n",
                 (unsigned long)result.candidates_seen,
                 (unsigned long)result.candidates_matched,
                 result.stopped_early);
          ++failures;
        } else if (!read_tmpfile(nested_out, buf, sizeof(buf), &len) ||
                   strcmp(buf, "{\"id\":\"a\",\"status\":\"done\"}\n"
                               "{\"id\":\"b\",\"status\":\"done\"}\n"
                               "{\"id\":\"c\",\"status\":\"closed\"}\n") !=
                       0) {
          printf("candidate mutation nested output mismatch: %s\n", buf);
          ++failures;
        }
      }
      if (nested_source != NULL) {
        fclose(nested_source);
      }
      if (nested_out != NULL) {
        fclose(nested_out);
      }
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  test_ctx->selector_destroy(test_ctx, selector);
  fclose(source);
  if (out != NULL) {
    fclose(out);
  }
}

static void expect_source_candidate_mutation_api(void) {
  FILE *out;
  lql_error error;
  lql_status st;
  lql_selector *selector;
  lql_mutation_plan *plan;
  lql_query_result result;
  lql_query_options options;
  chunk_reader reader;
  fail_after_reader fail_reader;
  const char *expr;
  const char *mutation;
  char buf[512];
  size_t len;
  static const char doc[] = "[{\"id\":\"a\",\"status\":\"open\"},{\"id\":\"b\","
                            "\"status\":\"closed\"}]";

  out = tmpfile();
  if (out == NULL) {
    printf("source candidate mutation tmpfile failed\n");
    ++failures;
    return;
  }
  selector = NULL;
  plan = NULL;
  expr = "/status=\"open\"";
  mutation = "/status=done";
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("source candidate mutation selector parse failed: %s\n",
           error.message);
    ++failures;
  }
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, &mutation, 1u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("source candidate mutation plan parse failed: %s\n", error.message);
    ++failures;
  }
  if (selector != NULL && plan != NULL) {
    memset(&reader, 0, sizeof(reader));
    reader.data = doc;
    reader.len = strlen(doc);
    reader.chunk_size = 5u;
    memset(&result, 0, sizeof(result));
    lql_error_init(&error);
    st =
        test_ctx->mutate_source_candidates(test_ctx, selector, plan, read_chunk,
                                           &reader, out, 1, 0, &result, &error);
    if (st != LQL_STATUS_OK) {
      printf("source candidate mutation failed: %s\n", error.message);
      ++failures;
    } else if (reader.calls <= 1) {
      printf("source candidate mutation did not consume fragmented reads\n");
      ++failures;
    } else if (result.candidates_seen != 2u ||
               result.candidates_matched != 1u || result.stopped_early ||
               result.bytes_read == 0u) {
      printf("source candidate mutation result mismatch: seen=%lu matched=%lu "
             "stopped=%d bytes=%lu\n",
             (unsigned long)result.candidates_seen,
             (unsigned long)result.candidates_matched, result.stopped_early,
             (unsigned long)result.bytes_read);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf, "{\"id\":\"a\",\"status\":\"done\"}\n"
                           "{\"id\":\"b\",\"status\":\"closed\"}\n") != 0) {
      printf("source candidate mutation output mismatch: %s\n", buf);
      ++failures;
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source candidate mutation options tmpfile failed\n");
      ++failures;
    } else {
      static const char limit_doc[] =
          "{\"event\":\"tabs_update\",\"id\":1}\n"
          "{\"event\":\"tabs_update\",\"id\":2}\n"
          "{\"event\":\"noop\",\"id\":3}\n";
      lql_selector *limit_selector;
      limit_selector = NULL;
      lql_error_init(&error);
      st = test_ctx->selector_parse(test_ctx, "/event=\"tabs_update\"",
                                    &limit_selector, &error);
      if (st != LQL_STATUS_OK) {
        printf("source candidate mutation options selector failed: %s\n",
               error.message);
        ++failures;
      } else {
        memset(&reader, 0, sizeof(reader));
        reader.data = limit_doc;
        reader.len = strlen(limit_doc);
        reader.chunk_size = 5u;
        memset(&options, 0, sizeof(options));
        options.max_matches = 1u;
        memset(&result, 0, sizeof(result));
        lql_error_init(&error);
        st = test_ctx->mutate_source_candidates_with_options(
            test_ctx, limit_selector, plan, read_chunk, &reader, out, 1, 1,
            &options, &result, &error);
        if (st != LQL_STATUS_OK) {
          printf("source candidate mutation options failed: %s\n",
                 error.message);
          ++failures;
        } else if (result.candidates_seen != 1u ||
                   result.candidates_matched != 1u ||
                   !result.stopped_early ||
                   result.stop_reason != LQL_QUERY_STOP_MATCH_LIMIT ||
                   reader.calls <= 1) {
          printf("source candidate mutation options result mismatch: seen=%lu "
                 "matched=%lu stopped=%d reason=%d reads=%d\n",
                 (unsigned long)result.candidates_seen,
                 (unsigned long)result.candidates_matched,
                 result.stopped_early, (int)result.stop_reason, reader.calls);
          ++failures;
        } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
                   strcmp(buf, "{\"event\":\"tabs_update\",\"id\":1,"
                               "\"status\":\"done\"}\n") != 0) {
          printf("source candidate mutation options output mismatch: %s\n", buf);
          ++failures;
        }
      }
      test_ctx->selector_destroy(test_ctx, limit_selector);
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source candidate mutation matches-only tmpfile failed\n");
      ++failures;
    } else {
      memset(&reader, 0, sizeof(reader));
      reader.data = doc;
      reader.len = strlen(doc);
      reader.chunk_size = 4u;
      memset(&result, 0, sizeof(result));
      lql_error_init(&error);
      st = test_ctx->mutate_source_candidates(test_ctx, selector, plan,
                                              read_chunk, &reader, out, 1, 1,
                                              &result, &error);
      if (st != LQL_STATUS_OK) {
        printf("source candidate mutation matches-only failed: %s\n",
               error.message);
        ++failures;
      } else if (result.candidates_seen != 2u ||
                 result.candidates_matched != 1u || result.stopped_early) {
        printf("source candidate mutation matches-only result mismatch\n");
        ++failures;
      } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
                 strcmp(buf, "{\"id\":\"a\",\"status\":\"done\"}\n") != 0) {
        printf("source candidate mutation matches-only output mismatch: %s\n",
               buf);
        ++failures;
      }
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source candidate mutation query-mutate tmpfile failed\n");
      ++failures;
    } else {
      lql_selector *event_selector;
      lql_mutation_plan *event_plan;
      const char *event_mutations[2];
      static const char event_doc[] =
          "{\"event\":\"tabs_update\",\"component\":\"host\",\"id\":1}\n"
          "{\"event\":\"noop\",\"component\":\"host\",\"id\":2}\n"
          "{\"event\":\"tabs_update\",\"component\":\"host\",\"id\":3}\n"
          "{\"event\":\"tabs_update\",\"component\":\"host\",\"id\":4}";
      event_selector = NULL;
      event_plan = NULL;
      event_mutations[0] = "/processed=true";
      event_mutations[1] = "time:/processed_at=2023-11-14T22:13:20Z";
      lql_error_init(&error);
      st = test_ctx->selector_parse(test_ctx, "/event=\"tabs_update\"",
                                    &event_selector, &error);
      if (st != LQL_STATUS_OK) {
        printf("source candidate mutation query-mutate selector failed: %s\n",
               error.message);
        ++failures;
      }
      lql_error_init(&error);
      st = test_ctx->mutation_plan_parse(test_ctx, event_mutations, 2u,
                                         &event_plan, &error);
      if (st != LQL_STATUS_OK) {
        printf("source candidate mutation query-mutate plan failed: %s\n",
               error.message);
        ++failures;
      }
      if (event_selector != NULL && event_plan != NULL) {
        memset(&reader, 0, sizeof(reader));
        reader.data = event_doc;
        reader.len = strlen(event_doc);
        reader.chunk_size = 7u;
        memset(&result, 0, sizeof(result));
        lql_error_init(&error);
        st = test_ctx->mutate_source_candidates(
            test_ctx, event_selector, event_plan, read_chunk, &reader, out, 1,
            1, &result, &error);
        if (st != LQL_STATUS_OK) {
          printf("source candidate mutation query-mutate failed: %s\n",
                 error.message);
          ++failures;
        } else if (reader.calls <= 1 || result.candidates_seen != 4u ||
                   result.candidates_matched != 3u ||
                   result.stopped_early || result.bytes_read == 0u) {
          printf("source candidate mutation query-mutate result mismatch: "
                 "seen=%lu matched=%lu stopped=%d bytes=%lu reads=%d\n",
                 (unsigned long)result.candidates_seen,
                 (unsigned long)result.candidates_matched,
                 result.stopped_early, (unsigned long)result.bytes_read,
                 reader.calls);
          ++failures;
        } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
                   strcmp(buf,
                          "{\"event\":\"tabs_update\",\"component\":\"host\","
                          "\"id\":1,\"processed\":true,\"processed_at\":"
                          "\"2023-11-14T22:13:20Z\"}\n"
                          "{\"event\":\"tabs_update\",\"component\":\"host\","
                          "\"id\":3,\"processed\":true,\"processed_at\":"
                          "\"2023-11-14T22:13:20Z\"}\n"
                          "{\"event\":\"tabs_update\",\"component\":\"host\","
                          "\"id\":4,\"processed\":true,\"processed_at\":"
                          "\"2023-11-14T22:13:20Z\"}\n") != 0) {
          printf("source candidate mutation query-mutate output mismatch: %s\n",
                 buf);
          ++failures;
        }
      }
      test_ctx->mutation_plan_destroy(test_ctx, event_plan);
      test_ctx->selector_destroy(test_ctx, event_selector);
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source candidate mutation lockd handoff tmpfile failed\n");
      ++failures;
    } else {
      lql_selector *lockd_selector;
      lql_mutation_plan *lockd_plan;
      const char *lockd_mutations[2];
      char lockd_doc[7200];
      size_t pos;
      unsigned int processed_count;
      char *cursor;
      lockd_selector = NULL;
      lockd_plan = NULL;
      lockd_mutations[0] = "/processed=true";
      lockd_mutations[1] = "time:/processed_at=2023-11-14T22:13:20Z";
      pos = 0u;
      lockd_doc[0] = '\0';
      if (!append_literal(lockd_doc, sizeof(lockd_doc),
                          &pos,
                          "{\"event\":\"tabs_update\",\"component\":\"host\","
                          "\"blob\":\"") ||
          !append_repeated(lockd_doc, sizeof(lockd_doc), &pos, 'x', 2048u) ||
          !append_literal(lockd_doc, sizeof(lockd_doc),
                          &pos,
                          "\"}\n{\"event\":\"noop\",\"component\":\"host\","
                          "\"blob\":\"") ||
          !append_repeated(lockd_doc, sizeof(lockd_doc), &pos, 'y', 1024u) ||
          !append_literal(lockd_doc, sizeof(lockd_doc),
                          &pos,
                          "\"}\n{\"event\":\"tabs_update\",\"component\":"
                          "\"host\",\"blob\":\"") ||
          !append_repeated(lockd_doc, sizeof(lockd_doc), &pos, 'z', 1536u) ||
          !append_literal(lockd_doc, sizeof(lockd_doc),
                          &pos,
                          "\"}\n{\"event\":\"tabs_update\",\"component\":"
                          "\"host\",\"blob\":\"") ||
          !append_repeated(lockd_doc, sizeof(lockd_doc), &pos, 'w', 1536u) ||
          !append_literal(lockd_doc, sizeof(lockd_doc), &pos, "\"}")) {
        printf("source candidate mutation lockd fixture build failed\n");
        ++failures;
      }
      lql_error_init(&error);
      st = test_ctx->selector_parse(test_ctx, "/event=\"tabs_update\"",
                                    &lockd_selector, &error);
      if (st != LQL_STATUS_OK) {
        printf("source candidate mutation lockd selector failed: %s\n",
               error.message);
        ++failures;
      }
      lql_error_init(&error);
      st = test_ctx->mutation_plan_parse(test_ctx, lockd_mutations, 2u,
                                         &lockd_plan, &error);
      if (st != LQL_STATUS_OK) {
        printf("source candidate mutation lockd plan failed: %s\n",
               error.message);
        ++failures;
      }
      if (lockd_selector != NULL && lockd_plan != NULL && pos > 0u) {
        memset(&reader, 0, sizeof(reader));
        reader.data = lockd_doc;
        reader.len = pos;
        reader.chunk_size = 13u;
        memset(&options, 0, sizeof(options));
        options.max_matches = 2u;
        memset(&result, 0, sizeof(result));
        lql_error_init(&error);
        st = test_ctx->mutate_source_candidates_with_options(
            test_ctx, lockd_selector, lockd_plan, read_chunk, &reader, out, 1,
            1, &options, &result, &error);
        if (st != LQL_STATUS_OK) {
          printf("source candidate mutation lockd handoff failed: %s\n",
                 error.message);
          ++failures;
        } else if (reader.calls <= 1 || result.candidates_seen != 3u ||
                   result.candidates_matched != 2u ||
                   !result.stopped_early ||
                   result.stop_reason != LQL_QUERY_STOP_MATCH_LIMIT ||
                   result.bytes_read == 0u) {
          printf("source candidate mutation lockd result mismatch: seen=%lu "
                 "matched=%lu stopped=%d reason=%d bytes=%lu reads=%d\n",
                 (unsigned long)result.candidates_seen,
                 (unsigned long)result.candidates_matched,
                 result.stopped_early, (int)result.stop_reason,
                 (unsigned long)result.bytes_read, reader.calls);
          ++failures;
        } else if (!read_tmpfile(out, lockd_doc, sizeof(lockd_doc), &len)) {
          printf("source candidate mutation lockd output read failed\n");
          ++failures;
        } else {
          processed_count = 0u;
          cursor = lockd_doc;
          while ((cursor = strstr(cursor, "\"processed\":true")) != NULL) {
            ++processed_count;
            cursor += strlen("\"processed\":true");
          }
          if (processed_count != 2u ||
              strstr(lockd_doc, "\"event\":\"noop\"") != NULL ||
              strstr(lockd_doc, "\"processed_at\":"
                               "\"2023-11-14T22:13:20Z\"") == NULL ||
              strstr(lockd_doc, "\"blob\":\"xxxxxxxx") == NULL ||
              strstr(lockd_doc, "\"blob\":\"zzzzzzzz") == NULL ||
              strstr(lockd_doc, "\"blob\":\"wwwwwwww") != NULL) {
            printf("source candidate mutation lockd output mismatch\n");
            ++failures;
          }
        }
      }
      test_ctx->mutation_plan_destroy(test_ctx, lockd_plan);
      test_ctx->selector_destroy(test_ctx, lockd_selector);
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source candidate mutation match-all tmpfile failed\n");
      ++failures;
    } else {
      memset(&reader, 0, sizeof(reader));
      reader.data = doc;
      reader.len = strlen(doc);
      reader.chunk_size = 3u;
      memset(&result, 0, sizeof(result));
      lql_error_init(&error);
      st = test_ctx->mutate_source_candidates(test_ctx, NULL, plan, read_chunk,
                                              &reader, out, 1, 0, &result,
                                              &error);
      if (st != LQL_STATUS_OK) {
        printf("source candidate mutation match-all failed: %s\n",
               error.message);
        ++failures;
      } else if (result.candidates_seen != 2u ||
                 result.candidates_matched != 2u || result.stopped_early) {
        printf("source candidate mutation match-all result mismatch: seen=%lu "
               "matched=%lu stopped=%d\n",
               (unsigned long)result.candidates_seen,
               (unsigned long)result.candidates_matched,
               result.stopped_early);
        ++failures;
      } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
                 strcmp(buf, "{\"id\":\"a\",\"status\":\"done\"}\n"
                             "{\"id\":\"b\",\"status\":\"done\"}\n") != 0) {
        printf("source candidate mutation match-all output mismatch: %s\n",
               buf);
        ++failures;
      }
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source candidate mutation nested tmpfile failed\n");
      ++failures;
    } else {
      static const char nested_doc[] =
          "[{\"id\":\"a\",\"status\":\"open\"},[{\"id\":\"b\",\"status\":"
          "\"open\"}],{\"id\":\"c\",\"status\":\"closed\"}]";
      memset(&reader, 0, sizeof(reader));
      reader.data = nested_doc;
      reader.len = strlen(nested_doc);
      reader.chunk_size = 6u;
      memset(&result, 0, sizeof(result));
      lql_error_init(&error);
      st = test_ctx->mutate_source_candidates(test_ctx, selector, plan,
                                              read_chunk, &reader, out, 1, 0,
                                              &result, &error);
      if (st != LQL_STATUS_OK) {
        printf("source candidate mutation nested failed: %s\n",
               error.message);
        ++failures;
      } else if (reader.calls <= 1) {
        printf("source candidate mutation nested did not fragment reads\n");
        ++failures;
      } else if (result.candidates_seen != 3u ||
                 result.candidates_matched != 2u || result.stopped_early) {
        printf("source candidate mutation nested result mismatch: seen=%lu "
               "matched=%lu stopped=%d\n",
               (unsigned long)result.candidates_seen,
               (unsigned long)result.candidates_matched,
               result.stopped_early);
        ++failures;
      } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
                 strcmp(buf, "{\"id\":\"a\",\"status\":\"done\"}\n"
                             "{\"id\":\"b\",\"status\":\"done\"}\n"
                             "{\"id\":\"c\",\"status\":\"closed\"}\n") != 0) {
        printf("source candidate mutation nested output mismatch: %s\n", buf);
        ++failures;
      }
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source candidate mutation mixed tmpfile failed\n");
      ++failures;
    } else {
      static const char mixed_doc[] =
          "[{\"id\":\"a\",\"status\":\"open\"},7,[{\"id\":\"b\",\"status\":"
          "\"open\"}],true]";
      memset(&reader, 0, sizeof(reader));
      reader.data = mixed_doc;
      reader.len = strlen(mixed_doc);
      reader.chunk_size = 5u;
      memset(&result, 0, sizeof(result));
      lql_error_init(&error);
      st = test_ctx->mutate_source_candidates(test_ctx, NULL, plan, read_chunk,
                                              &reader, out, 1, 0, &result,
                                              &error);
      if (st != LQL_STATUS_OK) {
        printf("source candidate mutation mixed failed: %s\n", error.message);
        ++failures;
      } else if (reader.calls <= 1) {
        printf("source candidate mutation mixed did not fragment reads\n");
        ++failures;
      } else if (result.candidates_seen != 4u ||
                 result.candidates_matched != 4u || result.stopped_early) {
        printf("source candidate mutation mixed result mismatch: seen=%lu "
               "matched=%lu stopped=%d\n",
               (unsigned long)result.candidates_seen,
               (unsigned long)result.candidates_matched, result.stopped_early);
        ++failures;
      } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
                 strcmp(buf, "{\"id\":\"a\",\"status\":\"done\"}\n"
                             "7\n"
                             "{\"id\":\"b\",\"status\":\"done\"}\n"
                             "true\n") != 0) {
        printf("source candidate mutation mixed output mismatch: %s\n", buf);
        ++failures;
      }
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source candidate mutation read-fail tmpfile failed\n");
      ++failures;
    } else {
      lql_error_init(&error);
      st = test_ctx->mutate_source_candidates(test_ctx, selector, plan,
                                              read_fail_once, NULL, out, 1, 0,
                                              NULL, &error);
      if (st != LQL_STATUS_JSON_ERROR ||
          strcmp(error.message, "source read failed") != 0) {
        printf("source candidate mutation read error mismatch: %s\n",
               error.message);
        ++failures;
      } else if (!read_tmpfile(out, buf, sizeof(buf), &len) || len != 0u) {
        printf("source candidate mutation read error wrote output: %s\n", buf);
        ++failures;
      }
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source candidate mutation over-capacity tmpfile failed\n");
      ++failures;
    } else {
      memset(&result, 0x5a, sizeof(result));
      lql_error_init(&error);
      st = test_ctx->mutate_source_candidates(test_ctx, selector, plan,
                                              read_over_capacity, NULL, out, 1,
                                              0, &result, &error);
      if (st != LQL_STATUS_JSON_ERROR ||
          strcmp(error.message, "source read failed") != 0 ||
          !query_result_is_zero(&result)) {
        printf("source candidate mutation over-capacity mismatch: status=%s "
               "seen=%lu matched=%lu bytes=%lu error=%s\n",
               lql_status_string(st), (unsigned long)result.candidates_seen,
               (unsigned long)result.candidates_matched,
               (unsigned long)result.bytes_read, error.message);
        ++failures;
      } else if (!read_tmpfile(out, buf, sizeof(buf), &len) || len != 0u) {
        printf("source candidate mutation over-capacity wrote output: %s\n",
               buf);
        ++failures;
      }
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source candidate mutation partial-fail tmpfile failed\n");
      ++failures;
    } else {
      memset(&fail_reader, 0, sizeof(fail_reader));
      memset(&result, 0, sizeof(result));
      fail_reader.data = doc;
      fail_reader.len = strlen(doc);
      fail_reader.chunk_size = 6u;
      fail_reader.fail_offset = strlen("{\"id\":\"a\",\"status\":\"open\"},");
      lql_error_init(&error);
      st = test_ctx->mutate_source_candidates(
          test_ctx, selector, plan, read_until_offset_then_fail, &fail_reader,
          out, 1, 0, &result, &error);
      if (st != LQL_STATUS_JSON_ERROR ||
          strcmp(error.message, "source read failed") != 0 ||
          result.candidates_seen != 1u || result.candidates_matched != 1u ||
          result.bytes_read !=
              (lql_uint64)strlen("{\"id\":\"a\",\"status\":\"open\"},") ||
          fail_reader.calls <= 1) {
        printf("source candidate mutation partial read error mismatch: "
               "status=%s seen=%lu matched=%lu bytes=%lu reads=%d error=%s\n",
               lql_status_string(st), (unsigned long)result.candidates_seen,
               (unsigned long)result.candidates_matched,
               (unsigned long)result.bytes_read, fail_reader.calls,
               error.message);
        ++failures;
      } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
                 strcmp(buf, "{\"id\":\"a\",\"status\":\"done\"}\n") != 0) {
        printf("source candidate mutation partial read output mismatch: %s\n",
               buf);
        ++failures;
      }
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  test_ctx->selector_destroy(test_ctx, selector);
  if (out != NULL) {
    fclose(out);
  }
}

static void expect_projected_candidate_mutation_api(void) {
  FILE *source;
  FILE *out;
  lql_error error;
  lql_status st;
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation_plan *plan;
  lql_query_result result;
  lql_query_options options;
  chunk_reader reader;
  const char *selector_expr;
  const char *fields[2];
  const char *mutation;
  char buf[512];
  size_t len;
  static const char doc[] =
      "[{\"id\":\"a\",\"status\":\"open\",\"state\":{\"count\":1},"
      "\"drop\":true},{\"id\":\"b\",\"status\":\"closed\",\"state\":{\"count\":"
      "2},\"drop\":true}]";

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("projected candidate mutation tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite(doc, 1u, strlen(doc), source) != strlen(doc) ||
      fflush(source) != 0 || fseek(source, 0L, SEEK_SET) != 0) {
    printf("projected candidate mutation source setup failed\n");
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }

  selector = NULL;
  projection = NULL;
  plan = NULL;
  selector_expr = "/status=\"open\"";
  fields[0] = "/id";
  fields[1] = "/state";
  mutation = "/state/count++";
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, selector_expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("projected candidate selector parse failed: %s\n", error.message);
    ++failures;
  }
  lql_error_init(&error);
  st = test_ctx->projection_parse(test_ctx, fields, 2u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("projected candidate projection parse failed: %s\n", error.message);
    ++failures;
  }
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, &mutation, 1u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("projected candidate mutation parse failed: %s\n", error.message);
    ++failures;
  }

  if (selector != NULL && projection != NULL && plan != NULL) {
    memset(&result, 0, sizeof(result));
    lql_error_init(&error);
    st = test_ctx->mutate_file_range_projected_candidates(
        test_ctx, selector, projection, plan, source, 0u,
        (lql_uint64)strlen(doc), out, 1, 0, &result, &error);
    if (st != LQL_STATUS_OK) {
      printf("file projected candidate mutation failed: %s\n", error.message);
      ++failures;
    } else if (result.candidates_seen != 2u ||
               result.candidates_matched != 1u || result.stopped_early ||
               result.bytes_read == 0u) {
      printf("file projected candidate result mismatch: seen=%lu matched=%lu "
             "stopped=%d bytes=%lu\n",
             (unsigned long)result.candidates_seen,
             (unsigned long)result.candidates_matched, result.stopped_early,
             (unsigned long)result.bytes_read);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf, "{\"id\":\"a\",\"state\":{\"count\":2}}\n"
                           "{\"id\":\"b\",\"state\":{\"count\":2}}\n") != 0) {
      printf("file projected candidate output mismatch: %s\n", buf);
      ++failures;
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("file projected candidate matches-only tmpfile failed\n");
      ++failures;
    } else {
      if (fseek(source, 0L, SEEK_SET) != 0) {
        printf("file projected candidate matches-only rewind failed\n");
        ++failures;
      } else {
        memset(&result, 0, sizeof(result));
        memset(&options, 0, sizeof(options));
        options.max_matches = 1u;
        lql_error_init(&error);
        st = test_ctx->mutate_file_range_projected_candidates_with_options(
            test_ctx, selector, projection, plan, source, 0u,
            (lql_uint64)strlen(doc), out, 1, 1, &options, &result, &error);
        if (st != LQL_STATUS_OK) {
          printf("file projected candidate matches-only mutation failed: %s\n",
                 error.message);
          ++failures;
        } else if (result.candidates_seen != 1u ||
                   result.candidates_matched != 1u ||
                   !result.stopped_early ||
                   result.stop_reason != LQL_QUERY_STOP_MATCH_LIMIT ||
                   result.bytes_read == 0u) {
          printf("file projected candidate matches-only result mismatch: "
                 "seen=%lu matched=%lu stopped=%d bytes=%lu\n",
                 (unsigned long)result.candidates_seen,
                 (unsigned long)result.candidates_matched,
                 result.stopped_early, (unsigned long)result.bytes_read);
          ++failures;
        } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
                   strcmp(buf, "{\"id\":\"a\",\"state\":{\"count\":2}}\n") !=
                       0) {
          printf("file projected candidate matches-only output mismatch: %s\n",
                 buf);
          ++failures;
        }
      }
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source projected candidate preserve tmpfile failed\n");
      ++failures;
    } else {
      memset(&reader, 0, sizeof(reader));
      memset(&result, 0, sizeof(result));
      reader.data = doc;
      reader.len = strlen(doc);
      reader.chunk_size = 7u;
      lql_error_init(&error);
      st = test_ctx->mutate_source_projected_candidates(
          test_ctx, selector, projection, plan, read_chunk, &reader, out, 1, 0,
          &result, &error);
      if (st != LQL_STATUS_OK) {
        printf("source projected candidate preserve mutation failed: %s\n",
               error.message);
        ++failures;
      } else if (reader.calls <= 1) {
        printf("source projected candidate preserve did not fragment reads\n");
        ++failures;
      } else if (result.candidates_seen != 2u ||
                 result.candidates_matched != 1u || result.stopped_early) {
        printf("source projected candidate preserve result mismatch: seen=%lu "
               "matched=%lu stopped=%d\n",
               (unsigned long)result.candidates_seen,
               (unsigned long)result.candidates_matched, result.stopped_early);
        ++failures;
      } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
                 strcmp(buf, "{\"id\":\"a\",\"state\":{\"count\":2}}\n"
                             "{\"id\":\"b\",\"state\":{\"count\":2}}\n") != 0) {
        printf("source projected candidate preserve output mismatch: %s\n",
               buf);
        ++failures;
      }
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source projected candidate tmpfile failed\n");
      ++failures;
    } else {
      memset(&reader, 0, sizeof(reader));
      memset(&result, 0, sizeof(result));
      memset(&options, 0, sizeof(options));
      options.max_matches = 1u;
      reader.data = doc;
      reader.len = strlen(doc);
      reader.chunk_size = 7u;
      lql_error_init(&error);
      st = test_ctx->mutate_source_projected_candidates_with_options(
          test_ctx, selector, projection, plan, read_chunk, &reader, out, 1, 1,
          &options, &result, &error);
      if (st != LQL_STATUS_OK) {
        printf("source projected candidate mutation failed: %s\n",
               error.message);
        ++failures;
      } else if (reader.calls <= 1) {
        printf("source projected candidate mutation did not fragment reads\n");
        ++failures;
      } else if (result.candidates_seen != 1u ||
                 result.candidates_matched != 1u || !result.stopped_early ||
                 result.stop_reason != LQL_QUERY_STOP_MATCH_LIMIT) {
        printf("source projected candidate result mismatch: seen=%lu "
               "matched=%lu stopped=%d\n",
               (unsigned long)result.candidates_seen,
               (unsigned long)result.candidates_matched, result.stopped_early);
        ++failures;
      } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
                 strcmp(buf, "{\"id\":\"a\",\"state\":{\"count\":2}}\n") != 0) {
        printf("source projected candidate output mismatch: %s\n", buf);
        ++failures;
      }
    }

    fclose(out);
    out = tmpfile();
    if (out == NULL) {
      printf("source projected candidate over-capacity tmpfile failed\n");
      ++failures;
    } else {
      memset(&result, 0x5a, sizeof(result));
      lql_error_init(&error);
      st = test_ctx->mutate_source_projected_candidates(
          test_ctx, selector, projection, plan, read_over_capacity, NULL, out,
          1, 0, &result, &error);
      if (st != LQL_STATUS_JSON_ERROR ||
          strcmp(error.message, "source read failed") != 0 ||
          !query_result_is_zero(&result)) {
        printf("source projected candidate over-capacity mismatch: status=%s "
               "seen=%lu matched=%lu bytes=%lu error=%s\n",
               lql_status_string(st), (unsigned long)result.candidates_seen,
               (unsigned long)result.candidates_matched,
               (unsigned long)result.bytes_read, error.message);
        ++failures;
      } else if (!read_tmpfile(out, buf, sizeof(buf), &len) || len != 0u) {
        printf("source projected candidate over-capacity wrote output: %s\n",
               buf);
        ++failures;
      }
    }

    memset(&result, 0x5a, sizeof(result));
    lql_error_init(&error);
    st = test_ctx->mutate_file_range_projected_candidates(
        test_ctx, selector, NULL, plan, source, 0u, (lql_uint64)strlen(doc),
        out, 1, 0, &result, &error);
    if (st != LQL_STATUS_INVALID_ARGUMENT ||
        strcmp(error.message, "projection, plan, file, and out are required") !=
            0 ||
        !query_result_is_zero(&result)) {
      printf("projected candidate NULL projection mismatch: %s\n",
             error.message);
      ++failures;
    }
  }

  test_ctx->mutation_plan_destroy(test_ctx, plan);
  test_ctx->projection_destroy(test_ctx, projection);
  test_ctx->selector_destroy(test_ctx, selector);
  fclose(source);
  if (out != NULL) {
    fclose(out);
  }
}

static void expect_mutation_quoted_value_api(void) {
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *exprs[4];
  char buf[512];
  size_t len;
  static const char doc[] = "{\"state\":{\"status\":\"open\"}}";

  out = tmpfile();
  if (out == NULL) {
    printf("quoted mutation tmpfile failed\n");
    ++failures;
    return;
  }
  exprs[0] = "/state/text=\"a\\\"b\\\\c\"";
  exprs[1] = "/state/truth=\"true\"";
  exprs[2] = "/state/nothing=\"null\"";
  exprs[3] = "/state/number=\"2\"";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 4u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("quoted mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_json(test_ctx, plan, doc, strlen(doc), out, &error);
    if (st != LQL_STATUS_OK) {
      printf("quoted mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"state\":{\"status\":\"open\",\"text\":\"a\\\\\\\"b"
                      "\\\\\\\\c\",\"truth\":true,\"nothing\":null,"
                      "\"number\":2}}") != 0) {
      printf("quoted mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  fclose(out);
}

static void expect_mutation_file_backed_value_api(void) {
  FILE *payload;
  FILE *source;
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  lql_mutation_parse_options options;
  const char *exprs[4];
  chunk_reader reader;
  char buf[512];
  size_t len;
  static const char expected[] =
      "{\"payload\":\"hello\\n\\\"quoted\\\"\",\"encoded\":\"AAECYQ==\","
      "\"auto_text\":\"hello\\n\\\"quoted\\\"\",\"auto_bin\":\"AAECYQ==\"}";

  payload = fopen("lql-test-sdk-file-backed.txt", "wb");
  if (payload == NULL) {
    printf("SDK file-backed text payload open failed\n");
    ++failures;
    return;
  }
  if (fwrite("hello\n\"quoted\"", 1u, strlen("hello\n\"quoted\""), payload) !=
          strlen("hello\n\"quoted\"") ||
      fclose(payload) != 0) {
    printf("SDK file-backed text payload write failed\n");
    ++failures;
    return;
  }
  payload = fopen("lql-test-sdk-file-backed.bin", "wb");
  if (payload == NULL) {
    printf("SDK file-backed binary payload open failed\n");
    remove("lql-test-sdk-file-backed.txt");
    ++failures;
    return;
  }
  if (fwrite("\000\001\002a", 1u, 4u, payload) != 4u || fclose(payload) != 0) {
    printf("SDK file-backed binary payload write failed\n");
    remove("lql-test-sdk-file-backed.txt");
    ++failures;
    return;
  }

  exprs[0] = "textfile:/payload=lql-test-sdk-file-backed.txt";
  exprs[1] = "base64file:/encoded=lql-test-sdk-file-backed.bin";
  exprs[2] = "file:/auto_text=lql-test-sdk-file-backed.txt";
  exprs[3] = "file:/auto_bin=lql-test-sdk-file-backed.bin";
  memset(&options, 0, sizeof(options));
  options.enable_file_values = 1;
  options.file_value_base_dir = ".";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse_with_options(test_ctx, exprs, 4u, &options,
                                                  &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("SDK file-backed mutation parse failed: %s\n", error.message);
    ++failures;
    remove("lql-test-sdk-file-backed.txt");
    remove("lql-test-sdk-file-backed.bin");
    return;
  }

  out = tmpfile();
  if (out == NULL) {
    printf("SDK file-backed buffered output tmpfile failed\n");
    ++failures;
  } else {
    st = test_ctx->mutate_json(test_ctx, plan, "{}", strlen("{}"), out, &error);
    if (st != LQL_STATUS_OK) {
      printf("SDK file-backed buffered mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf, expected) != 0) {
      printf("SDK file-backed buffered output mismatch: %s\n", buf);
      ++failures;
    }
    fclose(out);
  }

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("SDK file-backed range tmpfile failed\n");
    ++failures;
  } else if (fwrite("{}", 1u, strlen("{}"), source) != strlen("{}") ||
             fflush(source) != 0 || fseek(source, 0L, SEEK_SET) != 0) {
    printf("SDK file-backed range source setup failed\n");
    ++failures;
  } else {
    st = test_ctx->mutate_file_range_paths(
        test_ctx, plan, source, 0u, (lql_uint64)strlen("{}"), out, &error);
    if (st != LQL_STATUS_OK) {
      printf("SDK file-backed range mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf, expected) != 0) {
      printf("SDK file-backed range output mismatch: %s\n", buf);
      ++failures;
    }
  }
  if (source != NULL) {
    fclose(source);
  }
  if (out != NULL) {
    fclose(out);
  }

  out = tmpfile();
  if (out == NULL) {
    printf("SDK file-backed source output tmpfile failed\n");
    ++failures;
  } else {
    memset(&reader, 0, sizeof(reader));
    reader.data = "{}";
    reader.len = strlen("{}");
    reader.chunk_size = 1u;
    lql_error_init(&error);
    st = test_ctx->mutate_source_paths(test_ctx, plan, read_chunk, &reader, out,
                                       &error);
    if (st != LQL_STATUS_OK) {
      printf("SDK file-backed source mutation failed: %s\n", error.message);
      ++failures;
    } else if (reader.calls <= 1) {
      printf("SDK file-backed source mutation did not consume fragmented "
             "reads\n");
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf, expected) != 0) {
      printf("SDK file-backed source output mismatch: %s\n", buf);
      ++failures;
    }
    fclose(out);
    out = NULL;
  }

  test_ctx->mutation_plan_destroy(test_ctx, plan);
  remove("lql-test-sdk-file-backed.txt");
  remove("lql-test-sdk-file-backed.bin");
}

static int write_bytes_file(const char *path, const unsigned char *data,
                            size_t len) {
  FILE *file;
  file = fopen(path, "wb");
  if (file == NULL) {
    return 0;
  }
  if (fwrite(data, 1u, len, file) != len || fclose(file) != 0) {
    remove(path);
    return 0;
  }
  return 1;
}

static void expect_mutation_file_backed_text_validation_api(void) {
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  lql_mutation_parse_options options;
  const char *expr;
  static const unsigned char invalid_utf8[] = {'h', 'i', 0xffu};
  static const unsigned char nul_text[] = {'h', 'i', 0u, 'x'};

  if (!write_bytes_file("lql-test-invalid-utf8.txt", invalid_utf8,
                        sizeof(invalid_utf8)) ||
      !write_bytes_file("lql-test-nul-text.txt", nul_text, sizeof(nul_text))) {
    printf("file-backed text validation payload write failed\n");
    ++failures;
    remove("lql-test-invalid-utf8.txt");
    remove("lql-test-nul-text.txt");
    return;
  }
  memset(&options, 0, sizeof(options));
  options.enable_file_values = 1;
  options.file_value_base_dir = ".";

  expr = "textfile:/payload=lql-test-invalid-utf8.txt";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse_with_options(test_ctx, &expr, 1u,
                                                  &options, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("invalid UTF-8 textfile mutation parse failed: %s\n",
           error.message);
    ++failures;
  } else {
    out = tmpfile();
    if (out == NULL) {
      printf("invalid UTF-8 textfile mutation tmpfile failed\n");
      ++failures;
    } else {
      lql_error_init(&error);
      st = test_ctx->mutate_json(test_ctx, plan, "{}", strlen("{}"), out,
                                 &error);
      if (st == LQL_STATUS_OK || strstr(error.message, "UTF") == NULL) {
        printf("invalid UTF-8 textfile mutation mismatch: status=%s error=%s\n",
               lql_status_string(st), error.message);
        ++failures;
      }
      fclose(out);
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);

  expr = "textfile:/payload=lql-test-nul-text.txt";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse_with_options(test_ctx, &expr, 1u,
                                                  &options, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("NUL textfile mutation parse failed: %s\n", error.message);
    ++failures;
  } else {
    out = tmpfile();
    if (out == NULL) {
      printf("NUL textfile mutation tmpfile failed\n");
      ++failures;
    } else {
      lql_error_init(&error);
      st = test_ctx->mutate_json(test_ctx, plan, "{}", strlen("{}"), out,
                                 &error);
      if (st == LQL_STATUS_OK || strstr(error.message, "NUL") == NULL) {
        printf("NUL textfile mutation mismatch: status=%s error=%s\n",
               lql_status_string(st), error.message);
        ++failures;
      }
      fclose(out);
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  remove("lql-test-invalid-utf8.txt");
  remove("lql-test-nul-text.txt");
}

static void expect_mutation_shorthand_api(void) {
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *brace_exprs[2];
  const char *escaped_exprs[3];
  char buf[512];
  size_t len;
  static const char brace_doc[] =
      "{\"state\":{\"status\":\"open\",\"count\":1,\"old\":true,\"owner\":"
      "\"bob\"},\"id\":\"a\"}";
  static const char escaped_doc[] =
      "{\"a/b\":{\"~key\":\"old\",\"remove\":true},\"plain\":\"keep\"}";

  out = tmpfile();
  if (out == NULL) {
    printf("mutation shorthand tmpfile failed\n");
    ++failures;
    return;
  }

  brace_exprs[0] = "/state{/status=done,/count=+2,rm:/old,/owner=\"alice\"}";
  brace_exprs[1] = "/audit{/created=true,/nested/score=3}";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, brace_exprs, 2u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("brace mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else if (test_ctx->mutation_plan_count(test_ctx, plan) != 6u) {
    printf("brace mutation expansion count mismatch: %lu\n",
           (unsigned long)test_ctx->mutation_plan_count(test_ctx, plan));
    ++failures;
  } else {
    st = test_ctx->mutate_json(test_ctx, plan, brace_doc, strlen(brace_doc),
                               out, &error);
    if (st != LQL_STATUS_OK) {
      printf("brace mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"state\":{\"status\":\"done\",\"count\":3,\"owner\":"
                      "\"alice\"},\"id\":\"a\",\"audit\":{\"created\":true,"
                      "\"nested\":{\"score\":3}}}") != 0) {
      printf("brace mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  fclose(out);

  out = tmpfile();
  if (out == NULL) {
    printf("escaped mutation tmpfile failed\n");
    ++failures;
    return;
  }
  escaped_exprs[0] = "/a~1b/~0key=ready";
  escaped_exprs[1] = "rm:/a~1b/remove";
  escaped_exprs[2] = "/a~1b/created=1";
  plan = NULL;
  lql_error_init(&error);
  st =
      test_ctx->mutation_plan_parse(test_ctx, escaped_exprs, 3u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("escaped mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_json(test_ctx, plan, escaped_doc, strlen(escaped_doc),
                               out, &error);
    if (st != LQL_STATUS_OK) {
      printf("escaped mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"a/b\":{\"~key\":\"ready\",\"created\":1},\"plain\":"
                      "\"keep\"}") != 0) {
      printf("escaped mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
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
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 4u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("array mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_file_range_paths(
        test_ctx, plan, source, 0u, (lql_uint64)strlen(doc), out, &error);
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
  test_ctx->mutation_plan_destroy(test_ctx, plan);
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
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 3u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("wildcard mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_file_range_paths(
        test_ctx, plan, source, 0u, (lql_uint64)strlen(doc), out, &error);
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
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  fclose(source);
  fclose(out);
}

static void expect_wildcard_remove_mutation_api(void) {
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *exprs[4];
  char buf[1024];
  size_t len;
  static const char doc[] =
      "{\"labels\":{\"env\":\"prod\",\"owner\":\"alice\"},\"items\":[{"
      "\"sku\":\"A\",\"price\":10},{\"sku\":\"B\",\"price\":20}],\"nested\":{"
      "\"items\":[{\"sku\":\"C\",\"price\":30}]}}";

  out = tmpfile();
  if (out == NULL) {
    printf("wildcard remove mutation tmpfile failed\n");
    ++failures;
    return;
  }
  exprs[0] = "rm:/labels/*";
  exprs[1] = "rm:/items[]/price";
  exprs[2] = "rm:/items/**/sku";
  exprs[3] = "rm:/nested/.../price";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 4u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("wildcard remove mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_json(test_ctx, plan, doc, strlen(doc), out, &error);
    if (st != LQL_STATUS_OK) {
      printf("wildcard remove mutation failed: %s\n", error.message);
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
               strcmp(buf,
                      "{\"labels\":{},\"items\":[{},{}],\"nested\":{"
                      "\"items\":[{\"sku\":\"C\"}]}}") != 0) {
      printf("wildcard remove mutation output mismatch: %s\n", buf);
      ++failures;
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  fclose(out);
}

static void expect_wildcard_mutation_error_precedence_api(void) {
  FILE *out;
  lql_error error;
  lql_status st;
  lql_mutation_plan *plan;
  const char *exprs[2];
  static const char doc[] = "{\"a\":{\"b\":\"x\"}}";

  out = tmpfile();
  if (out == NULL) {
    printf("wildcard mutation error tmpfile failed\n");
    ++failures;
    return;
  }
  exprs[0] = "/a/*=+1";
  exprs[1] = "/a=2";
  plan = NULL;
  lql_error_init(&error);
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 2u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("wildcard mutation error plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_json(test_ctx, plan, doc, strlen(doc), out, &error);
    if (st == LQL_STATUS_OK) {
      printf("wildcard mutation error was masked by later direct set\n");
      ++failures;
    } else if (strstr(error.message, "not numeric") == NULL) {
      printf("wildcard mutation error mismatch: %s\n", error.message);
      ++failures;
    }
  }
  test_ctx->mutation_plan_destroy(test_ctx, plan);
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
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 4u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("recursive mutation plan parse failed: %s\n", error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_file_range_paths(
        test_ctx, plan, source, 0u, (lql_uint64)strlen(doc), out, &error);
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
  test_ctx->mutation_plan_destroy(test_ctx, plan);
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
  st = test_ctx->mutation_plan_parse(test_ctx, exprs, 5u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    printf("array wildcard value mutation plan parse failed: %s\n",
           error.message);
    ++failures;
  } else {
    st = test_ctx->mutate_file_range_paths(
        test_ctx, plan, source, 0u, (lql_uint64)strlen(doc), out, &error);
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
  test_ctx->mutation_plan_destroy(test_ctx, plan);
  fclose(source);
  fclose(out);
}

typedef void (*sdk_contract_test_fn)(void);

typedef struct sdk_contract_requirement {
  const char *surface;
  const char *requirement;
  sdk_contract_test_fn test;
} sdk_contract_requirement;

typedef struct sdk_contract_surface_count {
  const char *surface;
  int expected;
} sdk_contract_surface_count;

static void expect_selector_match_api(void);
static void expect_selector_wildcard_path_api(void);
static void expect_selector_string_term_semantics_api(void);
static void expect_selector_logical_composition_api(void);
static void expect_selector_omitted_string_path_api(void);
static void expect_selector_parse_equivalence_api(void);
static void expect_selector_quoted_parse_api(void);
static void expect_selector_any_or_equivalence_api(void);
static void expect_selector_match_all_alias_api(void);
static void expect_selector_or_api(void);
static void expect_selector_parse_error_api(void);
static void expect_selector_inspection_api(void);
static void expect_selector_ast_api(void);
static void expect_selector_builder_api(void);

static void expect_sdk_contract_manifest(void) {
  static const sdk_contract_requirement manifest[] = {
      {"receiver", "instantiatable receiver object and method table",
       expect_receiver_api},
      {"utility",
       "status, error, ownership, selector emptiness, and invalid "
       "argument helpers",
       expect_public_utility_api},
      {"api-contract",
       "failure output state, callback diagnostics, and partial result "
       "propagation",
       expect_output_state_contract_api},
      {"api-contract",
       "handle ownership, optional diagnostics, and cleanup nullability",
       expect_handle_ownership_contract_api},
      {"version", "version and capability public API", expect_version_api},
      {"selector",
       "scalar, string, numeric, temporal, path, wildcard, "
       "existence, and logical matching",
       expect_selector_match_api},
      {"selector",
       "wildcard, recursive, array, object, exists, and in-any path traversal",
       expect_selector_wildcard_path_api},
      {"selector",
       "string selector case modes, any values, empty-value match-all, and "
       "path assertions",
       expect_selector_string_term_semantics_api},
      {"selector",
       "logical composition across AND, OR, indexed groups, NOT, and aliases",
       expect_selector_logical_composition_api},
      {"selector",
       "omitted-value string selectors assert path existence across value kinds",
       expect_selector_omitted_string_path_api},
      {"selector", "parse-equivalence invariants",
       expect_selector_parse_equivalence_api},
      {"selector", "quoted selector value and pointer parsing",
       expect_selector_quoted_parse_api},
      {"selector", "contains-any and explicit OR evaluation equivalence",
       expect_selector_any_or_equivalence_api},
      {"selector", "match-all aliases and parser regression inputs",
       expect_selector_match_all_alias_api},
      {"selector", "OR parse/evaluation public API", expect_selector_or_api},
      {"selector", "selector capability and execution-trait inspection",
       expect_selector_inspection_api},
      {"selector", "public selector AST traversal and JSON serialization",
       expect_selector_ast_api},
      {"selector", "public selector AST builders", expect_selector_builder_api},
      {"selector", "parse-error invariants", expect_selector_parse_error_api},
      {"streaming", "seekable FILE decision streams", expect_stream_file},
      {"streaming", "numeric object-key and array-index path segment streams",
       expect_stream_numeric_path_segments},
      {"streaming", "escaped JSON Pointer selector streams",
       expect_stream_escaped_json_pointer_segments},
      {"streaming", "mixed scalar and object candidate decision streams",
       expect_stream_mixed_scalar_candidates},
      {"streaming", "callback-source decision streams", expect_source_stream},
      {"streaming",
       "large irrelevant scalar fields do not affect streaming "
       "selector results",
       expect_stream_large_irrelevant_scalar_api},
      {"streaming", "callback-source spooled matched payloads",
       expect_source_spooled_payload_api},
      {"streaming", "top-level array candidate streams",
       expect_stream_array_items},
      {"streaming", "nested array candidate streams",
       expect_stream_nested_array_items},
      {"streaming", "stream stop controls", expect_stream_stop_controls},
      {"streaming", "query and payload public API error contracts",
       expect_stream_error_api},
      {"streaming", "malformed JSON corpus across stream modes",
       expect_stream_error_corpus_api},
      {"streaming", "seekable matched payload ranges",
       expect_seekable_payload_api},
      {"projection", "seekable file-range projection", expect_projection_api},
      {"projection", "projection parser normalization",
       expect_projection_parse_normalization_api},
      {"projection", "caller-buffered JSON projection",
       expect_buffered_projection_api},
      {"projection", "caller-provided source projection",
       expect_source_projection_api},
      {"projection", "duplicate and conflicting projection path invariants",
       expect_projection_path_invariant_api},
      {"projection", "projection parser failure corpus",
       expect_projection_parse_error_corpus_api},
      {"projection", "projection and compact public API error contracts",
       expect_projection_compact_error_api},
      {"compact", "seekable and buffered JSON compaction", expect_compact_api},
      {"compact", "buffered and seekable malformed JSON error corpus",
       expect_compact_error_corpus_api},
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
      {"mutation", "caller-buffered wildcard JSON mutation",
       expect_buffered_wildcard_mutation_api},
      {"mutation", "caller-provided source mutation",
       expect_source_mutation_api},
      {"mutation", "seekable candidate stream mutation",
       expect_file_range_candidate_mutation_api},
      {"mutation", "callback-source candidate stream mutation",
       expect_source_candidate_mutation_api},
      {"mutation", "projection-before-mutation candidate streams",
       expect_projected_candidate_mutation_api},
      {"mutation", "quoted mutation value typing",
       expect_mutation_quoted_value_api},
      {"mutation", "file-backed mutation value execution",
       expect_mutation_file_backed_value_api},
      {"mutation", "file-backed text mutation value validation",
       expect_mutation_file_backed_text_validation_api},
      {"mutation", "brace shorthand and escaped JSON Pointer mutation",
       expect_mutation_shorthand_api},
      {"mutation", "concrete array element mutation",
       expect_array_element_mutation_api},
      {"mutation", "wildcard path mutation", expect_wildcard_mutation_api},
      {"mutation", "wildcard remove mutation",
       expect_wildcard_remove_mutation_api},
      {"mutation", "wildcard mutation error precedence",
       expect_wildcard_mutation_error_precedence_api},
      {"mutation", "one-child and recursive path mutation",
       expect_recursive_mutation_api},
      {"mutation", "array wildcard value mutation",
       expect_array_wildcard_value_mutation_api},
  };
  static const sdk_contract_surface_count surface_counts[] = {
      {"receiver", 1},     {"utility", 1},  {"api-contract", 2},
      {"version", 1},      {"selector", 14}, {"streaming", 13},
      {"projection", 7},   {"compact", 2},  {"mutation", 20},
  };
  size_t i;
  size_t j;
  int count;
  int known_surface;

  for (i = 0u; i < sizeof(manifest) / sizeof(manifest[0]); ++i) {
    if (manifest[i].surface == NULL || manifest[i].surface[0] == '\0' ||
        manifest[i].requirement == NULL || manifest[i].requirement[0] == '\0') {
      printf("SDK contract manifest has an empty entry at %lu\n",
             (unsigned long)i);
      ++failures;
    }
    if (manifest[i].test == NULL) {
      printf("SDK contract manifest entry lacks a C unit function: %s/%s\n",
             manifest[i].surface, manifest[i].requirement);
      ++failures;
    }
    known_surface = 0;
    for (j = 0u; j < sizeof(surface_counts) / sizeof(surface_counts[0]); ++j) {
      if (manifest[i].surface != NULL &&
          strcmp(manifest[i].surface, surface_counts[j].surface) == 0) {
        known_surface = 1;
      }
    }
    if (!known_surface) {
      printf("SDK contract manifest has unknown surface: %s\n",
             manifest[i].surface != NULL ? manifest[i].surface : "(null)");
      ++failures;
    }
  }
  for (i = 0u; i < sizeof(surface_counts) / sizeof(surface_counts[0]); ++i) {
    count = 0;
    for (j = 0u; j < sizeof(manifest) / sizeof(manifest[0]); ++j) {
      if (strcmp(manifest[j].surface, surface_counts[i].surface) == 0) {
        ++count;
      }
    }
    if (count != surface_counts[i].expected) {
      printf("SDK contract manifest %s accounting mismatch: %d\n",
             surface_counts[i].surface, count);
      ++failures;
    }
  }
}

static void expect_selector_match_api(void) {
  expect_match("/status=\"open\"", "{\"status\":\"open\"}", 1);
  expect_match(" /status = \"open\" ", "{\"status\":\"open\"}", 1);
  expect_match("/status=\"closed\"", "{\"status\":\"open\"}", 0);
  expect_match("eq{field=/status,field=/status,value=open,value=open}",
               "{\"status\":\"open\"}", 1);
  expect_match("eq{field=/status value=open}", "{\"status\":\"open\"}", 1);
  expect_match("eq{field=/status,value='open,closed'}",
               "{\"status\":\"open,closed\"}", 1);
  expect_match("and.eq{\nfield=/message\nvalue=\"hi, world\"},and.eq{field=/"
               "status value=\"okili dokili\"}",
               "{\"message\":\"hi, world\",\"status\":\"okili dokili\"}", 1);
  expect_match("and.eq{\nfield=/message\nvalue=\"hi, world\"},and.eq{field=/"
               "status value=\"okili dokili\"}",
               "{\"message\":\"hi\",\"status\":\"okili dokili\"}", 0);
  expect_match("/progress>=50", "{\"progress\":72}", 1);
  expect_match(" /progress >= 50 ", "{\"progress\":72}", 1);
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
  expect_match("/timestamp=\"2026-03-11T01:11:28\"",
               "{\"timestamp\":\"2026-03-11T01:11:28Z\"}", 1);
  expect_match("/timestamp=\"2026-03-11T01:11:28\"",
               "{\"timestamp\":\"2026-03-11T01:11:28+01:00\"}", 0);
  expect_match("/timestamp=\"2026-03-11T01:11:28.123456789\"",
               "{\"timestamp\":\"2026-03-11T01:11:28.123456789Z\"}", 1);
  expect_match("/timestamp=\"2026-03-11T01:11:28.123456789\"",
               "{\"timestamp\":\"2026-03-11T01:11:28.123456788Z\"}", 0);
  expect_match("/timestamp=\"2026-03-11T01:11:28.123+01:00\"",
               "{\"timestamp\":\"2026-03-11T00:11:28.123Z\"}", 1);
  expect_match("/timestamp=\"2026-03-11T01:11:28.123+01:00\"",
               "{\"timestamp\":\"2026-03-11T01:11:28.123Z\"}", 0);
  expect_match("/timestamp=\"2026-03-11T01:11:28Z\"",
               "{\"timestamp\":\"2026-03-11T01:11:28Z\"}", 1);
  expect_match("/timestamp=\"2026-03-11T01:11:28.1\"",
               "{\"timestamp\":\"2026-03-11T01:11:28.100000000Z\"}", 1);
  expect_match("/timestamp=\"2026-03-11T01:11:28.123456789Z\"",
               "{\"timestamp\":\"2026-03-11T01:11:28.123456789Z\"}", 1);
  expect_match("/timestamp=\"2026-03-11T01:11:28.123456789+01:30\"",
               "{\"timestamp\":\"2026-03-10T23:41:28.123456789Z\"}", 1);
  expect_match("/timestamp=\"2026-03-11T01:11:28.123456789-02:30\"",
               "{\"timestamp\":\"2026-03-11T03:41:28.123456789Z\"}", 1);
  expect_match("/timestamp>=2026-03-11T01:11:28.123456789",
               "{\"timestamp\":\"2026-03-11T01:11:28.123456790Z\"}", 1);
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
  expect_match("date{field=/timestamp,gte=2025-01-01,lt=2025-01-03}",
               "{\"timestamp\":\"2025-01-02T00:00:00Z\"}", 1);
  expect_match("date{field=/timestamp,gte=2025-01-01,lt=2025-01-03}",
               "{\"timestamp\":\"2025-01-03T00:00:00Z\"}", 0);
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
  expect_match("contains{f=/hello/world}", "{\"hello\":{\"world\":null}}", 1);
  expect_match("icontains{f=/hello/world}", "{\"hello\":{\"world\":null}}", 1);
  expect_match("prefix{f=/hello/world}", "{\"hello\":{\"world\":null}}", 1);
  expect_match("iprefix{f=/hello/world}", "{\"hello\":{\"world\":null}}", 1);
  expect_match("exists{/hello/world}", "{\"hello\":{\"world\":null}}", 0);
  expect_match("/hello/world=\"\"", "{\"hello\":{\"world\":null}}", 0);
  expect_match("contains{f=/hello/world,v=\"\"}",
               "{\"hello\":{\"world\":null}}", 0);
  expect_match("prefix{f=/hello/world,v=\"\"}", "{\"hello\":{\"world\":null}}",
               0);
  expect_match("in{f=/hello/world,any=null|\"\"}",
               "{\"hello\":{\"world\":null}}", 0);
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
  expect_match("contains{field=/message,any=hello world|degraded}",
               "{\"message\":\"hello world\"}", 1);
  expect_match("contains{field=/message,value='hello world'}",
               "{\"message\":\"hello world\"}", 1);
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
  expect_match("in{field=/greeting,any=\"hello world|goodbye jupiter\"}",
               "{\"greeting\":\"goodbye jupiter\"}", 1);
  expect_match("contains{f=/hello/*}", "{\"hello\":{\"name\":\"alice\"}}", 1);
  expect_match("contains{f=/hello/[]}", "{\"hello\":{\"0\":\"alice\"}}", 0);
  expect_match("contains{f=/arrays/[]/id}", "{\"arrays\":[{\"id\":1}]}", 1);
  expect_match("contains{f=/arrays/*/id}", "{\"arrays\":[{\"id\":1}]}", 0);
  expect_match("contains{f=/items[]/sku}", "{\"items\":[{\"sku\":\"a\"}]}", 1);
  expect_match("contains{f=/items/**/sku}", "{\"items\":[{\"sku\":\"a\"}]}", 1);
  expect_match("contains{f=/groups/.../sku}",
               "{\"groups\":[{\"items\":[{\"sku\":\"b\"}]}]}", 1);
  expect_match("exists{/meta/.../etag}",
               "{\"meta\":{\"nested\":{\"etag\":\"abc\"}}}", 1);
  expect_match("exists{/meta/.../etag}", "{\"meta\":{\"nested\":{}}}", 0);
  expect_match("/items[]/price>=20",
               "{\"items\":[{\"sku\":\"A\",\"price\":10},{\"sku\":\"B\","
               "\"price\":25}]}",
               1);
  expect_match("/items[]/price>=20",
               "{\"items\":[{\"sku\":\"A\",\"price\":10}]}", 0);
  expect_match("/metrics/**/battery_mv<3600",
               "{\"metrics\":[{\"battery_mv\":4100},{\"battery_mv\":3300}]}",
               1);
  expect_match("/metrics/**/battery_mv<3600",
               "{\"metrics\":[{\"battery_mv\":4100}]}", 0);
  expect_match("/items/*/price>=20",
               "{\"items\":[{\"sku\":\"B\",\"price\":25}]}", 0);
  expect_match("/items[]/sku=\"B\"", "{\"items\":{\"sku\":\"B\"}}", 0);
  expect_match("/arrEmpty[]/sku=\"A\"", "{\"arrEmpty\":[]}", 0);
  expect_match("/voucher/lines/10/amount>=3000",
               "{\"voucher\":{\"lines\":{\"10\":{\"amount\":3500,\"status\":"
               "\"open\"}}}}",
               1);
  expect_match("/voucher/lines/10/amount>=3000",
               "{\"voucher\":{\"lines\":[{\"amount\":0},{\"amount\":1},{"
               "\"amount\":2},{\"amount\":3},{\"amount\":4},{\"amount\":5},{"
               "\"amount\":6},{\"amount\":7},{\"amount\":8},{\"amount\":9},{"
               "\"amount\":3600,\"status\":\"closed\"}]}}",
               1);
  expect_match("/voucher/lines/10/amount>=3000",
               "{\"voucher\":{\"lines\":{\"10\":{\"amount\":1200,\"status\":"
               "\"processing\"}}}}",
               0);
  expect_match("in{field=/voucher/lines/10/status,any=open|closed}",
               "{\"voucher\":{\"lines\":{\"10\":{\"amount\":3500,\"status\":"
               "\"open\"}}}}",
               1);
  expect_match("in{field=/voucher/lines/10/status,any=open|closed}",
               "{\"voucher\":{\"lines\":[{\"status\":\"0\"},{\"status\":\"1\"},"
               "{\"status\":\"2\"},{\"status\":\"3\"},{\"status\":\"4\"},{"
               "\"status\":\"5\"},{\"status\":\"6\"},{\"status\":\"7\"},{"
               "\"status\":\"8\"},{\"status\":\"9\"},{\"amount\":3600,"
               "\"status\":\"closed\"}]}}",
               1);
  expect_match("contains{field=/voucher/lines/10/msg,value=hello}",
               "{\"voucher\":{\"lines\":{\"10\":{\"msg\":\"hello object "
               "line\"}}}}",
               1);
  expect_match("iprefix{field=/voucher/lines/10/code,value=auth-10}",
               "{\"voucher\":{\"lines\":[{\"code\":\"0\"},{\"code\":\"1\"},{"
               "\"code\":\"2\"},{\"code\":\"3\"},{\"code\":\"4\"},{\"code\":"
               "\"5\"},{\"code\":\"6\"},{\"code\":\"7\"},{\"code\":\"8\"},{"
               "\"code\":\"9\"},{\"code\":\"AUTH-10-ARRAY\"}]}}",
               1);
  expect_match("/voucher/.../10/amount>=3000",
               "{\"voucher\":{\"lines\":{\"10\":{\"amount\":3500}}}}", 1);
  expect_match("exists{/metadata/etag}", "{\"metadata\":{\"etag\":\"x\"}}", 1);
  expect_match("exists{'/meta,etag'}", "{\"meta,etag\":\"x\"}", 1);
  expect_match("exists{/metadata}", "{\"metadata\":{\"etag\":\"x\"}}", 1);
  expect_match("exists{/items/.../sku}", "{\"items\":[{\"sku\":\"A\"}]}", 1);
  expect_match("/metadata=\"\"", "{\"metadata\":{\"etag\":\"x\"}}", 0);
  expect_match("/items/0/sku=\"a\"", "{\"items\":[{\"sku\":\"a\"}]}", 1);
  expect_match("/status=\"open\",/progress>=50",
               "{\"status\":\"open\",\"progress\":72}", 1);
  expect_match("/status=\"open\",/progress>=50",
               "{\"status\":\"open\",\"progress\":4}", 0);
  expect_match("/status=\"open\"\n/progress>=50",
               "{\"status\":\"open\",\"progress\":72}", 1);
  expect_match("/status=\"open\"\n/progress>=50",
               "{\"status\":\"open\",\"progress\":4}", 0);
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
  expect_match("not.eq{field=/status,value=closed}", "{\"status\":\"open\"}",
               1);
  expect_match("not.eq{field=/status,value=closed}", "{\"status\":\"closed\"}",
               0);
}

static void expect_selector_wildcard_path_api(void) {
  static const char path_doc[] =
      "{\"labels\":{\"env\":\"prod\",\"owner\":\"alice\"},\"items\":[{\"sku\":"
      "\"A\",\"price\":10},{\"sku\":\"B\",\"price\":25}],\"groups\":[{\"items"
      "\":[{\"sku\":\"A\"},{\"sku\":\"B\"}]}],\"metrics\":[{\"battery_mv\":"
      "4100},{\"battery_mv\":3300}],\"scalar\":\"x\",\"arrEmpty\":[]}";

  expect_match("/labels/*=\"alice\"", path_doc, 1);
  expect_match("/items/*/sku=\"B\"", path_doc, 0);
  expect_match("/items/**/sku=\"B\"", path_doc, 1);
  expect_match("/items[]/sku=\"B\"", path_doc, 1);
  expect_match("/groups[]/items/**/sku=\"B\"", path_doc, 1);
  expect_match("/groups/.../sku=\"B\"", path_doc, 1);
  expect_match("/items[]/sku=\"C\"", path_doc, 0);
  expect_match("/scalar/*=\"x\"", path_doc, 0);
  expect_match("/arrEmpty[]/sku=\"A\"", path_doc, 0);
  expect_match("/items[]/sku=\"B\"", "{\"items\":{\"sku\":\"B\"}}", 0);
  expect_match("/items[]/price>=20", path_doc, 1);
  expect_match("/metrics/**/battery_mv<3600", path_doc, 1);
  expect_match("/items/*/price>=20", path_doc, 0);
  expect_match("in{field=/labels/*,any=prod|stage}",
               "{\"labels\":{\"env\":\"prod\",\"owner\":\"alice\"}}", 1);
  expect_match("exists{/items/.../sku}", "{\"items\":[{\"sku\":\"A\"}]}", 1);
}

static void expect_selector_string_term_semantics_api(void) {
  static const char case_doc[] =
      "{\"msg\":\"Error: Timeout while reading\",\"service\":\"Auth-Service\","
      "\"labels\":{\"owner\":\"ALICE-Team\",\"env\":\"prod\"}}";
  static const char path_doc[] =
      "{\"hello\":{\"world\":{\"nested\":true},\"names\":[\"alice\","
      "\"bob\"]},\"arrays\":[{\"id\":1},{\"id\":2}]}";

  expect_match("contains{field=/msg,value=Timeout}", case_doc, 1);
  expect_match("contains{field=/msg,value=timeout}", case_doc, 0);
  expect_match("contains{field=/msg,value=timeout,ic=t}", case_doc, 1);
  expect_match("contains{field=/msg,value=timeout,ignoreCase=f}", case_doc, 0);
  expect_match("icontains{field=/msg,value=timeout}", case_doc, 1);
  expect_match("icontains{field=/msg,value=timeout,ignoreCase=f}", case_doc, 1);
  expect_match("prefix{field=/service,value=auth}", case_doc, 0);
  expect_match("prefix{field=/service,value=auth,ignoreCase=true}", case_doc,
               1);
  expect_match("iprefix{field=/service,value=auth}", case_doc, 1);
  expect_match("iprefix{field=/service,value=auth,ignoreCase=f}", case_doc, 1);
  expect_match("icontains{field=/labels/*,value=alice}", case_doc, 1);
  expect_match("contains{f=/msg,a=warn|Timeout}", case_doc, 1);
  expect_match("contains{f=/msg,a=warn|fatal}", case_doc, 0);
  expect_match("icontains{f=/msg,a=warn|timeout}", case_doc, 1);
  expect_match("icontains{f=/msg,a=warn|fatal}", case_doc, 0);
  expect_match("contains{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("icontains{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("prefix{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("iprefix{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("not.icontains{f=/,v=\"\"}", "{\"status\":\"open\"}", 0);
  expect_match("contains{f=/hello/world}",
               "{\"hello\":{\"world\":{\"nested\":true}}}", 1);
  expect_match("contains{f=/hello/world}",
               "{\"hello\":{\"world\":[1,2,3]}}", 1);
  expect_match("contains{f=/hello/world}", "{\"hello\":{\"world\":null}}", 1);
  expect_match("contains{f=/hello/world}", "{\"hello\":{\"other\":\"x\"}}", 0);
  expect_match("icontains{f=/hello/world}",
               "{\"hello\":{\"world\":{\"nested\":true}}}", 1);
  expect_match("prefix{f=/hello/world}",
               "{\"hello\":{\"world\":[1,2,3]}}", 1);
  expect_match("iprefix{f=/hello/world}", "{\"hello\":{\"world\":null}}", 1);
  expect_match("contains{f=/hello/*}", path_doc, 1);
  expect_match("contains{f=/hello/...}", path_doc, 1);
  expect_match("contains{f=/arrays/[]}", path_doc, 1);
  expect_match("contains{f=/arrays/[]/id}", path_doc, 1);
  expect_match("contains{f=/hello/missing}", path_doc, 0);
  expect_match("contains{f=/missing/*}", path_doc, 0);
  expect_match("contains{f=/missing/...}", path_doc, 0);
  expect_match("contains{f=/missing/[]}", path_doc, 0);
}

static void expect_selector_logical_composition_api(void) {
  expect_match("/field=\"value\",/status=\"ok\"",
               "{\"field\":\"value\",\"status\":\"ok\"}", 1);
  expect_match("/field=\"value\",/status=\"ok\"",
               "{\"field\":\"value\",\"status\":\"nope\"}", 0);
  expect_match("/field=\"value\",/status=\"ok\"",
               "{\"field\":\"nope\",\"status\":\"ok\"}", 0);
  expect_match("and.eq{field=/status,value=ok},/msg=\"done\"",
               "{\"status\":\"ok\",\"msg\":\"done\"}", 1);
  expect_match("and.eq{field=/status,value=ok},/msg=\"done\"",
               "{\"status\":\"ok\",\"msg\":\"nope\"}", 0);
  expect_match(
      "or.0.eq{field=/status,value=ok},or.0.range{field=/progress,gte=10}",
      "{\"status\":\"ok\",\"progress\":10}", 1);
  expect_match(
      "or.0.eq{field=/status,value=ok},or.0.range{field=/progress,gte=10}",
      "{\"status\":\"ok\",\"progress\":5}", 0);
  expect_match(
      "and.0.eq{field=/status,value=ok},and.0.range{field=/progress,gte=10}",
      "{\"status\":\"ok\",\"progress\":10}", 1);
  expect_match(
      "and.0.eq{field=/status,value=ok},and.0.range{field=/progress,gte=10}",
      "{\"status\":\"nope\",\"progress\":10}", 0);
  expect_match("not.eq{field=/status,value=closed},/region=\"us\"",
               "{\"status\":\"open\",\"region\":\"us\"}", 1);
  expect_match("not.eq{field=/status,value=closed},/region=\"us\"",
               "{\"status\":\"closed\",\"region\":\"us\"}", 0);
  expect_match("eq{f=/status,v=ok},in{f=/env,a=prod|stage}",
               "{\"status\":\"ok\",\"env\":\"prod\"}", 1);
  expect_match("eq{f=/status,v=ok},in{f=/env,a=prod|stage}",
               "{\"status\":\"ok\",\"env\":\"dev\"}", 0);
  expect_match("/field=\"value\",/status=\"ok\",or.eq{field=/msg,value=done},"
               "or.eq{field=/msg,value=complete}",
               "{\"field\":\"value\",\"status\":\"ok\",\"msg\":\"done\"}", 1);
  expect_match("/field=\"value\",/status=\"ok\",or.eq{field=/msg,value=done},"
               "or.eq{field=/msg,value=complete}",
               "{\"field\":\"value\",\"status\":\"ok\",\"msg\":\"nope\"}", 0);
}

static void expect_selector_omitted_string_path_api(void) {
  static const char object_doc[] =
      "{\"hello\":{\"world\":{\"nested\":true}},\"arrays\":[{\"id\":1}]}";
  static const char array_doc[] =
      "{\"hello\":{\"world\":[1,2,3]},\"arrays\":[{\"id\":1}]}";
  static const char null_doc[] =
      "{\"hello\":{\"world\":null},\"arrays\":[{\"id\":1}]}";
  static const char missing_doc[] =
      "{\"hello\":{\"other\":\"x\"},\"arrays\":[{\"id\":1}]}";
  static const char wildcard_doc[] =
      "{\"hello\":{\"world\":{\"nested\":true},\"names\":[\"alice\","
      "\"bob\"]},\"arrays\":[{\"id\":1},{\"id\":2}]}";
  static const char *const selectors[] = {
      "contains{f=/hello/world}", "icontains{f=/hello/world}",
      "prefix{f=/hello/world}", "iprefix{f=/hello/world}"};
  size_t i;

  for (i = 0u; i < sizeof(selectors) / sizeof(selectors[0]); ++i) {
    expect_match(selectors[i], object_doc, 1);
    expect_match(selectors[i], array_doc, 1);
    expect_match(selectors[i], null_doc, 1);
    expect_match(selectors[i], missing_doc, 0);
  }

  expect_match("contains{f=/hello/*}", wildcard_doc, 1);
  expect_match("contains{f=/hello/...}", wildcard_doc, 1);
  expect_match("contains{f=/arrays/[]}", wildcard_doc, 1);
  expect_match("contains{f=/arrays/[]/id}", wildcard_doc, 1);
  expect_match("contains{f=/hello/missing}", wildcard_doc, 0);
  expect_match("contains{f=/missing/*}", wildcard_doc, 0);
  expect_match("contains{f=/missing/...}", wildcard_doc, 0);
  expect_match("contains{f=/missing/[]}", wildcard_doc, 0);

  expect_match("contains{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("icontains{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("prefix{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("iprefix{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("not.icontains{f=/,v=\"\"}", "{\"status\":\"open\"}", 0);
}

static void expect_selector_parse_equivalence_api(void) {
  {
    static const char *const exprs[] = {
        "eq{field=/status,value=open}", "eq{value=open,field=/status}",
        "eq{f=/status,v=open}",
        "eq{field=/status,field=/status,value=open,value=open}",
        "eq{field=/status value=open}", " /status = \"open\" "};
    expect_selector_equivalent_forms("eq aliases", exprs,
                                     sizeof(exprs) / sizeof(exprs[0]),
                                     "{\"status\":\"open\"}",
                                     "{\"status\":\"closed\"}");
  }
  expect_match("and.eq{field=/status,value=open}", "{\"status\":\"open\"}", 1);
  expect_match("and.eq{field=/status,value=open}", "{\"status\":\"closed\"}",
               0);
  expect_match("and.0.eq{field=/status,value=open}",
               "{\"status\":\"open\"}", 1);
  expect_match("and.0.eq{field=/status,value=open}",
               "{\"status\":\"closed\"}", 0);
  expect_match("or.eq{field=/status,value=open}", "{\"status\":\"open\"}", 1);
  expect_match("or.eq{field=/status,value=open}", "{\"status\":\"closed\"}",
               0);
  expect_match("or.0.eq{field=/status,value=open}",
               "{\"status\":\"open\"}", 1);
  expect_match("or.0.eq{field=/status,value=open}",
               "{\"status\":\"closed\"}", 0);
  expect_match("not.eq{field=/status,value=closed}", "{\"status\":\"open\"}",
               1);
  expect_match("not.eq{field=/status,value=closed}", "{\"status\":\"closed\"}",
               0);
  expect_match("and.not.eq{field=/status,value=closed}",
               "{\"status\":\"open\"}", 1);
  expect_match("and.not.eq{field=/status,value=closed}",
               "{\"status\":\"closed\"}", 0);
  expect_match("or.not.eq{field=/status,value=closed}",
               "{\"status\":\"open\"}", 1);
  expect_match("or.not.eq{field=/status,value=closed}",
               "{\"status\":\"closed\"}", 0);
  expect_match("and.or.0.eq{field=/status,value=open}",
               "{\"status\":\"open\"}", 1);
  expect_match("and.or.0.eq{field=/status,value=open}",
               "{\"status\":\"closed\"}", 0);
  expect_match("or.and.0.eq{field=/status,value=open}",
               "{\"status\":\"open\"}", 1);
  expect_match("or.and.0.eq{field=/status,value=open}",
               "{\"status\":\"closed\"}", 0);
  {
    static const char *const exprs[] = {
        "contains{field=/msg,any=timeout|error}",
        "contains{any=timeout|error,field=/msg}",
        "contains{f=/msg,a=timeout|error}",
        "contains{ field=/msg,\nany=timeout|error }"};
    expect_selector_equivalent_forms("contains any aliases", exprs,
                                     sizeof(exprs) / sizeof(exprs[0]),
                                     "{\"msg\":\"upstream timeout\"}",
                                     "{\"msg\":\"healthy\"}");
  }
  {
    static const char *const exprs[] = {
        "in{field=/env,any=prod|stage}", "in{any=prod|stage,field=/env}",
        "in{f=/env,a=prod|stage}", "in{field=/env,any=\"prod|stage\"}"};
    expect_selector_equivalent_forms("in any aliases", exprs,
                                     sizeof(exprs) / sizeof(exprs[0]),
                                     "{\"env\":\"stage\"}",
                                     "{\"env\":\"dev\"}");
  }
  {
    static const char *const exprs[] = {
        "range{field=/timestamp,gte=2026-03-05T10:28:21Z,"
        "lt=2026-03-05T10:30:00Z}",
        "range{lt=2026-03-05T10:30:00Z,field=/timestamp,"
        "gte=2026-03-05T10:28:21Z}",
        "range{ field=/timestamp, gte=2026-03-05T10:28:21Z, "
        "lt=2026-03-05T10:30:00Z }"};
    expect_selector_equivalent_forms(
        "range assignment order", exprs, sizeof(exprs) / sizeof(exprs[0]),
        "{\"timestamp\":\"2026-03-05T10:29:00Z\"}",
        "{\"timestamp\":\"2026-03-05T10:30:00Z\"}");
  }
  {
    static const char *const exprs[] = {
        "date{field=/timestamp,after=2025-01-01,before=2025-01-03}",
        "date{before=2025-01-03,field=/timestamp,after=2025-01-01}",
        "date{f=/timestamp,a=2025-01-01,b=2025-01-03}"};
    expect_selector_equivalent_forms(
        "date aliases", exprs, sizeof(exprs) / sizeof(exprs[0]),
        "{\"timestamp\":\"2025-01-02T06:00:00Z\"}",
        "{\"timestamp\":\"2025-01-03T00:00:00Z\"}");
  }
  {
    static const char *const exprs[] = {
        "date{f=/timestamp,a=2025-01-01,b=2025-01-03}",
        "date{a=2025-01-01,f=/timestamp,b=2025-01-03}",
        "date{b=2025-01-03,a=2025-01-01,f=/timestamp}"};
    expect_selector_equivalent_forms(
        "date alias order matrix", exprs, sizeof(exprs) / sizeof(exprs[0]),
        "{\"timestamp\":\"2025-01-02T06:00:00Z\"}",
        "{\"timestamp\":\"2025-01-03T00:00:00Z\"}");
  }
  {
    static const char *const exprs[] = {
        "eq{field=/status,value=open},in{field=/env,any=prod|stage}",
        "eq{field=/status,value=open}\nin{field=/env,any=prod|stage}",
        " eq{field=/status,value=open},\n in{field=/env,any=prod|stage} "};
    expect_selector_equivalent_forms(
        "multiline implicit and", exprs, sizeof(exprs) / sizeof(exprs[0]),
        "{\"status\":\"open\",\"env\":\"prod\"}",
        "{\"status\":\"open\",\"env\":\"dev\"}");
  }
  {
    static const char *const exprs[] = {
        "and.0.or.0.not.eq{field=/status,value=closed}",
        "and.0.or.0.not.eq{value=closed,field=/status}"};
    expect_selector_equivalent_forms(
        "nested wrapper aliases", exprs, sizeof(exprs) / sizeof(exprs[0]),
        "{\"status\":\"open\"}", "{\"status\":\"closed\"}");
  }
  expect_match("and.0.or.0.and.0.eq{field=/status,value=open},and.0.or.0."
               "and.0.range{field=/progress,gte=10}",
               "{\"status\":\"open\",\"progress\":12}", 1);
  expect_match("and.0.or.0.and.0.eq{field=/status,value=open},and.0.or.0."
               "and.0.range{field=/progress,gte=10}",
               "{\"status\":\"open\",\"progress\":4}", 0);
  expect_match("or.0.and.0.or.0.eq{field=/status,value=open},or.0.and.0."
               "or.0.exists{/meta/etag}",
               "{\"status\":\"open\",\"meta\":{\"etag\":\"x\"}}", 1);
  expect_match("or.0.and.0.or.0.eq{field=/status,value=open},or.0.and.0."
               "or.0.exists{/meta/etag}",
               "{\"status\":\"open\",\"meta\":{}}", 0);
  expect_match("and.0.or.0.not.in{field=/env,any=prod|stage}",
               "{\"env\":\"dev\"}", 1);
  expect_match("and.0.or.0.not.in{field=/env,any=prod|stage}",
               "{\"env\":\"prod\"}", 0);
}

static void expect_selector_quoted_parse_api(void) {
  expect_match("eq{field=/status,value='open,closed'}",
               "{\"status\":\"open,closed\"}", 1);
  expect_match("eq{field=/status,value='open,closed'}",
               "{\"status\":\"open\"}", 0);
  expect_match("and.eq{field=/message,value=\"hi, world\"},and.eq{field=/"
               "status,value=\"okili dokili\"}",
               "{\"message\":\"hi, world\",\"status\":\"okili dokili\"}", 1);
  expect_match("and.eq{field=/message,value=\"hi, world\"},and.eq{field=/"
               "status,value=\"okili dokili\"}",
               "{\"message\":\"hi\",\"status\":\"okili dokili\"}", 0);
  expect_match("contains{field=/msg,value='hello world'}",
               "{\"msg\":\"well hello world\"}", 1);
  expect_match("contains{field=/msg,value='hello world'}",
               "{\"msg\":\"hello\"}", 0);
  expect_match("in{field=/greeting,any=\"hello world|goodbye jupiter\"}",
               "{\"greeting\":\"goodbye jupiter\"}", 1);
  expect_match("in{field=/greeting,any=\"hello world|goodbye jupiter\"}",
               "{\"greeting\":\"hello\"}", 0);
  expect_match("exists{'/meta,etag'}", "{\"meta,etag\":\"x\"}", 1);
  expect_match("exists{'/meta,etag'}", "{\"meta\":{\"etag\":\"x\"}}", 0);
  expect_match("/a~1b/~0key=\"ready\"", "{\"a/b\":{\"~key\":\"ready\"}}", 1);
  expect_match("/a~1b/~0key=\"ready\"", "{\"a\":{\"b\":{\"~key\":\"ready\"}}}",
               0);
}

static void expect_selector_any_or_equivalence_api(void) {
  static const char contains_any[] = "contains{f=/msg,a=warn|timeout}";
  static const char contains_or[] =
      "or.contains{f=/msg,v=warn},or.contains{f=/msg,v=timeout}";
  static const char icontains_any[] = "icontains{f=/msg,a=warn|timeout}";
  static const char icontains_or[] =
      "or.icontains{f=/msg,v=warn},or.icontains{f=/msg,v=timeout}";

  expect_selector_equivalent_match_result(
      "contains any first value", contains_any, contains_or,
      "{\"msg\":\"warn: cache miss\"}");
  expect_selector_equivalent_match_result(
      "contains any second value", contains_any, contains_or,
      "{\"msg\":\"timeout waiting for reply\"}");
  expect_selector_equivalent_match_result("contains any no match", contains_any,
                                          contains_or,
                                          "{\"msg\":\"all good\"}");
  expect_selector_equivalent_match_result(
      "icontains any first value", icontains_any, icontains_or,
      "{\"msg\":\"WARN: cache miss\"}");
  expect_selector_equivalent_match_result(
      "icontains any second value", icontains_any, icontains_or,
      "{\"msg\":\"TIMEOUT waiting for reply\"}");
  expect_selector_equivalent_match_result("icontains any no match",
                                          icontains_any, icontains_or,
                                          "{\"msg\":\"all good\"}");
}

static void expect_selector_match_all_alias_api(void) {
  static const char *const aliases[] = {"", "{}", ".", "/"};
  static const char *const regressions[] = {
      "And.-1", "ANd.\xe0\xa5\xb0\x8b.30zz v1\nst",
      "ANd.066666666000"};
  size_t i;
  lql_selector *selector;
  lql_error error;
  lql_status st;

  for (i = 0u; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
    selector = NULL;
    lql_error_init(&error);
    st = test_ctx->selector_parse(test_ctx, aliases[i], &selector, &error);
    if (st != LQL_STATUS_OK || !test_ctx->selector_is_empty(test_ctx, selector)) {
      printf("match-all alias %s mismatch: status=%s empty=%d error=%s\n",
             aliases[i], lql_status_string(st),
             test_ctx->selector_is_empty(test_ctx, selector), error.message);
      ++failures;
    }
    test_ctx->selector_destroy(test_ctx, selector);
  }

  for (i = 0u; i < sizeof(regressions) / sizeof(regressions[0]); ++i) {
    selector = NULL;
    lql_error_init(&error);
    st = test_ctx->selector_parse(test_ctx, regressions[i], &selector, &error);
    if (st == LQL_STATUS_OK && selector != NULL) {
      test_ctx->selector_destroy(test_ctx, selector);
    }
  }
}

static void expect_selector_or_api(void) {
  expect_match_or("/status=\"open\",/progress>=50",
                  "{\"status\":\"closed\",\"progress\":72}", 1);
  expect_match_or("/status=\"open\",/progress>=50",
                  "{\"status\":\"closed\",\"progress\":4}", 0);
  expect_match_or("/status=\"open\"\n/progress>=50",
                  "{\"status\":\"closed\",\"progress\":72}", 1);
  expect_match_or("/status=\"open\"\n/progress>=50",
                  "{\"status\":\"closed\",\"progress\":4}", 0);
  expect_match_or("eq{field=/status,value=open},range{field=/progress,gte=50}",
                  "{\"status\":\"open\",\"progress\":4}", 1);
  expect_match_or("eq{field=/status,value=open},range{field=/progress,gte=50}",
                  "{\"status\":\"closed\",\"progress\":72}", 1);
  expect_match_or("eq{field=/status,value=open},range{field=/progress,gte=50}",
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
  expect_match("and.0.or.0.not.eq{field=/status,value=closed}",
               "{\"status\":\"open\"}", 1);
  expect_match("and.0.or.0.not.eq{field=/status,value=closed}",
               "{\"status\":\"closed\"}", 0);
  expect_match("and.0.or.0.and.0.eq{field=/status,value=open},and.0.or.0."
               "and.0.range{field=/progress,gte=10}",
               "{\"status\":\"open\",\"progress\":12}", 1);
  expect_match("and.0.or.0.and.0.eq{field=/status,value=open},and.0.or.0."
               "and.0.range{field=/progress,gte=10}",
               "{\"status\":\"open\",\"progress\":4}", 0);
  expect_match("or.0.and.0.or.0.eq{field=/status,value=open},or.0.and.0."
               "or.0.range{field=/progress,gte=10}",
               "{\"status\":\"open\",\"progress\":12}", 1);
  expect_match("or.0.and.0.or.0.eq{field=/status,value=open},or.0.and.0."
               "or.0.range{field=/progress,gte=10}",
               "{\"status\":\"closed\",\"progress\":12}", 0);
  expect_match(
      "and.0.eq{field=/status,value=open},and.1.or.0.in{field=/env,"
      "any=prod|stage},and.1.or.1.exists{/meta/etag}",
      "{\"status\":\"open\",\"env\":\"dev\",\"meta\":{\"etag\":\"x\"}}", 1);
  expect_match("and.0.eq{field=/status,value=open},and.1.or.0.in{field=/env,"
               "any=prod|stage},and.1.or.1.exists{/meta/etag}",
               "{\"status\":\"open\",\"env\":\"dev\",\"meta\":{}}", 0);
  expect_match(
      "or.0.eq{field=/status,value=open},or.1.and.0.range{field=/"
      "progress,gte=10},or.1.and.0.exists{/meta/etag}",
      "{\"status\":\"closed\",\"progress\":11,\"meta\":{\"etag\":\"x\"}}", 1);
  expect_match("or.0.eq{field=/status,value=open},or.1.and.0.range{field=/"
               "progress,gte=10},or.1.and.0.exists{/meta/etag}",
               "{\"status\":\"closed\",\"progress\":11,\"meta\":{}}", 0);
  expect_match("and.or.0.eq{field=/status,value=open}", "{\"status\":\"open\"}",
               1);
  expect_match("and.or.0.eq{field=/status,value=open}",
               "{\"status\":\"closed\"}", 0);
  expect_match("or.and.0.eq{field=/status,value=open}", "{\"status\":\"open\"}",
               1);
  expect_match("or.and.0.eq{field=/status,value=open}",
               "{\"status\":\"closed\"}", 0);
}

static void expect_selector_parse_error_api(void) {
  expect_parse_error("contains{field=/message,value=timeout,any=error}");
  expect_parse_error("icontains{field=/message,value=timeout,any=error}");
  expect_parse_error("contains{field=/message,any=}");
  expect_parse_error("contains{field=/message,any=||}");
  expect_parse_error("contains{field=/message,value=timeout,value=error}");
  expect_parse_error("contains{field=/message,value=timeout,ignoreCase=maybe}");
  expect_parse_error("eq{field=/status,f=/other,value=open}");
  expect_parse_error("eq{field=/msg,any=foo|bar}");
  expect_parse_error("eq{field=/status,value=open,foo=bar}");
  expect_parse_error("eq{field=/status,value=open,ignoreCase=true}");
  expect_parse_error(
      "or.0.eq{field=/status,value=open},or.0.eq{field=/status,value=closed}");
  expect_parse_error("and.0.eq{field=/status,value=open},and.0.eq{field=/"
                     "status,value=closed}");
  expect_parse_error("and.0.or.0.and.0.eq{field=/status,value=open},and.0."
                     "or.0.and.0.eq{field=/status,value=closed}");
  expect_parse_error("or.0.and.0.or.0.exists{/meta/etag},or.0.and.0.or.0."
                     "exists{/meta/id}");
  expect_parse_error("and.0.or.0.not.in{field=/env,any=prod|stage},and.0."
                     "or.0.not.in{field=/env,any=dev}");
  expect_parse_error("range{field=/progress,gte=10,gte=20}");
  expect_parse_error("range{field=/progress,gte=10,foo=bar}");
  expect_parse_error("range{field=/progress,gte=10,lt=2025-01-01}");
  expect_parse_error("range{field=/timestamp,gte=yesterday}");
  expect_parse_error("/timestamp>=yesterday");
  expect_parse_error("date{field=/timestamp,value=2025-01-01 00:00:00}");
  expect_parse_error("date{field=/timestamp,value=2026-03-11t01:11:28Z}");
  expect_parse_error("date{field=/timestamp,value=2026-03-11T01:11:28z}");
  expect_parse_error("date{field=/timestamp,value=2026-03-11T01:11}");
  expect_parse_error("date{field=/timestamp,value=2026-03-11T01:11:28+0100}");
  expect_parse_error("date{field=/timestamp,value=2026-03-11T01:11:28+01}");
  expect_parse_error("date{field=/timestamp,value=2026-03-11T01:11:60Z}");
  expect_parse_error("date{field=/timestamp,after=2025-01-01,foo=bar}");
  expect_parse_error("date{field=/timestamp,since=yesterday,after=2025-01-01}");
  expect_parse_error("date{field=/timestamp,after=2025-01-01,gt=2025-01-02}");
  expect_parse_error("date{field=/timestamp,before=2025-01-03,lt=2025-01-02}");
  expect_parse_error("date{after=2025-01-01}");
  expect_parse_error("date{field=/timestamp,since=tomorrowish}");
  expect_parse_error("prefix{field=/service,any=auth|edge}");
  expect_parse_error("iprefix{field=/service,any=auth|edge}");
  expect_parse_error("in{field=/env,any=}");
  expect_parse_error("in{field=/env}");
  expect_parse_error("in{any=prod|stage}");
  expect_parse_error("in{field=/env,any= prod | stage }");
  expect_parse_error("in{field=/env,any=prod|stage,a=dev}");
  expect_parse_error("in{field=/env,any=prod|stage,foo=bar}");
  expect_parse_error("range{field=/progress}");
  expect_parse_error("range{gte=10}");
  expect_parse_error("exists{}");
  expect_parse_error("exists{/meta/etag,field=/status}");
  expect_parse_error("exists{/meta/etag,/meta/id}");
  expect_parse_error("exists{field=/meta/etag}");
  expect_parse_error("and..eq{field=/status,value=open}");
  expect_parse_error("and.foo.eq{field=/status,value=open}");
  expect_parse_error("or.0.and.foo.exists{/meta/etag}");
  expect_parse_error("or.0..eq{field=/status,value=open}");
  expect_parse_error("nonsense");
  expect_parse_error("eq{field=/status,value=open},nonsense");
  expect_parse_error("eq{field=/status,value=\"open}");
  expect_parse_error("and.eq{field=/status,value=open");
  expect_parse_error("eq{field=/status,value=open}}");
  expect_parse_error("/count>=");

  expect_parse_or_error("contains{field=/message,value=timeout,any=error}");
  expect_parse_or_error("eq{field=/status,f=/other,value=open}");
  expect_parse_or_error("range{field=/progress}");
  expect_parse_or_error("range{gte=10}");
  expect_parse_or_error("date{after=2025-01-01}");
  expect_parse_or_error("in{field=/env}");
  expect_parse_or_error("in{any=prod|stage}");
  expect_parse_or_error("exists{/meta/etag,field=/status}");
  expect_parse_or_error("or.0.and.foo.exists{/meta/etag}");
  expect_parse_or_error("eq{field=/status,value=open},nonsense");
  expect_parse_or_error("eq{field=/status,value=open}}");
  expect_parse_or_error("/count>=");
}

static void expect_selector_inspection_api(void) {
  lql_selector *selector;
  lql_selector_capabilities caps;
  lql_selector_execution_traits traits;
  lql_error error;
  lql_status st;
  const char *families;
  const char *paths;

  memset(&caps, 1, sizeof(caps));
  test_ctx->selector_capabilities_get(test_ctx, NULL, &caps);
  if (caps.and_ || caps.or_ || caps.not_ || caps.eq || caps.range ||
      caps.date || caps.in || caps.prefix || caps.contains || caps.exists ||
      caps.wildcard_path || caps.recursive_path) {
    printf("NULL selector capabilities should be empty\n");
    ++failures;
  }
  memset(&traits, 1, sizeof(traits));
  test_ctx->selector_execution_traits_get(test_ctx, NULL, &traits);
  if (traits.uses_contains_like || traits.uses_recursive_path ||
      traits.uses_wildcard_path || traits.requires_object_root ||
      traits.early_non_match_likely) {
    printf("NULL selector traits should be empty\n");
    ++failures;
  }
  test_ctx->selector_capabilities_get(test_ctx, NULL, NULL);
  test_ctx->selector_execution_traits_get(test_ctx, NULL, NULL);

  families =
      "and.eq{field=/status,value=open},"
      "and.range{field=/progress,gte=5},"
      "or.in{field=/env,any=prod|stage},"
      "not.eq{field=/state,value=disabled},"
      "exists{/meta/etag},"
      "icontains{field=/msg,value=timeout},"
      "iprefix{field=/service,value=auth}";
  selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, families, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("selector inspection families parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  memset(&caps, 0, sizeof(caps));
  test_ctx->selector_capabilities_get(test_ctx, selector, &caps);
  if (!caps.and_ || !caps.or_ || !caps.not_ || !caps.eq || !caps.range ||
      caps.date || !caps.in || !caps.prefix || !caps.contains ||
      !caps.exists || caps.wildcard_path || caps.recursive_path) {
    printf("selector inspection family flags mismatch\n");
    ++failures;
  }
  test_ctx->selector_destroy(test_ctx, selector);

  paths = "/items[]/sku=\"A\",/groups/**/sku=\"B\",exists{/meta/.../etag}";
  selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, paths, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("selector inspection path parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  memset(&caps, 0, sizeof(caps));
  test_ctx->selector_capabilities_get(test_ctx, selector, &caps);
  if (!caps.eq || !caps.exists || !caps.wildcard_path ||
      !caps.recursive_path) {
    printf("selector inspection path flags mismatch\n");
    ++failures;
  }
  test_ctx->selector_destroy(test_ctx, selector);

  selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse(
      test_ctx,
      "and.eq{field=/status,value=open},icontains{field=/msg,value=timeout},"
      "exists{/meta/**/etag}",
      &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("selector inspection traits parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  memset(&traits, 0, sizeof(traits));
  test_ctx->selector_execution_traits_get(test_ctx, selector, &traits);
  if (!traits.uses_contains_like || !traits.uses_recursive_path ||
      !traits.uses_wildcard_path || !traits.requires_object_root ||
      traits.early_non_match_likely) {
    printf("selector execution traits recursive mismatch\n");
    ++failures;
  }
  test_ctx->selector_destroy(test_ctx, selector);

  selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, "icontains{f=/,v=\"\"}", &selector,
                                &error);
  if (st != LQL_STATUS_OK) {
    printf("selector inspection match-all parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  memset(&traits, 0, sizeof(traits));
  test_ctx->selector_execution_traits_get(test_ctx, selector, &traits);
  if (traits.uses_contains_like || traits.uses_recursive_path ||
      traits.uses_wildcard_path || traits.requires_object_root ||
      traits.early_non_match_likely) {
    printf("selector execution traits match-all mismatch\n");
    ++failures;
  }
  test_ctx->selector_destroy(test_ctx, selector);

  selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, "/status=\"open\"", &selector,
                                &error);
  if (st != LQL_STATUS_OK) {
    printf("selector inspection simple parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  memset(&traits, 0, sizeof(traits));
  test_ctx->selector_execution_traits_get(test_ctx, selector, &traits);
  if (traits.uses_contains_like || traits.uses_recursive_path ||
      traits.uses_wildcard_path || !traits.requires_object_root ||
      !traits.early_non_match_likely) {
    printf("selector execution traits simple mismatch\n");
    ++failures;
  }
  test_ctx->selector_destroy(test_ctx, selector);
}

static void expect_selector_json_output(const char *expr, const char *want) {
  lql_selector *selector;
  lql_error error;
  lql_status st;
  FILE *out;
  char buf[512];
  size_t len;

  selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("selector AST JSON parse failed for %s: %s\n", expr,
           error.message);
    ++failures;
    return;
  }
  out = tmpfile();
  if (out == NULL) {
    printf("selector AST JSON tmpfile failed\n");
    ++failures;
    test_ctx->selector_destroy(test_ctx, selector);
    return;
  }
  lql_error_init(&error);
  st = test_ctx->selector_write_json(test_ctx, selector, out, &error);
  if (st != LQL_STATUS_OK) {
    printf("selector AST JSON write failed for %s: %s\n", expr, error.message);
    ++failures;
  } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
             strcmp(buf, want) != 0) {
    printf("selector AST JSON mismatch for %s\n got: %s\nwant: %s\n", expr,
           buf, want);
    ++failures;
  }
  fclose(out);
  test_ctx->selector_destroy(test_ctx, selector);
}

static void expect_selector_handle_json(const char *label,
                                        const lql_selector *selector,
                                        const char *want) {
  FILE *out;
  char buf[512];
  size_t len;
  lql_error error;
  lql_status st;

  out = tmpfile();
  if (out == NULL) {
    printf("selector JSON tmpfile failed for %s\n", label);
    ++failures;
    return;
  }
  lql_error_init(&error);
  st = test_ctx->selector_write_json(test_ctx, selector, out, &error);
  if (st != LQL_STATUS_OK) {
    printf("selector JSON write failed for %s: %s\n", label, error.message);
    ++failures;
  } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
             strcmp(buf, want) != 0) {
    printf("selector JSON mismatch for %s\n got: %s\nwant: %s\n", label, buf,
           want);
    ++failures;
  }
  fclose(out);
}

static void expect_selector_ast_api(void) {
  lql_selector *selector;
  lql_selector *json_selector;
  lql_selector_node root;
  lql_selector_node child;
  lql_selector_string_term string_term;
  lql_selector_range_term range_term;
  lql_selector_date_term date_term;
  lql_selector_in_term in_term;
  lql_string_view view;
  lql_error error;
  lql_status st;
  size_t count;
  const char *expr;
  const char *json;
  int matched;

  lql_error_init(&error);
  st = test_ctx->selector_write_json(test_ctx, NULL, NULL, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "output file required") != 0) {
    printf("selector AST JSON invalid output mismatch: %s\n", error.message);
    ++failures;
  }

  json_selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse_json(test_ctx, NULL, 1u, &json_selector,
                                     &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT ||
      strcmp(error.message, "selector JSON required") != 0 ||
      json_selector != NULL) {
    printf("selector AST JSON invalid input mismatch: %s\n", error.message);
    ++failures;
  }

  expect_selector_json_output("contains{f=/hello/world}",
                              "{\"contains\":{\"field\":\"/hello/world\"}}");
  expect_selector_json_output(
      "contains{f=/hello/world,v=\"\"}",
      "{\"contains\":{\"field\":\"/hello/world\",\"value\":\"\"}}");
  expect_selector_json_output("icontains{f=/hello/world}",
                              "{\"icontains\":{\"field\":\"/hello/world\"}}");
  expect_selector_json_output(
      "icontains{f=/hello/world,v=\"\"}",
      "{\"icontains\":{\"field\":\"/hello/world\",\"value\":\"\"}}");
  expect_selector_json_output("prefix{f=/hello/world}",
                              "{\"prefix\":{\"field\":\"/hello/world\"}}");
  expect_selector_json_output(
      "prefix{f=/hello/world,v=\"\"}",
      "{\"prefix\":{\"field\":\"/hello/world\",\"value\":\"\"}}");
  expect_selector_json_output("iprefix{f=/hello/world}",
                              "{\"iprefix\":{\"field\":\"/hello/world\"}}");
  expect_selector_json_output(
      "iprefix{f=/hello/world,v=\"\"}",
      "{\"iprefix\":{\"field\":\"/hello/world\",\"value\":\"\"}}");

  json = "{\"contains\":{\"field\":\"/hello/world\"}}";
  json_selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse_json(test_ctx, json, strlen(json),
                                     &json_selector, &error);
  if (st != LQL_STATUS_OK || json_selector == NULL) {
    printf("selector AST JSON parse omitted value failed: %s\n",
           error.message);
    ++failures;
  } else {
    memset(&root, 0, sizeof(root));
    st = test_ctx->selector_root(test_ctx, json_selector, &root, &error);
    memset(&string_term, 0, sizeof(string_term));
    if (st != LQL_STATUS_OK ||
        test_ctx->selector_node_string_term(test_ctx, root, &string_term,
                                            &error) != LQL_STATUS_OK ||
        !view_equals(string_term.field, "/hello/world") ||
        string_term.value_present) {
      printf("selector AST JSON omitted value term mismatch: %s\n",
             error.message);
      ++failures;
    }
    matched = 0;
    st = test_ctx->matches_json(test_ctx, json_selector,
                                "{\"hello\":{\"world\":\"anything\"}}",
                                strlen("{\"hello\":{\"world\":\"anything\"}}"),
                                &matched, &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("selector AST JSON omitted value match mismatch: %s\n",
             error.message);
      ++failures;
    }
    test_ctx->selector_destroy(test_ctx, json_selector);
  }

  json = "{\"contains\":{\"field\":\"/hello/world\",\"value\":\"\"}}";
  json_selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse_json(test_ctx, json, strlen(json),
                                     &json_selector, &error);
  if (st != LQL_STATUS_OK || json_selector == NULL) {
    printf("selector AST JSON parse explicit empty failed: %s\n",
           error.message);
    ++failures;
  } else {
    memset(&root, 0, sizeof(root));
    st = test_ctx->selector_root(test_ctx, json_selector, &root, &error);
    memset(&string_term, 0, sizeof(string_term));
    if (st != LQL_STATUS_OK ||
        test_ctx->selector_node_string_term(test_ctx, root, &string_term,
                                            &error) != LQL_STATUS_OK ||
        !string_term.value_present || !view_equals(string_term.value, "")) {
      printf("selector AST JSON explicit empty term mismatch: %s\n",
             error.message);
      ++failures;
    }
    matched = 0;
    st = test_ctx->matches_json(test_ctx, json_selector,
                                "{\"hello\":{\"world\":\"\"}}",
                                strlen("{\"hello\":{\"world\":\"\"}}"),
                                &matched, &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("selector AST JSON explicit empty match mismatch: %s\n",
             error.message);
      ++failures;
    }
    test_ctx->selector_destroy(test_ctx, json_selector);
  }

  json = "{\"or\":[{\"eq\":{\"field\":\"/region\",\"value\":\"us\"}},"
         "{\"eq\":{\"field\":\"/region\",\"value\":\"eu\"}}]}";
  json_selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse_json(test_ctx, json, strlen(json),
                                     &json_selector, &error);
  if (st != LQL_STATUS_OK || json_selector == NULL) {
    printf("selector AST JSON parse or failed: %s\n", error.message);
    ++failures;
  } else {
    memset(&root, 0, sizeof(root));
    count = 0u;
    st = test_ctx->selector_root(test_ctx, json_selector, &root, &error);
    if (st != LQL_STATUS_OK || root.kind != LQL_SELECTOR_NODE_OR ||
        test_ctx->selector_node_child_count(test_ctx, root, &count, &error) !=
            LQL_STATUS_OK ||
        count != 2u) {
      printf("selector AST JSON or traversal mismatch: %s\n", error.message);
      ++failures;
    }
    matched = 0;
    st = test_ctx->matches_json(test_ctx, json_selector, "{\"region\":\"eu\"}",
                                strlen("{\"region\":\"eu\"}"), &matched,
                                &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("selector AST JSON or match mismatch: %s\n", error.message);
      ++failures;
    }
    test_ctx->selector_destroy(test_ctx, json_selector);
  }

  expr = "contains{field=/msg,any=warn|timeout},"
         "range{field=/progress,gte=10},"
         "range{field=/timestamp,lt=2026-03-05T11:29:41.265+01:00},"
         "date{field=/timestamp,after=2025-01-01,before=2025-01-03},"
         "in{field=/env,any=prod|stage},"
         "exists{/meta/etag}";
  selector = NULL;
  lql_error_init(&error);
  st = test_ctx->selector_parse(test_ctx, expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("selector AST parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  memset(&root, 0, sizeof(root));
  st = test_ctx->selector_root(test_ctx, selector, &root, &error);
  if (st != LQL_STATUS_OK || root.kind != LQL_SELECTOR_NODE_AND) {
    printf("selector AST root mismatch: %s\n", error.message);
    ++failures;
  }
  count = 0u;
  st = test_ctx->selector_node_child_count(test_ctx, root, &count, &error);
  if (st != LQL_STATUS_OK || count != 6u) {
    printf("selector AST child count mismatch: %lu %s\n",
           (unsigned long)count, error.message);
    ++failures;
  }
  st = test_ctx->selector_node_child(test_ctx, root, 0u, &child, &error);
  if (st != LQL_STATUS_OK || child.kind != LQL_SELECTOR_NODE_CONTAINS) {
    printf("selector AST contains child mismatch: %s\n", error.message);
    ++failures;
  } else {
    memset(&string_term, 0, sizeof(string_term));
    st = test_ctx->selector_node_string_term(test_ctx, child, &string_term,
                                             &error);
    if (st != LQL_STATUS_OK || !view_equals(string_term.field, "/msg") ||
        string_term.value_present || string_term.any_count != 2u) {
      printf("selector AST string term mismatch: %s\n", error.message);
      ++failures;
    }
    st = test_ctx->selector_node_string_term_any(test_ctx, child, 1u, &view,
                                                 &error);
    if (st != LQL_STATUS_OK || !view_equals(view, "timeout")) {
      printf("selector AST string any mismatch: %s\n", error.message);
      ++failures;
    }
  }
  st = test_ctx->selector_node_child(test_ctx, root, 1u, &child, &error);
  if (st != LQL_STATUS_OK || child.kind != LQL_SELECTOR_NODE_RANGE) {
    printf("selector AST numeric range child mismatch: %s\n", error.message);
    ++failures;
  } else {
    memset(&range_term, 0, sizeof(range_term));
    st = test_ctx->selector_node_range_term(test_ctx, child, &range_term,
                                            &error);
    if (st != LQL_STATUS_OK || !view_equals(range_term.field, "/progress") ||
        range_term.gte.kind != LQL_SELECTOR_BOUND_NUMBER ||
        range_term.gte.number != 10.0) {
      printf("selector AST numeric range mismatch: %s\n", error.message);
      ++failures;
    }
  }
  st = test_ctx->selector_node_child(test_ctx, root, 2u, &child, &error);
  if (st != LQL_STATUS_OK || child.kind != LQL_SELECTOR_NODE_RANGE) {
    printf("selector AST datetime range child mismatch: %s\n", error.message);
    ++failures;
  } else {
    memset(&range_term, 0, sizeof(range_term));
    st = test_ctx->selector_node_range_term(test_ctx, child, &range_term,
                                            &error);
    if (st != LQL_STATUS_OK ||
        range_term.lt.kind != LQL_SELECTOR_BOUND_DATETIME ||
        !view_equals(range_term.lt.datetime,
                     "2026-03-05T11:29:41.265+01:00")) {
      printf("selector AST datetime range mismatch: %s\n", error.message);
      ++failures;
    }
  }
  st = test_ctx->selector_node_child(test_ctx, root, 3u, &child, &error);
  if (st != LQL_STATUS_OK || child.kind != LQL_SELECTOR_NODE_DATE) {
    printf("selector AST date child mismatch: %s\n", error.message);
    ++failures;
  } else {
    memset(&date_term, 0, sizeof(date_term));
    st = test_ctx->selector_node_date_term(test_ctx, child, &date_term,
                                           &error);
    if (st != LQL_STATUS_OK || !view_equals(date_term.field, "/timestamp") ||
        !view_equals(date_term.after, "2025-01-01") ||
        !view_equals(date_term.before, "2025-01-03")) {
      printf("selector AST date term mismatch: %s\n", error.message);
      ++failures;
    }
  }
  st = test_ctx->selector_node_child(test_ctx, root, 4u, &child, &error);
  if (st != LQL_STATUS_OK || child.kind != LQL_SELECTOR_NODE_IN) {
    printf("selector AST in child mismatch: %s\n", error.message);
    ++failures;
  } else {
    memset(&in_term, 0, sizeof(in_term));
    st = test_ctx->selector_node_in_term(test_ctx, child, &in_term, &error);
    if (st != LQL_STATUS_OK || !view_equals(in_term.field, "/env") ||
        in_term.any_count != 2u) {
      printf("selector AST in term mismatch: %s\n", error.message);
      ++failures;
    }
    st = test_ctx->selector_node_in_term_any(test_ctx, child, 0u, &view,
                                             &error);
    if (st != LQL_STATUS_OK || !view_equals(view, "prod")) {
      printf("selector AST in any mismatch: %s\n", error.message);
      ++failures;
    }
  }
  st = test_ctx->selector_node_child(test_ctx, root, 5u, &child, &error);
  if (st != LQL_STATUS_OK || child.kind != LQL_SELECTOR_NODE_EXISTS) {
    printf("selector AST exists child mismatch: %s\n", error.message);
    ++failures;
  } else {
    st = test_ctx->selector_node_exists_path(test_ctx, child, &view, &error);
    if (st != LQL_STATUS_OK || !view_equals(view, "/meta/etag")) {
      printf("selector AST exists path mismatch: %s\n", error.message);
      ++failures;
    }
  }
  st = test_ctx->selector_node_child(test_ctx, root, 6u, &child, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT) {
    printf("selector AST child bounds should fail\n");
    ++failures;
  }
  test_ctx->selector_destroy(test_ctx, selector);
}

static void expect_selector_builder_api(void) {
  lql_selector *all_selector;
  lql_selector *eq_selector;
  lql_selector *range_selector;
  lql_selector *and_selector;
  lql_selector *closed_selector;
  lql_selector *not_selector;
  lql_selector *contains_selector;
  lql_selector *contains_value_selector;
  lql_selector *datetime_range_selector;
  lql_selector *date_selector;
  lql_selector *in_selector;
  lql_selector *exists_selector;
  lql_selector *selector;
  const lql_selector *children[2];
  lql_selector_string_term string_term;
  lql_selector_range_term range_term;
  lql_selector_date_term date_term;
  lql_selector_in_term in_term;
  lql_string_view any_values[2];
  lql_selector_node root;
  lql_error error;
  lql_status st;
  int matched;

  all_selector = NULL;
  eq_selector = NULL;
  range_selector = NULL;
  and_selector = NULL;
  closed_selector = NULL;
  not_selector = NULL;
  contains_selector = NULL;
  contains_value_selector = NULL;
  datetime_range_selector = NULL;
  date_selector = NULL;
  in_selector = NULL;
  exists_selector = NULL;
  selector = NULL;

  lql_error_init(&error);
  st = test_ctx->selector_build_all(test_ctx, &all_selector, &error);
  if (st != LQL_STATUS_OK || all_selector == NULL ||
      !test_ctx->selector_is_empty(test_ctx, all_selector)) {
    printf("selector_build_all mismatch: %s\n", error.message);
    ++failures;
  }
  expect_selector_handle_json("build all", all_selector, "{}");

  memset(&string_term, 0, sizeof(string_term));
  string_term.field = view_from_cstr("/status");
  string_term.value_present = 1;
  string_term.value = view_from_cstr("open");
  lql_error_init(&error);
  st = test_ctx->selector_build_string(
      test_ctx, LQL_SELECTOR_NODE_EQ, &string_term, NULL, &eq_selector,
      &error);
  if (st != LQL_STATUS_OK || eq_selector == NULL) {
    printf("selector_build_string eq failed: %s\n", error.message);
    ++failures;
  } else {
    matched = 0;
    st = test_ctx->matches_json(test_ctx, eq_selector, "{\"status\":\"open\"}",
                                strlen("{\"status\":\"open\"}"), &matched,
                                &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("selector_build_string eq match mismatch: %s\n", error.message);
      ++failures;
    }
    expect_selector_handle_json(
        "build eq", eq_selector,
        "{\"eq\":{\"field\":\"/status\",\"value\":\"open\"}}");
  }

  memset(&range_term, 0, sizeof(range_term));
  range_term.field = view_from_cstr("/progress");
  range_term.gte.kind = LQL_SELECTOR_BOUND_NUMBER;
  range_term.gte.number = 10.0;
  lql_error_init(&error);
  st = test_ctx->selector_build_range(test_ctx, &range_term, &range_selector,
                                      &error);
  if (st != LQL_STATUS_OK || range_selector == NULL) {
    printf("selector_build_range numeric failed: %s\n", error.message);
    ++failures;
  }

  children[0] = eq_selector;
  children[1] = range_selector;
  lql_error_init(&error);
  st = test_ctx->selector_build_compound(
      test_ctx, LQL_SELECTOR_NODE_AND, children, 2u, &and_selector, &error);
  if (st != LQL_STATUS_OK || and_selector == NULL) {
    printf("selector_build_compound failed: %s\n", error.message);
    ++failures;
  } else {
    matched = 0;
    st = test_ctx->matches_json(
        test_ctx, and_selector, "{\"status\":\"open\",\"progress\":25}",
        strlen("{\"status\":\"open\",\"progress\":25}"), &matched, &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("selector_build_compound match mismatch: %s\n", error.message);
      ++failures;
    }
    matched = 1;
    st = test_ctx->matches_json(
        test_ctx, and_selector, "{\"status\":\"open\",\"progress\":5}",
        strlen("{\"status\":\"open\",\"progress\":5}"), &matched, &error);
    if (st != LQL_STATUS_OK || matched) {
      printf("selector_build_compound reject mismatch: %s\n", error.message);
      ++failures;
    }
  }

  memset(&string_term, 0, sizeof(string_term));
  string_term.field = view_from_cstr("/status");
  string_term.value_present = 1;
  string_term.value = view_from_cstr("closed");
  lql_error_init(&error);
  st = test_ctx->selector_build_string(
      test_ctx, LQL_SELECTOR_NODE_EQ, &string_term, NULL, &closed_selector,
      &error);
  if (st != LQL_STATUS_OK || closed_selector == NULL) {
    printf("selector_build_string closed failed: %s\n", error.message);
    ++failures;
  }
  lql_error_init(&error);
  st = test_ctx->selector_build_not(test_ctx, closed_selector, &not_selector,
                                    &error);
  if (st != LQL_STATUS_OK || not_selector == NULL) {
    printf("selector_build_not failed: %s\n", error.message);
    ++failures;
  } else {
    matched = 0;
    st = test_ctx->matches_json(test_ctx, not_selector, "{\"status\":\"open\"}",
                                strlen("{\"status\":\"open\"}"), &matched,
                                &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("selector_build_not match mismatch: %s\n", error.message);
      ++failures;
    }
  }

  memset(&string_term, 0, sizeof(string_term));
  string_term.field = view_from_cstr("/msg");
  string_term.ignore_case = 1;
  string_term.any_count = 2u;
  any_values[0] = view_from_cstr("WARN");
  any_values[1] = view_from_cstr("timeout");
  lql_error_init(&error);
  st = test_ctx->selector_build_string(
      test_ctx, LQL_SELECTOR_NODE_CONTAINS, &string_term, any_values,
      &contains_selector, &error);
  if (st != LQL_STATUS_OK || contains_selector == NULL) {
    printf("selector_build_string contains any failed: %s\n", error.message);
    ++failures;
  } else {
    matched = 0;
    st = test_ctx->matches_json(test_ctx, contains_selector,
                                "{\"msg\":\"warn before timeout\"}",
                                strlen("{\"msg\":\"warn before timeout\"}"),
                                &matched, &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("selector_build_string contains any mismatch: %s\n",
             error.message);
      ++failures;
    }
  }

  memset(&string_term, 0, sizeof(string_term));
  string_term.field = view_from_cstr("/msg");
  string_term.value = view_from_cstr("needle");
  lql_error_init(&error);
  st = test_ctx->selector_build_string(
      test_ctx, LQL_SELECTOR_NODE_CONTAINS, &string_term, NULL,
      &contains_value_selector, &error);
  if (st != LQL_STATUS_OK || contains_value_selector == NULL) {
    printf("selector_build_string contains value failed: %s\n",
           error.message);
    ++failures;
  } else {
    expect_selector_handle_json(
        "build contains value without explicit value_present",
        contains_value_selector,
        "{\"contains\":{\"field\":\"/msg\",\"value\":\"needle\"}}");
  }

  memset(&range_term, 0, sizeof(range_term));
  range_term.field = view_from_cstr("/timestamp");
  range_term.gte.kind = LQL_SELECTOR_BOUND_DATETIME;
  range_term.gte.datetime = view_from_cstr(" 2026-03-05T10:28:21Z ");
  range_term.lt.kind = LQL_SELECTOR_BOUND_DATETIME;
  range_term.lt.datetime = view_from_cstr("2026-03-05T10:30:00Z");
  lql_error_init(&error);
  st = test_ctx->selector_build_range(test_ctx, &range_term,
                                      &datetime_range_selector, &error);
  if (st != LQL_STATUS_OK || datetime_range_selector == NULL) {
    printf("selector_build_range datetime failed: %s\n", error.message);
    ++failures;
  } else {
    matched = 0;
    st = test_ctx->matches_json(
        test_ctx, datetime_range_selector,
        "{\"timestamp\":\"2026-03-05T10:29:00Z\"}",
        strlen("{\"timestamp\":\"2026-03-05T10:29:00Z\"}"), &matched,
        &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("selector_build_range datetime mismatch: %s\n", error.message);
      ++failures;
    }
    expect_selector_handle_json(
        "build datetime range", datetime_range_selector,
        "{\"range\":{\"field\":\"/timestamp\",\"gte\":\"2026-03-05T10:28:21Z\",\"lt\":\"2026-03-05T10:30:00Z\"}}");
  }

  memset(&date_term, 0, sizeof(date_term));
  date_term.field = view_from_cstr("/timestamp");
  date_term.after = view_from_cstr("2025-01-01");
  date_term.before = view_from_cstr("2025-01-03");
  lql_error_init(&error);
  st = test_ctx->selector_build_date(test_ctx, &date_term, &date_selector,
                                     &error);
  if (st != LQL_STATUS_OK || date_selector == NULL) {
    printf("selector_build_date failed: %s\n", error.message);
    ++failures;
  } else {
    matched = 0;
    st = test_ctx->matches_json(
        test_ctx, date_selector, "{\"timestamp\":\"2025-01-02T00:00:00Z\"}",
        strlen("{\"timestamp\":\"2025-01-02T00:00:00Z\"}"), &matched, &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("selector_build_date match mismatch: %s\n", error.message);
      ++failures;
    }
  }

  memset(&in_term, 0, sizeof(in_term));
  in_term.field = view_from_cstr("/env");
  in_term.any_count = 2u;
  any_values[0] = view_from_cstr("prod");
  any_values[1] = view_from_cstr("stage");
  lql_error_init(&error);
  st = test_ctx->selector_build_in(test_ctx, &in_term, any_values,
                                   &in_selector, &error);
  if (st != LQL_STATUS_OK || in_selector == NULL) {
    printf("selector_build_in failed: %s\n", error.message);
    ++failures;
  } else {
    matched = 0;
    st = test_ctx->matches_json(test_ctx, in_selector, "{\"env\":\"stage\"}",
                                strlen("{\"env\":\"stage\"}"), &matched,
                                &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("selector_build_in match mismatch: %s\n", error.message);
      ++failures;
    }
  }

  lql_error_init(&error);
  st = test_ctx->selector_build_exists(test_ctx, view_from_cstr("/meta/etag"),
                                       &exists_selector, &error);
  if (st != LQL_STATUS_OK || exists_selector == NULL) {
    printf("selector_build_exists failed: %s\n", error.message);
    ++failures;
  } else {
    memset(&root, 0, sizeof(root));
    st = test_ctx->selector_root(test_ctx, exists_selector, &root, &error);
    if (st != LQL_STATUS_OK || root.kind != LQL_SELECTOR_NODE_EXISTS) {
      printf("selector_build_exists traversal mismatch: %s\n", error.message);
      ++failures;
    }
    matched = 0;
    st = test_ctx->matches_json(test_ctx, exists_selector,
                                "{\"meta\":{\"etag\":\"abc\"}}",
                                strlen("{\"meta\":{\"etag\":\"abc\"}}"),
                                &matched, &error);
    if (st != LQL_STATUS_OK || !matched) {
      printf("selector_build_exists match mismatch: %s\n", error.message);
      ++failures;
    }
  }

  lql_error_init(&error);
  st = test_ctx->selector_build_not(test_ctx, NULL, &selector, &error);
  if (st != LQL_STATUS_INVALID_ARGUMENT) {
    printf("selector_build_not invalid mismatch: %s\n", error.message);
    ++failures;
  }
  memset(&range_term, 0, sizeof(range_term));
  range_term.field = view_from_cstr("/progress");
  lql_error_init(&error);
  st = test_ctx->selector_build_range(test_ctx, &range_term, &selector,
                                      &error);
  if (st != LQL_STATUS_PARSE_ERROR ||
      strcmp(error.message, "range selector requires at least one bound") !=
          0) {
    printf("selector_build_range invalid mismatch: %s\n", error.message);
    ++failures;
  }
  memset(&string_term, 0, sizeof(string_term));
  string_term.field = view_from_cstr("/status");
  string_term.any_count = 1u;
  any_values[0] = view_from_cstr("open");
  lql_error_init(&error);
  st = test_ctx->selector_build_string(
      test_ctx, LQL_SELECTOR_NODE_EQ, &string_term, any_values, &selector,
      &error);
  if (st != LQL_STATUS_PARSE_ERROR ||
      strcmp(error.message, "selector operator does not support any") != 0) {
    printf("selector_build_string invalid mismatch: %s\n", error.message);
    ++failures;
  }

  test_ctx->selector_destroy(test_ctx, all_selector);
  test_ctx->selector_destroy(test_ctx, eq_selector);
  test_ctx->selector_destroy(test_ctx, range_selector);
  test_ctx->selector_destroy(test_ctx, and_selector);
  test_ctx->selector_destroy(test_ctx, closed_selector);
  test_ctx->selector_destroy(test_ctx, not_selector);
  test_ctx->selector_destroy(test_ctx, contains_selector);
  test_ctx->selector_destroy(test_ctx, contains_value_selector);
  test_ctx->selector_destroy(test_ctx, datetime_range_selector);
  test_ctx->selector_destroy(test_ctx, date_selector);
  test_ctx->selector_destroy(test_ctx, in_selector);
  test_ctx->selector_destroy(test_ctx, exists_selector);
}

int main(void) {
  lql_error error;

  lql_error_init(&error);
  if (lql_new(&test_ctx, &error) != LQL_STATUS_OK || test_ctx == NULL) {
    printf("test receiver setup failed: %s\n", error.message);
    return 1;
  }
  expect_sdk_contract_manifest();
  expect_receiver_api();
  expect_public_utility_api();
  expect_output_state_contract_api();
  expect_handle_ownership_contract_api();
  expect_selector_match_api();
  expect_selector_wildcard_path_api();
  expect_selector_string_term_semantics_api();
  expect_selector_logical_composition_api();
  expect_selector_omitted_string_path_api();
  expect_selector_parse_equivalence_api();
  expect_selector_quoted_parse_api();
  expect_selector_any_or_equivalence_api();
  expect_selector_match_all_alias_api();
  expect_selector_or_api();
  expect_selector_inspection_api();
  expect_selector_ast_api();
  expect_selector_builder_api();
  expect_selector_parse_error_api();
  expect_version_api();
  expect_stream_file();
  expect_stream_numeric_path_segments();
  expect_stream_escaped_json_pointer_segments();
  expect_stream_mixed_scalar_candidates();
  expect_source_stream();
  expect_stream_large_irrelevant_scalar_api();
  expect_source_spooled_payload_api();
  expect_stream_array_items();
  expect_stream_nested_array_items();
  expect_stream_stop_controls();
  expect_stream_error_api();
  expect_stream_error_corpus_api();
  expect_seekable_payload_api();
  expect_projection_api();
  expect_projection_parse_normalization_api();
  expect_buffered_projection_api();
  expect_source_projection_api();
  expect_projection_path_invariant_api();
  expect_projection_parse_error_corpus_api();
  expect_projection_compact_error_api();
  expect_compact_api();
  expect_compact_error_corpus_api();
  expect_mutation_plan_api();
  expect_mutation_error_api();
  expect_root_field_mutation_api();
  expect_path_mutation_api();
  expect_buffered_mutation_api();
  expect_buffered_wildcard_mutation_api();
  expect_source_mutation_api();
  expect_file_range_candidate_mutation_api();
  expect_source_candidate_mutation_api();
  expect_projected_candidate_mutation_api();
  expect_mutation_quoted_value_api();
  expect_mutation_file_backed_value_api();
  expect_mutation_file_backed_text_validation_api();
  expect_mutation_shorthand_api();
  expect_array_element_mutation_api();
  expect_wildcard_mutation_api();
  expect_wildcard_remove_mutation_api();
  expect_wildcard_mutation_error_precedence_api();
  expect_recursive_mutation_api();
  expect_array_wildcard_value_mutation_api();
  test_ctx->destroy(test_ctx);
  test_ctx = NULL;
  return failures == 0 ? 0 : 1;
}
