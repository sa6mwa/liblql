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
