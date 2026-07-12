#include "lql_lonejson_internal.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct lql_mutation_emit_context {
  const lql_mutation_action *action;
} lql_mutation_emit_context;

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
  char *end_ptr;
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
  } else {
    errno = 0;
    ignored_number = strtod(begin, &end_ptr);
    (void)ignored_number;
    if (end_ptr == end && errno != ERANGE) {
      action->value_kind = LQL_MUTATION_VALUE_NUMBER;
    } else {
      action->value_kind = LQL_MUTATION_VALUE_STRING;
    }
  }
  action->value = mutation_copy(allocator, value_begin, value_len);
  if (action->value == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  return LQL_STATUS_OK;
}

static lql_status mutation_parse_one(lql_allocator *allocator, const char *raw,
                                     lql_mutation_action *action,
                                     lql_error *error) {
  const char *begin;
  const char *end;
  const char *equal;
  const char *delta_text;
  char *end_ptr;
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
  errno = 0;
  delta = strtod(delta_text, &end_ptr);
  if (delta_text < end && (*delta_text == '+' || *delta_text == '-') &&
      end_ptr == end && errno != ERANGE && delta != 0.0) {
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
        mutation_parse_one(allocator, expressions[i],
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

static lonejson_status mutation_spool_sink(void *user, const void *data,
                                           size_t len, lonejson_error *error) {
  return lonejson_spooled_append((lonejson_spooled *)user, data, len, error);
}

static lonejson_read_result
mutation_spool_read(void *user, unsigned char *buffer, size_t capacity) {
  return lonejson_spooled_read((lonejson_spooled *)user, buffer, capacity);
}

static lonejson_status mutation_emit_value(lonejson_writer *writer, void *user,
                                           lonejson_error *error) {
  const lql_mutation_emit_context *context;
  const lql_mutation_action *action;
  context = (const lql_mutation_emit_context *)user;
  action = context->action;
  switch (action->value_kind) {
  case LQL_MUTATION_VALUE_STRING:
    return lonejson_writer_string(writer, action->value, strlen(action->value),
                                  error);
  case LQL_MUTATION_VALUE_NUMBER:
    return lonejson_writer_number_text(writer, action->value,
                                       strlen(action->value), error);
  case LQL_MUTATION_VALUE_BOOL:
    return lonejson_writer_bool(writer, action->value[0] == 't', error);
  case LQL_MUTATION_VALUE_NULL:
    return lonejson_writer_null(writer, error);
  default:
    return LONEJSON_STATUS_INVALID_ARGUMENT;
  }
}

static lonejson_status
mutation_emit_increment(lonejson_writer *writer,
                        const lonejson_value_rewrite_old_value *old_value,
                        void *user, lonejson_error *error) {
  const lql_mutation_emit_context *context;
  char *end_ptr;
  double current;
  double next;

  context = (const lql_mutation_emit_context *)user;
  if (!old_value->present) {
    return lonejson_writer_f64(writer, context->action->delta, error);
  }
  if (old_value->type != LONEJSON_VALUE_NUMBER) {
    if (error != NULL) {
      error->code = LONEJSON_STATUS_TYPE_MISMATCH;
      strcpy(error->message, "increment target is not numeric");
    }
    return LONEJSON_STATUS_TYPE_MISMATCH;
  }
  errno = 0;
  current = strtod(old_value->number, &end_ptr);
  if (end_ptr != old_value->number + old_value->number_len || errno == ERANGE) {
    if (error != NULL) {
      error->code = LONEJSON_STATUS_TYPE_MISMATCH;
      strcpy(error->message, "increment target number is invalid");
    }
    return LONEJSON_STATUS_TYPE_MISMATCH;
  }
  next = current + context->action->delta;
  if (next != next || next == HUGE_VAL || next == -HUGE_VAL) {
    if (error != NULL) {
      error->code = LONEJSON_STATUS_OVERFLOW;
      strcpy(error->message, "increment result is not finite");
    }
    return LONEJSON_STATUS_OVERFLOW;
  }
  return lonejson_writer_f64(writer, next, error);
}

static lql_status mutation_lonejson_status(lonejson_status status,
                                           const lonejson_error *source,
                                           lql_error *error) {
  if (status == LONEJSON_STATUS_OK) {
    return LQL_STATUS_OK;
  }
  if (status == LONEJSON_STATUS_ALLOCATION_FAILED) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "mutation allocation failed");
    return LQL_STATUS_NO_MEMORY;
  }
  lql_set_error(error, LQL_STATUS_JSON_ERROR,
                source != NULL && source->message[0] != '\0'
                    ? source->message
                    : "mutation JSON processing failed");
  return LQL_STATUS_JSON_ERROR;
}

LQL_INTERNAL_SYMBOL lql_status lql_mutation_render(
    lql *self, const lql_mutation *mutation, const lonejson_spooled *input,
    lonejson_sink_fn sink, void *sink_user, lql_error *error) {
  lonejson *runtime;
  lonejson_error lonejson_error;
  lonejson_spooled input_cursor;
  lonejson_spooled first;
  lonejson_spooled second;
  lonejson_spooled *source;
  lonejson_spooled *target;
  lonejson_value_rewrite_options options;
  lql_mutation_emit_context emit_context;
  lonejson_status status;
  lql_status out;
  size_t i;

  if (mutation == NULL || input == NULL || sink == NULL ||
      mutation->action_count == 0u) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "mutation renderer arguments are invalid");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lonejson_error_init(&lonejson_error);
  runtime = lql_lonejson_new(self, &lonejson_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY,
                  "mutation runtime allocation failed");
    return LQL_STATUS_NO_MEMORY;
  }
  lonejson_spooled_init_class(runtime, &first, LONEJSON_SPOOL_CLASS_LARGE_TEXT);
  lonejson_spooled_init_class(runtime, &second,
                              LONEJSON_SPOOL_CLASS_LARGE_TEXT);
  input_cursor = *input;
  status = lonejson_spooled_rewind(&input_cursor, &lonejson_error);
  if (status != LONEJSON_STATUS_OK) {
    out = mutation_lonejson_status(status, &lonejson_error, error);
    goto cleanup;
  }
  source = &input_cursor;
  out = LQL_STATUS_OK;
  for (i = 0u; i < mutation->action_count; ++i) {
    const lql_mutation_action *action;
    action = &mutation->actions[i];
    memset(&options, 0, sizeof(options));
    options.target_segments = (const char *const *)action->segments;
    options.target_segment_count = action->segment_count;
    emit_context.action = action;
    if (action->kind == LQL_MUTATION_REMOVE) {
      options.action = LONEJSON_VALUE_REWRITE_DROP;
    } else if (action->kind == LQL_MUTATION_INCREMENT) {
      options.action = LONEJSON_VALUE_REWRITE_REPLACE_WITH;
      options.replace = mutation_emit_increment;
      options.replace_user = &emit_context;
    } else {
      options.action = LONEJSON_VALUE_REWRITE_REPLACE;
      options.replacement.emit = mutation_emit_value;
      options.replacement.emit_user = &emit_context;
    }
    if (i + 1u == mutation->action_count) {
      status = lonejson_value_rewrite_reader(runtime, mutation_spool_read,
                                             source, sink, sink_user, &options,
                                             &lonejson_error);
    } else {
      target = (i & 1u) == 0u ? &first : &second;
      lonejson_spooled_reset(target);
      status = lonejson_value_rewrite_reader(runtime, mutation_spool_read,
                                             source, mutation_spool_sink,
                                             target, &options, &lonejson_error);
      if (status == LONEJSON_STATUS_OK) {
        status = lonejson_spooled_rewind(target, &lonejson_error);
      }
      source = target;
    }
    if (status != LONEJSON_STATUS_OK) {
      out = mutation_lonejson_status(status, &lonejson_error, error);
      goto cleanup;
    }
  }

cleanup:
  lonejson_spooled_cleanup(&second);
  lonejson_spooled_cleanup(&first);
  lonejson_free(runtime);
  return out;
}
