#include <lonejson.h>

#include <string.h>

typedef struct object_sink {
  char data[256];
  size_t len;
} object_sink;

static lonejson_status object_sink_write(void *user, const void *data,
                                         size_t len, lonejson_error *error) {
  object_sink *sink;
  (void)error;
  sink = (object_sink *)user;
  if (sink == NULL || len > sizeof(sink->data) - sink->len) {
    return LONEJSON_STATUS_OVERFLOW;
  }
  memcpy(sink->data + sink->len, data, len);
  sink->len += len;
  return LONEJSON_STATUS_OK;
}

static lonejson_status object_emit_input(const lonejson_value_visitor *visitor,
                                         void *user, lonejson_error *error) {
  lonejson_status status;
  status = visitor->object_begin(user, error);
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_key_begin(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_key_chunk(user, "id", 2u, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_key_end(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->number_begin(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->number_chunk(user, "7", 1u, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->number_end(user, error);
  }
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
    status = visitor->object_key_begin(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_key_chunk(user, "payload", 7u, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_key_end(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_begin(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_key_begin(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_key_chunk(user, "name", 4u, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_key_end(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->string_begin(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->string_chunk(user, "x", 1u, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->string_end(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_end(user, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = visitor->object_end(user, error);
  }
  return status;
}

static int object_run(lonejson *runtime, lonejson_object_rewrite_options *opts,
                      const char *expected) {
  lonejson_object_rewriter rewriter;
  lonejson_value_visitor visitor;
  void *user;
  lonejson_error error;
  object_sink sink;
  lonejson_status status;

  memset(&sink, 0, sizeof(sink));
  lonejson_error_init(&error);
  lonejson_object_rewriter_init(&rewriter);
  status = lonejson_object_rewriter_open(&rewriter, runtime, object_sink_write,
                                         &sink, opts, &visitor, &user, &error);
  if (status == LONEJSON_STATUS_OK) {
    status = object_emit_input(&visitor, user, &error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_object_rewriter_close(&rewriter, &error);
  }
  if (status != LONEJSON_STATUS_OK || sink.len != strlen(expected) ||
      memcmp(sink.data, expected, sink.len) != 0) {
    lonejson_object_rewriter_cleanup(&rewriter);
    return 0;
  }
  return 1;
}

int main(void) {
  lonejson *runtime;
  lonejson_object_rewrite_options options;
  lonejson_error error;

  lonejson_error_init(&error);
  runtime = lonejson_new(NULL, &error);
  if (runtime == NULL) {
    return 1;
  }
  memset(&options, 0, sizeof(options));
  options.member_key = "status";
  options.action = LONEJSON_VALUE_REWRITE_REPLACE;
  options.scalar.kind = LONEJSON_OBJECT_REWRITE_SCALAR_BOOL;
  options.scalar.boolean_value = 1;
  if (!object_run(runtime, &options,
                  "{\"id\":7,\"status\":true,\"payload\":{\"name\":\"x\"}}")) {
    lonejson_free(runtime);
    return 1;
  }
  memset(&options, 0, sizeof(options));
  options.member_key = "status";
  options.action = LONEJSON_VALUE_REWRITE_DROP;
  if (!object_run(runtime, &options,
                  "{\"id\":7,\"payload\":{\"name\":\"x\"}}")) {
    lonejson_free(runtime);
    return 1;
  }
  memset(&options, 0, sizeof(options));
  options.member_key = "touched";
  options.action = LONEJSON_VALUE_REWRITE_REPLACE;
  options.scalar.kind = LONEJSON_OBJECT_REWRITE_SCALAR_BOOL;
  options.scalar.boolean_value = 1;
  options.append_if_missing = 1;
  if (!object_run(runtime, &options,
                  "{\"id\":7,\"status\":\"open\",\"payload\":{\"name\":\"x\"},\"touched\":true}")) {
    lonejson_free(runtime);
    return 1;
  }
  lonejson_free(runtime);
  return 0;
}
