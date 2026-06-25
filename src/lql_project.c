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

typedef struct projection_path {
  char **segments;
  size_t segment_count;
} projection_path;

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
  const projection_path *open_path;
  size_t open_count;
} projection_state;

struct lql_projection {
  projection_path *paths;
  size_t path_count;
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

static int segment_is_array_index(const char *field) {
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

static int add_segment(projection_path *path, char *segment) {
  char **next;
  next = (char **)realloc(path->segments,
                          sizeof(path->segments[0]) *
                              (path->segment_count + 1u));
  if (next == NULL) {
    return 0;
  }
  path->segments = next;
  path->segments[path->segment_count++] = segment;
  return 1;
}

static char *decode_path_segment(const char *src, size_t len) {
  char *out;
  size_t i;
  size_t j;
  out = (char *)malloc(len + 1u);
  if (out == NULL) {
    return NULL;
  }
  i = 0u;
  j = 0u;
  while (i < len) {
    if (src[i] == '~' && i + 1u < len) {
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
  return out;
}

static void projection_path_cleanup(projection_path *path) {
  size_t i;
  if (path == NULL) {
    return;
  }
  for (i = 0u; i < path->segment_count; ++i) {
    free(path->segments[i]);
  }
  free(path->segments);
  path->segments = NULL;
  path->segment_count = 0u;
}

static int parse_projection_path(const char *raw, projection_path *out) {
  const char *start;
  const char *end;
  const char *seg;
  char *decoded;
  size_t len;
  memset(out, 0, sizeof(*out));
  if (raw == NULL) {
    return 0;
  }
  start = raw;
  while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
    ++start;
  }
  end = start + strlen(start);
  while (end > start &&
         (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
          end[-1] == '\n')) {
    --end;
  }
  if (end == start) {
    return 1;
  }
  if (*start != '/' || end == start + 1) {
    return 0;
  }
  seg = start + 1;
  while (seg <= end) {
    const char *slash;
    slash = seg;
    while (slash < end && *slash != '/') {
      ++slash;
    }
    len = (size_t)(slash - seg);
    decoded = decode_path_segment(seg, len);
    if (decoded == NULL) {
      projection_path_cleanup(out);
      return 0;
    }
    if (out->segment_count == 0u && segment_is_array_index(decoded)) {
      free(decoded);
      projection_path_cleanup(out);
      return 0;
    }
    if (segment_is_array_index(decoded)) {
      free(decoded);
      projection_path_cleanup(out);
      return 0;
    }
    if (!add_segment(out, decoded)) {
      free(decoded);
      projection_path_cleanup(out);
      return 0;
    }
    if (slash == end) {
      break;
    }
    seg = slash + 1;
  }
  return out->segment_count != 0u;
}

static int projection_paths_equal(const projection_path *a,
                                  const projection_path *b) {
  size_t i;
  if (a->segment_count != b->segment_count) {
    return 0;
  }
  for (i = 0u; i < a->segment_count; ++i) {
    if (strcmp(a->segments[i], b->segments[i]) != 0) {
      return 0;
    }
  }
  return 1;
}

static int projection_path_is_prefix(const projection_path *prefix,
                                     const projection_path *path) {
  size_t i;
  if (prefix->segment_count >= path->segment_count) {
    return 0;
  }
  for (i = 0u; i < prefix->segment_count; ++i) {
    if (strcmp(prefix->segments[i], path->segments[i]) != 0) {
      return 0;
    }
  }
  return 1;
}

static int add_path(lql_projection *projection, projection_path *path) {
  projection_path *next;
  size_t i;
  if (path->segment_count == 0u) {
    return 1;
  }
  for (i = 0u; i < projection->path_count; ++i) {
    if (projection_paths_equal(&projection->paths[i], path)) {
      projection_path_cleanup(path);
      return 1;
    }
    if (projection_path_is_prefix(&projection->paths[i], path) ||
        projection_path_is_prefix(path, &projection->paths[i])) {
      return 0;
    }
  }
  next = (projection_path *)realloc(
      projection->paths, sizeof(projection->paths[0]) *
                             (projection->path_count + 1u));
  if (next == NULL) {
    return 0;
  }
  projection->paths = next;
  projection->paths[projection->path_count++] = *path;
  path->segments = NULL;
  path->segment_count = 0u;
  return 1;
}

static int value_path_matches(const lonejson_value_path *value_path,
                              const projection_path *projection_path) {
  size_t i;
  if (value_path == NULL ||
      value_path->segment_count != projection_path->segment_count) {
    return 0;
  }
  for (i = 0u; i < projection_path->segment_count; ++i) {
    if (strlen(projection_path->segments[i]) != value_path->segments[i].len ||
        memcmp(projection_path->segments[i], value_path->segments[i].data,
               value_path->segments[i].len) != 0) {
      return 0;
    }
  }
  return 1;
}

static const projection_path *selected_path(const lql_projection *projection,
                                            const lonejson_value_path *path) {
  size_t i;
  if (projection == NULL) {
    return NULL;
  }
  for (i = 0u; i < projection->path_count; ++i) {
    if (value_path_matches(path, &projection->paths[i])) {
      return &projection->paths[i];
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

static size_t common_open_prefix(const projection_state *state,
                                 const projection_path *path,
                                 size_t parent_count) {
  size_t common;
  common = 0u;
  while (common < state->open_count && common < parent_count &&
         strcmp(state->open_path->segments[common], path->segments[common]) ==
             0) {
    ++common;
  }
  return common;
}

static lonejson_status close_open_objects(projection_state *state,
                                          size_t keep_count) {
  while (state->open_count > keep_count) {
    if (lonejson_writer_end_object(&state->writer, state->error) !=
        LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    --state->open_count;
  }
  if (state->open_count == 0u) {
    state->open_path = NULL;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_key(projection_state *state,
                                      const projection_path *path) {
  size_t parent_count;
  size_t common;
  size_t i;
  if (!state->object_started) {
    if (lonejson_writer_begin_object(&state->writer, state->error) !=
        LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->object_started = 1;
  }
  parent_count = path->segment_count - 1u;
  common = common_open_prefix(state, path, parent_count);
  if (close_open_objects(state, common) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  for (i = common; i < parent_count; ++i) {
    const char *segment;
    segment = path->segments[i];
    if (lonejson_writer_key(&state->writer, segment, strlen(segment),
                            state->error) != LONEJSON_STATUS_OK ||
        lonejson_writer_begin_object(&state->writer, state->error) !=
            LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->open_path = path;
    ++state->open_count;
  }
  if (lonejson_writer_key(&state->writer, path->segments[parent_count],
                          strlen(path->segments[parent_count]),
                          state->error) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  state->open_path = parent_count == 0u ? NULL : path;
  state->found = 1;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_object_begin(void *user,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  projection_state *state;
  const projection_path *projected_path;
  (void)error;
  state = (projection_state *)user;
  projected_path = selected_path(state->projection, path);
  if (projected_path != NULL && !state->capturing) {
    if (projection_key(state, projected_path) != LONEJSON_STATUS_OK ||
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
  const projection_path *projected_path;
  (void)error;
  state = (projection_state *)user;
  projected_path = selected_path(state->projection, path);
  if (projected_path != NULL && !state->capturing) {
    if (projection_key(state, projected_path) != LONEJSON_STATUS_OK ||
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
  const projection_path *projected_path;
  (void)error;
  state = (projection_state *)user;
  projected_path = selected_path(state->projection, path);
  if (projected_path != NULL && !state->capturing) {
    if (projection_key(state, projected_path) != LONEJSON_STATUS_OK ||
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
      state->capturing || selected_path(state->projection, path) != NULL;
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
  const projection_path *projected_path;
  (void)error;
  state = (projection_state *)user;
  projected_path = selected_path(state->projection, path);
  if (projected_path != NULL && !state->capturing &&
      projection_key(state, projected_path) != LONEJSON_STATUS_OK) {
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
  const projection_path *projected_path;
  (void)error;
  state = (projection_state *)user;
  projected_path = selected_path(state->projection, path);
  if (projected_path != NULL && !state->capturing &&
      projection_key(state, projected_path) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if ((state->capturing || projected_path != NULL) &&
      lonejson_writer_bool(&state->writer, value, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_null(void *user, const lonejson_value_path *path,
                               lonejson_error *error) {
  projection_state *state;
  const projection_path *projected_path;
  (void)error;
  state = (projection_state *)user;
  projected_path = selected_path(state->projection, path);
  if (projected_path != NULL && !state->capturing &&
      projection_key(state, projected_path) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if ((state->capturing || projected_path != NULL) &&
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
  projection_path path;
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
    if (!parse_projection_path(fields[i], &path)) {
      lql_projection_free(projection);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "invalid or unsupported projection field path");
      return LQL_STATUS_PARSE_ERROR;
    }
    if (!add_path(projection, &path)) {
      projection_path_cleanup(&path);
      lql_projection_free(projection);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "conflicting projection field path");
      return LQL_STATUS_PARSE_ERROR;
    }
  }
  if (projection->path_count == 0u) {
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
  for (i = 0u; i < projection->path_count; ++i) {
    projection_path_cleanup(&projection->paths[i]);
  }
  free(projection->paths);
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
    st = close_open_objects(&state, 0u);
  }
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
