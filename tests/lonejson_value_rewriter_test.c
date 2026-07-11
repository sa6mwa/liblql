#include <lonejson.h>

#include <string.h>

typedef struct rewriter_sink {
  char data[128];
  size_t len;
} rewriter_sink;

static lonejson_status rewriter_sink_write(void *user, const void *data,
                                           size_t len, lonejson_error *error) {
  rewriter_sink *sink;
  (void)error;
  sink = (rewriter_sink *)user;
  if (sink == NULL || len > sizeof(sink->data) - sink->len) {
    return LONEJSON_STATUS_OVERFLOW;
  }
  memcpy(sink->data + sink->len, data, len);
  sink->len += len;
  return LONEJSON_STATUS_OK;
}

static lonejson_status rewriter_emit_true(lonejson_writer *writer, void *user,
                                          lonejson_error *error) {
  (void)user;
  return lonejson_writer_bool(writer, 1, error);
}

int main(void) {
  static const char *const target[] = {"meta", "ready"};
  lonejson *runtime;
  lonejson_value_rewriter rewriter;
  lonejson_value_rewrite_options options;
  lonejson_value_visitor visitor;
  void *user;
  lonejson_error error;
  rewriter_sink sink;
  lonejson_status status;

  memset(&sink, 0, sizeof(sink));
  memset(&options, 0, sizeof(options));
  options.target_segments = target;
  options.target_segment_count = 2u;
  options.action = LONEJSON_VALUE_REWRITE_REPLACE;
  options.replacement.emit = rewriter_emit_true;
  lonejson_error_init(&error);
  runtime = lonejson_new(NULL, &error);
  if (runtime == NULL) {
    return 1;
  }
  lonejson_value_rewriter_init(&rewriter);
  status = lonejson_value_rewriter_open(&rewriter, runtime, rewriter_sink_write,
                                        &sink, &options, &visitor, &user,
                                        &error);
  if (status != LONEJSON_STATUS_OK ||
      visitor.object_begin(user, &error) != LONEJSON_STATUS_OK ||
      visitor.object_key_begin(user, &error) != LONEJSON_STATUS_OK ||
      visitor.object_key_chunk(user, "status", 6u, &error) !=
          LONEJSON_STATUS_OK ||
      visitor.object_key_end(user, &error) != LONEJSON_STATUS_OK ||
      visitor.string_begin(user, &error) != LONEJSON_STATUS_OK ||
      visitor.string_chunk(user, "open", 4u, &error) != LONEJSON_STATUS_OK ||
      visitor.string_end(user, &error) != LONEJSON_STATUS_OK ||
      visitor.object_end(user, &error) != LONEJSON_STATUS_OK ||
      lonejson_value_rewriter_close(&rewriter, &error) != LONEJSON_STATUS_OK ||
      sink.len != strlen("{\"status\":\"open\",\"meta\":{\"ready\":true}}") ||
      memcmp(sink.data, "{\"status\":\"open\",\"meta\":{\"ready\":true}}",
             sink.len) != 0) {
    lonejson_value_rewriter_cleanup(&rewriter);
    lonejson_free(runtime);
    return 1;
  }
  lonejson_value_rewriter_cleanup(&rewriter);
  lonejson_free(runtime);
  return 0;
}
