#include <lonejson.h>

#include <string.h>

typedef struct tape_sink {
  char data[64];
  size_t len;
} tape_sink;

static lonejson_status tape_sink_write(void *user, const void *data, size_t len,
                                       lonejson_error *error) {
  tape_sink *sink;
  (void)error;
  sink = (tape_sink *)user;
  if (sink == NULL || len > sizeof(sink->data) - sink->len) {
    return LONEJSON_STATUS_OVERFLOW;
  }
  memcpy(sink->data + sink->len, data, len);
  sink->len += len;
  return LONEJSON_STATUS_OK;
}

static lonejson_status tape_emit_input(const lonejson_value_visitor *visitor,
                                       void *user, lonejson_error *error) {
  lonejson_status status;
  status = visitor->object_begin(user, error);
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_key_begin(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_key_chunk(user, "status", 6u, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_key_end(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->string_begin(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->string_chunk(user, "open", 4u, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->string_end(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_end(user, error);
  }
  return status;
}

int main(void) {
  lonejson *runtime;
  lonejson_value_event_tape tape;
  lonejson_value_rewriter rewriter;
  lonejson_spooled spool;
  lonejson_value_rewrite_options options;
  lonejson_value_visitor tape_visitor;
  lonejson_value_visitor rewriter_visitor;
  void *tape_user;
  void *rewriter_user;
  lonejson_error error;
  tape_sink sink;
  lonejson_status status;

  memset(&sink, 0, sizeof(sink));
  memset(&options, 0, sizeof(options));
  lonejson_error_init(&error);
  runtime = lonejson_new(NULL, &error);
  if (runtime == NULL) {
    return 1;
  }
  lonejson_value_event_tape_init(&tape);
  lonejson_value_rewriter_init(&rewriter);
  status = lonejson_value_event_tape_open(&tape, runtime, 1024u,
                                          &tape_visitor, &tape_user, &error);
  if (status != LONEJSON_STATUS_OK ||
      tape_emit_input(&tape_visitor, tape_user, &error) != LONEJSON_STATUS_OK ||
      lonejson_value_rewriter_open(&rewriter, runtime, tape_sink_write, &sink,
                                   &options, &rewriter_visitor, &rewriter_user,
                                   &error) != LONEJSON_STATUS_OK ||
      lonejson_value_event_tape_replay(&tape, &rewriter_visitor, rewriter_user,
                                       &error) != LONEJSON_STATUS_OK ||
      lonejson_value_rewriter_close(&rewriter, &error) != LONEJSON_STATUS_OK ||
      sink.len != strlen("{\"status\":\"open\"}") ||
      memcmp(sink.data, "{\"status\":\"open\"}", sink.len) != 0) {
    lonejson_value_rewriter_cleanup(&rewriter);
    lonejson_value_event_tape_cleanup(&tape);
    lonejson_free(runtime);
    return 1;
  }
  lonejson_value_event_tape_reset(&tape);
  memset(&sink, 0, sizeof(sink));
  if (tape_emit_input(&tape_visitor, tape_user, &error) != LONEJSON_STATUS_OK ||
      lonejson_value_rewriter_open(&rewriter, runtime, tape_sink_write, &sink,
                                   &options, &rewriter_visitor, &rewriter_user,
                                   &error) != LONEJSON_STATUS_OK ||
      lonejson_value_event_tape_replay(&tape, &rewriter_visitor, rewriter_user,
                                       &error) != LONEJSON_STATUS_OK ||
      lonejson_value_rewriter_close(&rewriter, &error) != LONEJSON_STATUS_OK ||
      sink.len != strlen("{\"status\":\"open\"}") ||
      memcmp(sink.data, "{\"status\":\"open\"}", sink.len) != 0) {
    lonejson_value_rewriter_cleanup(&rewriter);
    lonejson_value_event_tape_cleanup(&tape);
    lonejson_free(runtime);
    return 1;
  }
  lonejson_value_event_tape_reset(&tape);
  status = lonejson_value_event_tape_open(&tape, runtime, 1u, &tape_visitor,
                                          &tape_user, &error);
  if (status != LONEJSON_STATUS_OK ||
      tape_visitor.object_begin(tape_user, &error) != LONEJSON_STATUS_OK ||
      tape_visitor.object_end(tape_user, &error) != LONEJSON_STATUS_OVERFLOW) {
    lonejson_value_rewriter_cleanup(&rewriter);
    lonejson_value_event_tape_cleanup(&tape);
    lonejson_free(runtime);
    return 1;
  }
  lonejson_value_rewriter_cleanup(&rewriter);
  lonejson_value_event_tape_cleanup(&tape);
  lonejson_spooled_init(runtime, &spool);
  memset(&sink, 0, sizeof(sink));
  if (lonejson_spooled_append(&spool, "direct", 6u, &error) !=
          LONEJSON_STATUS_OK ||
      lonejson_spooled_write_to_sink(&spool, tape_sink_write, &sink, &error) !=
          LONEJSON_STATUS_OK ||
      sink.len != 6u || memcmp(sink.data, "direct", sink.len) != 0) {
    lonejson_spooled_cleanup(&spool);
    lonejson_free(runtime);
    return 1;
  }
  lonejson_spooled_cleanup(&spool);
  lonejson_free(runtime);
  return 0;
}
