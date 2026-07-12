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
  return 0;
}
