#include "lql_internal.h"

#include <string.h>

static int projection_is_space(unsigned char ch) {
  return ch == (unsigned char)' ' || ch == (unsigned char)'\t' ||
         ch == (unsigned char)'\r' || ch == (unsigned char)'\n';
}

static int projection_valid_pointer(const char *path) {
  if (path == NULL || path[0] != '/' || path[1] == '\0') {
    return 0;
  }
  return 1;
}

static void projection_path_cleanup(lql_allocator *allocator,
                                    lql_projection_path *path) {
  size_t i;
  if (allocator == NULL || path == NULL) {
    return;
  }
  for (i = 0u; i < path->segment_count; ++i) {
    allocator->destroy(allocator, path->segments[i]);
  }
  allocator->destroy(allocator, path->segments);
  path->segments = NULL;
  path->segment_count = 0u;
}

static lql_status projection_decode_path(lql_allocator *allocator,
                                         const char *path,
                                         lql_projection_path *out,
                                         lql_error *error) {
  const char *part;
  const char *end;
  const char *slash;
  char **segments;
  size_t count;
  size_t capacity;
  size_t raw_len;
  size_t decoded_len;
  size_t i;
  char *decoded;

  if (allocator == NULL || path == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection path decoder arguments are invalid");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  part = path + 1;
  end = path + strlen(path);
  segments = NULL;
  count = 0u;
  capacity = 0u;
  for (;;) {
    slash = part;
    while (slash < end && *slash != '/') {
      ++slash;
    }
    raw_len = (size_t)(slash - part);
    decoded = (char *)allocator->alloc(allocator, raw_len + 1u);
    if (decoded == NULL) {
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      goto fail;
    }
    decoded_len = 0u;
    for (i = 0u; i < raw_len; ++i) {
      if (part[i] == '~' && i + 1u < raw_len &&
          (part[i + 1u] == '0' || part[i + 1u] == '1')) {
        decoded[decoded_len++] = part[i + 1u] == '0' ? '~' : '/';
        ++i;
      } else {
        decoded[decoded_len++] = part[i];
      }
    }
    decoded[decoded_len] = '\0';
    if (count == capacity) {
      size_t next_capacity;
      char **next;
      next_capacity = capacity == 0u ? 4u : capacity * 2u;
      next = (char **)allocator->realloc(allocator, segments,
                                         next_capacity * sizeof(*segments));
      if (next == NULL) {
        allocator->destroy(allocator, decoded);
        lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
        goto fail;
      }
      segments = next;
      capacity = next_capacity;
    }
    segments[count++] = decoded;
    if (slash == end) {
      break;
    }
    part = slash + 1;
  }
  out->segments = segments;
  out->segment_count = count;
  return LQL_STATUS_OK;

fail:
  for (i = 0u; i < count; ++i) {
    allocator->destroy(allocator, segments[i]);
  }
  allocator->destroy(allocator, segments);
  return error == NULL ? LQL_STATUS_PARSE_ERROR : error->code;
}

static int projection_leading_index(const lql_projection_path *path) {
  const char *p;
  if (path == NULL || path->segment_count == 0u || path->segments[0] == NULL ||
      path->segments[0][0] == '\0') {
    return 0;
  }
  p = path->segments[0];
  while (*p != '\0') {
    if (*p < '0' || *p > '9') {
      return 0;
    }
    ++p;
  }
  return 1;
}

static int projection_conflicts(const char *left, const char *right) {
  size_t left_len;
  size_t right_len;
  if (strcmp(left, right) == 0) {
    return 0;
  }
  left_len = strlen(left);
  right_len = strlen(right);
  return (left_len < right_len && right[left_len] == '/' &&
          memcmp(left, right, left_len) == 0) ||
         (right_len < left_len && left[right_len] == '/' &&
          memcmp(left, right, right_len) == 0);
}

LQL_INTERNAL_SYMBOL lql_status lql_projection_parse_internal(
    lql *self, const char *const *paths, size_t path_count,
    lql_projection **out, lql_error *error) {
  lql_allocator *allocator;
  lql_projection *projection;
  size_t i;
  size_t j;
  size_t begin;
  size_t end;
  size_t len;
  char *copy;

  if (out == NULL || (paths == NULL && path_count != 0u)) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection paths and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = NULL;
  if (path_count == 0u) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "projection paths required");
    return LQL_STATUS_PARSE_ERROR;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection receiver required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  projection =
      (lql_projection *)allocator->calloc(allocator, 1u, sizeof(*projection));
  if (projection == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  projection->paths = (char **)allocator->calloc(allocator, path_count,
                                                 sizeof(*projection->paths));
  projection->compiled_paths = (lql_projection_path *)allocator->calloc(
      allocator, path_count, sizeof(*projection->compiled_paths));
  if (projection->paths == NULL || projection->compiled_paths == NULL) {
    allocator->destroy(allocator, projection->compiled_paths);
    allocator->destroy(allocator, projection->paths);
    allocator->destroy(allocator, projection);
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  for (i = 0u; i < path_count; ++i) {
    if (paths[i] == NULL) {
      continue;
    }
    begin = 0u;
    end = strlen(paths[i]);
    while (begin < end && projection_is_space((unsigned char)paths[i][begin])) {
      ++begin;
    }
    while (end > begin &&
           projection_is_space((unsigned char)paths[i][end - 1u])) {
      --end;
    }
    if (begin == end) {
      continue;
    }
    len = end - begin;
    copy = (char *)allocator->alloc(allocator, len + 1u);
    if (copy == NULL) {
      lql_projection_destroy_internal(self, projection);
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      return LQL_STATUS_NO_MEMORY;
    }
    memcpy(copy, paths[i] + begin, len);
    copy[len] = '\0';
    if (!projection_valid_pointer(copy)) {
      allocator->destroy(allocator, copy);
      lql_projection_destroy_internal(self, projection);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "projection path is invalid");
      return LQL_STATUS_PARSE_ERROR;
    }
    for (j = 0u; j < projection->path_count; ++j) {
      if (strcmp(copy, projection->paths[j]) == 0) {
        allocator->destroy(allocator, copy);
        copy = NULL;
        break;
      }
      if (projection_conflicts(copy, projection->paths[j])) {
        allocator->destroy(allocator, copy);
        lql_projection_destroy_internal(self, projection);
        lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                      "projection paths have a parent-child conflict");
        return LQL_STATUS_PARSE_ERROR;
      }
    }
    if (copy != NULL) {
      lql_status status;
      status = projection_decode_path(
          allocator, copy, &projection->compiled_paths[projection->path_count],
          error);
      if (status != LQL_STATUS_OK ||
          projection_leading_index(
              &projection->compiled_paths[projection->path_count])) {
        projection_path_cleanup(
            allocator, &projection->compiled_paths[projection->path_count]);
        allocator->destroy(allocator, copy);
        lql_projection_destroy_internal(self, projection);
        if (status == LQL_STATUS_OK) {
          lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                        "projection path is invalid");
          return LQL_STATUS_PARSE_ERROR;
        }
        return status;
      }
      projection->paths[projection->path_count] = copy;
      ++projection->path_count;
    }
  }
  if (projection->path_count == 0u) {
    lql_projection_destroy_internal(self, projection);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "projection paths required");
    return LQL_STATUS_PARSE_ERROR;
  }
  *out = projection;
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL void
lql_projection_destroy_internal(lql *self, lql_projection *projection) {
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
    allocator->destroy(allocator, projection->paths[i]);
    projection_path_cleanup(allocator, &projection->compiled_paths[i]);
  }
  allocator->destroy(allocator, projection->paths);
  allocator->destroy(allocator, projection->compiled_paths);
  allocator->destroy(allocator, projection);
}

typedef struct projection_capture {
  lql *receiver;
  lonejson *runtime;
  const lql_projection_path *path;
  lonejson_spooled *spool;
  lonejson_writer writer;
  char *key;
  size_t key_len;
  size_t key_capacity;
  char *scalar;
  size_t scalar_len;
  size_t scalar_capacity;
  int scalar_kind;
  int root_object;
  int active;
  int writer_ready;
  int key_active;
  int found;
} projection_capture;

#define PROJECTION_SCALAR_NONE 0
#define PROJECTION_SCALAR_STRING 1
#define PROJECTION_SCALAR_NUMBER 2
#define PROJECTION_SCALAR_BOOL 3
#define PROJECTION_SCALAR_NULL 4

typedef struct projection_render_state {
  const lql_projection *projection;
  lonejson_spooled *values;
  projection_capture *captures;
  lonejson_writer *writer;
  lonejson_error *error;
} projection_render_state;

typedef struct projection_capture_set {
  projection_capture *captures;
  size_t count;
} projection_capture_set;

struct lql_projection_capture {
  lql *receiver;
  const lql_projection *projection;
  lonejson *runtime;
  lql_allocator *allocator;
  lonejson_spooled *values;
  projection_capture *captures;
  unsigned char *found;
  size_t *members;
  projection_capture_set set;
};

#define LQL_PROJECTION_MAX_INDEX 1048576u

static lonejson_read_result
projection_spool_read(void *user, unsigned char *buffer, size_t capacity) {
  return lonejson_spooled_read((lonejson_spooled *)user, buffer, capacity);
}

static lonejson_status projection_spool_sink(void *user, const void *data,
                                             size_t len,
                                             lonejson_error *error) {
  return lonejson_spooled_append((lonejson_spooled *)user, data, len, error);
}

static lql_status projection_lonejson_status(lonejson_status status,
                                             const lonejson_error *source,
                                             lql_error *error) {
  if (status == LONEJSON_STATUS_OK) {
    return LQL_STATUS_OK;
  }
  if (status == LONEJSON_STATUS_ALLOCATION_FAILED) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "projection allocation failed");
    return LQL_STATUS_NO_MEMORY;
  }
  lql_set_error(error, LQL_STATUS_JSON_ERROR,
                source != NULL && source->message[0] != '\0'
                    ? source->message
                    : "projection JSON processing failed");
  return LQL_STATUS_JSON_ERROR;
}

static int projection_path_exact(const lql_projection_path *target,
                                 const lonejson_value_path *path) {
  size_t i;
  if (target == NULL || path == NULL ||
      target->segment_count != path->segment_count) {
    return 0;
  }
  for (i = 0u; i < target->segment_count; ++i) {
    if (strlen(target->segments[i]) != path->segments[i].len ||
        memcmp(target->segments[i], path->segments[i].data,
               path->segments[i].len) != 0) {
      return 0;
    }
  }
  return 1;
}

static int projection_path_prefix(const lql_projection_path *target,
                                  const lonejson_value_path *path) {
  size_t i;
  if (target == NULL || path == NULL ||
      path->segment_count < target->segment_count) {
    return 0;
  }
  for (i = 0u; i < target->segment_count; ++i) {
    if (strlen(target->segments[i]) != path->segments[i].len ||
        memcmp(target->segments[i], path->segments[i].data,
               path->segments[i].len) != 0) {
      return 0;
    }
  }
  return 1;
}

static lonejson_status projection_capture_grow_key(projection_capture *capture,
                                                   size_t additional,
                                                   lonejson_error *error) {
  lql_allocator *allocator;
  size_t required;
  size_t capacity;
  char *next;
  if (capture == NULL || additional > (size_t)-1 - capture->key_len) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  required = capture->key_len + additional;
  if (required <= capture->key_capacity) {
    return LONEJSON_STATUS_OK;
  }
  allocator = lql_allocator_from_receiver(capture->receiver);
  if (allocator == NULL) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  capacity = capture->key_capacity == 0u ? 64u : capture->key_capacity;
  while (capacity < required) {
    if (capacity > (size_t)-1 / 2u) {
      capacity = required;
      break;
    }
    capacity *= 2u;
  }
  next = (char *)allocator->realloc(allocator, capture->key, capacity);
  if (next == NULL) {
    if (error != NULL) {
      error->code = LONEJSON_STATUS_ALLOCATION_FAILED;
      strcpy(error->message, "projection key allocation failed");
    }
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  capture->key = next;
  capture->key_capacity = capacity;
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_capture_grow_scalar(
    projection_capture *capture, size_t additional, lonejson_error *error) {
  lql_allocator *allocator;
  size_t required;
  size_t capacity;
  char *next;
  if (capture == NULL || additional > (size_t)-1 - capture->scalar_len) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  required = capture->scalar_len + additional;
  if (required <= capture->scalar_capacity) {
    return LONEJSON_STATUS_OK;
  }
  allocator = lql_allocator_from_receiver(capture->receiver);
  if (allocator == NULL) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  capacity = capture->scalar_capacity == 0u ? 64u : capture->scalar_capacity;
  while (capacity < required) {
    if (capacity > (size_t)-1 / 2u) {
      capacity = required;
      break;
    }
    capacity *= 2u;
  }
  next = (char *)allocator->realloc(allocator, capture->scalar, capacity);
  if (next == NULL) {
    if (error != NULL) {
      error->code = LONEJSON_STATUS_ALLOCATION_FAILED;
      strcpy(error->message, "projection scalar allocation failed");
    }
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  capture->scalar = next;
  capture->scalar_capacity = capacity;
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_capture_scalar_start(
    projection_capture *capture, int kind, lonejson_error *error) {
  if (capture == NULL) {
    return LONEJSON_STATUS_INVALID_ARGUMENT;
  }
  lonejson_spooled_reset(capture->spool);
  capture->scalar_len = 0u;
  capture->scalar_kind = kind;
  capture->active = 1;
  capture->writer_ready = 0;
  capture->key_active = 0;
  capture->found = 0;
  (void)error;
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_capture_scalar_finish(
    projection_capture *capture) {
  if (capture == NULL || !capture->active) {
    return LONEJSON_STATUS_INVALID_ARGUMENT;
  }
  capture->active = 0;
  capture->found = 1;
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_capture_finish(projection_capture *capture,
                                                 lonejson_error *error) {
  lonejson_status status;
  if (capture == NULL || !capture->writer_ready) {
    return LONEJSON_STATUS_INVALID_ARGUMENT;
  }
  status = lonejson_writer_finish(&capture->writer, error);
  lonejson_writer_cleanup(&capture->writer);
  capture->writer_ready = 0;
  capture->active = 0;
  if (status == LONEJSON_STATUS_OK) {
    capture->found = 1;
  }
  return status;
}

static lonejson_status projection_capture_start(projection_capture *capture,
                                                lonejson_error *error) {
  lonejson_status status;
  if (capture == NULL || capture->runtime == NULL || capture->spool == NULL) {
    return LONEJSON_STATUS_INVALID_ARGUMENT;
  }
  lonejson_spooled_reset(capture->spool);
  status =
      lonejson_writer_init_sink(capture->runtime, &capture->writer,
                                projection_spool_sink, capture->spool, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  capture->writer_ready = 1;
  capture->active = 1;
  capture->key_active = 0;
  capture->scalar_len = 0u;
  capture->scalar_kind = PROJECTION_SCALAR_NONE;
  capture->found = 0;
  return LONEJSON_STATUS_OK;
}

static lonejson_status
projection_capture_object_begin(void *user, const lonejson_value_path *path,
                                lonejson_error *error) {
  projection_capture *capture;
  lonejson_status status;
  capture = (projection_capture *)user;
  if (path->segment_count == 0u) {
    capture->root_object = 1;
  }
  if (!capture->active && projection_path_exact(capture->path, path)) {
    status = projection_capture_start(capture, error);
    if (status != LONEJSON_STATUS_OK) {
      return status;
    }
    return lonejson_writer_begin_object(&capture->writer, error);
  }
  if (capture->active && projection_path_prefix(capture->path, path)) {
    return lonejson_writer_begin_object(&capture->writer, error);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
projection_capture_object_end(void *user, const lonejson_value_path *path,
                              lonejson_error *error) {
  projection_capture *capture;
  lonejson_status status;
  capture = (projection_capture *)user;
  if (!capture->active || !projection_path_prefix(capture->path, path)) {
    return LONEJSON_STATUS_OK;
  }
  status = lonejson_writer_end_object(&capture->writer, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  if (projection_path_exact(capture->path, path)) {
    return projection_capture_finish(capture, error);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
projection_capture_array_begin(void *user, const lonejson_value_path *path,
                               lonejson_error *error) {
  projection_capture *capture;
  lonejson_status status;
  capture = (projection_capture *)user;
  if (!capture->active && projection_path_exact(capture->path, path)) {
    status = projection_capture_start(capture, error);
    if (status != LONEJSON_STATUS_OK) {
      return status;
    }
    return lonejson_writer_begin_array(&capture->writer, error);
  }
  if (capture->active && projection_path_prefix(capture->path, path)) {
    return lonejson_writer_begin_array(&capture->writer, error);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
projection_capture_array_end(void *user, const lonejson_value_path *path,
                             lonejson_error *error) {
  projection_capture *capture;
  lonejson_status status;
  capture = (projection_capture *)user;
  if (!capture->active || !projection_path_prefix(capture->path, path)) {
    return LONEJSON_STATUS_OK;
  }
  status = lonejson_writer_end_array(&capture->writer, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  if (projection_path_exact(capture->path, path)) {
    return projection_capture_finish(capture, error);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
projection_capture_key_begin(void *user, const lonejson_value_path *path,
                             lonejson_error *error) {
  projection_capture *capture;
  (void)error;
  capture = (projection_capture *)user;
  if (capture->active && projection_path_prefix(capture->path, path)) {
    capture->key_len = 0u;
    capture->key_active = 1;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
projection_capture_key_chunk(void *user, const lonejson_value_path *path,
                             const char *data, size_t len,
                             lonejson_error *error) {
  projection_capture *capture;
  lonejson_status status;
  capture = (projection_capture *)user;
  if (!capture->key_active || !projection_path_prefix(capture->path, path)) {
    return LONEJSON_STATUS_OK;
  }
  status = projection_capture_grow_key(capture, len, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  if (len != 0u) {
    memcpy(capture->key + capture->key_len, data, len);
    capture->key_len += len;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
projection_capture_key_end(void *user, const lonejson_value_path *path,
                           lonejson_error *error) {
  projection_capture *capture;
  capture = (projection_capture *)user;
  if (!capture->key_active || !projection_path_prefix(capture->path, path)) {
    return LONEJSON_STATUS_OK;
  }
  capture->key_active = 0;
  return lonejson_writer_key(&capture->writer, capture->key, capture->key_len,
                             error);
}

static lonejson_status
projection_capture_string_begin(void *user, const lonejson_value_path *path,
                                lonejson_error *error) {
  projection_capture *capture;
  capture = (projection_capture *)user;
  if (!capture->active && projection_path_exact(capture->path, path)) {
    return projection_capture_scalar_start(capture, PROJECTION_SCALAR_STRING,
                                           error);
  }
  if (capture->active && projection_path_prefix(capture->path, path)) {
    return lonejson_writer_string_begin(&capture->writer, error);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
projection_capture_string_chunk(void *user, const lonejson_value_path *path,
                                const char *data, size_t len,
                                lonejson_error *error) {
  projection_capture *capture;
  capture = (projection_capture *)user;
  if (capture->active && capture->scalar_kind == PROJECTION_SCALAR_STRING) {
    lonejson_status status;
    status = projection_capture_grow_scalar(capture, len, error);
    if (status != LONEJSON_STATUS_OK) {
      return status;
    }
    memcpy(capture->scalar + capture->scalar_len, data, len);
    capture->scalar_len += len;
    return LONEJSON_STATUS_OK;
  }
  if (capture->active && projection_path_prefix(capture->path, path)) {
    return lonejson_writer_string_chunk(&capture->writer, data, len, error);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
projection_capture_string_end(void *user, const lonejson_value_path *path,
                              lonejson_error *error) {
  projection_capture *capture;
  lonejson_status status;
  capture = (projection_capture *)user;
  if (capture->active && capture->scalar_kind == PROJECTION_SCALAR_STRING) {
    return projection_capture_scalar_finish(capture);
  }
  if (!capture->active || !projection_path_prefix(capture->path, path)) {
    return LONEJSON_STATUS_OK;
  }
  status = lonejson_writer_string_end(&capture->writer, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  if (projection_path_exact(capture->path, path)) {
    return projection_capture_finish(capture, error);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
projection_capture_number_begin(void *user, const lonejson_value_path *path,
                                lonejson_error *error) {
  projection_capture *capture;
  capture = (projection_capture *)user;
  if (!capture->active && projection_path_exact(capture->path, path)) {
    return projection_capture_scalar_start(capture, PROJECTION_SCALAR_NUMBER,
                                           error);
  }
  if (capture->active && projection_path_prefix(capture->path, path)) {
    return lonejson_writer_number_begin(&capture->writer, error);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
projection_capture_number_chunk(void *user, const lonejson_value_path *path,
                                const char *data, size_t len,
                                lonejson_error *error) {
  projection_capture *capture;
  capture = (projection_capture *)user;
  if (capture->active && capture->scalar_kind == PROJECTION_SCALAR_NUMBER) {
    lonejson_status status;
    status = projection_capture_grow_scalar(capture, len, error);
    if (status != LONEJSON_STATUS_OK) {
      return status;
    }
    memcpy(capture->scalar + capture->scalar_len, data, len);
    capture->scalar_len += len;
    return LONEJSON_STATUS_OK;
  }
  if (capture->active && projection_path_prefix(capture->path, path)) {
    return lonejson_writer_number_chunk(&capture->writer, data, len, error);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
projection_capture_number_end(void *user, const lonejson_value_path *path,
                              lonejson_error *error) {
  projection_capture *capture;
  lonejson_status status;
  capture = (projection_capture *)user;
  if (capture->active && capture->scalar_kind == PROJECTION_SCALAR_NUMBER) {
    return projection_capture_scalar_finish(capture);
  }
  if (!capture->active || !projection_path_prefix(capture->path, path)) {
    return LONEJSON_STATUS_OK;
  }
  status = lonejson_writer_number_end(&capture->writer, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  if (projection_path_exact(capture->path, path)) {
    return projection_capture_finish(capture, error);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
projection_capture_boolean(void *user, const lonejson_value_path *path,
                           int value, lonejson_error *error) {
  projection_capture *capture;
  lonejson_status status;
  capture = (projection_capture *)user;
  if (!projection_path_exact(capture->path, path)) {
    return LONEJSON_STATUS_OK;
  }
  status = projection_capture_scalar_start(capture, PROJECTION_SCALAR_BOOL,
                                           error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  capture->scalar_len = value ? 4u : 5u;
  status = projection_capture_grow_scalar(capture, capture->scalar_len, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  memcpy(capture->scalar, value ? "true" : "false", capture->scalar_len);
  return projection_capture_scalar_finish(capture);
}

static lonejson_status projection_capture_null(void *user,
                                               const lonejson_value_path *path,
                                               lonejson_error *error) {
  projection_capture *capture;
  lonejson_status status;
  capture = (projection_capture *)user;
  if (!projection_path_exact(capture->path, path)) {
    return LONEJSON_STATUS_OK;
  }
  status = projection_capture_scalar_start(capture, PROJECTION_SCALAR_NULL,
                                           error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  status = projection_capture_grow_scalar(capture, 4u, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  memcpy(capture->scalar, "null", 4u);
  capture->scalar_len = 4u;
  return projection_capture_scalar_finish(capture);
}

#define PROJECTION_CAPTURE_SET_EVENT(name)                                     \
  static lonejson_status projection_capture_set_##name(                        \
      void *user, const lonejson_value_path *path, lonejson_error *error) {    \
    projection_capture_set *set;                                               \
    lonejson_status status;                                                    \
    size_t i;                                                                  \
    set = (projection_capture_set *)user;                                      \
    for (i = 0u; i < set->count; ++i) {                                        \
      status = projection_capture_##name(&set->captures[i], path, error);      \
      if (status != LONEJSON_STATUS_OK) {                                      \
        return status;                                                         \
      }                                                                        \
    }                                                                          \
    return LONEJSON_STATUS_OK;                                                 \
  }

#define PROJECTION_CAPTURE_SET_CHUNK(name)                                     \
  static lonejson_status projection_capture_set_##name(                        \
      void *user, const lonejson_value_path *path, const char *data,           \
      size_t len, lonejson_error *error) {                                     \
    projection_capture_set *set;                                               \
    lonejson_status status;                                                    \
    size_t i;                                                                  \
    set = (projection_capture_set *)user;                                      \
    for (i = 0u; i < set->count; ++i) {                                        \
      status = projection_capture_##name(&set->captures[i], path, data, len,   \
                                         error);                               \
      if (status != LONEJSON_STATUS_OK) {                                      \
        return status;                                                         \
      }                                                                        \
    }                                                                          \
    return LONEJSON_STATUS_OK;                                                 \
  }

PROJECTION_CAPTURE_SET_EVENT(object_begin)
PROJECTION_CAPTURE_SET_EVENT(object_end)
PROJECTION_CAPTURE_SET_EVENT(array_begin)
PROJECTION_CAPTURE_SET_EVENT(array_end)
PROJECTION_CAPTURE_SET_EVENT(key_begin)
PROJECTION_CAPTURE_SET_EVENT(key_end)
PROJECTION_CAPTURE_SET_EVENT(string_begin)
PROJECTION_CAPTURE_SET_EVENT(string_end)
PROJECTION_CAPTURE_SET_EVENT(number_begin)
PROJECTION_CAPTURE_SET_EVENT(number_end)
PROJECTION_CAPTURE_SET_CHUNK(key_chunk)
PROJECTION_CAPTURE_SET_CHUNK(string_chunk)
PROJECTION_CAPTURE_SET_CHUNK(number_chunk)

static lonejson_status
projection_capture_set_boolean(void *user, const lonejson_value_path *path,
                               int value, lonejson_error *error) {
  projection_capture_set *set;
  lonejson_status status;
  size_t i;
  set = (projection_capture_set *)user;
  for (i = 0u; i < set->count; ++i) {
    status = projection_capture_boolean(&set->captures[i], path, value, error);
    if (status != LONEJSON_STATUS_OK) {
      return status;
    }
  }
  return LONEJSON_STATUS_OK;
}

PROJECTION_CAPTURE_SET_EVENT(null)

#undef PROJECTION_CAPTURE_SET_CHUNK
#undef PROJECTION_CAPTURE_SET_EVENT

static int projection_parse_index(const char *text, size_t *out) {
  size_t value;
  const char *p;
  if (text == NULL || *text == '\0') {
    return 0;
  }
  value = 0u;
  for (p = text; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9' || value > LQL_PROJECTION_MAX_INDEX / 10u) {
      return 0;
    }
    value = value * 10u + (size_t)(*p - '0');
    if (value > LQL_PROJECTION_MAX_INDEX) {
      return 0;
    }
  }
  if (out != NULL) {
    *out = value;
  }
  return 1;
}

static int projection_member_compare(const projection_render_state *state,
                                     size_t left, size_t right, size_t depth,
                                     int numeric) {
  const char *left_segment;
  const char *right_segment;
  if (numeric) {
    size_t left_index = 0u;
    size_t right_index = 0u;
    (void)projection_parse_index(
        state->projection->compiled_paths[left].segments[depth], &left_index);
    (void)projection_parse_index(
        state->projection->compiled_paths[right].segments[depth], &right_index);
    return left_index < right_index ? -1 : (left_index > right_index ? 1 : 0);
  }
  left_segment = state->projection->compiled_paths[left].segments[depth];
  right_segment = state->projection->compiled_paths[right].segments[depth];
  return strcmp(left_segment, right_segment);
}

static lonejson_status projection_render_value(projection_render_state *state,
                                               size_t *members, size_t count,
                                               size_t depth) {
  size_t i;
  size_t j;
  size_t group_begin;
  size_t index;
  size_t expected;
  size_t current;
  int numeric;
  lonejson_status status;
  const lql_projection_path *path;

  if (count == 0u) {
    return LONEJSON_STATUS_INVALID_ARGUMENT;
  }
  path = &state->projection->compiled_paths[members[0]];
  if (path->segment_count == depth) {
    projection_capture *capture;
    if (count != 1u) {
      return LONEJSON_STATUS_INVALID_ARGUMENT;
    }
    capture = &state->captures[members[0]];
    if (capture->scalar_kind == PROJECTION_SCALAR_STRING) {
      return lonejson_writer_string(state->writer, capture->scalar,
                                    capture->scalar_len, state->error);
    }
    if (capture->scalar_kind == PROJECTION_SCALAR_NUMBER) {
      return lonejson_writer_number_text(state->writer, capture->scalar,
                                         capture->scalar_len, state->error);
    }
    if (capture->scalar_kind == PROJECTION_SCALAR_BOOL) {
      return lonejson_writer_bool(state->writer, capture->scalar_len == 4u,
                                  state->error);
    }
    if (capture->scalar_kind == PROJECTION_SCALAR_NULL) {
      return lonejson_writer_null(state->writer, state->error);
    }
    return lonejson_writer_json_value_spooled(
        state->writer, &state->values[members[0]], state->error);
  }
  numeric = projection_parse_index(path->segments[depth], NULL);
  for (i = 1u; i < count; ++i) {
    const lql_projection_path *candidate;
    candidate = &state->projection->compiled_paths[members[i]];
    if (candidate->segment_count <= depth ||
        projection_parse_index(candidate->segments[depth], NULL) != numeric) {
      return LONEJSON_STATUS_TYPE_MISMATCH;
    }
  }
  for (i = 0u; i < count; ++i) {
    size_t selected;
    selected = i;
    for (j = i + 1u; j < count; ++j) {
      if (projection_member_compare(state, members[j], members[selected], depth,
                                    numeric) < 0) {
        selected = j;
      }
    }
    if (selected != i) {
      size_t swap;
      swap = members[i];
      members[i] = members[selected];
      members[selected] = swap;
    }
  }
  status = numeric ? lonejson_writer_begin_array(state->writer, state->error)
                   : lonejson_writer_begin_object(state->writer, state->error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  expected = 0u;
  group_begin = 0u;
  while (group_begin < count) {
    const char *segment;
    size_t group_end;
    segment =
        state->projection->compiled_paths[members[group_begin]].segments[depth];
    group_end = group_begin + 1u;
    while (group_end < count &&
           strcmp(segment, state->projection->compiled_paths[members[group_end]]
                               .segments[depth]) == 0) {
      ++group_end;
    }
    if (numeric) {
      if (!projection_parse_index(segment, &index)) {
        return LONEJSON_STATUS_TYPE_MISMATCH;
      }
      if (index < expected) {
        return LONEJSON_STATUS_TYPE_MISMATCH;
      }
      while (expected < index) {
        status = lonejson_writer_null(state->writer, state->error);
        if (status != LONEJSON_STATUS_OK) {
          return status;
        }
        ++expected;
      }
      ++expected;
    } else {
      status = lonejson_writer_key(state->writer, segment, strlen(segment),
                                   state->error);
      if (status != LONEJSON_STATUS_OK) {
        return status;
      }
    }
    status = projection_render_value(state, members + group_begin,
                                     group_end - group_begin, depth + 1u);
    if (status != LONEJSON_STATUS_OK) {
      return status;
    }
    group_begin = group_end;
  }
  current = numeric ? lonejson_writer_end_array(state->writer, state->error)
                    : lonejson_writer_end_object(state->writer, state->error);
  return (lonejson_status)current;
}

LQL_INTERNAL_SYMBOL lql_status lql_projection_capture_create(
    lql *self, const lql_projection *projection, lonejson *runtime,
    lql_projection_capture **out, lql_error *error) {
  lql_projection_capture *capture;
  lql_allocator *allocator;
  size_t i;

  if (out == NULL || projection == NULL || runtime == NULL ||
      projection->path_count == 0u) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection capture arguments are invalid");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = NULL;
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection capture receiver is required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  capture = (lql_projection_capture *)allocator->calloc(allocator, 1u,
                                                        sizeof(*capture));
  if (capture == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "projection allocation failed");
    return LQL_STATUS_NO_MEMORY;
  }
  capture->receiver = self;
  capture->projection = projection;
  capture->runtime = runtime;
  capture->allocator = allocator;
  capture->values = (lonejson_spooled *)allocator->calloc(
      allocator, projection->path_count, sizeof(*capture->values));
  capture->captures = (projection_capture *)allocator->calloc(
      allocator, projection->path_count, sizeof(*capture->captures));
  capture->found = (unsigned char *)allocator->calloc(
      allocator, projection->path_count, sizeof(*capture->found));
  capture->members = (size_t *)allocator->alloc(
      allocator, projection->path_count * sizeof(*capture->members));
  if (capture->values == NULL || capture->captures == NULL ||
      capture->found == NULL || capture->members == NULL) {
    lql_projection_capture_destroy(capture);
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "projection allocation failed");
    return LQL_STATUS_NO_MEMORY;
  }
  for (i = 0u; i < projection->path_count; ++i) {
    lonejson_spooled_init_class(runtime, &capture->values[i],
                                LONEJSON_SPOOL_CLASS_LARGE_TEXT);
    capture->captures[i].receiver = self;
    capture->captures[i].runtime = runtime;
    capture->captures[i].path = &projection->compiled_paths[i];
    capture->captures[i].spool = &capture->values[i];
  }
  capture->set.captures = capture->captures;
  capture->set.count = projection->path_count;
  *out = capture;
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL void
lql_projection_capture_destroy(lql_projection_capture *capture) {
  size_t i;
  if (capture == NULL || capture->allocator == NULL) {
    return;
  }
  for (i = 0u; i < capture->set.count; ++i) {
    if (capture->captures[i].writer_ready) {
      lonejson_writer_cleanup(&capture->captures[i].writer);
    }
    capture->allocator->destroy(capture->allocator, capture->captures[i].key);
    lonejson_spooled_cleanup(&capture->values[i]);
  }
  capture->allocator->destroy(capture->allocator, capture->members);
  capture->allocator->destroy(capture->allocator, capture->found);
  capture->allocator->destroy(capture->allocator, capture->captures);
  capture->allocator->destroy(capture->allocator, capture->values);
  capture->allocator->destroy(capture->allocator, capture);
}

LQL_INTERNAL_SYMBOL void
lql_projection_capture_reset(lql_projection_capture *capture) {
  size_t i;
  if (capture == NULL) {
    return;
  }
  for (i = 0u; i < capture->set.count; ++i) {
    projection_capture *field;
    field = &capture->captures[i];
    if (field->writer_ready) {
      lonejson_writer_cleanup(&field->writer);
    }
    lonejson_spooled_reset(&capture->values[i]);
    field->key_len = 0u;
    field->root_object = 0;
    field->active = 0;
    field->writer_ready = 0;
    field->key_active = 0;
    field->found = 0;
    capture->found[i] = 0u;
  }
}

LQL_INTERNAL_SYMBOL void
lql_projection_capture_set_root_object(lql_projection_capture *capture) {
  size_t i;
  if (capture == NULL) {
    return;
  }
  for (i = 0u; i < capture->set.count; ++i) {
    capture->captures[i].root_object = 1;
  }
}

LQL_INTERNAL_SYMBOL void
lql_projection_capture_visitor(lonejson_path_value_visitor *out) {
  if (out == NULL) {
    return;
  }
  *out = lonejson_default_path_value_visitor();
  out->object_begin = projection_capture_set_object_begin;
  out->object_end = projection_capture_set_object_end;
  out->object_key_begin = projection_capture_set_key_begin;
  out->object_key_chunk = projection_capture_set_key_chunk;
  out->object_key_end = projection_capture_set_key_end;
  out->array_begin = projection_capture_set_array_begin;
  out->array_end = projection_capture_set_array_end;
  out->string_begin = projection_capture_set_string_begin;
  out->string_chunk = projection_capture_set_string_chunk;
  out->string_end = projection_capture_set_string_end;
  out->number_begin = projection_capture_set_number_begin;
  out->number_chunk = projection_capture_set_number_chunk;
  out->number_end = projection_capture_set_number_end;
  out->boolean_value = projection_capture_set_boolean;
  out->null_value = projection_capture_set_null;
}

LQL_INTERNAL_SYMBOL void *
lql_projection_capture_visitor_user(lql_projection_capture *capture) {
  return capture == NULL ? NULL : &capture->set;
}

LQL_INTERNAL_SYMBOL lql_status lql_projection_capture_render(
    lql_projection_capture *capture, lonejson_sink_fn sink, void *sink_user,
    int *out_emitted, lql_error *error) {
  projection_render_state render;
  lonejson_writer writer;
  lonejson_error lonejson_error;
  lonejson_status status;
  lql_status out;
  size_t count;
  size_t i;

  if (out_emitted != NULL) {
    *out_emitted = 0;
  }
  if (capture == NULL || sink == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection capture render arguments are invalid");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (!capture->captures[0].root_object) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "projection requires an object record");
    return LQL_STATUS_JSON_ERROR;
  }
  count = 0u;
  for (i = 0u; i < capture->set.count; ++i) {
    if (capture->captures[i].found) {
      capture->found[i] = 1u;
      capture->members[count++] = i;
    }
  }
  if (count == 0u) {
    return LQL_STATUS_OK;
  }
  lonejson_error_init(&lonejson_error);
  status = lonejson_writer_init_sink(capture->runtime, &writer, sink, sink_user,
                                     &lonejson_error);
  if (status != LONEJSON_STATUS_OK) {
    return projection_lonejson_status(status, &lonejson_error, error);
  }
  render.projection = capture->projection;
  render.values = capture->values;
  render.captures = capture->captures;
  render.writer = &writer;
  render.error = &lonejson_error;
  status = projection_render_value(&render, capture->members, count, 0u);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &lonejson_error);
  }
  lonejson_writer_cleanup(&writer);
  out = projection_lonejson_status(status, &lonejson_error, error);
  if (out == LQL_STATUS_OK && out_emitted != NULL) {
    *out_emitted = 1;
  }
  return out;
}

LQL_INTERNAL_SYMBOL lql_status lql_projection_render_top_level(
    lql *self, const lql_projection *projection, const lonejson_spooled *input,
    lonejson_sink_fn sink, void *sink_user, int *out_emitted,
    lql_error *error) {
  lonejson *runtime;
  lonejson_error lonejson_error;
  lonejson_path_value_visitor visitor;
  lonejson_spooled *values;
  unsigned char *found;
  size_t *members;
  projection_capture *captures;
  projection_capture_set capture_set;
  projection_render_state render;
  lonejson_writer writer;
  lonejson_spooled cursor;
  lonejson_status status;
  lql_allocator *allocator;
  lql_status out;
  size_t i;
  size_t count;

  if (out_emitted != NULL) {
    *out_emitted = 0;
  }
  if (projection == NULL || input == NULL || sink == NULL ||
      projection->path_count == 0u) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection renderer arguments are invalid");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection receiver is required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lonejson_error_init(&lonejson_error);
  runtime = lql_lonejson_new(self, &lonejson_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY,
                  "projection runtime allocation failed");
    return LQL_STATUS_NO_MEMORY;
  }
  values = (lonejson_spooled *)allocator->calloc(
      allocator, projection->path_count, sizeof(*values));
  captures = (projection_capture *)allocator->calloc(
      allocator, projection->path_count, sizeof(*captures));
  found = (unsigned char *)allocator->calloc(allocator, projection->path_count,
                                             sizeof(*found));
  members = (size_t *)allocator->alloc(allocator, projection->path_count *
                                                      sizeof(*members));
  if (values == NULL || captures == NULL || found == NULL || members == NULL) {
    allocator->destroy(allocator, members);
    allocator->destroy(allocator, found);
    allocator->destroy(allocator, captures);
    allocator->destroy(allocator, values);
    lonejson_free(runtime);
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "projection allocation failed");
    return LQL_STATUS_NO_MEMORY;
  }
  for (i = 0u; i < projection->path_count; ++i) {
    lonejson_spooled_init_class(runtime, &values[i],
                                LONEJSON_SPOOL_CLASS_LARGE_TEXT);
    captures[i].receiver = self;
    captures[i].runtime = runtime;
    captures[i].path = &projection->compiled_paths[i];
    captures[i].spool = &values[i];
  }
  capture_set.captures = captures;
  capture_set.count = projection->path_count;
  visitor = lonejson_default_path_value_visitor();
  visitor.object_begin = projection_capture_set_object_begin;
  visitor.object_end = projection_capture_set_object_end;
  visitor.object_key_begin = projection_capture_set_key_begin;
  visitor.object_key_chunk = projection_capture_set_key_chunk;
  visitor.object_key_end = projection_capture_set_key_end;
  visitor.array_begin = projection_capture_set_array_begin;
  visitor.array_end = projection_capture_set_array_end;
  visitor.string_begin = projection_capture_set_string_begin;
  visitor.string_chunk = projection_capture_set_string_chunk;
  visitor.string_end = projection_capture_set_string_end;
  visitor.number_begin = projection_capture_set_number_begin;
  visitor.number_chunk = projection_capture_set_number_chunk;
  visitor.number_end = projection_capture_set_number_end;
  visitor.boolean_value = projection_capture_set_boolean;
  visitor.null_value = projection_capture_set_null;
  out = LQL_STATUS_OK;
  count = 0u;
  cursor = *input;
  status = lonejson_spooled_rewind(&cursor, &lonejson_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_visit_path_value_reader(runtime, projection_spool_read,
                                              &cursor, &visitor, &capture_set,
                                              &lonejson_error);
  }
  if (status != LONEJSON_STATUS_OK) {
    out = projection_lonejson_status(status, &lonejson_error, error);
    goto cleanup;
  }
  if (!captures[0].root_object) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "projection requires an object record");
    out = LQL_STATUS_JSON_ERROR;
    goto cleanup;
  }
  for (i = 0u; i < projection->path_count; ++i) {
    if (captures[i].found) {
      found[i] = 1u;
      members[count++] = i;
    }
  }
  if (count == 0u) {
    goto cleanup;
  }
  status = lonejson_writer_init_sink(runtime, &writer, sink, sink_user,
                                     &lonejson_error);
  if (status != LONEJSON_STATUS_OK) {
    out = projection_lonejson_status(status, &lonejson_error, error);
    goto cleanup;
  }
  render.projection = projection;
  render.values = values;
  render.captures = captures;
  render.writer = &writer;
  render.error = &lonejson_error;
  status = projection_render_value(&render, members, count, 0u);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &lonejson_error);
  }
  lonejson_writer_cleanup(&writer);
  out = projection_lonejson_status(status, &lonejson_error, error);
  if (out == LQL_STATUS_OK && out_emitted != NULL) {
    *out_emitted = 1;
  }

cleanup:
  for (i = 0u; i < projection->path_count; ++i) {
    if (captures[i].writer_ready) {
      lonejson_writer_cleanup(&captures[i].writer);
    }
    allocator->destroy(allocator, captures[i].key);
    lonejson_spooled_cleanup(&values[i]);
  }
  allocator->destroy(allocator, members);
  allocator->destroy(allocator, found);
  allocator->destroy(allocator, captures);
  allocator->destroy(allocator, values);
  lonejson_free(runtime);
  return out;
}
