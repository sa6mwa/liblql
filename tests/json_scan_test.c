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
  size_t objects;
  size_t matches;
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
                                   int root_is_object, int matched,
                                   const lql_json_spool *spool,
                                   lql_error *error) {
  json_flat_result *result;
  (void)record_index;
  result = (json_flat_result *)user;
  if (result == NULL || spool == NULL) {
    return LQL_STATUS_CALLBACK_ERROR;
  }
  if (root_is_object) {
    ++result->objects;
  }
  if (!matched) {
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

static int json_test_run(const char *input, size_t chunk_size,
                         const char *expected, lql_status expected_status,
                         size_t expected_records) {
  lql_json_normalize_request request;
  json_test_reader reader;
  json_test_writer writer;
  lql_error error;
  size_t records;
  size_t bytes_read;
  lql_status status;
  memset(&reader, 0, sizeof(reader));
  memset(&writer, 0, sizeof(writer));
  reader.data = (const unsigned char *)input;
  reader.len = strlen(input);
  reader.chunk_size = chunk_size;
  memset(&request, 0, sizeof(request));
  request.reader = json_test_read;
  request.reader_user = &reader;
  request.writer = json_test_write;
  request.writer_user = &writer;
  lql_error_init(&error);
  status = lql_json_normalize_ndjson(&request, &records, &bytes_read, &error);
  if (status != expected_status || records != expected_records ||
      (expected_status == LQL_STATUS_OK && bytes_read != reader.len) ||
      (expected != NULL && (writer.len != strlen(expected) ||
                            memcmp(writer.data, expected, writer.len) != 0))) {
    return 1;
  }
  return 0;
}

int main(void) {
  static const char flat_input[] =
      " { \"sta\\u0074us\" : \"op\\u0065n\", \"id\" : 1 }\n"
      " { \"status\" : \"closed\", \"status\" : \"open\" }\n"
      " \"open\"\n";
  lql_json_flat_eq_request flat_request;
  lql_json_spool flat_spool;
  json_test_reader flat_reader;
  json_test_writer flat_writer;
  json_flat_result flat_result;
  lql_error flat_error;
  size_t flat_records;
  size_t flat_bytes;
  if (json_test_run(" { \"status\" : \"open\", \"nested\" : [ true, null, "
                    "-1.2e+3 ], \"face\" : \"\\uD83D\\uDE00\" }\n"
                    " \"scalar\\tvalue\" \n",
                    1u,
                    "{\"status\":\"open\",\"nested\":[true,null,-1.2e+3],"
                    "\"face\":\"\\uD83D\\uDE00\"}\n\"scalar\\tvalue\"\n",
                    LQL_STATUS_OK, 2u)) {
    return 1;
  }
  if (json_test_run("{\"first\":1}\n{\"bad\":}\n", 3u, NULL,
                    LQL_STATUS_JSON_ERROR, 1u)) {
    return 2;
  }
  if (json_test_run("[1,2]\n", 2u, "", LQL_STATUS_JSON_ERROR, 0u)) {
    return 3;
  }
  if (json_test_run("{\"bad\":\"\\uD800\"}\n", 2u, NULL, LQL_STATUS_JSON_ERROR,
                    0u)) {
    return 4;
  }
  if (json_test_run("{\"n\":01}\n", 4u, NULL, LQL_STATUS_JSON_ERROR, 0u)) {
    return 5;
  }
  if (json_test_run("{\"first\":1}{\"second\":2}", 2u, NULL,
                    LQL_STATUS_JSON_ERROR, 0u)) {
    return 6;
  }
  if (json_test_run("{\"text\":\"\303\245\"}\n", 1u,
                    "{\"text\":\"\303\245\"}\n", LQL_STATUS_OK, 1u)) {
    return 7;
  }
  if (json_test_run("{\"text\":\"\300\200\"}\n", 1u, NULL,
                    LQL_STATUS_JSON_ERROR, 0u)) {
    return 8;
  }
  memset(&flat_reader, 0, sizeof(flat_reader));
  memset(&flat_writer, 0, sizeof(flat_writer));
  memset(&flat_result, 0, sizeof(flat_result));
  flat_reader.data = (const unsigned char *)flat_input;
  flat_reader.len = sizeof(flat_input) - 1u;
  flat_reader.chunk_size = 1u;
  flat_result.writer = &flat_writer;
  memset(&flat_request, 0, sizeof(flat_request));
  flat_request.reader = json_test_read;
  flat_request.reader_user = &flat_reader;
  flat_request.field = "status";
  flat_request.field_len = 6u;
  flat_request.value = "open";
  flat_request.value_len = 4u;
  flat_request.spool = &flat_spool;
  flat_request.capture = 1;
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
  lql_json_spool_cleanup(&flat_spool);
  return 0;
}
