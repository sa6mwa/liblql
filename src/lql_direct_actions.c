#include "lql_internal.h"
#include "lql_temporal_internal.h"

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
  size_t j;
  size_t len;
  size_t base_len;
  size_t suffix_count;
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
    base_len = decoded_len;
    suffix_count = 0u;
    if (decoded_len > 2u && strcmp(decoded, "[]") != 0 &&
        strcmp(decoded, "*") != 0 && strcmp(decoded, "**") != 0 &&
        strcmp(decoded, "...") != 0) {
      while (base_len > 2u && decoded[base_len - 2u] == '[' &&
             decoded[base_len - 1u] == ']') {
        base_len -= 2u;
        ++suffix_count;
      }
    }
    if (suffix_count != 0u) {
      decoded[base_len] = '\0';
    }
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
    for (j = 0u; j < suffix_count; ++j) {
      char *wildcard;
      wildcard = mutation_copy(allocator, "[]", 2u);
      if (wildcard == NULL) {
        lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
        goto fail;
      }
      if (count == capacity) {
        size_t next_capacity;
        char **next;
        next_capacity = capacity == 0u ? 4u : capacity * 2u;
        next = (char **)allocator->realloc(allocator, segments,
                                           next_capacity * sizeof(*segments));
        if (next == NULL) {
          allocator->destroy(allocator, wildcard);
          lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
          goto fail;
        }
        segments = next;
        capacity = next_capacity;
      }
      segments[count++] = wildcard;
    }
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
mutation_parse_time_value(lql_allocator *allocator, const char *begin,
                          const char *end,
                          const lql_mutation_parse_options *options,
                          lql_mutation_action *action, lql_error *error) {
  lql_temporal temporal;
  char buffer[64];
  const char *value_begin;
  size_t value_len;

  mutation_trim(&begin, &end);
  if (begin == end) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "time mutation value is required");
    return LQL_STATUS_PARSE_ERROR;
  }
  value_begin = begin;
  value_len = (size_t)(end - begin);
  if (value_len >= 2u && ((begin[0] == '"' && end[-1] == '"') ||
                          (begin[0] == '\'' && end[-1] == '\''))) {
    value_begin = begin + 1;
    value_len -= 2u;
  }
  if (mutation_equal_ci(value_begin, value_len, "NOW")) {
    time_t now;
    now = options != NULL && options->time_now != NULL
              ? options->time_now(options->time_user)
              : time(NULL);
    if (!lql_temporal_from_time_t(now, 0, &temporal)) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "time mutation NOW value is invalid");
      return LQL_STATUS_PARSE_ERROR;
    }
  } else {
    char *literal;
    literal = mutation_copy(allocator, value_begin, value_len);
    if (literal == NULL) {
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      return LQL_STATUS_NO_MEMORY;
    }
    if (!lql_parse_temporal_literal(literal, &temporal)) {
      allocator->destroy(allocator, literal);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "time mutation value must be RFC3339");
      return LQL_STATUS_PARSE_ERROR;
    }
    allocator->destroy(allocator, literal);
  }
  if (!lql_temporal_format_rfc3339_nano(&temporal, buffer, sizeof(buffer))) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "time mutation value could not be formatted");
    return LQL_STATUS_PARSE_ERROR;
  }
  action->value_kind = LQL_MUTATION_VALUE_STRING;
  action->value = mutation_copy(allocator, buffer, strlen(buffer));
  if (action->value == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  return LQL_STATUS_OK;
}

static lql_status
mutation_parse_time_set(lql_allocator *allocator, const char *begin,
                        const char *end,
                        const lql_mutation_parse_options *options,
                        lql_mutation_action *action, lql_error *error) {
  const char *equal;
  lql_status status;

  if (mutation_has_prefix(begin, end, "rm:") ||
      mutation_has_prefix(begin, end, "remove:") ||
      mutation_has_prefix(begin, end, "delete:") ||
      mutation_has_prefix(begin, end, "del:")) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "time-prefixed mutation cannot be remove/delete");
    return LQL_STATUS_PARSE_ERROR;
  }
  if ((size_t)(end - begin) >= 2u && ((end[-2] == '+' && end[-1] == '+') ||
                                      (end[-2] == '-' && end[-1] == '-'))) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "time-prefixed mutation cannot be increment");
    return LQL_STATUS_PARSE_ERROR;
  }
  equal = begin;
  while (equal < end && *equal != '=') {
    ++equal;
  }
  if (equal == end) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "time-backed mutation requires path=value");
    return LQL_STATUS_PARSE_ERROR;
  }
  action->kind = LQL_MUTATION_SET;
  status = mutation_parse_path(allocator, begin, equal, action, error);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  status = mutation_parse_time_value(allocator, equal + 1, end, options, action,
                                     error);
  if (status != LQL_STATUS_OK) {
    mutation_action_cleanup(allocator, action);
  }
  return status;
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
      mutation_has_prefix(begin, end, "delete:") ||
      mutation_has_prefix(begin, end, "del:") ||
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
  if ((options->file_value_open == NULL) !=
      (options->file_value_close == NULL)) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "file value open and close callbacks must be paired");
    mutation_action_cleanup(allocator, action);
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (options->file_value_open != NULL) {
    const char *path_begin;
    const char *path_end;
    path_begin = equal + 1;
    path_end = end;
    mutation_trim(&path_begin, &path_end);
    if ((size_t)(path_end - path_begin) >= 2u &&
        ((path_begin[0] == '"' && path_end[-1] == '"') ||
         (path_begin[0] == '\'' && path_end[-1] == '\''))) {
      ++path_begin;
      --path_end;
      mutation_trim(&path_begin, &path_end);
    }
    if (path_begin == path_end) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "file-backed mutation path is required");
      mutation_action_cleanup(allocator, action);
      return LQL_STATUS_PARSE_ERROR;
    }
    action->value =
        mutation_copy(allocator, path_begin, (size_t)(path_end - path_begin));
    if (action->value == NULL) {
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      mutation_action_cleanup(allocator, action);
      return LQL_STATUS_NO_MEMORY;
    }
    action->file_value_open = options->file_value_open;
    action->file_value_close = options->file_value_close;
    action->file_value_user = options->file_value_user;
    return LQL_STATUS_OK;
  }
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
  if (mutation_has_prefix(begin, end, "time:")) {
    return mutation_parse_time_set(allocator, begin + 5u, end, options, action,
                                   error);
  }
  if ((size_t)(end - begin) >= 3u && memcmp(begin, "rm:", 3u) == 0) {
    action->kind = LQL_MUTATION_REMOVE;
    return mutation_parse_path(allocator, begin + 3u, end, action, error);
  }
  if ((size_t)(end - begin) >= 7u && memcmp(begin, "remove:", 7u) == 0) {
    action->kind = LQL_MUTATION_REMOVE;
    return mutation_parse_path(allocator, begin + 7u, end, action, error);
  }
  if ((size_t)(end - begin) >= 7u && memcmp(begin, "delete:", 7u) == 0) {
    action->kind = LQL_MUTATION_REMOVE;
    return mutation_parse_path(allocator, begin + 7u, end, action, error);
  }
  if ((size_t)(end - begin) >= 4u && memcmp(begin, "del:", 4u) == 0) {
    action->kind = LQL_MUTATION_REMOVE;
    return mutation_parse_path(allocator, begin + 4u, end, action, error);
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

static lql_status mutation_append_action(lql_allocator *allocator,
                                         lql_mutation *mutation,
                                         size_t *capacity,
                                         lql_mutation_action *action,
                                         lql_error *error) {
  lql_mutation_action *next;
  size_t next_capacity;

  if (mutation->action_count == *capacity) {
    next_capacity = *capacity == 0u ? 4u : *capacity * 2u;
    next = (lql_mutation_action *)allocator->realloc(
        allocator, mutation->actions, next_capacity * sizeof(*next));
    if (next == NULL) {
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      return LQL_STATUS_NO_MEMORY;
    }
    memset(next + *capacity, 0, (next_capacity - *capacity) * sizeof(*next));
    mutation->actions = next;
    *capacity = next_capacity;
  }
  mutation->actions[mutation->action_count++] = *action;
  memset(action, 0, sizeof(*action));
  return LQL_STATUS_OK;
}

static lql_status mutation_prepend_segments(lql_allocator *allocator,
                                            const lql_mutation_action *prefix,
                                            lql_mutation_action *action,
                                            lql_error *error) {
  char **segments;
  size_t i;
  size_t count;

  count = prefix->segment_count + action->segment_count;
  segments = (char **)allocator->calloc(allocator, count, sizeof(*segments));
  if (segments == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  for (i = 0u; i < prefix->segment_count; ++i) {
    segments[i] = mutation_copy(allocator, prefix->segments[i],
                                strlen(prefix->segments[i]));
    if (segments[i] == NULL) {
      while (i > 0u) {
        --i;
        allocator->destroy(allocator, segments[i]);
      }
      allocator->destroy(allocator, segments);
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      return LQL_STATUS_NO_MEMORY;
    }
  }
  for (i = 0u; i < action->segment_count; ++i) {
    segments[prefix->segment_count + i] = action->segments[i];
  }
  allocator->destroy(allocator, action->segments);
  action->segments = segments;
  action->segment_count = count;
  return LQL_STATUS_OK;
}

static int mutation_find_brace(const char *begin, const char *end,
                               const char **open_brace) {
  const char *cursor;
  const char *equal;
  if (end <= begin || end[-1] != '}') {
    return 0;
  }
  cursor = begin;
  equal = NULL;
  while (cursor < end) {
    if (*cursor == '=') {
      equal = cursor;
      break;
    }
    if (*cursor == '{') {
      *open_brace = cursor;
      return equal == NULL && cursor > begin;
    }
    ++cursor;
  }
  return 0;
}

static lql_status
mutation_parse_expression(lql_allocator *allocator, const char *raw,
                          const lql_mutation_parse_options *options,
                          lql_mutation *mutation, size_t *capacity,
                          lql_error *error) {
  const char *begin;
  const char *end;
  const char *open_brace;
  lql_mutation_action action;
  lql_status status;

  begin = raw;
  end = raw + strlen(raw);
  mutation_trim(&begin, &end);
  if (mutation_find_brace(begin, end, &open_brace)) {
    lql_mutation_action prefix;
    const char *part;
    const char *content_end;
    memset(&prefix, 0, sizeof(prefix));
    status = mutation_parse_path(allocator, begin, open_brace, &prefix, error);
    if (status != LQL_STATUS_OK) {
      return status;
    }
    part = open_brace + 1u;
    content_end = end - 1u;
    while (part < content_end) {
      const char *part_end;
      const char *scan;
      int quote;
      quote = 0;
      scan = part;
      while (scan < content_end) {
        if (quote != 0) {
          if (*scan == (char)quote) {
            quote = 0;
          }
        } else if (*scan == '"' || *scan == '\'') {
          quote = (unsigned char)*scan;
        } else if (*scan == ',' || *scan == '\n') {
          break;
        }
        ++scan;
      }
      part_end = scan;
      mutation_trim(&part, &part_end);
      if (part < part_end) {
        char *sub;
        sub = mutation_copy(allocator, part, (size_t)(part_end - part));
        if (sub == NULL) {
          mutation_action_cleanup(allocator, &prefix);
          lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
          return LQL_STATUS_NO_MEMORY;
        }
        memset(&action, 0, sizeof(action));
        status = mutation_parse_one(allocator, sub, options, &action, error);
        allocator->destroy(allocator, sub);
        if (status == LQL_STATUS_OK) {
          status =
              mutation_prepend_segments(allocator, &prefix, &action, error);
        }
        if (status == LQL_STATUS_OK) {
          status = mutation_append_action(allocator, mutation, capacity,
                                          &action, error);
        }
        mutation_action_cleanup(allocator, &action);
        if (status != LQL_STATUS_OK) {
          mutation_action_cleanup(allocator, &prefix);
          return status;
        }
      }
      part = scan < content_end ? scan + 1u : scan;
    }
    mutation_action_cleanup(allocator, &prefix);
    return LQL_STATUS_OK;
  }

  memset(&action, 0, sizeof(action));
  status = mutation_parse_one(allocator, raw, options, &action, error);
  if (status == LQL_STATUS_OK) {
    status =
        mutation_append_action(allocator, mutation, capacity, &action, error);
  }
  mutation_action_cleanup(allocator, &action);
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
  size_t capacity;
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
  capacity = 0u;
  for (i = 0u; i < expression_count; ++i) {
    status = mutation_parse_expression(allocator, expressions[i], options,
                                       mutation, &capacity, error);
    if (status != LQL_STATUS_OK) {
      lql_mutation_destroy_internal(self, mutation);
      return status;
    }
  }
  if (mutation->action_count == 0u) {
    lql_mutation_destroy_internal(self, mutation);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "mutation expressions are required");
    return LQL_STATUS_PARSE_ERROR;
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
