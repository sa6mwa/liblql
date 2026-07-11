#include <lonejson.h>

#include <string.h>

typedef struct mapped_candidate_reader {
  const unsigned char *data;
  size_t len;
  size_t offset;
} mapped_candidate_reader;

typedef struct mapped_candidate_record {
  char status[6];
} mapped_candidate_record;

static const lonejson_field mapped_candidate_fields[] = {
    LONEJSON_FIELD_STRING_FIXED(mapped_candidate_record, status, "status",
                                LONEJSON_OVERFLOW_TRUNCATE_SILENT)};
LONEJSON_MAP_DEFINE(mapped_candidate_map, mapped_candidate_record,
                    mapped_candidate_fields);

static lonejson_read_result mapped_candidate_read(void *user,
                                                  unsigned char *buffer,
                                                  size_t capacity) {
  mapped_candidate_reader *reader;
  lonejson_read_result result;
  size_t amount;
  reader = (mapped_candidate_reader *)user;
  result = lonejson_default_read_result();
  if (reader == NULL || buffer == NULL || capacity == 0u) {
    result.error_code = 1;
    return result;
  }
  if (reader->offset == reader->len) {
    result.eof = 1;
    return result;
  }
  amount = reader->len - reader->offset;
  if (amount > 3u) {
    amount = 3u;
  }
  if (amount > capacity) {
    amount = capacity;
  }
  memcpy(buffer, reader->data + reader->offset, amount);
  reader->offset += amount;
  result.bytes_read = amount;
  result.eof = reader->offset == reader->len;
  return result;
}

int main(void) {
  static const char input[] = "{\"status\":\"open\"}\n42\n[1]\n";
  mapped_candidate_reader reader;
  lonejson *runtime;
  lonejson_stream *stream;
  lonejson_stream_result result;
  lonejson_error error;
  mapped_candidate_record record;

  lonejson_error_init(&error);
  runtime = lonejson_new(NULL, &error);
  if (runtime == NULL) {
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.data = (const unsigned char *)input;
  reader.len = sizeof(input) - 1u;
  stream = lonejson_stream_open_candidates_reader(
      runtime, &mapped_candidate_map, mapped_candidate_read, &reader, &error);
  if (stream == NULL) {
    lonejson_free(runtime);
    return 1;
  }
  memset(&record, 0, sizeof(record));
  result = stream->next(stream, &record, &error);
  if (result != LONEJSON_STREAM_OBJECT ||
      stream->root_type != LONEJSON_VALUE_OBJECT ||
      strcmp(record.status, "open") != 0) {
    stream->close(stream);
    lonejson_free(runtime);
    return 1;
  }
  memset(&record, 0, sizeof(record));
  result = stream->next(stream, &record, &error);
  if (result != LONEJSON_STREAM_VALUE ||
      stream->root_type != LONEJSON_VALUE_NUMBER || record.status[0] != '\0') {
    stream->close(stream);
    lonejson_free(runtime);
    return 1;
  }
  memset(&record, 0, sizeof(record));
  result = stream->next(stream, &record, &error);
  if (result != LONEJSON_STREAM_VALUE ||
      stream->root_type != LONEJSON_VALUE_ARRAY || record.status[0] != '\0') {
    stream->close(stream);
    lonejson_free(runtime);
    return 1;
  }
  result = stream->next(stream, &record, &error);
  if (result != LONEJSON_STREAM_EOF) {
    stream->close(stream);
    lonejson_free(runtime);
    return 1;
  }
  stream->close(stream);
  lonejson_free(runtime);
  return 0;
}
