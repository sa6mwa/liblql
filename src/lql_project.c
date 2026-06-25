#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "lql_internal.h"

#include <lonejson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct limited_file_reader {
  FILE *file;
  lql_uint64 remaining;
} limited_file_reader;

typedef struct projection_state {
  const lql_projection *projection;
  lonejson_writer writer;
  lonejson_error *error;
  char *key_buf;
  size_t key_len;
  char *num_buf;
  size_t num_len;
  size_t capture_depth;
  int capturing;
  int in_string;
  int in_number;
  int found;
  int object_started;
} projection_state;

struct lql_projection {
  char **fields;
  size_t field_count;
};

static int seek_u64(FILE *file, lql_uint64 offset) {
  return fseeko(file, (off_t)offset, SEEK_SET) == 0;
}

static lonejson_status file_sink(void *user, const void *data, size_t len,
                                 lonejson_error *error) {
  FILE *out;
  (void)error;
  out = (FILE *)user;
  return fwrite(data, 1u, len, out) == len ? LONEJSON_STATUS_OK
                                           : LONEJSON_STATUS_CALLBACK_FAILED;
}

static lonejson_read_result limited_read(void *user, unsigned char *buffer,
                                         size_t capacity) {
  limited_file_reader *reader;
  lonejson_read_result result;
  size_t want;
  result = lonejson_default_read_result();
  reader = (limited_file_reader *)user;
  if (reader->remaining == 0u) {
    result.eof = 1;
    return result;
  }
  want = reader->remaining > (lql_uint64)capacity ? capacity
                                                  : (size_t)reader->remaining;
  result.bytes_read = fread(buffer, 1u, want, reader->file);
  reader->remaining -= (lql_uint64)result.bytes_read;
  if (result.bytes_read != want && ferror(reader->file)) {
    result.error_code = 1;
  }
  if (reader->remaining == 0u) {
    result.eof = 1;
  }
  return result;
}

static int field_is_array_index(const char *field) {
  size_t i;
  if (field == NULL || field[0] == '\0') {
    return 0;
  }
  for (i = 0u; field[i] != '\0'; ++i) {
    if (field[i] < '0' || field[i] > '9') {
      return 0;
    }
  }
  return 1;
}

static char *decode_root_field_path(const char *path) {
  const char *src;
  char *out;
  size_t len;
  size_t i;
  size_t j;
  if (path == NULL || path[0] != '/' || path[1] == '\0') {
    return NULL;
  }
  src = path + 1;
  len = strlen(src);
  out = (char *)malloc(len + 1u);
  if (out == NULL) {
    return NULL;
  }
  i = 0u;
  j = 0u;
  while (src[i] != '\0') {
    if (src[i] == '/') {
      free(out);
      return NULL;
    }
    if (src[i] == '~' && src[i + 1u] != '\0') {
      if (src[i + 1u] == '0') {
        out[j++] = '~';
        i += 2u;
        continue;
      }
      if (src[i + 1u] == '1') {
        out[j++] = '/';
        i += 2u;
        continue;
      }
    }
    out[j++] = src[i++];
  }
  out[j] = '\0';
  if (out[0] == '\0' || field_is_array_index(out)) {
    free(out);
    return NULL;
  }
  return out;
}

static int add_field(lql_projection *projection, const char *path) {
  char *decoded;
  char **next;
  size_t i;
  decoded = decode_root_field_path(path);
  if (decoded == NULL) {
    return 0;
  }
  for (i = 0u; i < projection->field_count; ++i) {
    if (strcmp(projection->fields[i], decoded) == 0) {
      free(decoded);
      return 1;
    }
  }
  next = (char **)realloc(projection->fields,
                          sizeof(projection->fields[0]) *
                              (projection->field_count + 1u));
  if (next == NULL) {
    free(decoded);
    return 0;
  }
  projection->fields = next;
  projection->fields[projection->field_count++] = decoded;
  return 1;
}

static int path_is_root_field(const lonejson_value_path *path,
                              const char *field) {
  return path != NULL && path->segment_count == 1u &&
         strlen(field) == path->segments[0].len &&
         memcmp(field, path->segments[0].data, path->segments[0].len) == 0;
}

static const char *selected_field(const lql_projection *projection,
                                  const lonejson_value_path *path) {
  size_t i;
  if (projection == NULL) {
    return NULL;
  }
  for (i = 0u; i < projection->field_count; ++i) {
    if (path_is_root_field(path, projection->fields[i])) {
      return projection->fields[i];
    }
  }
  return NULL;
}

static int append_buf(char **buf, size_t *len, const char *data, size_t n) {
  char *next;
  next = (char *)realloc(*buf, *len + n + 1u);
  if (next == NULL) {
    return 0;
  }
  *buf = next;
  memcpy(*buf + *len, data, n);
  *len += n;
  (*buf)[*len] = '\0';
  return 1;
}

static lonejson_status projection_key(projection_state *state,
                                      const char *key) {
  if (!state->object_started) {
    if (lonejson_writer_begin_object(&state->writer, state->error) !=
        LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->object_started = 1;
  }
  if (lonejson_writer_key(&state->writer, key, strlen(key), state->error) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  state->found = 1;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_object_begin(void *user,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  projection_state *state;
  const char *field;
  (void)error;
  state = (projection_state *)user;
  field = selected_field(state->projection, path);
  if (field != NULL && !state->capturing) {
    if (projection_key(state, field) != LONEJSON_STATUS_OK ||
        lonejson_writer_begin_object(&state->writer, state->error) !=
            LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->capturing = 1;
    state->capture_depth = path->segment_count;
    return LONEJSON_STATUS_OK;
  }
  if (state->capturing &&
      lonejson_writer_begin_object(&state->writer, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_object_end(void *user,
                                     const lonejson_value_path *path,
                                     lonejson_error *error) {
  projection_state *state;
  (void)error;
  state = (projection_state *)user;
  if (!state->capturing) {
    return LONEJSON_STATUS_OK;
  }
  if (lonejson_writer_end_object(&state->writer, state->error) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (path->segment_count == state->capture_depth) {
    state->capturing = 0;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_array_begin(void *user,
                                      const lonejson_value_path *path,
                                      lonejson_error *error) {
  projection_state *state;
  const char *field;
  (void)error;
  state = (projection_state *)user;
  field = selected_field(state->projection, path);
  if (field != NULL && !state->capturing) {
    if (projection_key(state, field) != LONEJSON_STATUS_OK ||
        lonejson_writer_begin_array(&state->writer, state->error) !=
            LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->capturing = 1;
    state->capture_depth = path->segment_count;
    return LONEJSON_STATUS_OK;
  }
  if (state->capturing &&
      lonejson_writer_begin_array(&state->writer, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_array_end(void *user, const lonejson_value_path *path,
                                    lonejson_error *error) {
  projection_state *state;
  (void)error;
  state = (projection_state *)user;
  if (!state->capturing) {
    return LONEJSON_STATUS_OK;
  }
  if (lonejson_writer_end_array(&state->writer, state->error) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (path->segment_count == state->capture_depth) {
    state->capturing = 0;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_object_key_begin(void *user,
                                           const lonejson_value_path *path,
                                           lonejson_error *error) {
  projection_state *state;
  (void)path;
  (void)error;
  state = (projection_state *)user;
  free(state->key_buf);
  state->key_buf = NULL;
  state->key_len = 0u;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_object_key_chunk(
    void *user, const lonejson_value_path *path, const char *data, size_t len,
    lonejson_error *error) {
  projection_state *state;
  (void)path;
  (void)error;
  state = (projection_state *)user;
  return append_buf(&state->key_buf, &state->key_len, data, len)
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_ALLOCATION_FAILED;
}

static lonejson_status on_object_key_end(void *user,
                                         const lonejson_value_path *path,
                                         lonejson_error *error) {
  projection_state *state;
  (void)path;
  (void)error;
  state = (projection_state *)user;
  if (state->capturing &&
      lonejson_writer_key(&state->writer, state->key_buf,
                          strlen(state->key_buf), state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_string_begin(void *user,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  projection_state *state;
  const char *field;
  (void)error;
  state = (projection_state *)user;
  field = selected_field(state->projection, path);
  if (field != NULL && !state->capturing) {
    if (projection_key(state, field) != LONEJSON_STATUS_OK ||
        lonejson_writer_string_begin(&state->writer, state->error) !=
            LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->in_string = 1;
  } else if (state->capturing) {
    if (lonejson_writer_string_begin(&state->writer, state->error) !=
        LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->in_string = 1;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_string_chunk(void *user,
                                       const lonejson_value_path *path,
                                       const char *data, size_t len,
                                       lonejson_error *error) {
  projection_state *state;
  (void)path;
  (void)error;
  state = (projection_state *)user;
  if (state->in_string &&
      lonejson_writer_string_chunk(&state->writer, data, len, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_string_end(void *user,
                                     const lonejson_value_path *path,
                                     lonejson_error *error) {
  projection_state *state;
  (void)path;
  (void)error;
  state = (projection_state *)user;
  if (state->in_string &&
      lonejson_writer_string_end(&state->writer, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  state->in_string = 0;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_number_begin(void *user,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  projection_state *state;
  (void)error;
  state = (projection_state *)user;
  free(state->num_buf);
  state->num_buf = NULL;
  state->num_len = 0u;
  state->in_number =
      state->capturing || selected_field(state->projection, path) != NULL;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_number_chunk(void *user,
                                       const lonejson_value_path *path,
                                       const char *data, size_t len,
                                       lonejson_error *error) {
  projection_state *state;
  (void)path;
  (void)error;
  state = (projection_state *)user;
  if (state->in_number &&
      !append_buf(&state->num_buf, &state->num_len, data, len)) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_number_end(void *user,
                                     const lonejson_value_path *path,
                                     lonejson_error *error) {
  projection_state *state;
  const char *field;
  (void)error;
  state = (projection_state *)user;
  field = selected_field(state->projection, path);
  if (field != NULL && !state->capturing &&
      projection_key(state, field) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (state->in_number &&
      lonejson_writer_number_text(&state->writer, state->num_buf,
                                  state->num_len, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  state->in_number = 0;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_boolean(void *user, const lonejson_value_path *path,
                                  int value, lonejson_error *error) {
  projection_state *state;
  const char *field;
  (void)error;
  state = (projection_state *)user;
  field = selected_field(state->projection, path);
  if (field != NULL && !state->capturing &&
      projection_key(state, field) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if ((state->capturing || field != NULL) &&
      lonejson_writer_bool(&state->writer, value, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_null(void *user, const lonejson_value_path *path,
                               lonejson_error *error) {
  projection_state *state;
  const char *field;
  (void)error;
  state = (projection_state *)user;
  field = selected_field(state->projection, path);
  if (field != NULL && !state->capturing &&
      projection_key(state, field) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if ((state->capturing || field != NULL) &&
      lonejson_writer_null(&state->writer, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static void init_projection_visitor(lonejson_path_value_visitor *visitor) {
  *visitor = lonejson_default_path_value_visitor();
  visitor->object_begin = on_object_begin;
  visitor->object_end = on_object_end;
  visitor->object_key_begin = on_object_key_begin;
  visitor->object_key_chunk = on_object_key_chunk;
  visitor->object_key_end = on_object_key_end;
  visitor->array_begin = on_array_begin;
  visitor->array_end = on_array_end;
  visitor->string_begin = on_string_begin;
  visitor->string_chunk = on_string_chunk;
  visitor->string_end = on_string_end;
  visitor->number_begin = on_number_begin;
  visitor->number_chunk = on_number_chunk;
  visitor->number_end = on_number_end;
  visitor->boolean_value = on_boolean;
  visitor->null_value = on_null;
}

lql_status lql_projection_parse(const char *const *fields, size_t field_count,
                                lql_projection **out, lql_error *error) {
  lql_projection *projection;
  size_t i;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out projection required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = NULL;
  if (fields == NULL || field_count == 0u) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "projection fields required");
    return LQL_STATUS_PARSE_ERROR;
  }
  projection = (lql_projection *)calloc(1u, sizeof(*projection));
  if (projection == NULL) {
    return LQL_STATUS_NO_MEMORY;
  }
  for (i = 0u; i < field_count; ++i) {
    if (!add_field(projection, fields[i])) {
      lql_projection_free(projection);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "invalid or unsupported projection field path");
      return LQL_STATUS_PARSE_ERROR;
    }
  }
  if (projection->field_count == 0u) {
    lql_projection_free(projection);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "projection fields required");
    return LQL_STATUS_PARSE_ERROR;
  }
  *out = projection;
  return LQL_STATUS_OK;
}

void lql_projection_free(lql_projection *projection) {
  size_t i;
  if (projection == NULL) {
    return;
  }
  for (i = 0u; i < projection->field_count; ++i) {
    free(projection->fields[i]);
  }
  free(projection->fields);
  free(projection);
}

lql_status lql_project_file_range(const lql_projection *projection, FILE *file,
                                  lql_uint64 offset, lql_uint64 size,
                                  FILE *out, int *out_found,
                                  lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  projection_state state;
  limited_file_reader reader;
  lonejson_status st;
  if (projection == NULL || file == NULL || out == NULL || out_found == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection, file, out, and out_found are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out_found = 0;
  if (!seek_u64(file, offset)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to seek projection source");
    return LQL_STATUS_JSON_ERROR;
  }
  runtime = lonejson_new(NULL, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  memset(&state, 0, sizeof(state));
  state.projection = projection;
  state.error = &lj_error;
  reader.file = file;
  reader.remaining = size;
  init_projection_visitor(&visitor);
  if (lonejson_writer_init_sink(runtime, &state.writer, file_sink, out,
                                &lj_error) != LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    lonejson_free(runtime);
    free(state.key_buf);
    free(state.num_buf);
    return LQL_STATUS_JSON_ERROR;
  }
  st = lonejson_visit_path_value_reader(runtime, limited_read, &reader,
                                        &visitor, &state, &lj_error);
  if (st == LONEJSON_STATUS_OK && state.object_started) {
    st = lonejson_writer_end_object(&state.writer, &lj_error);
  }
  if (st == LONEJSON_STATUS_OK && state.object_started) {
    st = lonejson_writer_finish(&state.writer, &lj_error);
  }
  *out_found = state.found;
  lonejson_writer_cleanup(&state.writer);
  lonejson_free(runtime);
  free(state.key_buf);
  free(state.num_buf);
  if (st != LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  return LQL_STATUS_OK;
}
