#include <lql/lql.h>

#include <string.h>

typedef struct test_reader {
  const unsigned char *data;
  size_t len;
  size_t offset;
  size_t chunk_size;
} test_reader;

typedef struct test_decisions {
  size_t count;
  size_t matches;
  int stop_after_first;
} test_decisions;

static lql_status test_read(void *user, unsigned char *buffer, size_t capacity,
                            size_t *out_len, lql_error *error) {
  test_reader *reader;
  size_t remaining;
  size_t amount;
  (void)error;
  reader = (test_reader *)user;
  if (out_len == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out_len = 0u;
  if (reader == NULL || buffer == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (reader->offset == reader->len) {
    return LQL_STATUS_OK;
  }
  remaining = reader->len - reader->offset;
  amount = remaining < capacity ? remaining : capacity;
  if (reader->chunk_size != 0u && amount > reader->chunk_size) {
    amount = reader->chunk_size;
  }
  memcpy(buffer, reader->data + reader->offset, amount);
  reader->offset += amount;
  *out_len = amount;
  return LQL_STATUS_OK;
}

static lql_stream_callback_result test_decide(void *user,
                                               const lql_stream_decision *value,
                                               lql_error *error) {
  test_decisions *decisions;
  (void)error;
  decisions = (test_decisions *)user;
  if (decisions == NULL || value == NULL) {
    return LQL_STREAM_CALLBACK_ERROR;
  }
  ++decisions->count;
  if (value->matched) {
    ++decisions->matches;
  }
  return decisions->stop_after_first && decisions->count == 1u
             ? LQL_STREAM_CALLBACK_STOP
             : LQL_STREAM_CALLBACK_CONTINUE;
}

static int run_status_selection(lql *ctx) {
  static const char input[] =
      "{\"status\":\"open\"}\n{\"status\":\"closed\"}\n"
      "{\"status\":42}\n{\"status\":{\"nested\":\"open\"}}\n"
      "{\"status\":\"openx\"}\n{\"status\":\"open\"}\n42\n";
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_decisions decisions;

  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 3u;
  memset(&decisions, 0, sizeof(decisions));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.on_decision = test_decide;
  request.decision_user = &decisions;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 7u || result.records_matched != 2u ||
      result.bytes_consumed != reader.len || result.stopped_early ||
      decisions.count != 7u || decisions.matches != 2u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_conjunction_selection(lql *ctx) {
  static const char input[] =
      "{\"status\":\"open\",\"region\":\"us-west\"}\n"
      "{\"status\":\"open\",\"region\":\"eu-north\"}\n"
      "{\"status\":\"closed\",\"region\":\"us-west\"}\n";
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;

  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\",/region=\"us-west\"",
                          &selector, &error) != LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 5u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 3u || result.records_matched != 1u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_or_selection(lql *ctx) {
  static const char input[] =
      "{\"status\":\"open\",\"region\":\"eu-north\"}\n"
      "{\"status\":\"closed\",\"region\":\"us-west\"}\n"
      "{\"status\":\"closed\",\"region\":\"eu-north\"}\n";
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;

  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "or./status=\"open\",or./region=\"us-west\"",
                          &selector, &error) != LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 3u || result.records_matched != 2u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_not_selection(lql *ctx) {
  static const char input[] =
      "{\"status\":\"open\"}\n{\"status\":\"closed\"}\n42\n";
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;

  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "not./status=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 3u || result.records_matched != 2u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_match_all(lql *ctx) {
  static const char input[] = "{\"ignored\":[1,2]}\n42\n";
  static const char root_array[] = "[1]\n";
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;

  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/", &selector, &error) != LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 2u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)root_array;
  reader.len = sizeof(root_array) - 1u;
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_JSON_ERROR) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_stop_and_root_array(lql *ctx) {
  static const char input[] =
      "{\"status\":\"open\"}\n{\"status\":\"open\"}\n";
  static const char root_array[] = "[{\"status\":\"open\"}]\n";
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_decisions decisions;

  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  memset(&decisions, 0, sizeof(decisions));
  decisions.stop_after_first = 1;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.on_decision = test_decide;
  request.decision_user = &decisions;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      !result.stopped_early || result.stop_reason != LQL_STREAM_STOP_CALLBACK ||
      result.records_seen != 1u || decisions.count != 1u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)root_array;
  reader.len = sizeof(root_array) - 1u;
  request.reader_user = &reader;
  request.on_decision = NULL;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_JSON_ERROR) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

int main(void) {
  lql *ctx;
  lql_error error;

  lql_error_init(&error);
  if (lql_new(&ctx, &error) != LQL_STATUS_OK) {
    return 1;
  }
  if (run_status_selection(ctx) || run_conjunction_selection(ctx) ||
      run_or_selection(ctx) ||
      run_not_selection(ctx) ||
      run_match_all(ctx) ||
      run_stop_and_root_array(ctx)) {
    ctx->destroy(ctx);
    return 1;
  }
  ctx->destroy(ctx);
  return 0;
}
