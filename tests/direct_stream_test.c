#include <lql/lql.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Historical semantic tests intentionally exercise the explicit spooled API. */
#define lql_stream_execute lql_stream_execute_spooled

typedef struct test_reader {
  const unsigned char *data;
  size_t len;
  size_t offset;
  size_t chunk_size;
  size_t range_writes;
} test_reader;

typedef struct test_file_source {
  const unsigned char *data;
  size_t len;
  size_t chunk_size;
  test_reader reader;
  size_t opens;
  size_t closes;
  int oversize_read;
} test_file_source;

typedef struct test_cancel_state {
  size_t checks;
  size_t stop_at;
} test_cancel_state;

typedef struct test_decisions {
  size_t count;
  size_t matches;
  int stop_after_first;
} test_decisions;

typedef struct test_writer {
  unsigned char data[1024];
  size_t len;
} test_writer;

typedef struct test_value_sink {
  test_writer *writer;
  size_t count;
  int stop_after_first;
  int fail_after_first;
} test_value_sink;

static int read_tmpfile(FILE *file, char *buffer, size_t capacity,
                        size_t *out_len) {
  long len;
  if (file == NULL || buffer == NULL || capacity == 0u || out_len == NULL ||
      fflush(file) != 0 || (len = ftell(file)) < 0L ||
      (unsigned long)len >= (unsigned long)capacity ||
      fseek(file, 0L, SEEK_SET) != 0) {
    return 1;
  }
  *out_len = fread(buffer, 1u, (size_t)len, file);
  if (*out_len != (size_t)len || ferror(file)) {
    return 1;
  }
  buffer[*out_len] = '\0';
  return 0;
}

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

static lql_status test_file_oversize_read(void *user, unsigned char *buffer,
                                          size_t capacity, size_t *out_len,
                                          lql_error *error) {
  (void)user;
  (void)buffer;
  (void)error;
  if (out_len == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  *out_len = capacity + 1u;
  return LQL_STATUS_OK;
}

static lql_status test_file_open(void *user, lql_string_view path,
                                 lql_stream_reader_fn *out_reader,
                                 void **out_reader_user, lql_error *error) {
  test_file_source *source;
  (void)error;
  source = (test_file_source *)user;
  if (source == NULL || out_reader == NULL || out_reader_user == NULL ||
      path.len != strlen("virtual:payload") ||
      memcmp(path.data, "virtual:payload", path.len) != 0) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&source->reader, 0, sizeof(source->reader));
  source->reader.data = source->data;
  source->reader.len = source->len;
  source->reader.chunk_size = source->chunk_size;
  ++source->opens;
  *out_reader = source->oversize_read ? test_file_oversize_read : test_read;
  *out_reader_user = &source->reader;
  return LQL_STATUS_OK;
}

static void test_file_close(void *user, void *reader_user) {
  test_file_source *source;
  source = (test_file_source *)user;
  if (source != NULL && reader_user == &source->reader)
    ++source->closes;
}

static time_t test_fixed_clock(void *user) { return *(const time_t *)user; }

static int test_cancel(void *user) {
  test_cancel_state *state;
  state = (test_cancel_state *)user;
  if (state == NULL)
    return 0;
  ++state->checks;
  return state->checks >= state->stop_at;
}

static lql_status test_write(void *user, const void *data, size_t len,
                             lql_error *error) {
  test_writer *writer;
  (void)error;
  writer = (test_writer *)user;
  if (writer == NULL || data == NULL ||
      len > sizeof(writer->data) - writer->len) {
    return LQL_STATUS_CALLBACK_ERROR;
  }
  memcpy(writer->data + writer->len, data, len);
  writer->len += len;
  return LQL_STATUS_OK;
}

static lql_status test_stop_read(void *user, unsigned char *buffer,
                                 size_t capacity, size_t *out_len,
                                 lql_error *error) {
  (void)user;
  (void)buffer;
  (void)capacity;
  (void)error;
  if (out_len != NULL) {
    *out_len = 0u;
  }
  return LQL_STATUS_STOP;
}

static lql_status test_stop_write(void *user, const void *data, size_t len,
                                  lql_error *error) {
  (void)user;
  (void)data;
  (void)len;
  (void)error;
  return LQL_STATUS_STOP;
}

static lql_status test_range_write(void *user, size_t offset, size_t len,
                                   lql_stream_writer_fn writer,
                                   void *writer_user, lql_error *error) {
  test_reader *reader;
  reader = (test_reader *)user;
  if (reader == NULL || writer == NULL || offset > reader->len ||
      len > reader->len - offset) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  ++reader->range_writes;
  return writer(writer_user, reader->data + offset, len, error);
}

static lql_stream_callback_result
test_decide(void *user, const lql_stream_decision *value, lql_error *error) {
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

static lql_stream_callback_result
test_value(void *user, const lql_stream_value *value, lql_error *error) {
  test_value_sink *sink;
  sink = (test_value_sink *)user;
  if (sink == NULL || sink->writer == NULL ||
      lql_stream_value_write_to(value, test_write, sink->writer, error) !=
          LQL_STATUS_OK)
    return LQL_STREAM_CALLBACK_ERROR;
  ++sink->count;
  if (sink->fail_after_first && sink->count == 1u) {
    return LQL_STREAM_CALLBACK_ERROR;
  }
  if (sink->stop_after_first && sink->count == 1u) {
    return LQL_STREAM_CALLBACK_STOP;
  }
  return LQL_STREAM_CALLBACK_CONTINUE;
}

static int run_selection(lql *ctx, const char *expr, const char *input,
                         size_t expected_records, size_t expected_matches) {
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;

  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, expr, &selector, &error) != LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = strlen(input);
  reader.chunk_size = 1u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != expected_records ||
      result.records_matched != expected_matches) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int selector_matches(lql *ctx, lql_selector *selector, const char *input,
                            size_t expected_records, size_t expected_matches) {
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;

  lql_error_init(&error);
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = strlen(input);
  reader.chunk_size = 1u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK) {
    return 0;
  }
  return result.records_seen == expected_records &&
         result.records_matched == expected_matches;
}

static int run_json_selector_selection(lql *ctx, const char *json_selector,
                                       const char *input,
                                       size_t expected_records,
                                       size_t expected_matches) {
  lql_selector *selector;
  lql_error error;
  int ok;

  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse_json(ctx, json_selector, strlen(json_selector),
                               &selector, &error) != LQL_STATUS_OK) {
    return 1;
  }
  ok = selector_matches(ctx, selector, input, expected_records,
                        expected_matches);
  ctx->selector_destroy(ctx, selector);
  return !ok;
}

static int run_scalar_json_semantic_regressions(lql *ctx) {
  static const char scalar_text_input[] = "{\"v\":1}\n"
                                          "{\"v\":\"1\"}\n"
                                          "{\"v\":true}\n"
                                          "{\"v\":\"true\"}\n"
                                          "{\"v\":null}\n"
                                          "{\"v\":\"null\"}\n"
                                          "{\"v\":false}\n"
                                          "{\"v\":\"false\"}\n"
                                          "{\"v\":\"x\"}\n";
  static const char typed_json_selector[] =
      "{\"and\":[{\"eq\":{\"field\":\"/n\",\"value\":1}},"
      "{\"eq\":{\"field\":\"/b\",\"value\":true}},"
      "{\"eq\":{\"field\":\"/z\",\"value\":null}},"
      "{\"in\":{\"field\":\"/tag\",\"any\":[1,false,null,\"x\"]}}]}";
  static const char typed_json_input[] =
      "{\"n\":1,\"b\":true,\"z\":null,\"tag\":1}\n"
      "{\"n\":\"1\",\"b\":true,\"z\":null,\"tag\":1}\n"
      "{\"n\":1,\"b\":\"true\",\"z\":null,\"tag\":1}\n"
      "{\"n\":1,\"b\":true,\"z\":\"null\",\"tag\":1}\n"
      "{\"n\":1,\"b\":true,\"z\":null,\"tag\":\"1\"}\n"
      "{\"n\":1,\"b\":true,\"z\":null,\"tag\":false}\n"
      "{\"n\":1,\"b\":true,\"z\":null,\"tag\":null}\n"
      "{\"n\":1,\"b\":true,\"z\":null,\"tag\":\"x\"}\n";
  static const char long_number_input[] =
      "{\"n\":0."
      "0000000000000000000000000000000000000000000000000000000000000000"
      "0000000000000000000000000000000000000000000000000000000000000000"
      "0001}\n";
  static const char numeric_text_input[] = "{\"v\":1}\n"
                                           "{\"v\":1.0}\n"
                                           "{\"v\":\"1\"}\n"
                                           "{\"v\":\"1.0\"}\n"
                                           "{\"v\":10}\n"
                                           "{\"v\":1e1}\n"
                                           "{\"v\":\"10\"}\n"
                                           "{\"v\":\"1e1\"}\n"
                                           "{\"v\":0.1}\n"
                                           "{\"v\":1e-1}\n"
                                           "{\"v\":\"0.1\"}\n"
                                           "{\"v\":\"1e-1\"}\n"
                                           "{\"v\":0}\n"
                                           "{\"v\":\"0\"}\n"
                                           "{\"v\":\"00\"}\n"
                                           "{\"v\":9223372036854775807}\n"
                                           "{\"v\":\"9223372036854776000\"}\n"
                                           "{\"v\":\"9223372036854775807\"}\n";
  static const char exact_number_input[] = "{\"v\":9007199254740992}\n"
                                           "{\"v\":9007199254740993}\n"
                                           "{\"v\":9007199254740992.0}\n"
                                           "{\"v\":9.007199254740993e15}\n";
  static const char numeric_object_key_input[] = "{\"a\":{\"0\":\"x\"}}\n"
                                                 "{\"a\":[\"x\"]}\n"
                                                 "{\"a\":{\"1\":\"x\"}}\n";
  static const char nul_string_input[] = "{\"x\":\"a\"}\n"
                                         "{\"x\":\"a\\u0000b\"}\n";
  static const char nul_string_eq_selector[] =
      "{\"eq\":{\"field\":\"/x\",\"value\":\"a\\u0000b\"}}";
  static const char nul_string_contains_selector[] =
      "{\"contains\":{\"field\":\"/x\",\"any\":[\"a\\u0000b\"]}}";
  static const char nul_string_prefix_selector[] =
      "{\"prefix\":{\"field\":\"/x\",\"value\":\"a\\u0000b\"}}";
  static const char quoted_operator_input[] = "{\"v\":\"a!=b\"}\n"
                                              "{\"v\":\"a>=b\"}\n"
                                              "{\"v\":\"a<=b\"}\n"
                                              "{\"v\":\"a=b\"}\n"
                                              "{\"v\":\"a>b\"}\n"
                                              "{\"v\":\"a<b\"}\n";
  static const char quoted_backslash_input[] = "{\"v\":\"a\\\\b\"}\n"
                                               "{\"v\":\"ab\"}\n"
                                               "{\"v\":\"a\\\\,b\"}\n"
                                               "{\"v\":\"a\\\\'b\"}\n";
  lql_selector *selector;
  lql_error error;

  /*
   * liblql intentionally diverges from Go lql here: unquoted JSON scalar
   * selector values are typed, quoted values are strings, and numeric equality
   * compares JSON numbers by value rather than by source spelling.
   */
  if (run_selection(ctx, "/v=1", scalar_text_input, 9u, 1u) ||
      run_selection(ctx, "/v=\"1\"", scalar_text_input, 9u, 1u) ||
      run_selection(ctx, "/v='1'", scalar_text_input, 9u, 1u) ||
      run_selection(ctx, "/v=true", scalar_text_input, 9u, 1u) ||
      run_selection(ctx, "/v=\"true\"", scalar_text_input, 9u, 1u) ||
      run_selection(ctx, "/v='true'", scalar_text_input, 9u, 1u) ||
      run_selection(ctx, "/v=false", scalar_text_input, 9u, 1u) ||
      run_selection(ctx, "/v=\"false\"", scalar_text_input, 9u, 1u) ||
      run_selection(ctx, "/v='false'", scalar_text_input, 9u, 1u) ||
      run_selection(ctx, "/v=null", scalar_text_input, 9u, 1u) ||
      run_selection(ctx, "/v=\"null\"", scalar_text_input, 9u, 1u) ||
      run_selection(ctx, "/v='null'", scalar_text_input, 9u, 1u) ||
      run_selection(ctx, "in{field=/v,any=1|true|null|false|x}",
                    scalar_text_input, 9u, 5u)) {
    return 1;
  }

  /*
   * Numeric equality is semantic for JSON numbers: source forms such as 10 and
   * 1e1 compare equal.  The selector parser still uses JSON number syntax, so
   * bare 00 is a string selector value and only matches the JSON string "00".
   */
  if (run_selection(ctx, "/v=1", numeric_text_input, 18u, 2u) ||
      run_selection(ctx, "/v=1.0", numeric_text_input, 18u, 2u) ||
      run_selection(ctx, "/v=\"1.0\"", numeric_text_input, 18u, 1u) ||
      run_selection(ctx, "/v='1.0'", numeric_text_input, 18u, 1u) ||
      run_selection(ctx, "/v=1e1", numeric_text_input, 18u, 2u) ||
      run_selection(ctx, "/v=\"1e1\"", numeric_text_input, 18u, 1u) ||
      run_selection(ctx, "/v='1e1'", numeric_text_input, 18u, 1u) ||
      run_selection(ctx, "/v=1e-1", numeric_text_input, 18u, 2u) ||
      run_selection(ctx, "/v=00", numeric_text_input, 18u, 1u) ||
      run_selection(ctx, "/v=9223372036854775807", numeric_text_input, 18u,
                    1u) ||
      run_selection(ctx, "in{field=/v,any=1.0|1e1|00}", numeric_text_input, 18u,
                    5u)) {
    return 1;
  }
  if (run_selection(ctx, "/v=9007199254740992", exact_number_input, 4u, 2u) ||
      run_selection(ctx, "/v=9007199254740993", exact_number_input, 4u, 2u) ||
      run_selection(ctx, "range{field=/v,gt=9007199254740992}",
                    exact_number_input, 4u, 2u) ||
      run_selection(ctx, "range{field=/v,lte=9007199254740992}",
                    exact_number_input, 4u, 2u)) {
    return 1;
  }
  if (run_selection(ctx, "/a/0=\"x\"", numeric_object_key_input, 3u, 2u) ||
      run_selection(ctx, "exists{/a/0}", numeric_object_key_input, 3u, 2u)) {
    return 1;
  }
  if (run_json_selector_selection(ctx, nul_string_eq_selector, nul_string_input,
                                  2u, 1u) ||
      run_json_selector_selection(ctx, nul_string_contains_selector,
                                  nul_string_input, 2u, 1u) ||
      run_json_selector_selection(ctx, nul_string_prefix_selector,
                                  nul_string_input, 2u, 1u)) {
    return 1;
  }
  if (run_selection(ctx, "/v=\"a!=b\"", quoted_operator_input, 6u, 1u) ||
      run_selection(ctx, "/v=\"a>=b\"", quoted_operator_input, 6u, 1u) ||
      run_selection(ctx, "/v=\"a<=b\"", quoted_operator_input, 6u, 1u) ||
      run_selection(ctx, "/v=\"a=b\"", quoted_operator_input, 6u, 1u) ||
      run_selection(ctx, "/v=\"a>b\"", quoted_operator_input, 6u, 1u) ||
      run_selection(ctx, "/v=\"a<b\"", quoted_operator_input, 6u, 1u)) {
    return 1;
  }
  if (run_selection(ctx, "/v='a\\b'", quoted_backslash_input, 4u, 1u) ||
      run_selection(ctx, "/v='a\\,b'", quoted_backslash_input, 4u, 1u) ||
      run_selection(ctx, "/v='a\\'b'", quoted_backslash_input, 4u, 1u)) {
    return 1;
  }

  /*
   * Selector JSON keeps the same typed scalar behavior as selector text.
   */
  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse_json(ctx, typed_json_selector,
                               sizeof(typed_json_selector) - 1u, &selector,
                               &error) != LQL_STATUS_OK ||
      !selector_matches(ctx, selector, typed_json_input, 8u, 4u)) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);

  if (run_selection(ctx, "range{field=/n,gt=0}", long_number_input, 1u, 1u) ||
      run_selection(ctx, "range{field=/n,lte=1}", long_number_input, 1u, 1u)) {
    return 1;
  }
  if (run_selection(ctx, "/v=1e9223372036854775808",
                    "{\"v\":1e9223372036854775808}\n"
                    "{\"v\":1e9223372036854775809}\n",
                    2u, 1u) ||
      run_selection(ctx, "/v>1e9223372036854775808",
                    "{\"v\":1e9223372036854775808}\n"
                    "{\"v\":1e9223372036854775809}\n",
                    2u, 1u) ||
      run_selection(ctx, "/v=1e-9223372036854775808",
                    "{\"v\":1e-9223372036854775808}\n"
                    "{\"v\":1e-9223372036854775809}\n",
                    2u, 1u)) {
    return 1;
  }
  return 0;
}

static int run_long_numeric_selector_regression(lql *ctx) {
  size_t digits_len;
  size_t exp_len;
  char *digits;
  char *expr_eq;
  char *expr_range;
  char *expr_late_range;
  char *expr_late_range_wide;
  char *expr_zero_tail_range;
  char *expr_saturated_exp_range;
  char *expr_adjusted_eq;
  char *expr_adjusted_lt;
  char *expr_adjusted_lte;
  char *expr_huge_negative_gt;
  char *expr_huge_negative_lt;
  char *input_eq;
  char *input_range;
  char *input_late_range;
  char *input_late_range_wide;
  char *input_zero_tail_range;
  char *input_saturated_exp_range;
  char *input_adjusted_exp;
  char *input_huge_negative_exp;
  int failed;

  digits_len = 9000u;
  exp_len = strlen("e-8999");
  digits = (char *)malloc(digits_len + 1u);
  expr_eq = (char *)malloc(strlen("/n=") + digits_len + 1u);
  expr_range = (char *)malloc(strlen("/n>1") + 1u);
  expr_late_range = (char *)malloc(strlen("/n>") + digits_len + 1u);
  expr_late_range_wide = (char *)malloc(strlen("/n>") + 2000u + 1u);
  expr_zero_tail_range = (char *)malloc(strlen("/n>1e8999") + 1u);
  expr_saturated_exp_range =
      (char *)malloc(strlen("/n>9e9223372036854775808") + 1u);
  expr_adjusted_eq = (char *)malloc(strlen("/n=1e9223372036854775808") + 1u);
  expr_adjusted_lt = (char *)malloc(strlen("/n<1e9223372036854775808") + 1u);
  expr_adjusted_lte = (char *)malloc(strlen("/n<=1e9223372036854775808") + 1u);
  expr_huge_negative_gt =
      (char *)malloc(strlen("/n>7e-60716785761073997719") + 1u);
  expr_huge_negative_lt =
      (char *)malloc(strlen("/n<7e-60716785761073997719") + 1u);
  input_eq =
      (char *)malloc(strlen("{\"n\":") + digits_len + strlen("}\n") + 1u);
  input_range = (char *)malloc(strlen("{\"n\":") + digits_len + exp_len +
                               strlen("}\n") + 1u);
  input_late_range =
      (char *)malloc(strlen("{\"n\":") + 10000u + strlen("e-1000}\n") + 1u);
  input_late_range_wide =
      (char *)malloc(strlen("{\"n\":") + 10000u + strlen("e-8000}\n") + 1u);
  input_zero_tail_range =
      (char *)malloc(strlen("{\"n\":") + digits_len + strlen("}\n") + 1u);
  input_saturated_exp_range =
      (char *)malloc(strlen("{\"n\":") + 1u + digits_len +
                     strlen("e9223372036854775808}\n") + 1u);
  input_adjusted_exp = (char *)malloc(strlen("{\"n\":") + 1u + 9000u +
                                      strlen("e9223372036854766808}\n") + 1u);
  input_huge_negative_exp =
      (char *)malloc(strlen("{\"n\":") + 1u + 8200u + strlen("e-") + 1024u +
                     strlen("}\n") + 1u);
  if (digits == NULL || expr_eq == NULL || expr_range == NULL ||
      expr_late_range == NULL || expr_late_range_wide == NULL ||
      expr_zero_tail_range == NULL || expr_saturated_exp_range == NULL ||
      expr_adjusted_eq == NULL || expr_adjusted_lt == NULL ||
      expr_adjusted_lte == NULL || expr_huge_negative_gt == NULL ||
      expr_huge_negative_lt == NULL || input_eq == NULL ||
      input_range == NULL || input_late_range == NULL ||
      input_late_range_wide == NULL || input_zero_tail_range == NULL ||
      input_saturated_exp_range == NULL || input_adjusted_exp == NULL ||
      input_huge_negative_exp == NULL) {
    free(digits);
    free(expr_eq);
    free(expr_range);
    free(expr_late_range);
    free(expr_late_range_wide);
    free(expr_zero_tail_range);
    free(expr_saturated_exp_range);
    free(expr_adjusted_eq);
    free(expr_adjusted_lt);
    free(expr_adjusted_lte);
    free(expr_huge_negative_gt);
    free(expr_huge_negative_lt);
    free(input_eq);
    free(input_range);
    free(input_late_range);
    free(input_late_range_wide);
    free(input_zero_tail_range);
    free(input_saturated_exp_range);
    free(input_adjusted_exp);
    free(input_huge_negative_exp);
    return 1;
  }
  memset(digits, '1', digits_len);
  digits[digits_len] = '\0';
  strcpy(expr_eq, "/n=");
  strcat(expr_eq, digits);
  strcpy(expr_range, "/n>1");
  strcpy(expr_late_range, "/n>");
  strcat(expr_late_range, digits);
  strcpy(expr_late_range_wide, "/n>");
  memset(expr_late_range_wide + strlen(expr_late_range_wide), '1', 2000u);
  expr_late_range_wide[strlen("/n>") + 2000u] = '\0';
  strcpy(expr_zero_tail_range, "/n>1e8999");
  strcpy(expr_saturated_exp_range, "/n>9e9223372036854775808");
  strcpy(expr_adjusted_eq, "/n=1e9223372036854775808");
  strcpy(expr_adjusted_lt, "/n<1e9223372036854775808");
  strcpy(expr_adjusted_lte, "/n<=1e9223372036854775808");
  strcpy(expr_huge_negative_gt, "/n>7e-60716785761073997719");
  strcpy(expr_huge_negative_lt, "/n<7e-60716785761073997719");
  strcpy(input_eq, "{\"n\":");
  strcat(input_eq, digits);
  strcat(input_eq, "}\n");
  strcpy(input_range, "{\"n\":");
  strcat(input_range, digits);
  strcat(input_range, "e-8999}\n");
  strcpy(input_late_range, "{\"n\":");
  memset(input_late_range + strlen(input_late_range), '1', 500u);
  input_late_range[strlen("{\"n\":") + 500u] = '2';
  memset(input_late_range + strlen("{\"n\":") + 501u, '1', 10000u - 501u);
  input_late_range[strlen("{\"n\":") + 10000u] = '\0';
  strcat(input_late_range, "e-1000}\n");
  strcpy(input_late_range_wide, "{\"n\":");
  memset(input_late_range_wide + strlen(input_late_range_wide), '1', 1500u);
  input_late_range_wide[strlen("{\"n\":") + 1500u] = '2';
  memset(input_late_range_wide + strlen("{\"n\":") + 1501u, '1',
         10000u - 1501u);
  input_late_range_wide[strlen("{\"n\":") + 10000u] = '\0';
  strcat(input_late_range_wide, "e-8000}\n");
  strcpy(input_zero_tail_range, "{\"n\":");
  input_zero_tail_range[strlen("{\"n\":")] = '1';
  memset(input_zero_tail_range + strlen("{\"n\":") + 1u, '0', digits_len - 2u);
  input_zero_tail_range[strlen("{\"n\":") + digits_len - 1u] = '1';
  input_zero_tail_range[strlen("{\"n\":") + digits_len] = '\0';
  strcat(input_zero_tail_range, "}\n");
  strcpy(input_saturated_exp_range, "{\"n\":");
  input_saturated_exp_range[strlen("{\"n\":")] = '1';
  memset(input_saturated_exp_range + strlen("{\"n\":") + 1u, '0', digits_len);
  input_saturated_exp_range[strlen("{\"n\":") + 1u + digits_len] = '\0';
  strcat(input_saturated_exp_range, "e9223372036854775808}\n");
  strcpy(input_adjusted_exp, "{\"n\":");
  input_adjusted_exp[strlen("{\"n\":")] = '1';
  memset(input_adjusted_exp + strlen("{\"n\":") + 1u, '0', 9000u);
  input_adjusted_exp[strlen("{\"n\":") + 1u + 9000u] = '\0';
  strcat(input_adjusted_exp, "e9223372036854766808}\n");
  strcpy(input_huge_negative_exp, "{\"n\":");
  input_huge_negative_exp[strlen("{\"n\":")] = '8';
  memset(input_huge_negative_exp + strlen("{\"n\":") + 1u, '1', 8200u);
  input_huge_negative_exp[strlen("{\"n\":") + 1u + 8200u] = '\0';
  strcat(input_huge_negative_exp, "e-");
  memset(input_huge_negative_exp + strlen("{\"n\":") + 1u + 8200u +
             strlen("e-"),
         '5', 1024u);
  input_huge_negative_exp[strlen("{\"n\":") + 1u + 8200u + strlen("e-") +
                          1024u] = '\0';
  strcat(input_huge_negative_exp, "}\n");
  failed =
      run_selection(ctx, expr_eq, input_eq, 1u, 1u) ||
      run_selection(ctx, expr_range, input_range, 1u, 1u) ||
      run_selection(ctx, expr_late_range, input_late_range, 1u, 1u) ||
      run_selection(ctx, expr_late_range_wide, input_late_range_wide, 1u, 1u) ||
      run_selection(ctx, expr_zero_tail_range, input_zero_tail_range, 1u, 1u) ||
      run_selection(ctx, expr_saturated_exp_range, input_saturated_exp_range,
                    1u, 1u) ||
      run_selection(ctx, expr_adjusted_eq, input_adjusted_exp, 1u, 1u) ||
      run_selection(ctx, expr_adjusted_lt, input_adjusted_exp, 1u, 0u) ||
      run_selection(ctx, expr_adjusted_lte, input_adjusted_exp, 1u, 1u) ||
      run_selection(ctx, expr_huge_negative_gt, input_huge_negative_exp, 1u,
                    0u) ||
      run_selection(ctx, expr_huge_negative_lt, input_huge_negative_exp, 1u,
                    1u);
  free(digits);
  free(expr_eq);
  free(expr_range);
  free(expr_late_range);
  free(expr_late_range_wide);
  free(expr_zero_tail_range);
  free(expr_saturated_exp_range);
  free(expr_adjusted_eq);
  free(expr_adjusted_lt);
  free(expr_adjusted_lte);
  free(expr_huge_negative_gt);
  free(expr_huge_negative_lt);
  free(input_eq);
  free(input_range);
  free(input_late_range);
  free(input_late_range_wide);
  free(input_zero_tail_range);
  free(input_saturated_exp_range);
  free(input_adjusted_exp);
  free(input_huge_negative_exp);
  return failed;
}

static int run_huge_numeric_selector_regression(lql *ctx) {
  size_t digits_len;
  size_t prefix_len;
  char *digits;
  char *expr_eq;
  char *expr_shifted_exp_eq;
  char *input_eq;
  char *input_diff;
  char *input_shifted_exp_eq;
  int failed;

  digits_len = 132000u;
  digits = (char *)malloc(digits_len + 1u);
  expr_eq = (char *)malloc(strlen("/n=") + digits_len + 1u);
  expr_shifted_exp_eq = (char *)malloc(strlen("/n=1e") + 8999u + 1u);
  input_eq =
      (char *)malloc(strlen("{\"n\":") + digits_len + strlen("}\n") + 1u);
  input_diff =
      (char *)malloc(strlen("{\"n\":") + digits_len + strlen("}\n") + 1u);
  input_shifted_exp_eq =
      (char *)malloc(strlen("{\"n\":0.1e") + 9000u + strlen("}\n") + 1u);
  if (digits == NULL || expr_eq == NULL || expr_shifted_exp_eq == NULL ||
      input_eq == NULL || input_diff == NULL || input_shifted_exp_eq == NULL) {
    free(digits);
    free(expr_eq);
    free(expr_shifted_exp_eq);
    free(input_eq);
    free(input_diff);
    free(input_shifted_exp_eq);
    return 1;
  }

  memset(digits, '1', digits_len);
  digits[digits_len] = '\0';
  strcpy(expr_eq, "/n=");
  strcat(expr_eq, digits);
  strcpy(input_eq, "{\"n\":");
  strcat(input_eq, digits);
  strcat(input_eq, "}\n");
  strcpy(input_diff, "{\"n\":");
  prefix_len = strlen("{\"n\":");
  memcpy(input_diff + prefix_len, digits, digits_len);
  input_diff[prefix_len + digits_len - 1u] = '2';
  input_diff[prefix_len + digits_len] = '\0';
  strcat(input_diff, "}\n");
  strcpy(expr_shifted_exp_eq, "/n=1e");
  memset(expr_shifted_exp_eq + strlen("/n=1e"), '9', 8999u);
  expr_shifted_exp_eq[strlen("/n=1e") + 8999u] = '\0';
  strcpy(input_shifted_exp_eq, "{\"n\":0.1e1");
  memset(input_shifted_exp_eq + strlen("{\"n\":0.1e1"), '0', 8999u);
  input_shifted_exp_eq[strlen("{\"n\":0.1e1") + 8999u] = '\0';
  strcat(input_shifted_exp_eq, "}\n");

  /*
   * Crosses the 128 KiB numeric capture limit and therefore exercises the
   * streaming summary path. The selector-side bound must be compared
   * incrementally; rescanning it per input digit is quadratic.
   */
  failed =
      run_selection(ctx, expr_eq, input_eq, 1u, 1u) ||
      run_selection(ctx, expr_eq, input_diff, 1u, 0u) ||
      run_selection(ctx, expr_shifted_exp_eq, input_shifted_exp_eq, 1u, 1u);
  free(digits);
  free(expr_eq);
  free(expr_shifted_exp_eq);
  free(input_eq);
  free(input_diff);
  free(input_shifted_exp_eq);
  return failed;
}

static int run_status_selection(lql *ctx) {
  static const char input[] =
      "{\"status\":\"open\"}\n{\"status\":\"closed\"}\n"
      "{\"status\":42}\n{\"status\":{\"nested\":\"open\"}}\n"
      "{\"status\":\"openx\"}\n{\"status\":\"open\"}\n"
      "{\"status\":\"closed\",\"status\":\"open\"}\n"
      "{\"status\":\"open\",\"status\":\"closed\"}\n42\n";
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
      result.records_seen != 9u || result.records_matched != 4u ||
      result.bytes_consumed != reader.len || result.stopped_early ||
      decisions.count != 9u || decisions.matches != 4u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_escaped_pointer_selection(lql *ctx) {
  static const char input[] = "{\"slash/key\":true,\"tilde~key\":true}\n"
                              "{\"slash\":{\"key\":true},\"tilde0key\":true}\n";
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;

  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/slash~1key=true,/tilde~0key=true", &selector,
                          &error) != LQL_STATUS_OK) {
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
      result.records_seen != 2u || result.records_matched != 1u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_post_hit_validation(lql *ctx) {
  static const char malformed_array[] = "{\"status\":\"open\",\"bad\":[1,]}\n";
  static const char malformed_string[] =
      "{\"status\":\"open\",\"bad\":\"unterminated}\n";
  static const unsigned char invalid_utf8[] = {
      '{', '"', 's', 't', 'a', 't', 'u', 's', '"', ':', '"',   'o', 'p', 'e',
      'n', '"', ',', '"', 'b', 'a', 'd', '"', ':', '"', 0xffu, '"', '}', '\n'};
  static const char malformed_and[] =
      "{\"status\":\"open\",\"region\":\"us-west\",\"bad\":[1,]}\n";
  static const char valid_nested[] =
      "{\"status\":\"open\",\"bad\":[1,{\"x\":\"y\"}]}\n";
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
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.selector = selector;
  request.on_decision = test_decide;
  request.decision_user = &decisions;

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)malformed_array;
  reader.len = sizeof(malformed_array) - 1u;
  reader.chunk_size = 2u;
  memset(&decisions, 0, sizeof(decisions));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 0u || result.records_matched != 0u ||
      decisions.count != 0u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)malformed_string;
  reader.len = sizeof(malformed_string) - 1u;
  reader.chunk_size = 3u;
  memset(&decisions, 0, sizeof(decisions));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 0u || result.records_matched != 0u ||
      decisions.count != 0u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = invalid_utf8;
  reader.len = sizeof(invalid_utf8);
  reader.chunk_size = 3u;
  memset(&decisions, 0, sizeof(decisions));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 0u || result.records_matched != 0u ||
      decisions.count != 0u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)valid_nested;
  reader.len = sizeof(valid_nested) - 1u;
  reader.chunk_size = 1u;
  memset(&decisions, 0, sizeof(decisions));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      decisions.count != 1u || decisions.matches != 1u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  ctx->selector_destroy(ctx, selector);
  selector = NULL;

  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\",/region=\"us-west\"",
                          &selector, &error) != LQL_STATUS_OK) {
    return 1;
  }
  request.selector = selector;

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)malformed_and;
  reader.len = sizeof(malformed_and) - 1u;
  reader.chunk_size = 2u;
  memset(&decisions, 0, sizeof(decisions));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 0u || result.records_matched != 0u ||
      decisions.count != 0u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_projection_parse(lql *ctx) {
  static const char *const paths[] = {" /id ", "/meta/trace", "/id"};
  static const char *const conflict[] = {"/meta", "/meta/trace"};
  static const char *const leading_index[] = {"/0/id"};
  lql_projection *projection;
  lql_error error;
  lql_string_view path;

  projection = NULL;
  lql_error_init(&error);
  if (ctx->projection_parse(ctx, paths, 3u, &projection, &error) !=
          LQL_STATUS_OK ||
      ctx->projection_path_count(ctx, projection) != 2u ||
      ctx->projection_path(ctx, projection, 0u, &path, &error) !=
          LQL_STATUS_OK ||
      path.len != 3u || memcmp(path.data, "/id", path.len) != 0 ||
      ctx->projection_path(ctx, projection, 1u, &path, &error) !=
          LQL_STATUS_OK ||
      path.len != 11u || memcmp(path.data, "/meta/trace", path.len) != 0) {
    ctx->projection_destroy(ctx, projection);
    return 1;
  }
  ctx->projection_destroy(ctx, projection);
  projection = NULL;
  if (ctx->projection_parse(ctx, conflict, 2u, &projection, &error) !=
          LQL_STATUS_PARSE_ERROR ||
      projection != NULL ||
      ctx->projection_parse(ctx, leading_index, 1u, &projection, &error) !=
          LQL_STATUS_PARSE_ERROR ||
      projection != NULL) {
    return 1;
  }
  return 0;
}

static int run_selector_json_write(lql *ctx) {
  static const char expected[] = "{\"eq\":{\"field\":\"/status\","
                                 "\"value\":\"open\"}}";
  static const char escaped_expected[] =
      "{\"eq\":{\"field\":\"/msg\",\"value\":\"a\\\"b\\n\"}}";
  static const char typed_expected[] =
      "{\"and\":[{\"eq\":{\"field\":\"/n\",\"value\":1.5}},"
      "{\"eq\":{\"field\":\"/enabled\",\"value\":true}},"
      "{\"eq\":{\"field\":\"/missing\",\"value\":null}},"
      "{\"in\":{\"field\":\"/tag\",\"any\":[1,false,null,\"x\"]}}]}";
  static const char typed_input[] =
      "{\"n\":1.5,\"enabled\":true,\"missing\":null,\"tag\":1}\n"
      "{\"n\":\"1.5\",\"enabled\":true,\"missing\":null,\"tag\":1}\n"
      "{\"n\":1.5,\"enabled\":\"true\",\"missing\":null,\"tag\":1}\n"
      "{\"n\":1.5,\"enabled\":true,\"missing\":\"null\",\"tag\":1}\n"
      "{\"n\":1.5,\"enabled\":true,\"missing\":null,\"tag\":\"1\"}\n"
      "{\"n\":1.5,\"enabled\":true,\"missing\":null,\"tag\":false}\n";
  static const char ne_expected[] =
      "{\"not\":{\"eq\":{\"field\":\"/status\",\"value\":\"open\"}}}";
  static const char ne_input[] =
      "{\"status\":\"open\"}\n{\"status\":\"closed\"}\n{\"status\":null}\n";
  static const char spaced_json[] =
      " \n { \"eq\" : { \"field\" : \"\\/status\" "
      ", \"value\" : \"\\u006fpen\" } } \t";
  lql_selector *selector;
  lql_selector *roundtrip;
  lql_selector_string_term term;
  lql_string_view escaped_value;
  lql_error error;
  FILE *file;
  char buffer[512];
  size_t len;

  selector = NULL;
  roundtrip = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }
  file = tmpfile();
  if (file == NULL) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  if (ctx->selector_write_json(ctx, selector, file, &error) != LQL_STATUS_OK ||
      read_tmpfile(file, buffer, sizeof(buffer), &len) ||
      len != sizeof(expected) - 1u ||
      memcmp(buffer, expected, sizeof(expected) - 1u) != 0 ||
      ctx->selector_parse_json(ctx, buffer, len, &roundtrip, &error) !=
          LQL_STATUS_OK) {
    fclose(file);
    if (roundtrip != NULL) {
      ctx->selector_destroy(ctx, roundtrip);
    }
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  fclose(file);
  ctx->selector_destroy(ctx, roundtrip);
  roundtrip = NULL;
  ctx->selector_destroy(ctx, selector);
  selector = NULL;

  memset(&term, 0, sizeof(term));
  term.field.data = "/msg";
  term.field.len = 4u;
  escaped_value.data = "a\"b\n";
  escaped_value.len = 4u;
  term.value = escaped_value;
  term.value_present = 1;
  if (ctx->selector_build_string(ctx, LQL_SELECTOR_NODE_EQ, &term, NULL,
                                 &selector, &error) != LQL_STATUS_OK) {
    return 1;
  }
  file = tmpfile();
  if (file == NULL) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  if (ctx->selector_write_json(ctx, selector, file, &error) != LQL_STATUS_OK ||
      read_tmpfile(file, buffer, sizeof(buffer), &len) ||
      len != sizeof(escaped_expected) - 1u ||
      memcmp(buffer, escaped_expected, sizeof(escaped_expected) - 1u) != 0 ||
      ctx->selector_parse_json(ctx, buffer, len, &roundtrip, &error) !=
          LQL_STATUS_OK) {
    fclose(file);
    if (roundtrip != NULL) {
      ctx->selector_destroy(ctx, roundtrip);
    }
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  fclose(file);
  ctx->selector_destroy(ctx, roundtrip);
  ctx->selector_destroy(ctx, selector);
  roundtrip = NULL;
  selector = NULL;

  if (ctx->selector_parse_json(ctx, spaced_json, sizeof(spaced_json) - 1u,
                               &selector, &error) != LQL_STATUS_OK) {
    return 1;
  }
  file = tmpfile();
  if (file == NULL) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  if (ctx->selector_write_json(ctx, selector, file, &error) != LQL_STATUS_OK ||
      read_tmpfile(file, buffer, sizeof(buffer), &len) ||
      len != sizeof(expected) - 1u ||
      memcmp(buffer, expected, sizeof(expected) - 1u) != 0) {
    fclose(file);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  fclose(file);
  ctx->selector_destroy(ctx, selector);
  selector = NULL;

  if (ctx->selector_parse(ctx,
                          "/n=1.5,/enabled=true,/missing=null,in{field=/"
                          "tag,any=1|false|null|x}",
                          &selector, &error) != LQL_STATUS_OK) {
    return 1;
  }
  file = tmpfile();
  if (file == NULL) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  if (ctx->selector_write_json(ctx, selector, file, &error) != LQL_STATUS_OK ||
      read_tmpfile(file, buffer, sizeof(buffer), &len) ||
      len != sizeof(typed_expected) - 1u ||
      memcmp(buffer, typed_expected, sizeof(typed_expected) - 1u) != 0 ||
      ctx->selector_parse_json(ctx, buffer, len, &roundtrip, &error) !=
          LQL_STATUS_OK ||
      !selector_matches(ctx, roundtrip, typed_input, 6u, 2u)) {
    fclose(file);
    if (roundtrip != NULL) {
      ctx->selector_destroy(ctx, roundtrip);
    }
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  fclose(file);
  ctx->selector_destroy(ctx, roundtrip);
  ctx->selector_destroy(ctx, selector);
  roundtrip = NULL;
  selector = NULL;

  if (ctx->selector_parse(ctx, "/status!=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }
  file = tmpfile();
  if (file == NULL) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  if (ctx->selector_write_json(ctx, selector, file, &error) != LQL_STATUS_OK ||
      read_tmpfile(file, buffer, sizeof(buffer), &len) ||
      len != sizeof(ne_expected) - 1u ||
      memcmp(buffer, ne_expected, sizeof(ne_expected) - 1u) != 0 ||
      ctx->selector_parse_json(ctx, buffer, len, &roundtrip, &error) !=
          LQL_STATUS_OK ||
      !selector_matches(ctx, roundtrip, ne_input, 3u, 2u)) {
    fclose(file);
    if (roundtrip != NULL) {
      ctx->selector_destroy(ctx, roundtrip);
    }
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  fclose(file);
  ctx->selector_destroy(ctx, roundtrip);
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
  static const char inequality_input[] =
      "{\"status\":\"open\"}\n{\"status\":\"closed\"}\n{\"region\":\"eu\"}\n";
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
  selector = NULL;
  if (ctx->selector_parse(ctx, "/status!=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 2;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)inequality_input;
  reader.len = sizeof(inequality_input) - 1u;
  reader.chunk_size = 1u;
  request.reader_user = &reader;
  request.selector = selector;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 3u || result.records_matched != 2u) {
    ctx->selector_destroy(ctx, selector);
    return 3;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_mapped_string_predicates(lql *ctx) {
  static const char input[] =
      "{\"status\":\"open\",\"msg\":\"Error: Timeout while reading\","
      "\"service\":\"Auth-Service\",\"env\":\"prod\",\"a\":true}\n"
      "{\"status\":\"pending\",\"msg\":\"timeout\",\"service\":\"Auth\","
      "\"env\":\"dev\",\"a\":false}\n"
      "{\"status\":\"closed\",\"msg\":\"all clear\",\"service\":\"Other\","
      "\"env\":\"stage\",\"a\":null}\n";
  static const char nested_input[] =
      "{\"items\":[{\"sku\":\"A\",\"price\":5},{\"sku\":\"B\",\"price\":20}],"
      "\"meta\":{\"etag\":\"x\"}}\n"
      "{\"items\":[{\"sku\":\"B\",\"price\":30}],\"meta\":{\"etag\":null}}\n"
      "{\"items\":[{\"sku\":\"C\",\"price\":1}]}\n";
  static const char temporal_input[] =
      "{\"timestamp\":\"2026-03-05T10:29:00+01:00\"}\n"
      "{\"timestamp\":\"2026-03-05T10:29:00Z\"}\n"
      "{\"timestamp\":\"2026-03-05\"}\n";
  static const char temporal_fraction_input[] =
      "{\"timestamp\":\"2026-03-11T01:11:28.123456789Z\"}\n"
      "{\"timestamp\":\"2026-03-11T01:11:28.123+01:00\"}\n"
      "{\"timestamp\":\"2026-03-11T00:11:28.123Z\"}\n"
      "{\"timestamp\":\"2026-03-11T01:11:28Z\"}\n";
  static const char wildcard_input[] =
      "{\"items\":[{\"sku\":\"A\"},{\"sku\":\"B\"}],"
      "\"object\":{\"0\":{\"state\":\"open\"}},"
      "\"array\":[{\"state\":\"open\"}],"
      "\"tree\":{\"branch\":{\"deep\":{\"sku\":\"needle\"}}}}\n"
      "{\"items\":[{\"sku\":\"B\"}],\"object\":{},\"array\":[],"
      "\"tree\":{\"branch\":{\"sku\":\"other\"}}}\n"
      "{\"items\":[{\"sku\":\"C\"}],\"object\":{},\"array\":[],"
      "\"tree\":{}}\n";
  static const char scalar_input[] =
      "{\"code\":1,\"enabled\":true}\n"
      "{\"code\":2,\"enabled\":false}\n"
      "{\"code\":1.0,\"enabled\":false}\n"
      "{\"code\":1,\"code\":2,\"enabled\":false}\n";
  static const char array_scalar_input[] =
      "{\"values\":[\"A\",\"B\"],\"codes\":[1,2]}\n"
      "{\"values\":[\"B\",\"A\"],\"codes\":[2,3]}\n"
      "{\"values\":[\"A\"],\"codes\":[1]}\n";
  static const char array_scalar_exists_input[] = "{\"values\":[null,\"B\"]}\n"
                                                  "{\"values\":[\"B\",null]}\n"
                                                  "{\"values\":[\"A\"]}\n";
  static const char null_input[] = "{\"empty\":null,\"code\":1}\n"
                                   "{\"empty\":false,\"code\":2}\n"
                                   "{\"empty\":\"null\",\"code\":3}\n";
  static const char root_wildcard_input[] =
      "{\"alpha\":{\"state\":\"open\"},\"beta\":{\"state\":\"closed\"}}\n"
      "{\"alpha\":{\"state\":\"closed\"}}\n"
      "42\n";
  static const char terminal_wildcard_input[] =
      "{\"status\":\"open\",\"region\":\"us-west\"}\n"
      "{\"status\":\"closed\",\"region\":\"open\"}\n"
      "{\"status\":\"closed\",\"region\":\"eu-north\"}\n"
      "42\n";
  static const char object_path_input[] =
      "{\"me\\u0074a\":{\"sta\\u0074e\":\"open\",\"code\":1},\"ignored\":[1]}\n"
      "{\"meta\":{\"state\":\"closed\",\"code\":2}}\n"
      "{\"meta\":{\"state\":\"closed\",\"state\":\"open\",\"code\":1}}\n"
      "{\"meta\":[{\"state\":\"open\"}]}\n";
  if (run_selection(ctx, "contains{f=/msg,a=Timeout|degraded}", input, 3u,
                    1u)) {
    return 1;
  }
  if (run_selection(ctx, "icontains{f=/msg,v=TIME}", input, 3u, 2u)) {
    return 100;
  }
  if (run_selection(ctx, "icontains{f=/msg,v=\303\245}",
                    "{\"msg\":\"\303\205ngstr\303\266m\"}\n"
                    "{\"msg\":\"other\"}\n",
                    2u, 1u)) {
    return 101;
  }
  if (run_selection(ctx, "icontains{f=/msg,v=k}",
                    "{\"msg\":\"\342\204\252ey\"}\n", 1u, 1u)) {
    return 102;
  }
  if (run_selection(ctx, "prefix{f=/service,v=Auth}", input, 3u, 2u)) {
    return 2;
  }
  if (run_selection(ctx, "iprefix{f=/service,v=auth}", input, 3u, 2u)) {
    return 103;
  }
  if (run_selection(ctx, "prefix{f=/service,v=\303\245,ic=true}",
                    "{\"service\":\"\303\205ngstr\303\266m\"}\n"
                    "{\"service\":\"other\"}\n",
                    2u, 1u)) {
    return 104;
  }
  if (run_selection(ctx, "eq{f=/msg}", input, 3u, 0u) ||
      run_selection(ctx, "contains{f=/msg}", input, 3u, 3u) ||
      run_selection(ctx, "icontains{f=/msg}", input, 3u, 3u) ||
      run_selection(ctx, "prefix{f=/msg}", input, 3u, 3u) ||
      run_selection(ctx, "iprefix{f=/msg}", input, 3u, 3u)) {
    return 105;
  }
  if (run_selection(ctx, "contains{f=/msg,v=\"\"}", input, 3u, 3u) ||
      run_selection(ctx, "icontains{f=/msg,v=\"\"}", input, 3u, 3u) ||
      run_selection(ctx, "iprefix{f=/service,v=\"\"}", input, 3u, 3u) ||
      run_selection(ctx, "contains{f=/a,v=\"\"}", input, 3u, 0u)) {
    return 111;
  }
  if (run_selection(ctx, "in{f=/env,a=prod|stage}", input, 3u, 2u)) {
    return 3;
  }
  if (run_selection(ctx, "exists{/a}", input, 3u, 2u)) {
    return 4;
  }
  if (run_selection(ctx,
                    "contains{f=/msg,a=Timeout|degraded},"
                    "prefix{f=/service,v=Auth},in{f=/env,a=prod|stage},"
                    "exists{/a}",
                    input, 3u, 1u)) {
    return 5;
  }
  if (run_selection(ctx, "or./status=\"open\",or./status=\"pending\"", input,
                    3u, 2u)) {
    return 6;
  }
  if (run_selection(ctx, "/items/1/sku=\"B\"", nested_input, 3u, 1u)) {
    return 7;
  }
  if (run_selection(ctx, "exists{/meta/etag}", nested_input, 3u, 1u)) {
    return 8;
  }
  if (run_selection(ctx, "range{field=/items/1/price,gte=20}", nested_input, 3u,
                    1u)) {
    return 9;
  }
  if (run_selection(ctx, "/timestamp=\"2026-03-05T09:29:00Z\"", temporal_input,
                    3u, 2u)) {
    return 10;
  }
  if (run_selection(ctx, "range{field=/timestamp,gte=2026-03-05T10:28:21Z}",
                    temporal_input, 3u, 1u)) {
    return 11;
  }
  if (run_selection(ctx,
                    "date{field=/timestamp,after=2026-03-05T10:28:21Z,"
                    "before=2026-03-05T10:30:00Z}",
                    temporal_input, 3u, 1u)) {
    return 12;
  }
  if (run_selection(ctx, "/timestamp=\"2026-03-11T01:11:28.123456789\"",
                    temporal_fraction_input, 4u, 1u) ||
      run_selection(ctx, "/timestamp=\"2026-03-11T00:11:28.123Z\"",
                    temporal_fraction_input, 4u, 2u) ||
      run_selection(ctx,
                    "date{field=/timestamp,after=2026-03-11T01:11:28.122Z,"
                    "before=2026-03-11T01:11:28.124Z}",
                    temporal_fraction_input, 4u, 1u)) {
    return 106;
  }
  if (run_selection(ctx, "date{f=/timestamp,since=yesterday}",
                    "{\"timestamp\":\"2999-01-01T00:00:00Z\"}\n"
                    "{\"timestamp\":\"2000-01-01T00:00:00Z\"}\n",
                    2u, 1u)) {
    return 105;
  }
  if (run_selection(ctx, "/items[]/sku=\"B\"", wildcard_input, 3u, 2u)) {
    return 13;
  }
  if (run_selection(ctx, "/object/*/state=\"open\"", wildcard_input, 3u, 1u) ||
      run_selection(ctx, "/array/*/state=\"open\"", wildcard_input, 3u, 0u) ||
      run_selection(ctx, "/array/[]/state=\"open\"", wildcard_input, 3u, 1u)) {
    return 14;
  }
  if (run_selection(ctx, "/tree/**/sku=\"other\"", wildcard_input, 3u, 1u) ||
      run_selection(ctx, "/tree/.../sku=\"needle\"", wildcard_input, 3u, 1u)) {
    return 15;
  }
  if (run_selection(ctx, "/status!=\"open\"", input, 3u, 2u)) {
    return 110;
  }
  if (run_selection(ctx, "/code=1", scalar_input, 4u, 3u) ||
      run_selection(ctx, "in{field=/code,any=1|2}", scalar_input, 4u, 4u) ||
      run_selection(ctx, "/enabled=true", scalar_input, 4u, 1u)) {
    return 16;
  }
  if (run_selection(ctx, "/values/1=\"B\"", array_scalar_input, 3u, 1u) ||
      run_selection(ctx, "/values/[]=\"B\"", array_scalar_input, 3u, 2u) ||
      run_selection(ctx, "range{field=/codes/1,gte=2}", array_scalar_input, 3u,
                    2u)) {
    return 108;
  }
  if (run_selection(ctx, "exists{/values/1}", array_scalar_exists_input, 3u,
                    1u) ||
      run_selection(ctx, "exists{/values/[]}", array_scalar_exists_input, 3u,
                    3u)) {
    return 109;
  }
  if (run_selection(ctx, "/empty=null", null_input, 3u, 1u) ||
      run_selection(ctx, "in{field=/empty,any=null|false}", null_input, 3u,
                    2u)) {
    return 107;
  }
  if (run_selection(ctx, "/meta/state=\"open\"", object_path_input, 4u, 2u) ||
      run_selection(ctx, "/meta/code=1", object_path_input, 4u, 2u) ||
      run_selection(ctx, "exists{/meta/state}", object_path_input, 4u, 3u)) {
    return 18;
  }
  if (run_selection(ctx, "/*/state=\"open\"", root_wildcard_input, 3u, 1u)) {
    return 17;
  }
  if (run_selection(ctx, "/*=\"open\"", terminal_wildcard_input, 4u, 2u)) {
    return 106;
  }
  return 0;
}

static int run_long_contains_regression(lql *ctx) {
  char *needle;
  char *upper_needle;
  char *contains_expr;
  char *icontains_expr;
  char *input;
  size_t needle_len;
  int failed;

  needle_len = 300u;
  needle = (char *)malloc(needle_len + 1u);
  upper_needle = (char *)malloc(needle_len + 1u);
  contains_expr =
      (char *)malloc(strlen("contains{f=/msg,v=\"\"}") + needle_len + 1u);
  icontains_expr =
      (char *)malloc(strlen("icontains{f=/msg,v=\"\"}") + needle_len + 1u);
  input = (char *)malloc(strlen("{\"msg\":\"x") + needle_len +
                         strlen("y\"}\n{\"msg\":\"short\"}\n") + 1u);
  if (needle == NULL || upper_needle == NULL || contains_expr == NULL ||
      icontains_expr == NULL || input == NULL) {
    free(needle);
    free(upper_needle);
    free(contains_expr);
    free(icontains_expr);
    free(input);
    return 1;
  }
  memset(needle, 'a', needle_len);
  needle[needle_len] = '\0';
  memset(upper_needle, 'A', needle_len);
  upper_needle[needle_len] = '\0';
  strcpy(contains_expr, "contains{f=/msg,v=\"");
  strcat(contains_expr, needle);
  strcat(contains_expr, "\"}");
  strcpy(icontains_expr, "icontains{f=/msg,v=\"");
  strcat(icontains_expr, upper_needle);
  strcat(icontains_expr, "\"}");
  strcpy(input, "{\"msg\":\"x");
  strcat(input, needle);
  strcat(input, "y\"}\n{\"msg\":\"short\"}\n");
  failed = run_selection(ctx, contains_expr, input, 2u, 1u) ||
           run_selection(ctx, icontains_expr, input, 2u, 1u);
  free(needle);
  free(upper_needle);
  free(contains_expr);
  free(icontains_expr);
  free(input);
  return failed;
}

static int run_match_all(lql *ctx) {
  static const char input[] = "{\"ignored\":[1,2]}\n42\n";
  static const char selected_input[] = " { \"a\" : 1 }\n true\n";
  static const char selected_output[] = "{\"a\":1}\ntrue\n";
  static const char object_input[] = " { \"a\" : 1, \"b\" : 2 }\n"
                                     "{\"a\":3,\"c\":4}\n";
  static const char projection_output[] = "{\"a\":1}\n{\"a\":3}\n";
  static const char mutation_output[] =
      "{\"a\":1,\"b\":2,\"t\":true}\n{\"a\":3,\"c\":4,\"t\":true}\n";
  static const char *const projection_paths[] = {"/a"};
  static const char *const mutations[] = {"/t=true"};
  static const char root_array[] = "[1]\n";
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;

  selector = NULL;
  projection = NULL;
  mutation = NULL;
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
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 1u;
  request.reader_user = &reader;
  request.selector = NULL;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK) {
    ctx->selector_destroy(ctx, selector);
    return 20;
  }
  if (result.records_seen != 2u) {
    ctx->selector_destroy(ctx, selector);
    return 21;
  }
  if (result.records_matched != 2u) {
    ctx->selector_destroy(ctx, selector);
    return 22;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)selected_input;
  reader.len = sizeof(selected_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 2u ||
      writer.len != sizeof(selected_output) - 1u ||
      memcmp(writer.data, selected_output, writer.len) != 0) {
    ctx->selector_destroy(ctx, selector);
    return 3;
  }
  if (ctx->projection_parse(ctx, projection_paths, 1u, &projection, &error) !=
      LQL_STATUS_OK) {
    ctx->selector_destroy(ctx, selector);
    return 30;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)object_input;
  reader.len = sizeof(object_input) - 1u;
  reader.chunk_size = 3u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.selector = NULL;
  request.projection = projection;
  request.output_mode = LQL_STREAM_OUTPUT_PROJECTION;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 2u ||
      writer.len != sizeof(projection_output) - 1u ||
      memcmp(writer.data, projection_output, writer.len) != 0) {
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 31;
  }
  ctx->projection_destroy(ctx, projection);
  projection = NULL;
  if (ctx->mutation_parse(ctx, mutations, 1u, &mutation, &error) !=
      LQL_STATUS_OK) {
    ctx->selector_destroy(ctx, selector);
    return 40;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)object_input;
  reader.len = sizeof(object_input) - 1u;
  reader.chunk_size = 3u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.selector = NULL;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 2u ||
      writer.len != sizeof(mutation_output) - 1u ||
      memcmp(writer.data, mutation_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 41;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  memset(&request, 0, sizeof(request));
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)root_array;
  reader.len = sizeof(root_array) - 1u;
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = NULL;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
      LQL_STATUS_JSON_ERROR) {
    ctx->selector_destroy(ctx, selector);
    return 4;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_root_wildcard_array_error(lql *ctx) {
  static const char root_array[] = "[{\"state\":\"open\"}]\n";
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;

  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/*/state=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)root_array;
  reader.len = sizeof(root_array) - 1u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
      LQL_STATUS_JSON_ERROR) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_selected_record_output(lql *ctx) {
  static const char input[] = "{ \"status\" : \"open\", \"n\" : 1 }\n"
                              "{\"status\":\"closed\",\"n\":2}\n";
  static const char matched_only[] = "{\"status\":\"open\",\"n\":1}\n";
  static const char ne_matched_only[] = "{\"status\":\"closed\",\"n\":2}\n";
  static const char all_records[] = "{\"status\":\"open\",\"n\":1}\n"
                                    "{\"status\":\"closed\",\"n\":2}\n";
  static const char *const projection_paths[] = {"/n", "/status"};
  static const char projection_output[] = "{\"n\":1,\"status\":\"open\"}\n";
  static const char quote_input[] = "{\"status\":\"open\",\"a\\\"b\":1}\n";
  static const char quote_output[] = "{\"a\\\"b\":1}\n";
  static const char *const quote_paths[] = {"/a\"b"};
  static const char nested_input[] =
      "{\"meta\":{\"type\":\"log\",\"trace\":9,\"trace\":10},"
      "\"id\":\"x\",\"status\":\"open\",\"unused\":true}\n";
  static const char nested_output[] =
      "{\"id\":\"x\",\"meta\":{\"trace\":10,\"type\":\"log\"}}\n";
  static const char *const nested_paths[] = {"/meta/type", "/id",
                                             "/meta/trace"};
  lql_selector *selector;
  lql_projection *projection;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;

  selector = NULL;
  projection = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 3u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.selector = selector;
  request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(matched_only) - 1u ||
      memcmp(writer.data, matched_only, writer.len) != 0) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  selector = NULL;
  if (ctx->selector_parse(ctx, "/status!=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 3u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.writer_user = &writer;
  request.selector = selector;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(ne_matched_only) - 1u ||
      memcmp(writer.data, ne_matched_only, writer.len) != 0) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  selector = NULL;
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 5u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.writer_user = &writer;
  request.selector = selector;
  request.matched_only = 0;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(all_records) - 1u ||
      memcmp(writer.data, all_records, writer.len) != 0) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  if (ctx->projection_parse(ctx, projection_paths, 2u, &projection, &error) !=
      LQL_STATUS_OK) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.output_mode = LQL_STREAM_OUTPUT_PROJECTION;
  request.projection = projection;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(projection_output) - 1u ||
      memcmp(writer.data, projection_output, writer.len) != 0) {
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->projection_destroy(ctx, projection);
  projection = NULL;
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)quote_input;
  reader.len = sizeof(quote_input) - 1u;
  reader.chunk_size = 1u;
  memset(&writer, 0, sizeof(writer));
  if (ctx->projection_parse(ctx, quote_paths, 1u, &projection, &error) !=
      LQL_STATUS_OK) {
    ctx->selector_destroy(ctx, selector);
    return 2;
  }
  request.reader_user = &reader;
  request.projection = projection;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(quote_output) - 1u ||
      memcmp(writer.data, quote_output, writer.len) != 0) {
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 3;
  }
  ctx->projection_destroy(ctx, projection);
  projection = NULL;
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)nested_input;
  reader.len = sizeof(nested_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  if (ctx->projection_parse(ctx, nested_paths, 3u, &projection, &error) !=
      LQL_STATUS_OK) {
    ctx->selector_destroy(ctx, selector);
    return 4;
  }
  request.reader_user = &reader;
  request.projection = projection;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(nested_output) - 1u ||
      memcmp(writer.data, nested_output, writer.len) != 0) {
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 5;
  }
  ctx->projection_destroy(ctx, projection);
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_value_callback(lql *ctx) {
  static const char input[] = "{\"status\":\"open\",\"n\":1}\n"
                              "{\"status\":\"closed\",\"n\":2}\n";
  static const char malformed[] = "{\"status\":\"open\",\"n\":1}\n"
                                  "{\"status\":}\n";
  static const char expected[] = "{\"status\":\"open\",\"n\":1}";
  static const char whitespace[] = "{ \"status\" : \"open\", \"n\" : 1 }\n";
  static const char compacted[] = "{\"status\":\"open\",\"n\":1}";
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;
  test_value_sink sink;
  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
      LQL_STATUS_OK)
    return 1;
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 3u;
  memset(&writer, 0, sizeof(writer));
  memset(&sink, 0, sizeof(sink));
  sink.writer = &writer;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.on_value = test_value;
  request.value_user = &sink;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      sink.count != 1u || writer.len != sizeof(expected) - 1u ||
      memcmp(writer.data, expected, writer.len) != 0) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  reader.range_writes = 0u;
  memset(&writer, 0, sizeof(writer));
  memset(&sink, 0, sizeof(sink));
  sink.writer = &writer;
  request.range_writer = test_range_write;
  request.range_user = &reader;
  request.input_is_compact = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      sink.count != 1u || reader.range_writes != 1u ||
      writer.len != sizeof(expected) - 1u ||
      memcmp(writer.data, expected, writer.len) != 0) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)whitespace;
  reader.len = sizeof(whitespace) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  memset(&sink, 0, sizeof(sink));
  sink.writer = &writer;
  request.reader_user = &reader;
  request.range_user = &reader;
  request.input_is_compact = 0;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      sink.count != 1u || reader.range_writes != 0u ||
      writer.len != sizeof(compacted) - 1u ||
      memcmp(writer.data, compacted, writer.len) != 0) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  request.range_writer = NULL;
  request.range_user = NULL;
  reader.offset = 0u;
  reader.data = (const unsigned char *)malformed;
  reader.len = sizeof(malformed) - 1u;
  memset(&writer, 0, sizeof(writer));
  memset(&sink, 0, sizeof(sink));
  sink.writer = &writer;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 1u || result.records_matched != 1u ||
      sink.count != 1u || writer.len != sizeof(expected) - 1u ||
      memcmp(writer.data, expected, writer.len) != 0) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_value_callback_control(lql *ctx) {
  static const char input[] = "{\"status\":\"open\"}\n"
                              "{\"status\":\"open\"}\n";
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;
  test_value_sink sink;

  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 1u;
  memset(&writer, 0, sizeof(writer));
  memset(&sink, 0, sizeof(sink));
  sink.writer = &writer;
  sink.stop_after_first = 1;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.on_value = test_value;
  request.value_user = &sink;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      !result.stopped_early || result.stop_reason != LQL_STREAM_STOP_CALLBACK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      sink.count != 1u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  memset(&sink, 0, sizeof(sink));
  sink.writer = &writer;
  sink.fail_after_first = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_CALLBACK_ERROR ||
      result.records_seen != 1u || result.records_matched != 1u ||
      sink.count != 1u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_nested_projection_output(lql *ctx) {
  static const char input[] =
      "{\"id\":\"x\",\"meta\":{\"trace\":9,\"ignore\":1},"
      "\"items\":[{\"sku\":\"A\"},{\"sku\":\"B\"}],"
      "\"slash/key\":true}\n";
  static const char *const paths[] = {"/meta/trace", "/items/1/sku", "/id",
                                      "/slash~1key"};
  static const char output[] = "{\"id\":\"x\",\"items\":[null,{\"sku\":\"B\"}],"
                               "\"meta\":{\"trace\":9},\"slash/key\":true}\n";
  static const char *const missing_paths[] = {"/missing"};
  static const char *const scalar_array_paths[] = {"/values/1"};
  static const char scalar_array_input[] =
      "{\"values\":[\"A\",\"B\",\"C\"],\"status\":\"open\"}\n";
  static const char scalar_array_output[] = "{\"values\":[null,\"B\"]}\n";
  static const char *const mixed_array_paths[] = {"/a/0", "/a/x"};
  static const char mixed_array_input[] = "{\"a\":[10,20]}\n";
  static const char mixed_array_output[] = "{\"a\":[10]}\n";
  static const char mixed_object_input[] = "{\"a\":{\"0\":1,\"x\":2}}\n";
  static const char scalar[] = "1\n";
  lql_selector *selector;
  lql_projection *projection;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;

  selector = NULL;
  projection = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "", &selector, &error) != LQL_STATUS_OK ||
      ctx->projection_parse(ctx, paths, 4u, &projection, &error) !=
          LQL_STATUS_OK) {
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 5u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.selector = selector;
  request.projection = projection;
  request.output_mode = LQL_STREAM_OUTPUT_PROJECTION;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(output) - 1u ||
      memcmp(writer.data, output, writer.len) != 0) {
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)scalar;
  reader.len = sizeof(scalar) - 1u;
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
      LQL_STATUS_JSON_ERROR) {
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->projection_destroy(ctx, projection);
  projection = NULL;
  if (ctx->projection_parse(ctx, missing_paths, 1u, &projection, &error) !=
      LQL_STATUS_OK) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 5u;
  memset(&writer, 0, sizeof(writer));
  request.projection = projection;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != 0u) {
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->projection_destroy(ctx, projection);
  projection = NULL;
  if (ctx->projection_parse(ctx, scalar_array_paths, 1u, &projection, &error) !=
      LQL_STATUS_OK) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)scalar_array_input;
  reader.len = sizeof(scalar_array_input) - 1u;
  reader.chunk_size = 5u;
  memset(&writer, 0, sizeof(writer));
  request.projection = projection;
  request.reader_user = &reader;
  request.writer_user = &writer;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(scalar_array_output) - 1u ||
      memcmp(writer.data, scalar_array_output, writer.len) != 0) {
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->projection_destroy(ctx, projection);
  projection = NULL;
  if (ctx->projection_parse(ctx, mixed_array_paths, 2u, &projection, &error) !=
      LQL_STATUS_OK) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)mixed_array_input;
  reader.len = sizeof(mixed_array_input) - 1u;
  reader.chunk_size = 5u;
  memset(&writer, 0, sizeof(writer));
  request.projection = projection;
  request.reader_user = &reader;
  request.writer_user = &writer;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(mixed_array_output) - 1u ||
      memcmp(writer.data, mixed_array_output, writer.len) != 0) {
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)mixed_object_input;
  reader.len = sizeof(mixed_object_input) - 1u;
  reader.chunk_size = 5u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.writer_user = &writer;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
      LQL_STATUS_JSON_ERROR) {
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->projection_destroy(ctx, projection);
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_mutation_output(lql *ctx) {
  static const char input[] = "{\"status\":\"open\",\"n\":1}\n"
                              "{\"status\":\"closed\",\"n\":2}\n";
  static const char *const increment[] = {"/n=+2"};
  static const char increment_output[] = "{\"status\":\"open\",\"n\":3}\n";
  static const char increment_all_output[] =
      "{\"status\":\"open\",\"n\":3}\n"
      "{\"status\":\"closed\",\"n\":2}\n";
  static const char *const increment_missing[] = {"/count=+2"};
  static const char increment_missing_output[] =
      "{\"status\":\"open\",\"n\":1,\"count\":2}\n";
  static const char *const increment_multi[] = {"/n=+2", "/n=+3"};
  static const char increment_multi_output[] =
      "{\"status\":\"open\",\"n\":6}\n";
  static const char *const increment_multi_missing[] = {"/count=+2",
                                                        "/count=+3"};
  static const char increment_multi_missing_output[] =
      "{\"status\":\"open\",\"n\":1,\"count\":5}\n";
  static const char *const nested_increment[] = {"/meta/count=+1"};
  static const char nested_increment_input[] =
      "{\"status\":\"open\",\"meta\":{\"count\":2}}\n"
      "{\"status\":\"open\",\"meta\":{\"count\":4}}\n";
  static const char nested_increment_output[] =
      "{\"status\":\"open\",\"meta\":{\"count\":3}}\n"
      "{\"status\":\"open\",\"meta\":{\"count\":5}}\n";
  static const char nested_increment_missing_input[] =
      "{\"status\":\"open\",\"meta\":{}}\n";
  static const char nested_increment_missing_output[] =
      "{\"status\":\"open\",\"meta\":{\"count\":1}}\n";
  static const char nested_increment_scalar_input[] =
      "{\"status\":\"open\",\"meta\":null}\n";
  static const char nested_increment_scalar_output[] =
      "{\"status\":\"open\",\"meta\":{\"count\":1}}\n";
  static const char *const direct_set[] = {"/bench/touched=true"};
  static const char direct_set_output[] =
      "{\"status\":\"open\",\"n\":1,\"bench\":{\"touched\":true}}\n";
  static const char nested_set_input[] =
      "{\"status\":\"open\",\"bench\":{\"old\":1}}\n";
  static const char nested_set_output[] =
      "{\"status\":\"open\",\"bench\":{\"old\":1,\"touched\":true}}\n";
  static const char nested_replace_input[] =
      "{\"status\":\"open\",\"bench\":{\"touched\":false,\"old\":1}}\n";
  static const char nested_replace_output[] =
      "{\"status\":\"open\",\"bench\":{\"touched\":true,\"old\":1}}\n";
  static const char nested_scalar_input[] =
      "{\"status\":\"open\",\"bench\":1}\n";
  static const char nested_scalar_output[] =
      "{\"status\":\"open\",\"bench\":{\"touched\":true}}\n";
  static const char *const nested_remove[] = {"rm:/bench/touched"};
  static const char nested_remove_input[] =
      "{\"status\":\"open\",\"bench\":{\"old\":1,\"touched\":true}}\n";
  static const char nested_remove_output[] =
      "{\"status\":\"open\",\"bench\":{\"old\":1}}\n";
  static const char nested_remove_missing_input[] =
      "{\"status\":\"open\",\"bench\":{\"old\":1}}\n";
  static const char nested_remove_missing_output[] =
      "{\"status\":\"open\",\"bench\":{\"old\":1}}\n";
  static const char nested_remove_scalar_input[] =
      "{\"status\":\"open\",\"bench\":1}\n";
  static const char nested_remove_scalar_output[] =
      "{\"status\":\"open\",\"bench\":1}\n";
  static const char *const nested_array_remove[] = {"rm:/bench/0/touched"};
  static const char nested_array_remove_input[] =
      "{\"status\":\"open\",\"bench\":[{\"touched\":true,\"old\":1},"
      "{\"touched\":true}]}\n";
  static const char nested_array_remove_output[] =
      "{\"status\":\"open\",\"bench\":[{\"old\":1},{\"touched\":true}]}\n";
  static const char *const nested_array_increment[] = {"/bench/0/touched=+1"};
  static const char nested_array_increment_input[] =
      "{\"status\":\"open\",\"bench\":[{\"touched\":1}]}\n";
  static const char nested_array_increment_output[] =
      "{\"status\":\"open\",\"bench\":{\"0\":{\"touched\":1}}}\n";
  static const char *const deep_set[] = {"/voucher/lines/10/bench=true"};
  static const char deep_set_input[] =
      "{\"status\":\"open\",\"voucher\":{\"lines\":{\"10\":{\"old\":1}}}}\n";
  static const char deep_set_output[] =
      "{\"status\":\"open\",\"voucher\":{\"lines\":{\"10\":{\"old\":1,"
      "\"bench\":true}}}}\n";
  static const char deep_set_missing_input[] = "{\"status\":\"open\"}\n";
  static const char deep_set_missing_output[] =
      "{\"status\":\"open\",\"voucher\":{\"lines\":{\"10\":{\"bench\":true}}}}"
      "\n";
  static const char deep_set_scalar_input[] =
      "{\"status\":\"open\",\"voucher\":{\"lines\":1}}\n";
  static const char deep_set_scalar_output[] =
      "{\"status\":\"open\",\"voucher\":{\"lines\":{\"10\":{\"bench\":true}}}}"
      "\n";
  static const char *const deep_remove[] = {"rm:/voucher/lines/10/bench"};
  static const char deep_remove_input[] =
      "{\"status\":\"open\",\"voucher\":{\"lines\":{\"10\":{\"old\":1,"
      "\"bench\":true}}}}\n";
  static const char deep_remove_output[] =
      "{\"status\":\"open\",\"voucher\":{\"lines\":{\"10\":{\"old\":1}}}}\n";
  static const char deep_remove_missing_input[] =
      "{\"status\":\"open\",\"voucher\":{\"lines\":{\"10\":{\"old\":1}}}}\n";
  static const char deep_remove_missing_output[] =
      "{\"status\":\"open\",\"voucher\":{\"lines\":{\"10\":{\"old\":1}}}}\n";
  static const char deep_remove_scalar_input[] =
      "{\"status\":\"open\",\"voucher\":{\"lines\":1}}\n";
  static const char deep_remove_scalar_output[] =
      "{\"status\":\"open\",\"voucher\":{\"lines\":1}}\n";
  static const char *const top_set[] = {"/processed=true"};
  static const char top_set_output[] =
      "{\"status\":\"open\",\"n\":1,\"processed\":true}\n";
  static const char *const top_set_multi[] = {"/status=ready", "/status=done"};
  static const char top_set_multi_output[] = "{\"status\":\"done\",\"n\":1}\n";
  static const char *const top_set_multi_missing[] = {"/processed=true",
                                                      "/processed=false"};
  static const char top_set_multi_missing_output[] =
      "{\"status\":\"open\",\"n\":1,\"processed\":false}\n";
  static const char *const top_remove[] = {"rm:/n"};
  static const char top_remove_output[] = "{\"status\":\"open\"}\n";
  static const char *const top_multi[] = {"rm:/status", "/n=+2",
                                          "/processed=true"};
  static const char top_multi_output[] = "{\"n\":3,\"processed\":true}\n";
  static const char *const mixed_nested_multi[] = {"rm:/status", "/n=+2",
                                                   "/meta/bench=true"};
  static const char mixed_nested_multi_output[] =
      "{\"n\":3,\"meta\":{\"bench\":true}}\n";
  static const char *const same_top_nested_multi[] = {"/meta/bench=true",
                                                      "/meta/state=done"};
  static const char same_top_nested_multi_input[] =
      "{\"status\":\"open\",\"meta\":{\"old\":1}}\n";
  static const char same_top_nested_multi_output[] =
      "{\"status\":\"open\",\"meta\":{\"old\":1,\"bench\":true,"
      "\"state\":\"done\"}}\n";
  static const char same_top_nested_multi_missing_input[] =
      "{\"status\":\"open\"}\n";
  static const char same_top_nested_multi_missing_output[] =
      "{\"status\":\"open\",\"meta\":{\"bench\":true,\"state\":\"done\"}}\n";
  static const char *const same_top_ordered[] = {
      "/meta/bench=true", "rm:/meta/bench", "/meta/state=done"};
  static const char same_top_ordered_input[] =
      "{\"status\":\"open\",\"meta\":{\"old\":1}}\n";
  static const char same_top_ordered_output[] =
      "{\"status\":\"open\",\"meta\":{\"old\":1,\"state\":\"done\"}}\n";
  static const char *const same_top_increment_multi[] = {"/meta/count=+1",
                                                         "/meta/state=done"};
  static const char same_top_increment_multi_input[] =
      "{\"status\":\"open\",\"meta\":{\"count\":2}}\n";
  static const char same_top_increment_multi_output[] =
      "{\"status\":\"open\",\"meta\":{\"count\":3,\"state\":\"done\"}}\n";
  static const char same_top_increment_missing_input[] =
      "{\"status\":\"open\",\"meta\":{}}\n";
  static const char same_top_increment_missing_output[] =
      "{\"status\":\"open\",\"meta\":{\"count\":1,\"state\":\"done\"}}\n";
  static const char *const same_top_create_increment[] = {
      "/meta/count=2", "/meta/count=+1", "/meta/state=done"};
  static const char same_top_create_increment_input[] =
      "{\"status\":\"open\"}\n";
  static const char same_top_create_increment_output[] =
      "{\"status\":\"open\",\"meta\":{\"count\":3,\"state\":\"done\"}}\n";
  static const char remove_only_input[] = "{\"status\":\"open\"}\n";
  static const char *const remove_only[] = {"rm:/status"};
  static const char remove_only_output[] = "{}\n";
  static const char *const ordered[] = {"/status=ready", "rm:/n",
                                        "/meta/a~1b=true"};
  static const char ordered_output[] =
      "{\"status\":\"ready\",\"meta\":{\"a/b\":true}}\n";
  static const char *const multiline_list[] = {"/state/details{\n"
                                               "  /owner = \"alice\"\n"
                                               "  /note = \"hi, world\"\n"
                                               "}\n"
                                               "rm:/n\n"
                                               "/status=ready"};
  static const char multiline_list_output[] =
      "{\"status\":\"ready\",\"state\":{\"details\":{\"owner\":\"alice\","
      "\"note\":\"hi, world\"}}}\n";
  static const char *const nested_brace[] = {"/a{/b{/c=1,/d=2},/e=3}"};
  static const char nested_brace_input[] = "{\"status\":\"open\"}\n";
  static const char nested_brace_output[] =
      "{\"status\":\"open\",\"a\":{\"b\":{\"c\":1,\"d\":2},\"e\":3}}\n";
  static const char *const invalid_root[] = {"/=value"};
  static const char *const invalid_json_numbers[] = {
      "/n=01", "/n=nan", "/n=inf", "/n=+nan", "/n=+inf", "/n=+0", "/n=1e9999"};
  lql_selector *selector;
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;
  size_t invalid_number_index;

  selector = NULL;
  mutation = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }
  if (ctx->mutation_parse(ctx, invalid_root, 1u, &mutation, &error) !=
          LQL_STATUS_PARSE_ERROR ||
      mutation != NULL) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  for (invalid_number_index = 0u;
       invalid_number_index <
       sizeof(invalid_json_numbers) / sizeof(invalid_json_numbers[0]);
       ++invalid_number_index) {
    if (ctx->mutation_parse(ctx, &invalid_json_numbers[invalid_number_index],
                            1u, &mutation, &error) != LQL_STATUS_PARSE_ERROR ||
        mutation != NULL) {
      ctx->mutation_destroy(ctx, mutation);
      ctx->selector_destroy(ctx, selector);
      return 1;
    }
  }
  if (ctx->mutation_parse(ctx, increment, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.selector = selector;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(increment_output) - 1u ||
      memcmp(writer.data, increment_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.matched_only = 0;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(increment_all_output) - 1u ||
      memcmp(writer.data, increment_all_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, increment_missing, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(increment_missing_output) - 1u ||
      memcmp(writer.data, increment_missing_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, increment_multi, 2u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 2u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(increment_multi_output) - 1u ||
      memcmp(writer.data, increment_multi_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, increment_multi_missing, 2u, &mutation,
                          &error) != LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 2u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(increment_multi_missing_output) - 1u ||
      memcmp(writer.data, increment_multi_missing_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, nested_increment, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)nested_increment_input;
  reader.len = sizeof(nested_increment_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 2u ||
      writer.len != sizeof(nested_increment_output) - 1u ||
      memcmp(writer.data, nested_increment_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)nested_increment_missing_input;
  reader.len = sizeof(nested_increment_missing_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(nested_increment_missing_output) - 1u ||
      memcmp(writer.data, nested_increment_missing_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)nested_increment_scalar_input;
  reader.len = sizeof(nested_increment_scalar_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(nested_increment_scalar_output) - 1u ||
      memcmp(writer.data, nested_increment_scalar_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  request.reader_user = &reader;
  if (ctx->mutation_parse(ctx, top_set, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(top_set_output) - 1u ||
      memcmp(writer.data, top_set_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, top_set_multi, 2u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 2u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(top_set_multi_output) - 1u ||
      memcmp(writer.data, top_set_multi_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, top_set_multi_missing, 2u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 2u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(top_set_multi_missing_output) - 1u ||
      memcmp(writer.data, top_set_multi_missing_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, top_remove, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(top_remove_output) - 1u ||
      memcmp(writer.data, top_remove_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, top_multi, 3u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 3u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(top_multi_output) - 1u ||
      memcmp(writer.data, top_multi_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, mixed_nested_multi, 3u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 3u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(mixed_nested_multi_output) - 1u ||
      memcmp(writer.data, mixed_nested_multi_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, same_top_nested_multi, 2u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 2u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)same_top_nested_multi_input;
  reader.len = sizeof(same_top_nested_multi_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(same_top_nested_multi_output) - 1u ||
      memcmp(writer.data, same_top_nested_multi_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)same_top_nested_multi_missing_input;
  reader.len = sizeof(same_top_nested_multi_missing_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(same_top_nested_multi_missing_output) - 1u ||
      memcmp(writer.data, same_top_nested_multi_missing_output, writer.len) !=
          0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, same_top_ordered, 3u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 3u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)same_top_ordered_input;
  reader.len = sizeof(same_top_ordered_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.mutation = mutation;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(same_top_ordered_output) - 1u ||
      memcmp(writer.data, same_top_ordered_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, same_top_increment_multi, 2u, &mutation,
                          &error) != LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 2u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)same_top_increment_multi_input;
  reader.len = sizeof(same_top_increment_multi_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.mutation = mutation;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(same_top_increment_multi_output) - 1u ||
      memcmp(writer.data, same_top_increment_multi_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)same_top_increment_missing_input;
  reader.len = sizeof(same_top_increment_missing_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(same_top_increment_missing_output) - 1u ||
      memcmp(writer.data, same_top_increment_missing_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, same_top_create_increment, 3u, &mutation,
                          &error) != LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 3u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)same_top_create_increment_input;
  reader.len = sizeof(same_top_create_increment_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.mutation = mutation;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(same_top_create_increment_output) - 1u ||
      memcmp(writer.data, same_top_create_increment_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, remove_only, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)remove_only_input;
  reader.len = sizeof(remove_only_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(remove_only_output) - 1u ||
      memcmp(writer.data, remove_only_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  request.reader_user = &reader;
  if (ctx->mutation_parse(ctx, direct_set, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(direct_set_output) - 1u ||
      memcmp(writer.data, direct_set_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)nested_set_input;
  reader.len = sizeof(nested_set_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(nested_set_output) - 1u ||
      memcmp(writer.data, nested_set_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)nested_replace_input;
  reader.len = sizeof(nested_replace_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(nested_replace_output) - 1u ||
      memcmp(writer.data, nested_replace_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)nested_scalar_input;
  reader.len = sizeof(nested_scalar_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(nested_scalar_output) - 1u ||
      memcmp(writer.data, nested_scalar_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  request.reader_user = &reader;
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, nested_remove, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)nested_remove_input;
  reader.len = sizeof(nested_remove_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(nested_remove_output) - 1u ||
      memcmp(writer.data, nested_remove_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)nested_remove_missing_input;
  reader.len = sizeof(nested_remove_missing_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(nested_remove_missing_output) - 1u ||
      memcmp(writer.data, nested_remove_missing_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)nested_remove_scalar_input;
  reader.len = sizeof(nested_remove_scalar_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(nested_remove_scalar_output) - 1u ||
      memcmp(writer.data, nested_remove_scalar_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, nested_array_remove, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)nested_array_remove_input;
  reader.len = sizeof(nested_array_remove_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(nested_array_remove_output) - 1u ||
      memcmp(writer.data, nested_array_remove_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, nested_array_increment, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)nested_array_increment_input;
  reader.len = sizeof(nested_array_increment_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(nested_array_increment_output) - 1u ||
      memcmp(writer.data, nested_array_increment_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  request.reader_user = &reader;
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, deep_set, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)deep_set_input;
  reader.len = sizeof(deep_set_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(deep_set_output) - 1u ||
      memcmp(writer.data, deep_set_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)deep_set_missing_input;
  reader.len = sizeof(deep_set_missing_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(deep_set_missing_output) - 1u ||
      memcmp(writer.data, deep_set_missing_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)deep_set_scalar_input;
  reader.len = sizeof(deep_set_scalar_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(deep_set_scalar_output) - 1u ||
      memcmp(writer.data, deep_set_scalar_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, deep_remove, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)deep_remove_input;
  reader.len = sizeof(deep_remove_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.mutation = mutation;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(deep_remove_output) - 1u ||
      memcmp(writer.data, deep_remove_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)deep_remove_missing_input;
  reader.len = sizeof(deep_remove_missing_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(deep_remove_missing_output) - 1u ||
      memcmp(writer.data, deep_remove_missing_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)deep_remove_scalar_input;
  reader.len = sizeof(deep_remove_scalar_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(deep_remove_scalar_output) - 1u ||
      memcmp(writer.data, deep_remove_scalar_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  request.reader_user = &reader;
  if (ctx->mutation_parse(ctx, ordered, 3u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 3u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(ordered_output) - 1u ||
      memcmp(writer.data, ordered_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  request.reader_user = &reader;
  if (ctx->mutation_parse(ctx, multiline_list, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 4u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  reader.offset = 0u;
  memset(&writer, 0, sizeof(writer));
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(multiline_list_output) - 1u ||
      memcmp(writer.data, multiline_list_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  if (ctx->mutation_parse(ctx, nested_brace, 1u, &mutation, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_count(ctx, mutation) != 3u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)nested_brace_input;
  reader.len = sizeof(nested_brace_input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.writer_user = &writer;
  request.mutation = mutation;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(nested_brace_output) - 1u ||
      memcmp(writer.data, nested_brace_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  ctx->selector_destroy(ctx, selector);
  return 0;
}

/*
 * Mutation quotes are LQL delimiters, not JSON string literals: Go LQL strips
 * only the outer matching quotes before json.Marshal encodes the stored value.
 * Keep that exact behavior while proving native UTF-8 survives JSON emission.
 */
static int run_mutation_literal_parity(lql *ctx) {
  static const char input[] = "{\"status\":\"open\"}\n";
  static const char *const escaped[] = {"/escaped=\"a\\\"b\\n\\uD83D\\uDE00\""};
  static const char escaped_output[] =
      "{\"status\":\"open\",\"escaped\":\"a\\\\\\\"b\\\\n\\\\uD83D\\\\uDE00\"}"
      "\n";
  static const char *const unicode[] = {"/japanese=日本語", "/emoji=😀",
                                        "/supplementary=𐐷"};
  static const char unicode_output[] =
      "{\"status\":\"open\",\"japanese\":\"日本語\",\"emoji\":\"😀\","
      "\"supplementary\":\"𐐷\"}\n";
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;

  mutation = NULL;
  lql_error_init(&error);
  if (ctx->mutation_parse(ctx, escaped, 1u, &mutation, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 1u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(escaped_output) - 1u ||
      memcmp(writer.data, escaped_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);

  mutation = NULL;
  if (ctx->mutation_parse(ctx, unicode, sizeof(unicode) / sizeof(unicode[0]),
                          &mutation, &error) != LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(unicode_output) - 1u ||
      memcmp(writer.data, unicode_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  return 0;
}

static int run_long_number_mutation_regression(lql *ctx) {
  static const char input[] =
      "{\"status\":\"open\",\"n\":0."
      "0000000000000000000000000000000000000000000000000000000000000000"
      "0000000000000000000000000000000000000000000000000000000000000000"
      "0001}\n";
  static const char output[] = "{\"status\":\"open\",\"n\":1}\n";
  static const char *const mutations[] = {"/n=+1"};
  lql_selector *selector;
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;

  selector = NULL;
  mutation = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_parse(ctx, mutations, 1u, &mutation, &error) !=
          LQL_STATUS_OK) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 7u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.selector = selector;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(output) - 1u ||
      memcmp(writer.data, output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_projection_then_mutation_output(lql *ctx) {
  static const char input[] = "{\"status\":\"open\",\"n\":1,\"drop\":9}\n";
  static const char mixed_input[] =
      "{\"status\":\"closed\",\"n\":1,\"drop\":9}\n"
      "{\"status\":\"open\",\"n\":1,\"drop\":9}\n";
  static const char *const projection_paths[] = {"/status", "/n"};
  static const char *const mutations[] = {"/n=+2", "/added=true"};
  static const char output[] = "{\"n\":3,\"status\":\"open\",\"added\":true}\n";
  static const char mixed_output[] =
      "{\"n\":1,\"status\":\"closed\"}\n"
      "{\"n\":3,\"status\":\"open\",\"added\":true}\n";
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;

  selector = NULL;
  projection = NULL;
  mutation = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
          LQL_STATUS_OK ||
      ctx->projection_parse(ctx, projection_paths, 2u, &projection, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_parse(ctx, mutations, 2u, &mutation, &error) !=
          LQL_STATUS_OK) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 3u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.selector = selector;
  request.projection = projection;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(output) - 1u ||
      memcmp(writer.data, output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)mixed_input;
  reader.len = sizeof(mixed_input) - 1u;
  reader.chunk_size = 3u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.matched_only = 0;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(mixed_output) - 1u ||
      memcmp(writer.data, mixed_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 2;
  }
  ctx->mutation_destroy(ctx, mutation);
  ctx->projection_destroy(ctx, projection);
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_output_modes_preserve_completed_before_malformed(lql *ctx) {
  static const char input[] = "{\"status\":\"open\",\"n\":1}\n"
                              "{\"status\":}\n";
  static const char selected_output[] = "{\"status\":\"open\",\"n\":1}\n";
  static const char projection_output[] = "{\"n\":1,\"status\":\"open\"}\n";
  static const char mutation_output[] =
      "{\"status\":\"open\",\"n\":1,\"added\":true}\n";
  static const char project_mutation_output[] =
      "{\"n\":1,\"status\":\"open\",\"added\":true}\n";
  static const char *const projection_paths[] = {"/n", "/status"};
  static const char *const mutations[] = {"/added=true"};
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;

  selector = NULL;
  projection = NULL;
  mutation = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
          LQL_STATUS_OK ||
      ctx->projection_parse(ctx, projection_paths, 2u, &projection, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_parse(ctx, mutations, 1u, &mutation, &error) !=
          LQL_STATUS_OK) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.writer = test_write;
  request.writer_user = &writer;
  request.selector = selector;
  request.matched_only = 1;

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 3u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(selected_output) - 1u ||
      memcmp(writer.data, selected_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.projection = projection;
  request.output_mode = LQL_STREAM_OUTPUT_PROJECTION;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(projection_output) - 1u ||
      memcmp(writer.data, projection_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 1u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.projection = NULL;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(mutation_output) - 1u ||
      memcmp(writer.data, mutation_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 4u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.projection = projection;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(project_mutation_output) - 1u ||
      memcmp(writer.data, project_mutation_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  ctx->mutation_destroy(ctx, mutation);
  ctx->projection_destroy(ctx, projection);
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_completed_record_before_root_array_error(lql *ctx) {
  static const char input[] = "{\"status\":\"open\",\"n\":1}\n"
                              "[{\"status\":\"open\"}]\n";
  static const char selected_output[] = "{\"status\":\"open\",\"n\":1}\n";
  static const char projection_output[] = "{\"n\":1,\"status\":\"open\"}\n";
  static const char mutation_output[] =
      "{\"status\":\"open\",\"n\":1,\"added\":true}\n";
  static const char project_mutation_output[] =
      "{\"n\":1,\"status\":\"open\",\"added\":true}\n";
  static const char *const projection_paths[] = {"/n", "/status"};
  static const char *const mutations[] = {"/added=true"};
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;
  test_decisions decisions;

  selector = NULL;
  projection = NULL;
  mutation = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
          LQL_STATUS_OK ||
      ctx->projection_parse(ctx, projection_paths, 2u, &projection, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_parse(ctx, mutations, 1u, &mutation, &error) !=
          LQL_STATUS_OK) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  memset(&decisions, 0, sizeof(decisions));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.on_decision = test_decide;
  request.decision_user = &decisions;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 1u || result.records_matched != 1u ||
      decisions.count != 1u || decisions.matches != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 3u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.selector = selector;
  request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(selected_output) - 1u ||
      memcmp(writer.data, selected_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 4u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.projection = projection;
  request.output_mode = LQL_STREAM_OUTPUT_PROJECTION;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(projection_output) - 1u ||
      memcmp(writer.data, projection_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 1u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.projection = NULL;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(mutation_output) - 1u ||
      memcmp(writer.data, mutation_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 5u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.projection = projection;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_JSON_ERROR ||
      result.records_seen != 1u || result.records_matched != 1u ||
      writer.len != sizeof(project_mutation_output) - 1u ||
      memcmp(writer.data, project_mutation_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  ctx->mutation_destroy(ctx, mutation);
  ctx->projection_destroy(ctx, projection);
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_output_modes_skip_scalar_roots(lql *ctx) {
  static const char input[] = " true \n"
                              "\t{\"status\":\"open\",\"n\":1}\n";
  static const char selected_output[] = "{\"status\":\"open\",\"n\":1}\n";
  static const char projection_output[] = "{\"n\":1,\"status\":\"open\"}\n";
  static const char mutation_output[] =
      "{\"status\":\"open\",\"n\":1,\"added\":true}\n";
  static const char project_mutation_output[] =
      "{\"n\":1,\"status\":\"open\",\"added\":true}\n";
  static const char *const projection_paths[] = {"/n", "/status"};
  static const char *const mutations[] = {"/added=true"};
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;

  selector = NULL;
  projection = NULL;
  mutation = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
          LQL_STATUS_OK ||
      ctx->projection_parse(ctx, projection_paths, 2u, &projection, &error) !=
          LQL_STATUS_OK ||
      ctx->mutation_parse(ctx, mutations, 1u, &mutation, &error) !=
          LQL_STATUS_OK) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 1u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.selector = selector;
  request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  request.matched_only = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(selected_output) - 1u ||
      memcmp(writer.data, selected_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.projection = projection;
  request.output_mode = LQL_STREAM_OUTPUT_PROJECTION;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(projection_output) - 1u ||
      memcmp(writer.data, projection_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 3u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.projection = NULL;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(mutation_output) - 1u ||
      memcmp(writer.data, mutation_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 4u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.projection = projection;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      writer.len != sizeof(project_mutation_output) - 1u ||
      memcmp(writer.data, project_mutation_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  ctx->mutation_destroy(ctx, mutation);
  ctx->projection_destroy(ctx, projection);
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_stop_and_root_array(lql *ctx) {
  static const char input[] = "{\"status\":\"open\"}\n{\"status\":\"open\"}\n";
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
  if (lql_stream_execute(ctx, &request, &result, &error) !=
      LQL_STATUS_JSON_ERROR) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_record_limit(lql *ctx) {
  static const char more_input[] =
      "{\"status\":\"open\"}\n{\"status\":\"closed\"}\n{\"status\":\"open\"}\n";
  static const char exact_input[] =
      "{\"status\":\"open\"}\n{\"status\":\"closed\"}\n";
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
  reader.data = (const unsigned char *)more_input;
  reader.len = sizeof(more_input) - 1u;
  reader.chunk_size = 2u;
  memset(&decisions, 0, sizeof(decisions));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.on_decision = test_decide;
  request.decision_user = &decisions;
  request.limits.max_records = 2u;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      !result.stopped_early ||
      result.stop_reason != LQL_STREAM_STOP_RECORD_LIMIT ||
      result.records_seen != 2u || result.records_matched != 1u ||
      decisions.count != 2u || decisions.matches != 1u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)exact_input;
  reader.len = sizeof(exact_input) - 1u;
  reader.chunk_size = 2u;
  memset(&decisions, 0, sizeof(decisions));
  request.reader_user = &reader;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.stopped_early || result.stop_reason != LQL_STREAM_STOP_NONE ||
      result.records_seen != 2u || result.records_matched != 1u ||
      decisions.count != 2u || decisions.matches != 1u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_byte_limit(lql *ctx) {
  static const char input[] =
      "{\"status\":\"open\"}\n{\"status\":\"closed\"}\n{\"status\":\"open\"}\n";
  static const char limited[] =
      "{\"status\":\"open\"}\n{\"status\":\"closed\"}\n";
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
  reader.chunk_size = 2u;
  memset(&decisions, 0, sizeof(decisions));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.on_decision = test_decide;
  request.decision_user = &decisions;
  request.limits.max_bytes = sizeof(limited) - 1u;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      !result.stopped_early ||
      result.stop_reason != LQL_STREAM_STOP_BYTE_LIMIT ||
      result.records_seen != 2u || result.records_matched != 1u ||
      result.bytes_consumed < request.limits.max_bytes ||
      decisions.count != 2u || decisions.matches != 1u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  memset(&decisions, 0, sizeof(decisions));
  request.reader_user = &reader;
  request.limits.max_records = 1u;
  request.limits.max_matches = 0u;
  request.limits.max_bytes = 1u;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      !result.stopped_early ||
      result.stop_reason != LQL_STREAM_STOP_RECORD_LIMIT ||
      result.records_seen != 1u || result.records_matched != 1u ||
      decisions.count != 1u || decisions.matches != 1u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 2u;
  memset(&decisions, 0, sizeof(decisions));
  request.reader_user = &reader;
  request.limits.max_records = 0u;
  request.limits.max_matches = 1u;
  request.limits.max_bytes = 1u;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      !result.stopped_early ||
      result.stop_reason != LQL_STREAM_STOP_MATCH_LIMIT ||
      result.records_seen != 1u || result.records_matched != 1u ||
      decisions.count != 1u || decisions.matches != 1u) {
    ctx->selector_destroy(ctx, selector);
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
  request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  request.matched_only = 1;
  request.writer = test_stop_write;
  request.limits.max_records = 1u;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_STOP ||
      result.stopped_early) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int run_unintentional_stop_status(lql *ctx) {
  static const char input[] = "{\"status\":\"open\"}\n";
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;

  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }

  memset(&request, 0, sizeof(request));
  request.reader = test_stop_read;
  request.selector = selector;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_STOP) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  request.matched_only = 1;
  request.writer = test_stop_write;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_STOP ||
      result.stopped_early) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  ctx->selector_destroy(ctx, selector);
  return 0;
}

#undef lql_stream_execute

static int run_true_stream_contract(lql *ctx) {
  static const char compact_input[] =
      "{\"status\":\"open\"}\n{\"status\":\"closed\"}\n";
  static const char spaced_input[] = "{ \"status\" : \"open\" }\n";
  static const char selected_output[] = "{\"status\":\"open\"}\n";
  const char *paths[1];
  lql_selector *selector;
  lql_projection *projection;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;
  test_decisions decisions;

  selector = NULL;
  projection = NULL;
  lql_error_init(&error);
  if (ctx->stream_execute == NULL || ctx->stream_execute_spooled == NULL) {
    return 1;
  }
  if (ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
      LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)compact_input;
  reader.len = sizeof(compact_input) - 1u;
  reader.chunk_size = 1u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.range_writer = test_range_write;
  request.range_user = &reader;
  request.input_is_compact = 1;
  request.writer = test_write;
  request.writer_user = &writer;
  request.selector = selector;
  request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  request.matched_only = 1;
  if (ctx->stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u ||
      reader.range_writes != 1u || writer.len != sizeof(selected_output) - 1u ||
      memcmp(writer.data, selected_output, writer.len) != 0) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)compact_input;
  reader.len = sizeof(compact_input) - 1u;
  reader.chunk_size = 1u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.range_writer = test_range_write;
  request.range_user = &reader;
  request.input_is_compact = 1;
  request.writer = test_stop_write;
  request.selector = selector;
  request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  request.matched_only = 1;
  if (ctx->stream_execute(ctx, &request, &result, &error) != LQL_STATUS_STOP ||
      result.stopped_early) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)spaced_input;
  reader.len = sizeof(spaced_input) - 1u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.range_user = &reader;
  request.input_is_compact = 0;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_UNSUPPORTED ||
      reader.offset != 0u || writer.len != 0u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)spaced_input;
  reader.len = sizeof(spaced_input) - 1u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.range_user = &reader;
  request.input_is_compact = 1;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_UNSUPPORTED ||
      reader.offset != reader.len || writer.len != 0u ||
      reader.range_writes != 0u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }

  paths[0] = "/status";
  if (ctx->projection_parse(ctx, paths, 1u, &projection, &error) !=
      LQL_STATUS_OK) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)compact_input;
  reader.len = sizeof(compact_input) - 1u;
  memset(&writer, 0, sizeof(writer));
  request.reader_user = &reader;
  request.range_user = &reader;
  request.input_is_compact = 1;
  request.projection = projection;
  request.output_mode = LQL_STREAM_OUTPUT_PROJECTION;
  if (lql_stream_execute(ctx, &request, &result, &error) !=
          LQL_STATUS_UNSUPPORTED ||
      reader.offset != 0u || writer.len != 0u) {
    ctx->projection_destroy(ctx, projection);
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->projection_destroy(ctx, projection);

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)spaced_input;
  reader.len = sizeof(spaced_input) - 1u;
  memset(&decisions, 0, sizeof(decisions));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.on_decision = test_decide;
  request.decision_user = &decisions;
  if (lql_stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 1u || result.records_matched != 1u ||
      decisions.count != 1u || decisions.matches != 1u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  return 0;
}

static int write_test_file(const char *path, const unsigned char *data,
                           size_t len) {
  FILE *file;
  file = fopen(path, "wb");
  if (file == NULL)
    return 1;
  if (len != 0u && fwrite(data, 1u, len, file) != len) {
    fclose(file);
    return 1;
  }
  return fclose(file) != 0;
}

static int run_file_backed_mutation_case(lql *ctx, const char *expr,
                                         lql_status expected_status,
                                         const char *expected_output) {
  static const char input[] = "{}\n";
  lql_mutation_parse_options options;
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;
  lql_status status;

  memset(&options, 0, sizeof(options));
  options.enable_file_values = 1;
  options.file_value_base_dir.data = ".";
  options.file_value_base_dir.len = 1u;
  mutation = NULL;
  lql_error_init(&error);
  status = ctx->mutation_parse_with_options(ctx, &expr, 1u, &options, &mutation,
                                            &error);
  if (status != LQL_STATUS_OK)
    return 1;
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 1u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  request.matched_only = 1;
  status = ctx->stream_execute_spooled(ctx, &request, &result, &error);
  ctx->mutation_destroy(ctx, mutation);
  if (status != expected_status)
    return 1;
  if (expected_output == NULL)
    return 0;
  if (writer.len != strlen(expected_output) ||
      memcmp(writer.data, expected_output, writer.len) != 0)
    return 1;
  return 0;
}

static int run_file_backed_mutation_preflight_case(lql *ctx, const char *expr) {
  static const char input[] = "{\"id\":\"first\"}\n{\"id\":\"match\"}\n";
  lql_mutation_parse_options options;
  lql_mutation *mutation;
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;
  lql_status status;

  memset(&options, 0, sizeof(options));
  options.enable_file_values = 1;
  options.file_value_base_dir.data = ".";
  options.file_value_base_dir.len = 1u;
  mutation = NULL;
  selector = NULL;
  lql_error_init(&error);
  status = ctx->mutation_parse_with_options(ctx, &expr, 1u, &options, &mutation,
                                            &error);
  if (status != LQL_STATUS_OK)
    return 1;
  if (ctx->selector_parse(ctx, "/id=\"match\"", &selector, &error) !=
      LQL_STATUS_OK) {
    ctx->mutation_destroy(ctx, mutation);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 1u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.selector = selector;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  request.matched_only = 0;
  status = ctx->stream_execute_spooled(ctx, &request, &result, &error);
  ctx->selector_destroy(ctx, selector);
  ctx->mutation_destroy(ctx, mutation);
  return status != LQL_STATUS_JSON_ERROR || writer.len != 0u;
}

static int run_file_backed_mutations(lql *ctx) {
  static const char text_path[] = "liblql-file-value-text.tmp";
  static const char utf8_path[] = "liblql-file-value-utf8.tmp";
  static const char binary_path[] = "liblql-file-value-binary.tmp";
  static const char invalid_path[] = "liblql-file-value-invalid.tmp";
  static const char nul_path[] = "liblql-file-value-nul.tmp";
  static const unsigned char text_payload[] = "hello world";
  static const unsigned char utf8_payload[] = "日本語 😀 こんにちは";
  static const unsigned char binary_payload[] = {0x00u, 0x01u, 0x02u};
  static const unsigned char invalid_payload[] = {0xc0u, 0xafu};
  static const unsigned char nul_payload[] = {'a', 0x00u, 'b'};
  static const char *const disabled_expr[] = {"file:/payload=payload.txt"};
  lql_mutation_parse_options options;
  lql_mutation *mutation;
  lql_error error;

  remove(text_path);
  remove(utf8_path);
  remove(binary_path);
  remove(invalid_path);
  remove(nul_path);
  if (write_test_file(text_path, text_payload, sizeof(text_payload) - 1u) ||
      write_test_file(utf8_path, utf8_payload, sizeof(utf8_payload) - 1u) ||
      write_test_file(binary_path, binary_payload, sizeof(binary_payload)) ||
      write_test_file(invalid_path, invalid_payload, sizeof(invalid_payload)) ||
      write_test_file(nul_path, nul_payload, sizeof(nul_payload)))
    return 1;

  memset(&options, 0, sizeof(options));
  mutation = NULL;
  lql_error_init(&error);
  if (ctx->mutation_parse_with_options(ctx, disabled_expr, 1u, &options,
                                       &mutation,
                                       &error) != LQL_STATUS_PARSE_ERROR) {
    remove(text_path);
    remove(utf8_path);
    remove(binary_path);
    remove(invalid_path);
    remove(nul_path);
    ctx->mutation_destroy(ctx, mutation);
    return 1;
  }
  if (run_file_backed_mutation_case(
          ctx, "textfile:/payload=liblql-file-value-text.tmp", LQL_STATUS_OK,
          "{\"payload\":\"hello world\"}\n") ||
      run_file_backed_mutation_case(
          ctx, "textfile:/payload=liblql-file-value-utf8.tmp", LQL_STATUS_OK,
          "{\"payload\":\"日本語 😀 こんにちは\"}\n") ||
      run_file_backed_mutation_case(
          ctx, "base64file:/payload=liblql-file-value-binary.tmp",
          LQL_STATUS_OK, "{\"payload\":\"AAEC\"}\n") ||
      run_file_backed_mutation_case(
          ctx, "file:/payload=liblql-file-value-binary.tmp", LQL_STATUS_OK,
          "{\"payload\":\"AAEC\"}\n") ||
      run_file_backed_mutation_case(
          ctx, "textfile:/payload=liblql-file-value-invalid.tmp",
          LQL_STATUS_JSON_ERROR, "") ||
      run_file_backed_mutation_case(
          ctx, "textfile:/payload=liblql-file-value-nul.tmp",
          LQL_STATUS_JSON_ERROR, "") ||
      run_file_backed_mutation_preflight_case(
          ctx, "textfile:/payload=liblql-file-value-nul.tmp")) {
    remove(text_path);
    remove(utf8_path);
    remove(binary_path);
    remove(invalid_path);
    remove(nul_path);
    return 1;
  }

  remove(text_path);
  remove(utf8_path);
  remove(binary_path);
  remove(invalid_path);
  remove(nul_path);
  return 0;
}

static int run_virtual_file_source(lql *ctx) {
  static const char input[] = "{}\n";
  static const unsigned char payload[] = {0x00u, 0x01u, 0x02u};
  static const char expected[] = "{\"payload\":\"AAEC\"}\n";
  static const char *const expressions[] = {"file:/payload=virtual:payload"};
  lql_mutation_parse_options options;
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_file_source source;
  test_reader reader;
  test_writer writer;

  memset(&source, 0, sizeof(source));
  source.data = payload;
  source.len = sizeof(payload);
  source.chunk_size = 1u;
  memset(&options, 0, sizeof(options));
  options.enable_file_values = 1;
  options.file_value_open = test_file_open;
  options.file_value_close = test_file_close;
  options.file_value_user = &source;
  mutation = NULL;
  lql_error_init(&error);
  if (ctx->mutation_parse_with_options(ctx, expressions, 1u, &options,
                                       &mutation, &error) != LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 1u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  request.matched_only = 1;
  if (ctx->stream_execute_spooled(ctx, &request, &result, &error) !=
          LQL_STATUS_OK ||
      source.opens != 2u || source.closes != 2u ||
      writer.len != sizeof(expected) - 1u ||
      memcmp(writer.data, expected, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  return 0;
}

static int run_clock_contract(lql *ctx) {
  static const char *const expressions[] = {"time:/now=NOW"};
  static const char mutation_input[] = "{}\n";
  static const char mutation_output[] = "{\"now\":\"2024-03-01T00:00:00Z\"}\n";
  static const char mutation_minus_one_output[] =
      "{\"now\":\"1969-12-31T23:59:59Z\"}\n";
  static const char selector_input[] =
      "{\"timestamp\":\"2024-02-29T23:59:59Z\"}\n"
      "{\"timestamp\":\"2024-03-01T00:00:00Z\"}\n";
  static const char date_whitespace_input[] =
      "{\"timestamp\":\"2025-01-01\"}\n"
      "{\"timestamp\":\"2025-01-01 \"}\n"
      "{\"timestamp\":\" 2025-01-01\"}\n"
      "{\"timestamp\":\" 2025-01-01 \"}\n";
  time_t fixed_now;
  lql_mutation_parse_options options;
  lql_mutation *mutation;
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_writer writer;

  fixed_now = (time_t)1709251200;
  memset(&options, 0, sizeof(options));
  options.time_now = test_fixed_clock;
  options.time_user = &fixed_now;
  mutation = NULL;
  selector = NULL;
  lql_error_init(&error);
  if (ctx->mutation_parse_with_options(ctx, expressions, 1u, &options,
                                       &mutation, &error) != LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)mutation_input;
  reader.len = sizeof(mutation_input) - 1u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  request.matched_only = 1;
  if (ctx->stream_execute_spooled(ctx, &request, &result, &error) !=
          LQL_STATUS_OK ||
      writer.len != sizeof(mutation_output) - 1u ||
      memcmp(writer.data, mutation_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  fixed_now = (time_t)-1;
  mutation = NULL;
  if (ctx->mutation_parse_with_options(ctx, expressions, 1u, &options,
                                       &mutation, &error) != LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)mutation_input;
  reader.len = sizeof(mutation_input) - 1u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  request.matched_only = 1;
  if (ctx->stream_execute_spooled(ctx, &request, &result, &error) !=
          LQL_STATUS_OK ||
      writer.len != sizeof(mutation_minus_one_output) - 1u ||
      memcmp(writer.data, mutation_minus_one_output, writer.len) != 0) {
    ctx->mutation_destroy(ctx, mutation);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  fixed_now = (time_t)253402300799;
  mutation = NULL;
  lql_error_init(&error);
  if (ctx->mutation_parse_with_options(ctx, expressions, 1u, &options,
                                       &mutation, &error) != LQL_STATUS_OK) {
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  fixed_now = (time_t)253402300800;
  mutation = NULL;
  lql_error_init(&error);
  if (ctx->mutation_parse_with_options(ctx, expressions, 1u, &options,
                                       &mutation, &error) !=
      LQL_STATUS_PARSE_ERROR) {
    if (mutation != NULL)
      ctx->mutation_destroy(ctx, mutation);
    return 1;
  }
  fixed_now = (time_t)1709251200;
  if (ctx->selector_parse(ctx, "date{f=/timestamp,since=today}", &selector,
                          &error) != LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)selector_input;
  reader.len = sizeof(selector_input) - 1u;
  reader.chunk_size = 1u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.time_now = test_fixed_clock;
  request.time_user = &fixed_now;
  if (ctx->stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 1u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  selector = NULL;
  if (ctx->selector_parse(ctx, "date{f=/timestamp,value=2025-01-01}", &selector,
                          &error) != LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)date_whitespace_input;
  reader.len = sizeof(date_whitespace_input) - 1u;
  reader.chunk_size = 1u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  if (ctx->stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 4u || result.records_matched != 4u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, "/timestamp>=\"2025-01-01 T00:00:00Z\"",
                          &selector, &error) != LQL_STATUS_PARSE_ERROR) {
    if (selector != NULL)
      ctx->selector_destroy(ctx, selector);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx,
                          "date{f=/timestamp,gte=2025-01-01 T00:00:00Z}",
                          &selector, &error) != LQL_STATUS_PARSE_ERROR) {
    if (selector != NULL)
      ctx->selector_destroy(ctx, selector);
    return 1;
  }
  selector = NULL;
  fixed_now = (time_t)-1;
  if (ctx->selector_parse(ctx, "date{f=/timestamp,since=now}", &selector,
                          &error) != LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)selector_input;
  reader.len = sizeof(selector_input) - 1u;
  reader.chunk_size = 1u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.time_now = test_fixed_clock;
  request.time_user = &fixed_now;
  if (ctx->stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      result.records_seen != 2u || result.records_matched != 2u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  fixed_now = (time_t)LONG_MIN;
  selector = NULL;
  if ((long)fixed_now == LONG_MIN &&
      ctx->selector_parse(ctx, "date{f=/timestamp,since=yesterday}", &selector,
                          &error) != LQL_STATUS_OK) {
    return 1;
  }
  if (selector != NULL) {
    memset(&reader, 0, sizeof(reader));
    reader.data = (const unsigned char *)selector_input;
    reader.len = sizeof(selector_input) - 1u;
    reader.chunk_size = 1u;
    memset(&request, 0, sizeof(request));
    request.reader = test_read;
    request.reader_user = &reader;
    request.selector = selector;
    request.time_now = test_fixed_clock;
    request.time_user = &fixed_now;
    if (ctx->stream_execute(ctx, &request, &result, &error) !=
        LQL_STATUS_UNSUPPORTED) {
      ctx->selector_destroy(ctx, selector);
      return 1;
    }
    ctx->selector_destroy(ctx, selector);
  }
  return 0;
}

static int run_option_failure_contract(lql *ctx) {
  static const char *const file_expression[] = {
      "file:/payload=virtual:payload"};
  static const char *const time_expression[] = {"time:/now=NOW"};
  lql_mutation_parse_options options;
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  time_t invalid_time;
  test_file_source source;
  test_reader reader;
  test_writer writer;

  memset(&options, 0, sizeof(options));
  options.enable_file_values = 1;
  options.file_value_open = test_file_open;
  mutation = NULL;
  lql_error_init(&error);
  if (ctx->mutation_parse_with_options(ctx, file_expression, 1u, &options,
                                       &mutation,
                                       &error) != LQL_STATUS_INVALID_ARGUMENT ||
      mutation != NULL) {
    ctx->mutation_destroy(ctx, mutation);
    return 1;
  }
  invalid_time = (time_t)-1;
  memset(&options, 0, sizeof(options));
  options.time_now = test_fixed_clock;
  options.time_user = &invalid_time;
  if (ctx->mutation_parse_with_options(ctx, time_expression, 1u, &options,
                                       &mutation, &error) != LQL_STATUS_OK ||
      mutation == NULL) {
    ctx->mutation_destroy(ctx, mutation);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  mutation = NULL;
  memset(&source, 0, sizeof(source));
  source.oversize_read = 1;
  memset(&options, 0, sizeof(options));
  options.enable_file_values = 1;
  options.file_value_open = test_file_open;
  options.file_value_close = test_file_close;
  options.file_value_user = &source;
  if (ctx->mutation_parse_with_options(ctx, file_expression, 1u, &options,
                                       &mutation, &error) != LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)"{}\n";
  reader.len = 3u;
  memset(&writer, 0, sizeof(writer));
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.writer = test_write;
  request.writer_user = &writer;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  request.matched_only = 1;
  if (ctx->stream_execute_spooled(ctx, &request, &result, &error) !=
          LQL_STATUS_CALLBACK_ERROR ||
      source.opens != 1u || source.closes != 1u) {
    ctx->mutation_destroy(ctx, mutation);
    return 1;
  }
  ctx->mutation_destroy(ctx, mutation);
  return 0;
}

static int run_cancellation_contract(lql *ctx) {
  static const char input[] = "{\"id\":1}\n{\"id\":2}\n";
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  test_cancel_state cancel;

  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  memset(&cancel, 0, sizeof(cancel));
  cancel.stop_at = 1u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.cancelled = test_cancel;
  request.cancel_user = &cancel;
  lql_error_init(&error);
  if (ctx->stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      !result.stopped_early ||
      result.stop_reason != LQL_STREAM_STOP_CANCELLED || reader.offset != 0u ||
      result.records_seen != 0u) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  reader.chunk_size = 1u;
  memset(&cancel, 0, sizeof(cancel));
  cancel.stop_at = 3u;
  request.reader_user = &reader;
  request.cancel_user = &cancel;
  if (ctx->stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      !result.stopped_early ||
      result.stop_reason != LQL_STREAM_STOP_CANCELLED || reader.offset == 0u ||
      result.records_seen != 0u) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  memset(&cancel, 0, sizeof(cancel));
  cancel.stop_at = 2u;
  request.reader_user = &reader;
  request.cancel_user = &cancel;
  if (ctx->stream_execute(ctx, &request, &result, &error) != LQL_STATUS_OK ||
      !result.stopped_early ||
      result.stop_reason != LQL_STREAM_STOP_CANCELLED ||
      reader.offset != reader.len || result.records_seen != 0u) {
    return 1;
  }
  return 0;
}

static int run_wide_plan_stream_contract(lql *ctx) {
  char expr[2048];
  size_t len;
  size_t i;
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  test_reader reader;
  lql_status status;

  len = 0u;
  for (i = 0u; i < 65u; ++i) {
    int written;
    written = sprintf(expr + len, "%sexists{/k%lu}", i == 0u ? "" : ",",
                      (unsigned long)i);
    if (written < 0 || (size_t)written >= sizeof(expr) - len) {
      return 1;
    }
    len += (size_t)written;
  }
  selector = NULL;
  lql_error_init(&error);
  if (ctx->selector_parse(ctx, expr, &selector, &error) != LQL_STATUS_OK) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)"{}\n";
  reader.len = 3u;
  memset(&request, 0, sizeof(request));
  request.reader = test_read;
  request.reader_user = &reader;
  request.selector = selector;
  status = ctx->stream_execute(ctx, &request, &result, &error);
  if (status != LQL_STATUS_UNSUPPORTED || reader.offset != 0u) {
    ctx->selector_destroy(ctx, selector);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)"{}\n";
  reader.len = 3u;
  status = ctx->stream_execute_spooled(ctx, &request, &result, &error);
  ctx->selector_destroy(ctx, selector);
  return status == LQL_STATUS_OK && result.records_seen == 1u &&
                 result.records_matched == 0u
             ? 0
             : 1;
}

static int run_instance_memory_contract(void) {
  lql *limited;
  lql *independent;
  lql_selector *selector;
  lql_error error;
  char *expr;
  size_t len;
  lql_status status;

  len = 8u * 1024u * 1024u;
  expr = (char *)malloc(len + 4u);
  if (expr == NULL) {
    return 1;
  }
  expr[0] = '/';
  memset(expr + 1, 'k', len);
  expr[len + 1u] = '=';
  expr[len + 2u] = 'x';
  expr[len + 3u] = '\0';
  limited = NULL;
  independent = NULL;
  selector = NULL;
  lql_error_init(&error);
  if (lql_new(&limited, &error) != LQL_STATUS_OK) {
    free(expr);
    return 1;
  }
  status = limited->selector_parse(limited, expr, &selector, &error);
  if (status != LQL_STATUS_NO_MEMORY || selector != NULL) {
    limited->selector_destroy(limited, selector);
    limited->destroy(limited);
    free(expr);
    return 1;
  }
  if (limited->selector_parse(limited, "/status=open", &selector, &error) !=
      LQL_STATUS_OK) {
    limited->destroy(limited);
    free(expr);
    return 1;
  }
  limited->selector_destroy(limited, selector);
  selector = NULL;
  if (lql_new(&independent, &error) != LQL_STATUS_OK ||
      independent->selector_parse(independent, "/status=open", &selector,
                                  &error) != LQL_STATUS_OK) {
    limited->destroy(limited);
    if (independent != NULL) {
      independent->selector_destroy(independent, selector);
      independent->destroy(independent);
    }
    free(expr);
    return 1;
  }
  independent->selector_destroy(independent, selector);
  independent->destroy(independent);
  limited->destroy(limited);
  free(expr);
  return 0;
}

int main(void) {
  lql *ctx;
  lql_error error;
  int match_all_status;

  lql_error_init(&error);
  if (lql_new(&ctx, &error) != LQL_STATUS_OK) {
    return 1;
  }
  match_all_status = 0;
  if (run_projection_parse(ctx) || run_selector_json_write(ctx) ||
      run_scalar_json_semantic_regressions(ctx) || run_status_selection(ctx) ||
      run_long_numeric_selector_regression(ctx) ||
      run_huge_numeric_selector_regression(ctx) ||
      run_escaped_pointer_selection(ctx) || run_conjunction_selection(ctx) ||
      run_or_selection(ctx) || run_post_hit_validation(ctx) ||
      run_not_selection(ctx) || run_mapped_string_predicates(ctx) ||
      run_long_contains_regression(ctx) ||
      ((match_all_status = run_match_all(ctx)) != 0) ||
      run_root_wildcard_array_error(ctx) || run_selected_record_output(ctx) ||
      run_value_callback(ctx) || run_value_callback_control(ctx) ||
      run_nested_projection_output(ctx) || run_mutation_output(ctx) ||
      run_mutation_literal_parity(ctx) ||
      run_long_number_mutation_regression(ctx) ||
      run_projection_then_mutation_output(ctx) ||
      run_output_modes_preserve_completed_before_malformed(ctx) ||
      run_completed_record_before_root_array_error(ctx) ||
      run_output_modes_skip_scalar_roots(ctx) || run_stop_and_root_array(ctx) ||
      run_record_limit(ctx) || run_byte_limit(ctx) ||
      run_unintentional_stop_status(ctx) || run_true_stream_contract(ctx) ||
      run_wide_plan_stream_contract(ctx) || run_instance_memory_contract() ||
      run_file_backed_mutations(ctx) || run_virtual_file_source(ctx) ||
      run_clock_contract(ctx) || run_option_failure_contract(ctx) ||
      run_cancellation_contract(ctx)) {
    ctx->destroy(ctx);
    return match_all_status != 0 ? match_all_status : 1;
  }
  ctx->destroy(ctx);
  return 0;
}
