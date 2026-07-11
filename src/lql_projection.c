#include "lql_internal.h"

#include <string.h>

static int projection_is_space(unsigned char ch) {
  return ch == (unsigned char)' ' || ch == (unsigned char)'\t' ||
         ch == (unsigned char)'\r' || ch == (unsigned char)'\n';
}

static int projection_valid_pointer(const char *path) {
  const char *p;
  if (path == NULL || path[0] != '/' || path[1] == '\0') {
    return 0;
  }
  for (p = path; *p != '\0'; ++p) {
    if (*p == '~' && p[1] != '0' && p[1] != '1') {
      return 0;
    }
  }
  return 1;
}

static int projection_leading_index(const char *path) {
  const char *p;
  if (path == NULL || path[0] != '/') {
    return 0;
  }
  p = path + 1;
  if (*p == '\0') {
    return 0;
  }
  while (*p != '\0' && *p != '/') {
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
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "projection receiver required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  projection = (lql_projection *)allocator->calloc(allocator, 1u,
                                                    sizeof(*projection));
  if (projection == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  projection->paths = (char **)allocator->calloc(allocator, path_count,
                                                  sizeof(*projection->paths));
  if (projection->paths == NULL) {
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
    while (end > begin && projection_is_space((unsigned char)paths[i][end - 1u])) {
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
    if (!projection_valid_pointer(copy) || projection_leading_index(copy)) {
      allocator->destroy(allocator, copy);
      lql_projection_destroy_internal(self, projection);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR, "projection path is invalid");
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

LQL_INTERNAL_SYMBOL void lql_projection_destroy_internal(
    lql *self, lql_projection *projection) {
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
  }
  allocator->destroy(allocator, projection->paths);
  allocator->destroy(allocator, projection);
}

static lonejson_read_result projection_spool_read(void *user,
                                                   unsigned char *buffer,
                                                   size_t capacity) {
  return lonejson_spooled_read((lonejson_spooled *)user, buffer, capacity);
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

LQL_INTERNAL_SYMBOL lql_status lql_projection_render_top_level(
    lql *self, const lql_projection *projection,
    const lonejson_spooled *input, lonejson_sink_fn sink, void *sink_user,
    lql_error *error) {
  lonejson *runtime;
  lonejson_error lonejson_error;
  lonejson_json_value values[sizeof(unsigned long) * CHAR_BIT];
  lonejson_field fields[sizeof(unsigned long) * CHAR_BIT];
  lonejson_map map;
  lonejson_spooled cursor;
  lonejson_writer writer;
  lonejson_status status;
  size_t i;
  lql_status out;

  if (projection == NULL || input == NULL || sink == NULL ||
      projection->path_count == 0u ||
      projection->path_count > sizeof(values) / sizeof(values[0])) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection renderer arguments are invalid");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  for (i = 0u; i < projection->path_count; ++i) {
    const char *path;
    path = projection->paths[i];
    if (path == NULL || path[0] != '/' || path[1] == '\0' ||
        strchr(path + 1, '/') != NULL || strchr(path + 1, '~') != NULL) {
      lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                    "nested projection rendering is not implemented");
      return LQL_STATUS_UNSUPPORTED;
    }
  }
  lonejson_error_init(&lonejson_error);
  runtime = lql_lonejson_new_mapped_stream(self, &lonejson_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "projection runtime allocation failed");
    return LQL_STATUS_NO_MEMORY;
  }
  memset(&map, 0, sizeof(map));
  for (i = 0u; i < projection->path_count; ++i) {
    const char *key;
    size_t key_len;
    key = projection->paths[i] + 1;
    key_len = strlen(key);
    memset(&fields[i], 0, sizeof(fields[i]));
    fields[i].json_key = key;
    fields[i].json_key_len = key_len;
    fields[i].json_key_first = (unsigned char)key[0];
    fields[i].json_key_last = (unsigned char)key[key_len - 1u];
    fields[i].struct_offset = i * sizeof(lonejson_json_value);
    fields[i].kind = LONEJSON_FIELD_KIND_JSON_VALUE;
    fields[i].storage = LONEJSON_STORAGE_FIXED;
    fields[i].overflow_policy = LONEJSON_OVERFLOW_FAIL;
    /* Parsing reseeds JSON_VALUE handles. Request capture from the public
     * mapping contract so capture mode survives that reseed. */
    fields[i].flags = LONEJSON_FIELD_JSON_VALUE_DEFAULT_CAPTURE;
    fields[i].spool_class = LONEJSON_SPOOL_CLASS_DEFAULT;
  }
  map.name = "lql_projection_top_level";
  map.struct_size = projection->path_count * sizeof(lonejson_json_value);
  map.fields = fields;
  map.field_count = projection->path_count;
  /* clear_destination_by_default is disabled for this reusable mapped
   * runtime, so initialize the mapped record before parsing.  The field flag
   * above arms each JSON_VALUE for capture during initialization. */
  lonejson_init(runtime, &map, values);
  /* Use a cursor copy, as the candidate-owned spool remains valid for the
   * complete callback and must not have its public read position changed. */
  cursor = *input;
  status = lonejson_spooled_rewind(&cursor, &lonejson_error);
  if (status != LONEJSON_STATUS_OK) {
    out = projection_lonejson_status(status, &lonejson_error, error);
    goto cleanup;
  }
  status = lonejson_parse_reader(runtime, &map, values, projection_spool_read,
                                 &cursor, &lonejson_error);
  if (status != LONEJSON_STATUS_OK) {
    out = projection_lonejson_status(status, &lonejson_error, error);
    goto cleanup;
  }
  status = lonejson_writer_init_sink(runtime, &writer, sink, sink_user,
                                     &lonejson_error);
  if (status != LONEJSON_STATUS_OK) {
    out = projection_lonejson_status(status, &lonejson_error, error);
    goto cleanup;
  }
  status = writer.begin_object(&writer, &lonejson_error);
  for (i = 0u; status == LONEJSON_STATUS_OK && i < projection->path_count; ++i) {
    if (values[i].json == NULL) {
      continue;
    }
    status = writer.key(&writer, projection->paths[i] + 1u,
                        strlen(projection->paths[i] + 1u), &lonejson_error);
    if (status == LONEJSON_STATUS_OK) {
      status = writer.json_value(&writer, &values[i], &lonejson_error);
    }
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_object(&writer, &lonejson_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.finish(&writer, &lonejson_error);
  }
  writer.cleanup(&writer);
  out = projection_lonejson_status(status, &lonejson_error, error);

cleanup:
  for (i = 0u; i < projection->path_count; ++i) {
    lonejson_json_value_cleanup(&values[i]);
  }
  lonejson_free(runtime);
  return out;
}
