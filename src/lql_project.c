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

typedef struct buffer_reader {
  const unsigned char *data;
  size_t len;
  size_t offset;
} buffer_reader;

typedef struct projection_source_reader {
  lql_read_fn read;
  void *user;
  int error_code;
} projection_source_reader;

typedef struct projection_path {
  char **segments;
  size_t *segment_lens;
  unsigned char *segment_is_array_index;
  size_t *array_indexes;
  size_t segment_count;
} projection_path;

#define PROJECTION_KEY_INLINE_CAP 128u
#define PROJECTION_NUM_INLINE_CAP 64u

typedef struct projection_parse_context {
  lql *self;
  lql_allocator *allocator;
} projection_parse_context;

typedef struct projection_state {
  lql_allocator *allocator;
  const lql_projection *projection;
  lonejson_writer writer;
  lonejson_error *error;
  char *key_buf;
  size_t key_len;
  char inline_key_buf[PROJECTION_KEY_INLINE_CAP];
  char *num_buf;
  size_t num_len;
  char inline_num_buf[PROJECTION_NUM_INLINE_CAP];
  size_t capture_depth;
  int capturing;
  int in_string;
  int in_number;
  int found;
  int object_started;
  int root_seen;
  int root_is_object;
  const projection_path *open_path;
  size_t open_count;
  char *open_kind;
  size_t *open_array_next;
  size_t open_capacity;
} projection_state;

struct lql_projection {
  projection_path *paths;
  size_t path_count;
  size_t min_segment_count;
  size_t max_segment_count;
};

static int seek_u64(FILE *file, lql_uint64 offset) {
  off_t seek_offset;
  seek_offset = (off_t)offset;
  if (seek_offset < (off_t)0 || (lql_uint64)seek_offset != offset) {
    return 0;
  }
  return fseeko(file, seek_offset, SEEK_SET) == 0;
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
  if (result.bytes_read != want) {
    result.error_code = 1;
  }
  if (reader->remaining == 0u) {
    result.eof = 1;
  }
  return result;
}

static lonejson_read_result buffer_read(void *user, unsigned char *buffer,
                                        size_t capacity) {
  buffer_reader *reader;
  lonejson_read_result result;
  size_t remaining;
  size_t want;

  result = lonejson_default_read_result();
  reader = (buffer_reader *)user;
  if (reader->offset >= reader->len) {
    result.eof = 1;
    return result;
  }
  remaining = reader->len - reader->offset;
  want = remaining > capacity ? capacity : remaining;
  if (want != 0u) {
    memcpy(buffer, reader->data + reader->offset, want);
    reader->offset += want;
  }
  result.bytes_read = want;
  if (reader->offset >= reader->len) {
    result.eof = 1;
  }
  return result;
}

static lonejson_read_result
projection_source_read(void *user, unsigned char *buffer, size_t capacity) {
  projection_source_reader *reader;
  lql_read_result source_result;
  lonejson_read_result result;

  result = lonejson_default_read_result();
  reader = (projection_source_reader *)user;
  source_result = reader->read(reader->user, buffer, capacity);
  if (source_result.bytes_read > capacity) {
    reader->error_code = 1;
    result.error_code = 1;
    return result;
  }
  if (source_result.error_code != 0) {
    reader->error_code = source_result.error_code;
    result.error_code = source_result.error_code;
    return result;
  }
  result.bytes_read = source_result.bytes_read;
  result.eof = source_result.eof;
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

static int parse_array_index(const char *field, size_t *out) {
  unsigned long value;
  char *end;
  if (!segment_is_array_index(field)) {
    return 0;
  }
  value = strtoul(field, &end, 10);
  if (end == field || *end != '\0' || value > 1048576ul) {
    return 0;
  }
  *out = (size_t)value;
  return 1;
}

static char path_container_kind(const projection_path *path, size_t index) {
  if (index + 1u < path->segment_count &&
      path->segment_is_array_index[index + 1u]) {
    return 'a';
  }
  return 'o';
}

static int add_segment(projection_parse_context *ctx, projection_path *path,
                       char *segment) {
  char **next_segments;
  size_t *next_lens;
  unsigned char *next_is_array_index;
  size_t *next_array_indexes;
  size_t index;
  int is_array_index;
  size_t array_index;

  index = path->segment_count;
  is_array_index = parse_array_index(segment, &array_index);
  next_segments = (char **)ctx->allocator->realloc(
      ctx->allocator, path->segments,
      sizeof(path->segments[0]) * (index + 1u));
  if (next_segments == NULL) {
    return 0;
  }
  path->segments = next_segments;
  next_lens = (size_t *)ctx->allocator->realloc(
      ctx->allocator, path->segment_lens,
      sizeof(path->segment_lens[0]) * (index + 1u));
  if (next_lens == NULL) {
    return 0;
  }
  path->segment_lens = next_lens;
  next_is_array_index = (unsigned char *)ctx->allocator->realloc(
      ctx->allocator, path->segment_is_array_index,
      sizeof(path->segment_is_array_index[0]) * (index + 1u));
  if (next_is_array_index == NULL) {
    return 0;
  }
  path->segment_is_array_index = next_is_array_index;
  next_array_indexes = (size_t *)ctx->allocator->realloc(
      ctx->allocator, path->array_indexes,
      sizeof(path->array_indexes[0]) * (index + 1u));
  if (next_array_indexes == NULL) {
    return 0;
  }
  path->array_indexes = next_array_indexes;
  path->segments[index] = segment;
  path->segment_lens[index] = strlen(segment);
  path->segment_is_array_index[index] = is_array_index ? 1u : 0u;
  path->array_indexes[index] = is_array_index ? array_index : 0u;
  path->segment_count = index + 1u;
  return 1;
}

static char *decode_path_segment(projection_parse_context *ctx, const char *src,
                                 size_t len) {
  char *out;
  size_t i;
  size_t j;
  out = (char *)ctx->allocator->alloc(ctx->allocator, len + 1u);
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

static void projection_path_cleanup(lql *self, projection_path *path) {
  lql_allocator *allocator;
  size_t i;
  if (path == NULL) {
    return;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    return;
  }
  for (i = 0u; i < path->segment_count; ++i) {
    allocator->destroy(allocator, path->segments[i]);
  }
  allocator->destroy(allocator, path->segments);
  allocator->destroy(allocator, path->segment_lens);
  allocator->destroy(allocator, path->segment_is_array_index);
  allocator->destroy(allocator, path->array_indexes);
  path->segments = NULL;
  path->segment_lens = NULL;
  path->segment_is_array_index = NULL;
  path->array_indexes = NULL;
  path->segment_count = 0u;
}

static int parse_projection_path(projection_parse_context *ctx, const char *raw,
                                 projection_path *out) {
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
  while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
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
    decoded = decode_path_segment(ctx, seg, len);
    if (decoded == NULL) {
      projection_path_cleanup(ctx->self, out);
      return 0;
    }
    if (parse_array_index(decoded, &len)) {
      if (out->segment_count == 0u) {
        ctx->allocator->destroy(ctx->allocator, decoded);
        projection_path_cleanup(ctx->self, out);
        return 0;
      }
    } else if (segment_is_array_index(decoded)) {
      ctx->allocator->destroy(ctx->allocator, decoded);
      projection_path_cleanup(ctx->self, out);
      return 0;
    }
    if (!add_segment(ctx, out, decoded)) {
      ctx->allocator->destroy(ctx->allocator, decoded);
      projection_path_cleanup(ctx->self, out);
      return 0;
    }
    if (slash == end) {
      break;
    }
    seg = slash + 1;
  }
  return out->segment_count != 0u;
}

static int projection_segments_equal(const projection_path *a,
                                     size_t a_index,
                                     const projection_path *b,
                                     size_t b_index) {
  return a->segment_lens[a_index] == b->segment_lens[b_index] &&
         memcmp(a->segments[a_index], b->segments[b_index],
                a->segment_lens[a_index]) == 0;
}

static int projection_paths_equal(const projection_path *a,
                                  const projection_path *b) {
  size_t i;
  if (a->segment_count != b->segment_count) {
    return 0;
  }
  for (i = 0u; i < a->segment_count; ++i) {
    if (!projection_segments_equal(a, i, b, i)) {
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
    if (!projection_segments_equal(prefix, i, path, i)) {
      return 0;
    }
  }
  return 1;
}

static int projection_paths_have_container_conflict(const projection_path *a,
                                                    const projection_path *b) {
  size_t i;
  size_t a_parent_count;
  size_t b_parent_count;
  a_parent_count = a->segment_count == 0u ? 0u : a->segment_count - 1u;
  b_parent_count = b->segment_count == 0u ? 0u : b->segment_count - 1u;
  for (i = 0u; i < a_parent_count && i < b_parent_count; ++i) {
    if (!projection_segments_equal(a, i, b, i)) {
      return 0;
    }
    if (path_container_kind(a, i) != path_container_kind(b, i)) {
      return 1;
    }
  }
  return 0;
}

static int add_path(projection_parse_context *ctx, lql_projection *projection,
                    projection_path *path) {
  projection_path *next;
  size_t i;
  if (path->segment_count == 0u) {
    return 1;
  }
  for (i = 0u; i < projection->path_count; ++i) {
    if (projection_paths_equal(&projection->paths[i], path)) {
      projection_path_cleanup(ctx->self, path);
      return 1;
    }
    if (projection_path_is_prefix(&projection->paths[i], path) ||
        projection_path_is_prefix(path, &projection->paths[i])) {
      return 0;
    }
    if (projection_paths_have_container_conflict(&projection->paths[i], path)) {
      return 0;
    }
  }
  next = (projection_path *)ctx->allocator->realloc(
      ctx->allocator, projection->paths,
      sizeof(projection->paths[0]) * (projection->path_count + 1u));
  if (next == NULL) {
    return 0;
  }
  projection->paths = next;
  if (projection->path_count == 0u ||
      path->segment_count < projection->min_segment_count) {
    projection->min_segment_count = path->segment_count;
  }
  if (path->segment_count > projection->max_segment_count) {
    projection->max_segment_count = path->segment_count;
  }
  projection->paths[projection->path_count++] = *path;
  path->segments = NULL;
  path->segment_lens = NULL;
  path->segment_is_array_index = NULL;
  path->array_indexes = NULL;
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
    if (projection_path->segment_lens[i] != value_path->segments[i].len ||
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
  if (projection == NULL || path == NULL ||
      path->segment_count < projection->min_segment_count ||
      path->segment_count > projection->max_segment_count) {
    return NULL;
  }
  if (projection->path_count == 1u) {
    return value_path_matches(path, &projection->paths[0])
               ? &projection->paths[0]
               : NULL;
  }
  for (i = 0u; i < projection->path_count; ++i) {
    if (value_path_matches(path, &projection->paths[i])) {
      return &projection->paths[i];
    }
  }
  return NULL;
}

static void projection_inline_buf_reset(projection_state *state, char **buf,
                                        size_t *len, char *inline_buf) {
  if (state == NULL || buf == NULL || len == NULL) {
    return;
  }
  if (*buf != NULL && *buf != inline_buf) {
    state->allocator->destroy(state->allocator, *buf);
  }
  *buf = NULL;
  *len = 0u;
}

static int projection_inline_buf_append(projection_state *state, char **buf,
                                        size_t *len, char *inline_buf,
                                        size_t inline_cap, const char *data,
                                        size_t n) {
  char *next;
  size_t next_len;
  if (state == NULL || buf == NULL || len == NULL || inline_buf == NULL ||
      data == NULL) {
    return 0;
  }
  if (n > (size_t)-1 - *len - 1u) {
    return 0;
  }
  next_len = *len + n;
  if (*buf == NULL || *buf == inline_buf) {
    if (next_len + 1u <= inline_cap) {
      if (*buf == NULL) {
        *buf = inline_buf;
      }
      memcpy(inline_buf + *len, data, n);
      *len = next_len;
      inline_buf[*len] = '\0';
      return 1;
    }
    next = (char *)state->allocator->alloc(state->allocator, next_len + 1u);
    if (next == NULL) {
      return 0;
    }
    if (*len != 0u) {
      memcpy(next, inline_buf, *len);
    }
  } else {
    next = (char *)state->allocator->realloc(state->allocator, *buf,
                                             next_len + 1u);
    if (next == NULL) {
      return 0;
    }
  }
  memcpy(next + *len, data, n);
  *len = next_len;
  next[*len] = '\0';
  *buf = next;
  return 1;
}

static void projection_key_reset(projection_state *state) {
  projection_inline_buf_reset(state, &state->key_buf, &state->key_len,
                              state->inline_key_buf);
}

static int projection_key_append(projection_state *state, const char *data,
                                 size_t n) {
  return projection_inline_buf_append(
      state, &state->key_buf, &state->key_len, state->inline_key_buf,
      sizeof(state->inline_key_buf), data, n);
}

static void projection_num_reset(projection_state *state) {
  projection_inline_buf_reset(state, &state->num_buf, &state->num_len,
                              state->inline_num_buf);
}

static int projection_num_append(projection_state *state, const char *data,
                                 size_t n) {
  return projection_inline_buf_append(
      state, &state->num_buf, &state->num_len, state->inline_num_buf,
      sizeof(state->inline_num_buf), data, n);
}

static size_t common_open_prefix(const projection_state *state,
                                 const projection_path *path,
                                 size_t parent_count) {
  size_t common;
  common = 0u;
  while (common < state->open_count && common < parent_count &&
         projection_segments_equal(state->open_path, common, path, common) &&
         state->open_kind[common] == path_container_kind(path, common)) {
    ++common;
  }
  return common;
}

static lonejson_status close_open_containers(projection_state *state,
                                             size_t keep_count) {
  while (state->open_count > keep_count) {
    if (state->open_kind[state->open_count - 1u] == 'a') {
      if (lonejson_writer_end_array(&state->writer, state->error) !=
          LONEJSON_STATUS_OK) {
        return LONEJSON_STATUS_CALLBACK_FAILED;
      }
    } else {
      if (lonejson_writer_end_object(&state->writer, state->error) !=
          LONEJSON_STATUS_OK) {
        return LONEJSON_STATUS_CALLBACK_FAILED;
      }
    }
    --state->open_count;
  }
  if (state->open_count == 0u) {
    state->open_path = NULL;
  }
  return LONEJSON_STATUS_OK;
}

static int ensure_open_capacity(projection_state *state, size_t need) {
  char *next_kind;
  size_t *next_array_next;
  size_t next_capacity;
  if (state->open_capacity >= need) {
    return 1;
  }
  next_capacity = state->open_capacity == 0u ? 4u : state->open_capacity;
  while (next_capacity < need) {
    next_capacity *= 2u;
  }
  next_kind = (char *)state->allocator->realloc(
      state->allocator, state->open_kind,
      sizeof(state->open_kind[0]) * next_capacity);
  if (next_kind == NULL) {
    return 0;
  }
  state->open_kind = next_kind;
  next_array_next = (size_t *)state->allocator->realloc(
      state->allocator, state->open_array_next,
      sizeof(state->open_array_next[0]) * next_capacity);
  if (next_array_next == NULL) {
    return 0;
  }
  state->open_array_next = next_array_next;
  state->open_capacity = next_capacity;
  return 1;
}

static lonejson_status write_array_index_prefix(projection_state *state,
                                                size_t level, size_t index) {
  while (state->open_array_next[level] < index) {
    if (lonejson_writer_null(&state->writer, state->error) !=
        LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    ++state->open_array_next[level];
  }
  if (state->open_array_next[level] != index) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  ++state->open_array_next[level];
  return LONEJSON_STATUS_OK;
}

static lonejson_status write_member_prefix(projection_state *state,
                                           const projection_path *path,
                                           size_t index) {
  if (index == 0u) {
    if (lonejson_writer_key(&state->writer, path->segments[index],
                            path->segment_lens[index],
                            state->error) != LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    return LONEJSON_STATUS_OK;
  }
  if (state->open_kind[index - 1u] == 'a') {
    return write_array_index_prefix(state, index - 1u,
                                    path->array_indexes[index]);
  }
  if (lonejson_writer_key(&state->writer, path->segments[index],
                          path->segment_lens[index],
                          state->error) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_key(projection_state *state,
                                      const projection_path *path) {
  size_t parent_count;
  size_t common;
  size_t i;
  char kind;
  if (!state->object_started) {
    if (lonejson_writer_begin_object(&state->writer, state->error) !=
        LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->object_started = 1;
  }
  parent_count = path->segment_count - 1u;
  common = common_open_prefix(state, path, parent_count);
  if (close_open_containers(state, common) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (!ensure_open_capacity(state, parent_count)) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  for (i = common; i < parent_count; ++i) {
    if (write_member_prefix(state, path, i) != LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    kind = path_container_kind(path, i);
    if (kind == 'a') {
      if (lonejson_writer_begin_array(&state->writer, state->error) !=
          LONEJSON_STATUS_OK) {
        return LONEJSON_STATUS_CALLBACK_FAILED;
      }
      state->open_array_next[i] = 0u;
    } else {
      if (lonejson_writer_begin_object(&state->writer, state->error) !=
          LONEJSON_STATUS_OK) {
        return LONEJSON_STATUS_CALLBACK_FAILED;
      }
      state->open_array_next[i] = 0u;
    }
    state->open_path = path;
    state->open_kind[i] = kind;
    ++state->open_count;
  }
  if (write_member_prefix(state, path, parent_count) != LONEJSON_STATUS_OK) {
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
  if (path != NULL && path->segment_count == 0u) {
    state->root_seen = 1;
    state->root_is_object = 1;
  }
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
  if (path != NULL && path->segment_count == 0u) {
    state->root_seen = 1;
    state->root_is_object = 0;
  }
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
  projection_key_reset(state);
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_object_key_chunk(void *user,
                                           const lonejson_value_path *path,
                                           const char *data, size_t len,
                                           lonejson_error *error) {
  projection_state *state;
  (void)path;
  (void)error;
  state = (projection_state *)user;
  if (!state->capturing) {
    return LONEJSON_STATUS_OK;
  }
  return projection_key_append(state, data, len)
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
                          state->key_len, state->error) != LONEJSON_STATUS_OK) {
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
  if (path != NULL && path->segment_count == 0u) {
    state->root_seen = 1;
    state->root_is_object = 0;
  }
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
  if (path != NULL && path->segment_count == 0u) {
    state->root_seen = 1;
    state->root_is_object = 0;
  }
  projection_num_reset(state);
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
      !projection_num_append(state, data, len)) {
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
  if (path != NULL && path->segment_count == 0u) {
    state->root_seen = 1;
    state->root_is_object = 0;
  }
  projected_path = selected_path(state->projection, path);
  if (projected_path != NULL && !state->capturing &&
      projection_key(state, projected_path) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (state->in_number && lonejson_writer_number_text(
                              &state->writer, state->num_buf, state->num_len,
                              state->error) != LONEJSON_STATUS_OK) {
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
  if (path != NULL && path->segment_count == 0u) {
    state->root_seen = 1;
    state->root_is_object = 0;
  }
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

static void projection_state_cleanup(projection_state *state) {
  if (state == NULL || state->allocator == NULL) {
    return;
  }
  if (state->key_buf != state->inline_key_buf) {
    state->allocator->destroy(state->allocator, state->key_buf);
  }
  if (state->num_buf != state->inline_num_buf) {
    state->allocator->destroy(state->allocator, state->num_buf);
  }
  state->allocator->destroy(state->allocator, state->open_kind);
  state->allocator->destroy(state->allocator, state->open_array_next);
  state->key_buf = NULL;
  state->num_buf = NULL;
  state->open_kind = NULL;
  state->open_array_next = NULL;
}

static lql_status projection_parse_method(
    lql *self, const char *const *fields, size_t field_count,
    lql_projection **out, lql_error *error) {
  lql_allocator *allocator;
  projection_parse_context parse_ctx;
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
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "lql receiver allocator required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  parse_ctx.self = self;
  parse_ctx.allocator = allocator;
  projection =
      (lql_projection *)allocator->calloc(allocator, 1u, sizeof(*projection));
  if (projection == NULL) {
    return LQL_STATUS_NO_MEMORY;
  }
  for (i = 0u; i < field_count; ++i) {
    if (!parse_projection_path(&parse_ctx, fields[i], &path)) {
      self->projection_destroy(self, projection);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "invalid or unsupported projection field path");
      return LQL_STATUS_PARSE_ERROR;
    }
    if (!add_path(&parse_ctx, projection, &path)) {
      projection_path_cleanup(self, &path);
      self->projection_destroy(self, projection);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "conflicting projection field path");
      return LQL_STATUS_PARSE_ERROR;
    }
  }
  if (projection->path_count == 0u) {
    self->projection_destroy(self, projection);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "projection fields required");
    return LQL_STATUS_PARSE_ERROR;
  }
  *out = projection;
  return LQL_STATUS_OK;
}

static void
projection_destroy_method(lql *self, lql_projection *projection) {
  lql_allocator *allocator;
  size_t i;
  if (projection == NULL) {
    return;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    return;
  }
  for (i = 0u; i < projection->path_count; ++i) {
    projection_path_cleanup(self, &projection->paths[i]);
  }
  allocator->destroy(allocator, projection->paths);
  allocator->destroy(allocator, projection);
}

static lql_status lql_project_reader(lql *self,
                                     const lql_projection *projection,
                                     lonejson_reader_fn reader_fn,
                                     void *reader_user, FILE *out,
                                     int *out_found, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  projection_state state;
  lonejson_status st;
  if (projection == NULL || reader_fn == NULL || out == NULL ||
      out_found == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection, reader, out, and out_found are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out_found = 0;
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  memset(&state, 0, sizeof(state));
  state.allocator = lql_allocator_from_receiver(self);
  if (state.allocator == NULL) {
    lonejson_free(runtime);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection receiver allocator required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  state.projection = projection;
  state.error = &lj_error;
  init_projection_visitor(&visitor);
  if (lonejson_writer_init_sink(runtime, &state.writer, file_sink, out,
                                &lj_error) != LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    lonejson_free(runtime);
    projection_state_cleanup(&state);
    return LQL_STATUS_JSON_ERROR;
  }
  st = lonejson_visit_path_value_reader(runtime, reader_fn, reader_user,
                                        &visitor, &state, &lj_error);
  if (st == LONEJSON_STATUS_OK && (!state.root_seen || !state.root_is_object)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "projection source must be a JSON object");
    lonejson_writer_cleanup(&state.writer);
    lonejson_free(runtime);
    projection_state_cleanup(&state);
    return LQL_STATUS_JSON_ERROR;
  }
  if (st == LONEJSON_STATUS_OK && state.object_started) {
    st = close_open_containers(&state, 0u);
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
  projection_state_cleanup(&state);
  if (st != LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  return LQL_STATUS_OK;
}

static lql_status project_file_range_method(
    lql *self, const lql_projection *projection, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, int *out_found, lql_error *error) {
  limited_file_reader reader;
  if (out_found != NULL) {
    *out_found = 0;
  }
  if (file == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection file is required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (!seek_u64(file, offset)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to seek projection source");
    return LQL_STATUS_JSON_ERROR;
  }
  reader.file = file;
  reader.remaining = size;
  return lql_project_reader(self, projection, limited_read, &reader, out,
                            out_found, error);
}

static lql_status project_source_method(
    lql *self, const lql_projection *projection, lql_read_fn read,
    void *read_user, FILE *out, int *out_found, lql_error *error) {
  projection_source_reader reader;
  lql_status st;

  if (out_found != NULL) {
    *out_found = 0;
  }
  if (read == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection source read callback is required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&reader, 0, sizeof(reader));
  reader.read = read;
  reader.user = read_user;
  st = lql_project_reader(self, projection, projection_source_read, &reader,
                          out, out_found, error);
  if (st == LQL_STATUS_JSON_ERROR && reader.error_code != 0) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "projection source read failed");
  }
  return st;
}

static lql_status project_json_method(
    lql *self, const lql_projection *projection, const char *json,
    size_t json_len, FILE *out, int *out_found, lql_error *error) {
  buffer_reader reader;

  if (out_found != NULL) {
    *out_found = 0;
  }
  if (json == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "json is required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  reader.data = (const unsigned char *)json;
  reader.len = json_len;
  reader.offset = 0u;
  return lql_project_reader(self, projection, buffer_read, &reader, out,
                            out_found, error);
}

static lql_status compact_reader(lql *self, lonejson_reader_fn read,
                                 void *read_user, FILE *out,
                                 lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_writer writer;
  lonejson_status st;

  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  if (lonejson_writer_init_sink(runtime, &writer, file_sink, out, &lj_error) !=
      LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    lonejson_free(runtime);
    return LQL_STATUS_JSON_ERROR;
  }
  st = lonejson_writer_json_value_reader(&writer, read, read_user, &lj_error);
  if (st == LONEJSON_STATUS_OK) {
    st = lonejson_writer_finish(&writer, &lj_error);
  }
  lonejson_writer_cleanup(&writer);
  lonejson_free(runtime);
  if (st != LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  return LQL_STATUS_OK;
}

static lql_status
compact_file_range_method(lql *self, FILE *file, lql_uint64 offset,
                            lql_uint64 size, FILE *out, lql_error *error) {
  limited_file_reader reader;

  if (file == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "file and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (!seek_u64(file, offset)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to seek compact source range");
    return LQL_STATUS_JSON_ERROR;
  }
  reader.file = file;
  reader.remaining = size;
  return compact_reader(self, limited_read, &reader, out, error);
}

static lql_status compact_source_method(
    lql *self, lql_read_fn read, void *read_user, FILE *out, lql_error *error) {
  projection_source_reader reader;
  lql_status st;

  if (read == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "read and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&reader, 0, sizeof(reader));
  reader.read = read;
  reader.user = read_user;
  st = compact_reader(self, projection_source_read, &reader, out, error);
  if (st == LQL_STATUS_JSON_ERROR && reader.error_code != 0) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, "compact source read failed");
  }
  return st;
}

static lql_status compact_json_method(lql *self,
                                                     const char *json,
                                                     size_t json_len, FILE *out,
                                                     lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_writer writer;
  lonejson_status st;

  if (json == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "json and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  if (lonejson_writer_init_sink(runtime, &writer, file_sink, out, &lj_error) !=
      LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    lonejson_free(runtime);
    return LQL_STATUS_JSON_ERROR;
  }
  st = lonejson_writer_json_value_buffer(&writer, json, json_len, &lj_error);
  if (st == LONEJSON_STATUS_OK) {
    st = lonejson_writer_finish(&writer, &lj_error);
  }
  lonejson_writer_cleanup(&writer);
  lonejson_free(runtime);
  if (st != LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL void lql_project_methods_install(lql *ctx) {
  ctx->projection_parse = projection_parse_method;
  ctx->projection_destroy = projection_destroy_method;
  ctx->project_file_range = project_file_range_method;
  ctx->project_source = project_source_method;
  ctx->project_json = project_json_method;
  ctx->compact_file_range = compact_file_range_method;
  ctx->compact_source = compact_source_method;
  ctx->compact_json = compact_json_method;
}
