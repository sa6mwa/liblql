#include <lonejson.h>

#include <string.h>

typedef struct visitor_sink {
  char data[128];
  size_t len;
} visitor_sink;

static lonejson_status visitor_sink_write(void *user, const void *data,
                                          size_t len, lonejson_error *error) {
  visitor_sink *sink;
  (void)error;
  sink = (visitor_sink *)user;
  if (sink == NULL || len > sizeof(sink->data) - sink->len) {
    return LONEJSON_STATUS_OVERFLOW;
  }
  memcpy(sink->data + sink->len, data, len);
  sink->len += len;
  return LONEJSON_STATUS_OK;
}

int main(void) {
  lonejson *runtime;
  lonejson_writer writer;
  lonejson_writer_visitor adapter;
  lonejson_value_visitor visitor;
  void *user;
  lonejson_error error;
  visitor_sink sink;
  lonejson_status status;

  memset(&sink, 0, sizeof(sink));
  lonejson_error_init(&error);
  runtime = lonejson_new(NULL, &error);
  if (runtime == NULL) {
    return 1;
  }
  status = lonejson_writer_init_sink(runtime, &writer, visitor_sink_write,
                                     &sink, &error);
  if (status != LONEJSON_STATUS_OK) {
    lonejson_free(runtime);
    return 1;
  }
  lonejson_writer_visitor_init(&adapter);
  status = lonejson_writer_visitor_open(&adapter, &writer, &visitor, &user,
                                        &error);
  if (status != LONEJSON_STATUS_OK ||
      visitor.object_begin(user, &error) != LONEJSON_STATUS_OK ||
      visitor.object_key_begin(user, &error) != LONEJSON_STATUS_OK ||
      visitor.object_key_chunk(user, "m", 1u, &error) != LONEJSON_STATUS_OK ||
      visitor.object_key_chunk(user, "sg", 2u, &error) != LONEJSON_STATUS_OK ||
      visitor.object_key_end(user, &error) != LONEJSON_STATUS_OK ||
      visitor.string_begin(user, &error) != LONEJSON_STATUS_OK ||
      visitor.string_chunk(user, "hel", 3u, &error) != LONEJSON_STATUS_OK ||
      visitor.string_chunk(user, "lo", 2u, &error) != LONEJSON_STATUS_OK ||
      visitor.string_end(user, &error) != LONEJSON_STATUS_OK ||
      visitor.object_end(user, &error) != LONEJSON_STATUS_OK ||
      lonejson_writer_visitor_close(&adapter, &error) != LONEJSON_STATUS_OK ||
      lonejson_writer_finish(&writer, &error) != LONEJSON_STATUS_OK ||
      sink.len != strlen("{\"msg\":\"hello\"}") ||
      memcmp(sink.data, "{\"msg\":\"hello\"}", sink.len) != 0) {
    lonejson_writer_visitor_cleanup(&adapter);
    lonejson_writer_cleanup(&writer);
    lonejson_free(runtime);
    return 1;
  }
  lonejson_writer_cleanup(&writer);
  lonejson_free(runtime);
  return 0;
}
