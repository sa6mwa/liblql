#include "lql_json_scan.h"

#include <string.h>

typedef struct json_test_reader {
  const unsigned char *data;
  size_t len;
  size_t offset;
  size_t chunk_size;
} json_test_reader;

typedef struct json_test_writer {
  unsigned char data[2048];
  size_t len;
} json_test_writer;

typedef struct json_flat_result {
  json_test_writer *writer;
  json_test_writer *capture_writer;
  lql_json_capture_span *capture_spans;
  size_t objects;
  size_t matches;
  size_t captures;
} json_flat_result;

static lql_status json_test_read(void *user, unsigned char *buffer,
                                 size_t capacity, size_t *out_len,
                                 lql_error *error) {
  json_test_reader *reader;
  size_t amount;
  (void)error;
  reader = (json_test_reader *)user;
  if (reader == NULL || buffer == NULL || out_len == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out_len = 0u;
  if (reader->offset == reader->len) {
    return LQL_STATUS_OK;
  }
  amount = reader->len - reader->offset;
  if (amount > capacity) {
    amount = capacity;
  }
  if (reader->chunk_size != 0u && amount > reader->chunk_size) {
    amount = reader->chunk_size;
  }
  memcpy(buffer, reader->data + reader->offset, amount);
  reader->offset += amount;
  *out_len = amount;
  return LQL_STATUS_OK;
}

static lql_status json_test_write(void *user, const void *data, size_t len,
                                  lql_error *error) {
  json_test_writer *writer;
  (void)error;
  writer = (json_test_writer *)user;
  if (writer == NULL || data == NULL ||
      len > sizeof(writer->data) - writer->len) {
    return LQL_STATUS_CALLBACK_ERROR;
  }
  memcpy(writer->data + writer->len, data, len);
  writer->len += len;
  return LQL_STATUS_OK;
}

static lql_status json_flat_record(void *user, size_t record_index,
                                   int root_is_object, unsigned long hits,
                                   const lql_json_spool *spool,
                                   size_t source_offset, size_t source_len,
                                   int source_compact, lql_error *error) {
  json_flat_result *result;
  (void)record_index;
  (void)source_offset;
  (void)source_len;
  (void)source_compact;
  result = (json_flat_result *)user;
  if (result == NULL || spool == NULL) {
    return LQL_STATUS_CALLBACK_ERROR;
  }
  if (root_is_object) {
    ++result->objects;
  }
  if (result->capture_writer != NULL && result->capture_spans != NULL &&
      result->capture_spans[0].found) {
    if (lql_json_spool_write_slice(spool, result->capture_spans[0].offset,
                                   result->capture_spans[0].len,
                                   json_test_write, result->capture_writer,
                                   error) != LQL_STATUS_OK)
      return LQL_STATUS_CALLBACK_ERROR;
    ++result->captures;
  }
  if (hits == 0ul) {
    return LQL_STATUS_OK;
  }
  ++result->matches;
  if (lql_json_spool_write_to(spool, json_test_write, result->writer, error) !=
          LQL_STATUS_OK ||
      json_test_write(result->writer, "\n", 1u, error) != LQL_STATUS_OK) {
    return LQL_STATUS_CALLBACK_ERROR;
  }
  return LQL_STATUS_OK;
}

int main(void) {
  static const char flat_input[] =
      " { \"sta\\u0074us\" : \"op\\u0065n\", \"id\" : 1 }\n"
      " { \"status\" : \"closed\", \"status\" : \"open\" }\n"
      " \"open\"\n";
  static const lql_json_flat_eq_term flat_terms[] = {
      {LQL_JSON_FLAT_TERM_EQ, "status", 6u, "open", 4u}};
  static const char *const capture_segments[] = {"status"};
  static const lql_json_capture_key capture_keys[] = {{capture_segments, 1u}};
  static const char scalar_flat_input[] =
      "{\"code\":1,\"enabled\":true,\"empty\":null}\n"
      "{\"code\":1.0,\"enabled\":false,\"empty\":\"null\"}\n"
      "{\"code\":2,\"enabled\":true,\"empty\":null}\n";
  static const lql_json_flat_eq_term scalar_flat_terms[] = {
      {LQL_JSON_FLAT_TERM_NUMBER_EQ, "code", 4u, "1", 1u},
      {LQL_JSON_FLAT_TERM_BOOL_EQ, "enabled", 7u, "true", 4u},
      {LQL_JSON_FLAT_TERM_NULL_EQ, "empty", 5u, "null", 4u}};
  static const char range_flat_input[] = "{\"code\":1}\n"
                                         "{\"code\":2}\n"
                                         "{\"code\":3.5}\n"
                                         "{\"code\":4}\n"
                                         "{\"code\":\"3\"}\n";
  static const lql_json_flat_eq_term range_flat_terms[] = {
      {LQL_JSON_FLAT_TERM_NUMBER_RANGE,
       "code",
       4u,
       NULL,
       0u,
       NULL,
       0u,
       0u,
       0ul,
       0ul,
       0ul,
       0ul,
       0ul,
       NULL,
       0u,
       0u,
       0.0,
       2.0,
       4.0,
       0.0,
       0,
       1,
       1,
       0}};
  static const char temporal_flat_input[] =
      "{\"timestamp\":\"2026-03-05T10:28:20Z\"}\n"
      "{\"timestamp\":\"2026-03-05T10:28:21Z\"}\n"
      "{\"timestamp\":\"2026-03-05T10:29:21Z\"}\n"
      "{\"timestamp\":\"not-a-date\"}\n";
  static const char nested_flat_input[] =
      "{\"me\\u0074a\":{\"sta\\u0074e\":\"open\"},\"ignored\":[1]}\n"
      "{\"meta\":{\"state\":\"closed\"}}\n"
      "{\"meta\":{\"state\":\"closed\",\"state\":\"open\"}}\n";
  static const lql_json_flat_eq_term nested_flat_terms[] = {
      {LQL_JSON_FLAT_TERM_EQ, "meta", 4u, "open", 4u, "/meta/state", 11u, 2u}};
  static const char indexed_flat_input[] =
      "{\"items\":[{\"sku\":\"A\"},{\"sku\":\"B\"}]}\n"
      "{\"items\":[{\"sku\":\"B\"}]}\n"
      "{\"items\":[]}\n";
  static const lql_json_flat_eq_term indexed_flat_terms[] = {
      {LQL_JSON_FLAT_TERM_EQ, "items", 5u, "B", 1u, "/items/1/sku", 12u, 3u,
       2ul}};
  static const char indexed_scalar_input[] = "{\"values\":[\"A\",\"B\"]}\n"
                                             "{\"values\":[\"B\",\"A\"]}\n"
                                             "{\"values\":[\"A\"]}\n";
  static const lql_json_flat_eq_term indexed_scalar_terms[] = {
      {LQL_JSON_FLAT_TERM_EQ, "values", 6u, "B", 1u, "/values/1", 9u, 2u, 2ul}};
  static const lql_json_flat_eq_term array_scalar_wildcard_terms[] = {
      {LQL_JSON_FLAT_TERM_EQ, "values", 6u, "B", 1u, "/values/[]", 10u, 2u, 0ul,
       0ul, 2ul}};
  static const char indexed_exists_input[] = "{\"values\":[null,\"B\"]}\n"
                                             "{\"values\":[\"B\",null]}\n"
                                             "{\"values\":[\"A\"]}\n";
  static const lql_json_flat_eq_term indexed_exists_terms[] = {
      {LQL_JSON_FLAT_TERM_EXISTS, "values", 6u, NULL, 0u, "/values/1", 9u, 2u,
       2ul}};
  static const char *const indexed_capture_segments[] = {"items", "1", "sku"};
  static const lql_json_capture_key indexed_capture_keys[] = {
      {indexed_capture_segments, 3u}};
  static const char array_wildcard_input[] =
      "{\"array\":[{\"state\":\"closed\"},{\"state\":\"open\"}]}\n"
      "{\"array\":[{\"state\":\"closed\"}]}\n"
      "{\"array\":{\"state\":\"open\"}}\n";
  static const lql_json_flat_eq_term array_wildcard_terms[] = {
      {LQL_JSON_FLAT_TERM_EQ, "array", 5u, "open", 4u, "/array/[]/state", 15u,
       3u, 0ul, 0ul, 2ul, 0ul}};
  static const char recursive_flat_input[] =
      "{\"tree\":{\"branch\":{\"deep\":{\"sku\":\"needle\"}}}}\n"
      "{\"tree\":{\"branch\":{\"sku\":\"other\"}}}\n"
      "{\"tree\":{\"branch\":{\"deep\":{\"sku\":\"other\"}}}}\n";
  static const lql_json_flat_eq_term recursive_flat_terms[] = {
      {LQL_JSON_FLAT_TERM_EQ, "tree", 4u, "needle", 6u, "/tree/.../sku", 13u,
       3u, 0ul, 0ul, 0ul, 0ul, 2ul}};
  static size_t contains_failure[] = {0u, 0u, 1u};
  static const char contains_flat_input[] = "{\"msg\":\"xxababa\"}\n"
                                            "{\"msg\":\"xxabb\"}\n"
                                            "{\"msg\":\"xxa\\u0062a\"}\n";
  static const lql_json_flat_eq_term contains_flat_terms[] = {
      {LQL_JSON_FLAT_TERM_CONTAINS,
       "msg",
       3u,
       "aba",
       3u,
       NULL,
       0u,
       0u,
       0ul,
       0ul,
       0ul,
       0ul,
       0ul,
       NULL,
       0u,
       0u,
       0.0,
       0.0,
       0.0,
       0.0,
       0,
       0,
       0,
       0,
       {0, 0, 0, 0, 0, 0},
       {0, 0, 0, 0, 0, 0},
       {0, 0, 0, 0, 0, 0},
       {0, 0, 0, 0, 0, 0},
       {0, 0, 0, 0, 0, 0},
       0,
       0,
       0,
       0,
       0,
       contains_failure}};
  lql_json_flat_eq_term temporal_flat_terms[1];
  lql_json_flat_eq_request flat_request;
  lql_json_spool flat_spool;
  lql_json_spool_reader spool_reader;
  lql_json_capture_span capture_spans[1];
  json_test_reader flat_reader;
  json_test_writer flat_writer;
  json_test_writer capture_writer;
  json_flat_result flat_result;
  lql_error flat_error;
  size_t flat_records;
  size_t flat_bytes;
  unsigned char spool_bytes[3];
  unsigned char spool_fill[1024];
  size_t string_end;
  size_t spool_len;
  size_t spool_index;
  memset(&flat_reader, 0, sizeof(flat_reader));
  memset(&flat_writer, 0, sizeof(flat_writer));
  memset(&capture_writer, 0, sizeof(capture_writer));
  memset(&flat_result, 0, sizeof(flat_result));
  flat_reader.data = (const unsigned char *)flat_input;
  flat_reader.len = sizeof(flat_input) - 1u;
  flat_reader.chunk_size = 1u;
  flat_result.writer = &flat_writer;
  flat_result.capture_writer = &capture_writer;
  flat_result.capture_spans = capture_spans;
  memset(&flat_request, 0, sizeof(flat_request));
  flat_request.reader = json_test_read;
  flat_request.reader_user = &flat_reader;
  flat_request.terms = flat_terms;
  flat_request.term_count = 1u;
  flat_request.spool = &flat_spool;
  flat_request.capture = 1;
  flat_request.capture_keys = capture_keys;
  flat_request.capture_key_count = 1u;
  flat_request.capture_spans = capture_spans;
  flat_request.record = json_flat_record;
  flat_request.record_user = &flat_result;
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK) {
    return 9;
  }
  if (lql_json_scan_flat_eq_ndjson(&flat_request, &flat_records, &flat_bytes,
                                   &flat_error) != LQL_STATUS_OK ||
      flat_records != 3u || flat_bytes != flat_reader.len ||
      flat_result.objects != 2u || flat_result.matches != 2u ||
      flat_writer.len !=
          strlen("{\"sta\\u0074us\":\"op\\u0065n\",\"id\":1}\n"
                 "{\"status\":\"closed\",\"status\":\"open\"}\n") ||
      memcmp(flat_writer.data,
             "{\"sta\\u0074us\":\"op\\u0065n\",\"id\":1}\n"
             "{\"status\":\"closed\",\"status\":\"open\"}\n",
             flat_writer.len) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 10;
  }
  if (flat_result.captures != 2u || capture_writer.len != 17u ||
      memcmp(capture_writer.data, "\"op\\u0065n\"\"open\"", 17u) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 30;
  }
  lql_json_spool_cleanup(&flat_spool);
  memset(&flat_reader, 0, sizeof(flat_reader));
  memset(&flat_writer, 0, sizeof(flat_writer));
  memset(&flat_result, 0, sizeof(flat_result));
  flat_reader.data = (const unsigned char *)range_flat_input;
  flat_reader.len = sizeof(range_flat_input) - 1u;
  flat_reader.chunk_size = 1u;
  flat_result.writer = &flat_writer;
  memset(&flat_request, 0, sizeof(flat_request));
  flat_request.reader = json_test_read;
  flat_request.reader_user = &flat_reader;
  flat_request.terms = range_flat_terms;
  flat_request.term_count =
      sizeof(range_flat_terms) / sizeof(range_flat_terms[0]);
  flat_request.spool = &flat_spool;
  flat_request.capture = 1;
  flat_request.record = json_flat_record;
  flat_request.record_user = &flat_result;
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK) {
    return 32;
  }
  if (lql_json_scan_flat_eq_ndjson(&flat_request, &flat_records, &flat_bytes,
                                   &flat_error) != LQL_STATUS_OK ||
      flat_records != 5u || flat_bytes != flat_reader.len ||
      flat_result.objects != 5u || flat_result.matches != 2u ||
      flat_writer.len != strlen("{\"code\":2}\n{\"code\":3.5}\n") ||
      memcmp(flat_writer.data, "{\"code\":2}\n{\"code\":3.5}\n",
             flat_writer.len) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 33;
  }
  lql_json_spool_cleanup(&flat_spool);
  memset(&flat_reader, 0, sizeof(flat_reader));
  memset(&flat_writer, 0, sizeof(flat_writer));
  memset(&flat_result, 0, sizeof(flat_result));
  memset(temporal_flat_terms, 0, sizeof(temporal_flat_terms));
  temporal_flat_terms[0].kind = LQL_JSON_FLAT_TERM_TEMPORAL_RANGE;
  temporal_flat_terms[0].field = "timestamp";
  temporal_flat_terms[0].field_len = 9u;
  temporal_flat_terms[0].path = NULL;
  temporal_flat_terms[0].path_len = 0u;
  temporal_flat_terms[0].path_segment_count = 0u;
  temporal_flat_terms[0].has_temporal_gte = 1;
  if (!lql_parse_temporal_literal("2026-03-05T10:28:21Z",
                                  &temporal_flat_terms[0].temporal_gte)) {
    return 34;
  }
  flat_reader.data = (const unsigned char *)temporal_flat_input;
  flat_reader.len = sizeof(temporal_flat_input) - 1u;
  flat_reader.chunk_size = 1u;
  flat_result.writer = &flat_writer;
  memset(&flat_request, 0, sizeof(flat_request));
  flat_request.reader = json_test_read;
  flat_request.reader_user = &flat_reader;
  flat_request.terms = temporal_flat_terms;
  flat_request.term_count = 1u;
  flat_request.spool = &flat_spool;
  flat_request.capture = 1;
  flat_request.record = json_flat_record;
  flat_request.record_user = &flat_result;
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK) {
    return 35;
  }
  if (lql_json_scan_flat_eq_ndjson(&flat_request, &flat_records, &flat_bytes,
                                   &flat_error) != LQL_STATUS_OK ||
      flat_records != 4u || flat_bytes != flat_reader.len ||
      flat_result.objects != 4u || flat_result.matches != 2u ||
      flat_writer.len != strlen("{\"timestamp\":\"2026-03-05T10:28:21Z\"}\n"
                                "{\"timestamp\":\"2026-03-05T10:29:21Z\"}\n") ||
      memcmp(flat_writer.data,
             "{\"timestamp\":\"2026-03-05T10:28:21Z\"}\n"
             "{\"timestamp\":\"2026-03-05T10:29:21Z\"}\n",
             flat_writer.len) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 36;
  }
  lql_json_spool_cleanup(&flat_spool);
  memset(&flat_reader, 0, sizeof(flat_reader));
  memset(&flat_writer, 0, sizeof(flat_writer));
  memset(&flat_result, 0, sizeof(flat_result));
  flat_reader.data = (const unsigned char *)scalar_flat_input;
  flat_reader.len = sizeof(scalar_flat_input) - 1u;
  flat_reader.chunk_size = 1u;
  flat_result.writer = &flat_writer;
  memset(&flat_request, 0, sizeof(flat_request));
  flat_request.reader = json_test_read;
  flat_request.reader_user = &flat_reader;
  flat_request.terms = scalar_flat_terms;
  flat_request.term_count =
      sizeof(scalar_flat_terms) / sizeof(scalar_flat_terms[0]);
  flat_request.spool = &flat_spool;
  flat_request.capture = 1;
  flat_request.record = json_flat_record;
  flat_request.record_user = &flat_result;
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK) {
    return 11;
  }
  if (lql_json_scan_flat_eq_ndjson(&flat_request, &flat_records, &flat_bytes,
                                   &flat_error) != LQL_STATUS_OK ||
      flat_records != 3u || flat_bytes != flat_reader.len ||
      flat_result.objects != 3u || flat_result.matches != 3u ||
      flat_writer.len !=
          strlen("{\"code\":1,\"enabled\":true,\"empty\":null}\n"
                 "{\"code\":1.0,\"enabled\":false,\"empty\":\"null\"}\n"
                 "{\"code\":2,\"enabled\":true,\"empty\":null}\n") ||
      memcmp(flat_writer.data,
             "{\"code\":1,\"enabled\":true,\"empty\":null}\n"
             "{\"code\":1.0,\"enabled\":false,\"empty\":\"null\"}\n"
             "{\"code\":2,\"enabled\":true,\"empty\":null}\n",
             flat_writer.len) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 12;
  }
  lql_json_spool_cleanup(&flat_spool);
  memset(&flat_reader, 0, sizeof(flat_reader));
  memset(&flat_writer, 0, sizeof(flat_writer));
  memset(&flat_result, 0, sizeof(flat_result));
  flat_reader.data = (const unsigned char *)nested_flat_input;
  flat_reader.len = sizeof(nested_flat_input) - 1u;
  flat_reader.chunk_size = 1u;
  flat_result.writer = &flat_writer;
  memset(&flat_request, 0, sizeof(flat_request));
  flat_request.reader = json_test_read;
  flat_request.reader_user = &flat_reader;
  flat_request.terms = nested_flat_terms;
  flat_request.term_count =
      sizeof(nested_flat_terms) / sizeof(nested_flat_terms[0]);
  flat_request.spool = &flat_spool;
  flat_request.capture = 1;
  flat_request.record = json_flat_record;
  flat_request.record_user = &flat_result;
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK) {
    return 13;
  }
  if (lql_json_scan_flat_eq_ndjson(&flat_request, &flat_records, &flat_bytes,
                                   &flat_error) != LQL_STATUS_OK ||
      flat_records != 3u || flat_bytes != flat_reader.len ||
      flat_result.objects != 3u || flat_result.matches != 2u ||
      flat_writer.len !=
          strlen("{\"me\\u0074a\":{\"sta\\u0074e\":\"open\"},\"ignored\":[1]}\n"
                 "{\"meta\":{\"state\":\"closed\",\"state\":\"open\"}}\n") ||
      memcmp(flat_writer.data,
             "{\"me\\u0074a\":{\"sta\\u0074e\":\"open\"},\"ignored\":[1]}\n"
             "{\"meta\":{\"state\":\"closed\",\"state\":\"open\"}}\n",
             flat_writer.len) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 14;
  }
  lql_json_spool_cleanup(&flat_spool);
  memset(&flat_reader, 0, sizeof(flat_reader));
  memset(&flat_writer, 0, sizeof(flat_writer));
  memset(&capture_writer, 0, sizeof(capture_writer));
  memset(&flat_result, 0, sizeof(flat_result));
  flat_reader.data = (const unsigned char *)indexed_flat_input;
  flat_reader.len = sizeof(indexed_flat_input) - 1u;
  flat_reader.chunk_size = 1u;
  flat_result.writer = &flat_writer;
  flat_result.capture_writer = &capture_writer;
  flat_result.capture_spans = capture_spans;
  memset(&flat_request, 0, sizeof(flat_request));
  flat_request.reader = json_test_read;
  flat_request.reader_user = &flat_reader;
  flat_request.terms = indexed_flat_terms;
  flat_request.term_count =
      sizeof(indexed_flat_terms) / sizeof(indexed_flat_terms[0]);
  flat_request.spool = &flat_spool;
  flat_request.capture = 1;
  flat_request.capture_keys = indexed_capture_keys;
  flat_request.capture_key_count = 1u;
  flat_request.capture_spans = capture_spans;
  flat_request.record = json_flat_record;
  flat_request.record_user = &flat_result;
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK) {
    return 15;
  }
  if (lql_json_scan_flat_eq_ndjson(&flat_request, &flat_records, &flat_bytes,
                                   &flat_error) != LQL_STATUS_OK ||
      flat_records != 3u || flat_bytes != flat_reader.len ||
      flat_result.objects != 3u || flat_result.matches != 1u ||
      flat_writer.len !=
          strlen("{\"items\":[{\"sku\":\"A\"},{\"sku\":\"B\"}]}\n") ||
      memcmp(flat_writer.data,
             "{\"items\":[{\"sku\":\"A\"},{\"sku\":\"B\"}]}\n",
             flat_writer.len) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 16;
  }
  if (flat_result.captures != 1u || capture_writer.len != 3u ||
      memcmp(capture_writer.data, "\"B\"", 3u) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 31;
  }
  lql_json_spool_cleanup(&flat_spool);
  memset(&flat_reader, 0, sizeof(flat_reader));
  memset(&flat_writer, 0, sizeof(flat_writer));
  memset(&flat_result, 0, sizeof(flat_result));
  flat_reader.data = (const unsigned char *)indexed_scalar_input;
  flat_reader.len = sizeof(indexed_scalar_input) - 1u;
  flat_reader.chunk_size = 1u;
  flat_result.writer = &flat_writer;
  memset(&flat_request, 0, sizeof(flat_request));
  flat_request.reader = json_test_read;
  flat_request.reader_user = &flat_reader;
  flat_request.terms = indexed_scalar_terms;
  flat_request.term_count =
      sizeof(indexed_scalar_terms) / sizeof(indexed_scalar_terms[0]);
  flat_request.spool = &flat_spool;
  flat_request.capture = 1;
  flat_request.record = json_flat_record;
  flat_request.record_user = &flat_result;
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK) {
    return 32;
  }
  if (lql_json_scan_flat_eq_ndjson(&flat_request, &flat_records, &flat_bytes,
                                   &flat_error) != LQL_STATUS_OK ||
      flat_records != 3u || flat_bytes != flat_reader.len ||
      flat_result.objects != 3u || flat_result.matches != 1u ||
      flat_writer.len != strlen("{\"values\":[\"A\",\"B\"]}\n") ||
      memcmp(flat_writer.data, "{\"values\":[\"A\",\"B\"]}\n",
             flat_writer.len) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 33;
  }
  lql_json_spool_cleanup(&flat_spool);
  memset(&flat_reader, 0, sizeof(flat_reader));
  memset(&flat_writer, 0, sizeof(flat_writer));
  memset(&flat_result, 0, sizeof(flat_result));
  flat_reader.data = (const unsigned char *)indexed_scalar_input;
  flat_reader.len = sizeof(indexed_scalar_input) - 1u;
  flat_reader.chunk_size = 1u;
  flat_result.writer = &flat_writer;
  memset(&flat_request, 0, sizeof(flat_request));
  flat_request.reader = json_test_read;
  flat_request.reader_user = &flat_reader;
  flat_request.terms = array_scalar_wildcard_terms;
  flat_request.term_count = sizeof(array_scalar_wildcard_terms) /
                            sizeof(array_scalar_wildcard_terms[0]);
  flat_request.spool = &flat_spool;
  flat_request.capture = 1;
  flat_request.record = json_flat_record;
  flat_request.record_user = &flat_result;
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK) {
    return 34;
  }
  if (lql_json_scan_flat_eq_ndjson(&flat_request, &flat_records, &flat_bytes,
                                   &flat_error) != LQL_STATUS_OK ||
      flat_records != 3u || flat_bytes != flat_reader.len ||
      flat_result.objects != 3u || flat_result.matches != 2u ||
      flat_writer.len != strlen("{\"values\":[\"A\",\"B\"]}\n"
                                "{\"values\":[\"B\",\"A\"]}\n") ||
      memcmp(flat_writer.data,
             "{\"values\":[\"A\",\"B\"]}\n{\"values\":[\"B\",\"A\"]}\n",
             flat_writer.len) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 35;
  }
  lql_json_spool_cleanup(&flat_spool);
  memset(&flat_reader, 0, sizeof(flat_reader));
  memset(&flat_writer, 0, sizeof(flat_writer));
  memset(&flat_result, 0, sizeof(flat_result));
  flat_reader.data = (const unsigned char *)indexed_exists_input;
  flat_reader.len = sizeof(indexed_exists_input) - 1u;
  flat_reader.chunk_size = 1u;
  flat_result.writer = &flat_writer;
  memset(&flat_request, 0, sizeof(flat_request));
  flat_request.reader = json_test_read;
  flat_request.reader_user = &flat_reader;
  flat_request.terms = indexed_exists_terms;
  flat_request.term_count =
      sizeof(indexed_exists_terms) / sizeof(indexed_exists_terms[0]);
  flat_request.spool = &flat_spool;
  flat_request.capture = 1;
  flat_request.record = json_flat_record;
  flat_request.record_user = &flat_result;
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK) {
    return 36;
  }
  if (lql_json_scan_flat_eq_ndjson(&flat_request, &flat_records, &flat_bytes,
                                   &flat_error) != LQL_STATUS_OK ||
      flat_records != 3u || flat_bytes != flat_reader.len ||
      flat_result.objects != 3u || flat_result.matches != 1u ||
      flat_writer.len != strlen("{\"values\":[null,\"B\"]}\n") ||
      memcmp(flat_writer.data, "{\"values\":[null,\"B\"]}\n",
             flat_writer.len) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 37;
  }
  lql_json_spool_cleanup(&flat_spool);
  memset(&flat_reader, 0, sizeof(flat_reader));
  memset(&flat_writer, 0, sizeof(flat_writer));
  memset(&flat_result, 0, sizeof(flat_result));
  flat_reader.data = (const unsigned char *)array_wildcard_input;
  flat_reader.len = sizeof(array_wildcard_input) - 1u;
  flat_reader.chunk_size = 1u;
  flat_result.writer = &flat_writer;
  memset(&flat_request, 0, sizeof(flat_request));
  flat_request.reader = json_test_read;
  flat_request.reader_user = &flat_reader;
  flat_request.terms = array_wildcard_terms;
  flat_request.term_count =
      sizeof(array_wildcard_terms) / sizeof(array_wildcard_terms[0]);
  flat_request.spool = &flat_spool;
  flat_request.capture = 1;
  flat_request.record = json_flat_record;
  flat_request.record_user = &flat_result;
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK) {
    return 17;
  }
  if (lql_json_scan_flat_eq_ndjson(&flat_request, &flat_records, &flat_bytes,
                                   &flat_error) != LQL_STATUS_OK ||
      flat_records != 3u || flat_bytes != flat_reader.len ||
      flat_result.objects != 3u || flat_result.matches != 1u ||
      flat_writer.len !=
          strlen(
              "{\"array\":[{\"state\":\"closed\"},{\"state\":\"open\"}]}\n") ||
      memcmp(flat_writer.data,
             "{\"array\":[{\"state\":\"closed\"},{\"state\":\"open\"}]}\n",
             flat_writer.len) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 18;
  }
  lql_json_spool_cleanup(&flat_spool);
  memset(&flat_reader, 0, sizeof(flat_reader));
  memset(&flat_writer, 0, sizeof(flat_writer));
  memset(&flat_result, 0, sizeof(flat_result));
  flat_reader.data = (const unsigned char *)recursive_flat_input;
  flat_reader.len = sizeof(recursive_flat_input) - 1u;
  flat_reader.chunk_size = 1u;
  flat_result.writer = &flat_writer;
  memset(&flat_request, 0, sizeof(flat_request));
  flat_request.reader = json_test_read;
  flat_request.reader_user = &flat_reader;
  flat_request.terms = recursive_flat_terms;
  flat_request.term_count =
      sizeof(recursive_flat_terms) / sizeof(recursive_flat_terms[0]);
  flat_request.spool = &flat_spool;
  flat_request.capture = 1;
  flat_request.record = json_flat_record;
  flat_request.record_user = &flat_result;
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK) {
    return 19;
  }
  if (lql_json_scan_flat_eq_ndjson(&flat_request, &flat_records, &flat_bytes,
                                   &flat_error) != LQL_STATUS_OK ||
      flat_records != 3u || flat_bytes != flat_reader.len ||
      flat_result.objects != 3u || flat_result.matches != 1u ||
      flat_writer.len !=
          strlen("{\"tree\":{\"branch\":{\"deep\":{\"sku\":\"needle\"}}}}\n") ||
      memcmp(flat_writer.data,
             "{\"tree\":{\"branch\":{\"deep\":{\"sku\":\"needle\"}}}}\n",
             flat_writer.len) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 20;
  }
  lql_json_spool_cleanup(&flat_spool);
  memset(&flat_reader, 0, sizeof(flat_reader));
  memset(&flat_writer, 0, sizeof(flat_writer));
  memset(&flat_result, 0, sizeof(flat_result));
  flat_reader.data = (const unsigned char *)contains_flat_input;
  flat_reader.len = sizeof(contains_flat_input) - 1u;
  flat_reader.chunk_size = 1u;
  flat_result.writer = &flat_writer;
  memset(&flat_request, 0, sizeof(flat_request));
  flat_request.reader = json_test_read;
  flat_request.reader_user = &flat_reader;
  flat_request.terms = contains_flat_terms;
  flat_request.term_count =
      sizeof(contains_flat_terms) / sizeof(contains_flat_terms[0]);
  flat_request.spool = &flat_spool;
  flat_request.capture = 1;
  flat_request.record = json_flat_record;
  flat_request.record_user = &flat_result;
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK) {
    return 21;
  }
  if (lql_json_scan_flat_eq_ndjson(&flat_request, &flat_records, &flat_bytes,
                                   &flat_error) != LQL_STATUS_OK ||
      flat_records != 3u || flat_bytes != flat_reader.len ||
      flat_result.objects != 3u || flat_result.matches != 2u ||
      flat_writer.len !=
          strlen("{\"msg\":\"xxababa\"}\n{\"msg\":\"xxa\\u0062a\"}\n") ||
      memcmp(flat_writer.data,
             "{\"msg\":\"xxababa\"}\n{\"msg\":\"xxa\\u0062a\"}\n",
             flat_writer.len) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 22;
  }
  lql_json_spool_cleanup(&flat_spool);
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK ||
      lql_json_spool_append(&flat_spool, "abc", 3u, &flat_error) !=
          LQL_STATUS_OK) {
    lql_json_spool_cleanup(&flat_spool);
    return 23;
  }
  lql_json_spool_reader_init(&spool_reader, &flat_spool);
  if (lql_json_spool_read(&spool_reader, spool_bytes, 2u, &spool_len,
                          &flat_error) != LQL_STATUS_OK ||
      spool_len != 2u || memcmp(spool_bytes, "ab", 2u) != 0 ||
      lql_json_spool_read(&spool_reader, spool_bytes, sizeof(spool_bytes),
                          &spool_len, &flat_error) != LQL_STATUS_OK ||
      spool_len != 1u || spool_bytes[0] != (unsigned char)'c' ||
      lql_json_spool_read(&spool_reader, spool_bytes, sizeof(spool_bytes),
                          &spool_len, &flat_error) != LQL_STATUS_OK ||
      spool_len != 0u) {
    lql_json_spool_cleanup(&flat_spool);
    return 24;
  }
  memset(&flat_writer, 0, sizeof(flat_writer));
  if (lql_json_spool_write_slice(&flat_spool, 1u, 2u, json_test_write,
                                 &flat_writer, &flat_error) != LQL_STATUS_OK ||
      flat_writer.len != 2u || memcmp(flat_writer.data, "bc", 2u) != 0) {
    lql_json_spool_cleanup(&flat_spool);
    return 28;
  }
  lql_json_spool_cleanup(&flat_spool);
  memset(spool_fill, (int)'x', sizeof(spool_fill));
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK) {
    return 25;
  }
  for (spool_index = 0u;
       spool_index <= LQL_JSON_SPOOL_MEMORY_BYTES / sizeof(spool_fill);
       ++spool_index) {
    if (lql_json_spool_append(&flat_spool, spool_fill, sizeof(spool_fill),
                              &flat_error) != LQL_STATUS_OK) {
      lql_json_spool_cleanup(&flat_spool);
      return 26;
    }
  }
  lql_json_spool_reader_init(&spool_reader, &flat_spool);
  if (flat_spool.file == NULL ||
      lql_json_spool_read(&spool_reader, spool_bytes, sizeof(spool_bytes),
                          &spool_len, &flat_error) != LQL_STATUS_OK ||
      spool_len != sizeof(spool_bytes) ||
      spool_bytes[0] != (unsigned char)'x' ||
      spool_bytes[1] != (unsigned char)'x' ||
      spool_bytes[2] != (unsigned char)'x') {
    lql_json_spool_cleanup(&flat_spool);
    return 27;
  }
  memset(&flat_writer, 0, sizeof(flat_writer));
  if (lql_json_spool_write_slice(&flat_spool, 1024u, sizeof(spool_bytes),
                                 json_test_write, &flat_writer,
                                 &flat_error) != LQL_STATUS_OK ||
      flat_writer.len != sizeof(spool_bytes) ||
      flat_writer.data[0] != (unsigned char)'x' ||
      flat_writer.data[1] != (unsigned char)'x' ||
      flat_writer.data[2] != (unsigned char)'x') {
    lql_json_spool_cleanup(&flat_spool);
    return 29;
  }
  lql_json_spool_cleanup(&flat_spool);
  memset(spool_fill, (int)'a', sizeof(spool_fill));
  lql_error_init(&flat_error);
  if (lql_json_spool_init(&flat_spool, &flat_error) != LQL_STATUS_OK ||
      lql_json_spool_append(&flat_spool, "\"", 1u, &flat_error) !=
          LQL_STATUS_OK ||
      lql_json_spool_append(&flat_spool, spool_fill, sizeof(spool_fill),
                            &flat_error) != LQL_STATUS_OK ||
      lql_json_spool_append(&flat_spool, spool_fill, sizeof(spool_fill),
                            &flat_error) != LQL_STATUS_OK ||
      lql_json_spool_append(&flat_spool, spool_fill, sizeof(spool_fill),
                            &flat_error) != LQL_STATUS_OK ||
      lql_json_spool_append(&flat_spool, spool_fill, 1023u, &flat_error) !=
          LQL_STATUS_OK ||
      lql_json_spool_append(&flat_spool, "\\\"", 2u, &flat_error) !=
          LQL_STATUS_OK) {
    lql_json_spool_cleanup(&flat_spool);
    return 30;
  }
  for (spool_index = 0u;
       spool_index <= LQL_JSON_SPOOL_MEMORY_BYTES / sizeof(spool_fill);
       ++spool_index) {
    if (lql_json_spool_append(&flat_spool, spool_fill, sizeof(spool_fill),
                              &flat_error) != LQL_STATUS_OK) {
      lql_json_spool_cleanup(&flat_spool);
      return 31;
    }
  }
  if (lql_json_spool_append(&flat_spool, "\"", 1u, &flat_error) !=
          LQL_STATUS_OK ||
      flat_spool.file == NULL ||
      lql_json_spool_find_string_end(
          &flat_spool, 0u, lql_json_spool_size(&flat_spool), &string_end,
          &flat_error) != LQL_STATUS_OK ||
      string_end != lql_json_spool_size(&flat_spool)) {
    lql_json_spool_cleanup(&flat_spool);
    return 32;
  }
  lql_json_spool_cleanup(&flat_spool);
  return 0;
}
