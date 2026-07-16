#include "lql_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int mutation_space(unsigned char ch) {
  return ch == (unsigned char)' ' || ch == (unsigned char)'\t' ||
         ch == (unsigned char)'\r' || ch == (unsigned char)'\n';
}

static void mutation_trim(const char **begin, const char **end) {
  while (*begin < *end && mutation_space((unsigned char)**begin)) {
    ++*begin;
  }
  while (*end > *begin && mutation_space((unsigned char)(*end)[-1])) {
    --*end;
  }
}

static int mutation_equal_ci(const char *text, size_t len,
                             const char *literal) {
  size_t i;
  if (strlen(literal) != len) {
    return 0;
  }
  for (i = 0u; i < len; ++i) {
    if (tolower((unsigned char)text[i]) != tolower((unsigned char)literal[i])) {
      return 0;
    }
  }
  return 1;
}

static int mutation_json_number(const char *begin, const char *end) {
  const char *cursor;

  cursor = begin;
  if (cursor < end && *cursor == '-') {
    ++cursor;
  }
  if (cursor == end) {
    return 0;
  }
  if (*cursor == '0') {
    ++cursor;
  } else if (*cursor >= '1' && *cursor <= '9') {
    do {
      ++cursor;
    } while (cursor < end && *cursor >= '0' && *cursor <= '9');
  } else {
    return 0;
  }
  if (cursor < end && *cursor == '.') {
    ++cursor;
    if (cursor == end || *cursor < '0' || *cursor > '9') {
      return 0;
    }
    do {
      ++cursor;
    } while (cursor < end && *cursor >= '0' && *cursor <= '9');
  }
  if (cursor < end && (*cursor == 'e' || *cursor == 'E')) {
    ++cursor;
    if (cursor < end && (*cursor == '+' || *cursor == '-')) {
      ++cursor;
    }
    if (cursor == end || *cursor < '0' || *cursor > '9') {
      return 0;
    }
    do {
      ++cursor;
    } while (cursor < end && *cursor >= '0' && *cursor <= '9');
  }
  return cursor == end;
}

static int mutation_numeric_candidate(const char *begin, const char *end) {
  if (begin == end) {
    return 0;
  }
  if (*begin == '+' || *begin == '-' || (*begin >= '0' && *begin <= '9')) {
    return 1;
  }
  return mutation_equal_ci(begin, (size_t)(end - begin), "nan") ||
         mutation_equal_ci(begin, (size_t)(end - begin), "inf") ||
         mutation_equal_ci(begin, (size_t)(end - begin), "infinity");
}

static int mutation_finite(double value) {
  /* Keep parse-time errors aligned for set and signed increment literals. */
  return value == value && value != HUGE_VAL && value != -HUGE_VAL;
}

static char *mutation_copy(lql_allocator *allocator, const char *data,
                           size_t len) {
  char *out;
  out = (char *)allocator->alloc(allocator, len + 1u);
  if (out == NULL) {
    return NULL;
  }
  if (len != 0u) {
    memcpy(out, data, len);
  }
  out[len] = '\0';
  return out;
}

static int mutation_has_prefix(const char *begin, const char *end,
                               const char *prefix) {
  size_t len;
  len = strlen(prefix);
  return (size_t)(end - begin) >= len && memcmp(begin, prefix, len) == 0;
}

static lql_status mutation_resolve_file_path(
    lql_allocator *allocator, const char *begin, const char *end,
    const lql_mutation_parse_options *options, char **out, lql_error *error) {
  const char *home;
  const char *base;
  size_t home_len;
  size_t base_len;
  size_t len;
  char *path;

  *out = NULL;
  mutation_trim(&begin, &end);
  if ((size_t)(end - begin) >= 2u && ((begin[0] == '"' && end[-1] == '"') ||
                                      (begin[0] == '\'' && end[-1] == '\''))) {
    ++begin;
    --end;
    mutation_trim(&begin, &end);
  }
  if (begin == end) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "file-backed mutation path is required");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (*begin == '~' && (begin + 1 == end || begin[1] == '/')) {
    home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
      lql_set_error(error, LQL_STATUS_IO_ERROR,
                    "HOME is required to resolve file-backed mutation path");
      return LQL_STATUS_IO_ERROR;
    }
    home_len = strlen(home);
    len = home_len + (size_t)(end - begin) - 1u;
    path = (char *)allocator->alloc(allocator, len + 1u);
    if (path == NULL) {
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      return LQL_STATUS_NO_MEMORY;
    }
    memcpy(path, home, home_len);
    if (begin + 1 < end) {
      memcpy(path + home_len, begin + 1, (size_t)(end - begin) - 1u);
    }
    path[len] = '\0';
    *out = path;
    return LQL_STATUS_OK;
  }
  if (*begin == '/') {
    path = mutation_copy(allocator, begin, (size_t)(end - begin));
    if (path == NULL) {
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      return LQL_STATUS_NO_MEMORY;
    }
    *out = path;
    return LQL_STATUS_OK;
  }
  if (options == NULL || options->file_value_base_dir.data == NULL ||
      options->file_value_base_dir.len == 0u) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "relative file-backed mutation path requires base dir");
    return LQL_STATUS_PARSE_ERROR;
  }
  base = options->file_value_base_dir.data;
  base_len = options->file_value_base_dir.len;
  len = base_len + 1u + (size_t)(end - begin);
  path = (char *)allocator->alloc(allocator, len + 1u);
  if (path == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  memcpy(path, base, base_len);
  if (base[base_len - 1u] == '/') {
    memcpy(path + base_len, begin, (size_t)(end - begin));
    path[base_len + (size_t)(end - begin)] = '\0';
  } else {
    path[base_len] = '/';
    memcpy(path + base_len + 1u, begin, (size_t)(end - begin));
    path[len] = '\0';
  }
  *out = path;
  return LQL_STATUS_OK;
}

static void mutation_action_cleanup(lql_allocator *allocator,
                                    lql_mutation_action *action) {
  size_t i;
  if (allocator == NULL || action == NULL) {
    return;
  }
  for (i = 0u; i < action->segment_count; ++i) {
    allocator->destroy(allocator, action->segments[i]);
  }
  allocator->destroy(allocator, action->segments);
  allocator->destroy(allocator, action->value);
  memset(action, 0, sizeof(*action));
}

static lql_status mutation_parse_path(lql_allocator *allocator,
                                      const char *begin, const char *end,
                                      lql_mutation_action *action,
                                      lql_error *error) {
  const char *part;
  const char *slash;
  char **segments;
  size_t count;
  size_t capacity;
  size_t i;
  size_t len;
  char *segment;
  char *decoded;
  size_t decoded_len;

  mutation_trim(&begin, &end);
  if (begin == end || *begin != '/') {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "mutation path must start with '/'");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (begin + 1 == end) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "mutation path must not refer to the document root");
    return LQL_STATUS_PARSE_ERROR;
  }
  segments = NULL;
  count = 0u;
  capacity = 0u;
  part = begin + 1;
  for (;;) {
    slash = part;
    while (slash < end && *slash != '/') {
      ++slash;
    }
    len = (size_t)(slash - part);
    segment = mutation_copy(allocator, part, len);
    if (segment == NULL) {
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      goto fail;
    }
    decoded = (char *)allocator->alloc(allocator, len + 1u);
    if (decoded == NULL) {
      allocator->destroy(allocator, segment);
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      goto fail;
    }
    decoded_len = 0u;
    for (i = 0u; i < len; ++i) {
      if (segment[i] == '~' && i + 1u < len &&
          (segment[i + 1u] == '0' || segment[i + 1u] == '1')) {
        decoded[decoded_len++] = segment[i + 1u] == '0' ? '~' : '/';
        ++i;
      } else {
        decoded[decoded_len++] = segment[i];
      }
    }
    decoded[decoded_len] = '\0';
    allocator->destroy(allocator, segment);
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
  action->segments = segments;
  action->segment_count = count;
  return LQL_STATUS_OK;

fail:
  for (i = 0u; i < count; ++i) {
    allocator->destroy(allocator, segments[i]);
  }
  allocator->destroy(allocator, segments);
  return error == NULL ? LQL_STATUS_PARSE_ERROR : error->code;
}

static lql_status mutation_parse_value(lql_allocator *allocator,
                                       const char *begin, const char *end,
                                       lql_mutation_action *action,
                                       lql_error *error) {
  double ignored_number;
  const char *value_begin;
  size_t value_len;

  mutation_trim(&begin, &end);
  if (begin == end) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "mutation value is required");
    return LQL_STATUS_PARSE_ERROR;
  }
  value_begin = begin;
  value_len = (size_t)(end - begin);
  if (value_len >= 2u && ((begin[0] == '"' && end[-1] == '"') ||
                          (begin[0] == '\'' && end[-1] == '\''))) {
    value_begin = begin + 1;
    value_len -= 2u;
    action->value_kind = LQL_MUTATION_VALUE_STRING;
  } else if (mutation_equal_ci(begin, value_len, "true")) {
    action->value_kind = LQL_MUTATION_VALUE_BOOL;
    value_begin = "true";
    value_len = 4u;
  } else if (mutation_equal_ci(begin, value_len, "false")) {
    action->value_kind = LQL_MUTATION_VALUE_BOOL;
    value_begin = "false";
    value_len = 5u;
  } else if (mutation_equal_ci(begin, value_len, "null")) {
    action->value_kind = LQL_MUTATION_VALUE_NULL;
    value_begin = "null";
    value_len = 4u;
  } else if (mutation_numeric_candidate(begin, end)) {
    if (!mutation_json_number(begin, end)) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "mutation numeric value must be a JSON number");
      return LQL_STATUS_PARSE_ERROR;
    }
    if (!lql_number_parse_json(begin, (size_t)(end - begin), &ignored_number) ||
        !mutation_finite(ignored_number)) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "mutation numeric value must be finite");
      return LQL_STATUS_PARSE_ERROR;
    }
    action->value_kind = LQL_MUTATION_VALUE_NUMBER;
  } else {
    action->value_kind = LQL_MUTATION_VALUE_STRING;
  }
  action->value = mutation_copy(allocator, value_begin, value_len);
  if (action->value == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  return LQL_STATUS_OK;
}

static lql_status
mutation_parse_file_set(lql_allocator *allocator, const char *begin,
                        const char *end,
                        const lql_mutation_parse_options *options,
                        lql_mutation_value_kind value_kind,
                        lql_mutation_action *action, lql_error *error) {
  const char *equal;
  lql_status status;

  if (options == NULL || !options->enable_file_values) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "file-backed mutations are disabled");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (mutation_has_prefix(begin, end, "rm:") ||
      mutation_has_prefix(begin, end, "remove:") ||
      mutation_has_prefix(begin, end, "time:")) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "file-backed mutation values require path=file");
    return LQL_STATUS_PARSE_ERROR;
  }
  if ((size_t)(end - begin) >= 2u && end[-2] == '+' && end[-1] == '+') {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "file-backed mutation values cannot use ++");
    return LQL_STATUS_PARSE_ERROR;
  }
  if ((size_t)(end - begin) >= 2u && end[-2] == '-' && end[-1] == '-') {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "file-backed mutation values cannot use --");
    return LQL_STATUS_PARSE_ERROR;
  }
  equal = begin;
  while (equal < end && *equal != '=') {
    ++equal;
  }
  if (equal == end) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "file-backed mutation requires path=file");
    return LQL_STATUS_PARSE_ERROR;
  }
  action->kind = LQL_MUTATION_SET;
  status = mutation_parse_path(allocator, begin, equal, action, error);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  action->value_kind = value_kind;
  status = mutation_resolve_file_path(allocator, equal + 1, end, options,
                                      &action->value, error);
  if (status != LQL_STATUS_OK) {
    mutation_action_cleanup(allocator, action);
  }
  return status;
}

static lql_status mutation_parse_one(lql_allocator *allocator, const char *raw,
                                     const lql_mutation_parse_options *options,
                                     lql_mutation_action *action,
                                     lql_error *error) {
  const char *begin;
  const char *end;
  const char *equal;
  const char *delta_text;
  double delta;
  lql_status status;

  if (raw == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "mutation expression is required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  begin = raw;
  end = raw + strlen(raw);
  mutation_trim(&begin, &end);
  if (begin == end) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "mutation expression is required");
    return LQL_STATUS_PARSE_ERROR;
  }
  memset(action, 0, sizeof(*action));
  if (mutation_has_prefix(begin, end, "textfile:")) {
    return mutation_parse_file_set(allocator, begin + 9u, end, options,
                                   LQL_MUTATION_VALUE_FILE_TEXT, action, error);
  }
  if (mutation_has_prefix(begin, end, "base64file:")) {
    return mutation_parse_file_set(allocator, begin + 11u, end, options,
                                   LQL_MUTATION_VALUE_FILE_BASE64, action,
                                   error);
  }
  if (mutation_has_prefix(begin, end, "file:")) {
    return mutation_parse_file_set(allocator, begin + 5u, end, options,
                                   LQL_MUTATION_VALUE_FILE_AUTO, action, error);
  }
  if ((size_t)(end - begin) >= 3u && memcmp(begin, "rm:", 3u) == 0) {
    action->kind = LQL_MUTATION_REMOVE;
    return mutation_parse_path(allocator, begin + 3u, end, action, error);
  }
  if ((size_t)(end - begin) >= 7u && memcmp(begin, "remove:", 7u) == 0) {
    action->kind = LQL_MUTATION_REMOVE;
    return mutation_parse_path(allocator, begin + 7u, end, action, error);
  }
  if ((size_t)(end - begin) >= 2u && end[-2] == '+' && end[-1] == '+') {
    action->kind = LQL_MUTATION_INCREMENT;
    action->delta = 1.0;
    return mutation_parse_path(allocator, begin, end - 2u, action, error);
  }
  if ((size_t)(end - begin) >= 2u && end[-2] == '-' && end[-1] == '-') {
    action->kind = LQL_MUTATION_INCREMENT;
    action->delta = -1.0;
    return mutation_parse_path(allocator, begin, end - 2u, action, error);
  }
  equal = begin;
  while (equal < end && *equal != '=') {
    ++equal;
  }
  if (equal == end) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "mutation must be remove, increment, or path=value");
    return LQL_STATUS_PARSE_ERROR;
  }
  status = mutation_parse_path(allocator, begin, equal, action, error);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  delta_text = equal + 1;
  mutation_trim(&delta_text, &end);
  if (delta_text < end && (*delta_text == '+' || *delta_text == '-') &&
      lql_number_parse_json(
          *delta_text == '+' ? delta_text + 1 : delta_text,
          (size_t)(end - (*delta_text == '+' ? delta_text + 1 : delta_text)),
          &delta) &&
      delta != 0.0 && mutation_finite(delta)) {
    action->kind = LQL_MUTATION_INCREMENT;
    action->delta = delta;
    return LQL_STATUS_OK;
  }
  action->kind = LQL_MUTATION_SET;
  status = mutation_parse_value(allocator, delta_text, end, action, error);
  if (status != LQL_STATUS_OK) {
    mutation_action_cleanup(allocator, action);
  }
  return status;
}

LQL_INTERNAL_SYMBOL lql_status lql_mutation_parse_internal(
    lql *self, const char *const *expressions, size_t expression_count,
    lql_mutation **out, lql_error *error) {
  return lql_mutation_parse_internal_with_options(
      self, expressions, expression_count, NULL, out, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_mutation_parse_internal_with_options(
    lql *self, const char *const *expressions, size_t expression_count,
    const lql_mutation_parse_options *options, lql_mutation **out,
    lql_error *error) {
  lql_allocator *allocator;
  lql_mutation *mutation;
  size_t i;
  lql_status status;

  if (out == NULL || (expressions == NULL && expression_count != 0u)) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "mutation expressions and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = NULL;
  if (expression_count == 0u) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "mutation expressions are required");
    return LQL_STATUS_PARSE_ERROR;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "mutation receiver is required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  mutation =
      (lql_mutation *)allocator->calloc(allocator, 1u, sizeof(*mutation));
  if (mutation == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  mutation->actions = (lql_mutation_action *)allocator->calloc(
      allocator, expression_count, sizeof(*mutation->actions));
  if (mutation->actions == NULL) {
    allocator->destroy(allocator, mutation);
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  for (i = 0u; i < expression_count; ++i) {
    status =
        mutation_parse_one(allocator, expressions[i], options,
                           &mutation->actions[mutation->action_count], error);
    if (status != LQL_STATUS_OK) {
      lql_mutation_destroy_internal(self, mutation);
      return status;
    }
    ++mutation->action_count;
  }
  *out = mutation;
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL void lql_mutation_destroy_internal(lql *self,
                                                       lql_mutation *mutation) {
  lql_allocator *allocator;
  size_t i;
  if (mutation == NULL) {
    return;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    return;
  }
  for (i = 0u; i < mutation->action_count; ++i) {
    mutation_action_cleanup(allocator, &mutation->actions[i]);
  }
  allocator->destroy(allocator, mutation->actions);
  allocator->destroy(allocator, mutation);
}
