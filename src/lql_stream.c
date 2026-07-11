#include "lql_internal.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LQL_STREAM_MUTATION_TAPE_MAX_BYTES (64u * 1024u)

typedef struct lql_stream_term {
  const lql_selector *selector;
  lql_selector_kind kind;
  size_t field_index;
  const char *value;
  size_t value_len;
  size_t *failure;
  unsigned long bit;
} lql_stream_term;

typedef struct lql_stream_field {
  const char *key;
  size_t key_len;
} lql_stream_field;

typedef struct lql_stream_member_context lql_stream_member_context;
typedef struct lql_mapped_projection_field lql_mapped_projection_field;

struct lql_stream_program {
  lql_stream_term terms[sizeof(unsigned long) * CHAR_BIT];
  size_t term_count;
  lql_stream_field fields[sizeof(unsigned long) * CHAR_BIT];
  size_t field_count;
  int has_nested_paths;
  int requires_generic_path;
  int needs_execution_clock;
  int match_all;
  lonejson_field mapped_fields[sizeof(unsigned long) * CHAR_BIT];
  lonejson_map mapped_map;
};

typedef struct lql_stream_state {
  lql *receiver;
  const lql_stream_request *request;
  lql_stream_result *result;
  const lql_stream_program *program;
  lql_error failure;
  size_t object_depth;
  size_t array_depth;
  size_t key_pos;
  unsigned long key_candidates;
  size_t current_term;
  size_t active_term;
  size_t active_pos;
  unsigned long hits;
  int active_mismatch;
  int key_active;
  int active;
  int root_value_started;
  lql_temporal execution_now;
  lql_temporal execution_today;
  lql_temporal execution_yesterday;
  int execution_now_ready;
  int execution_today_ready;
  int execution_yesterday_ready;
  lql_stream_member_context *generic_member;
  lql_projection_capture *projection_capture;
  const lonejson_path_value_visitor *projection_visitor;
  lonejson *runtime;
  lonejson_spooled mutation_spool;
  lonejson_value_rewriter mutation_rewriter;
  lonejson_value_visitor mutation_visitor;
  void *mutation_visitor_user;
  lonejson_value_event_tape mutation_tape;
  lonejson_value_visitor mutation_tape_visitor;
  void *mutation_tape_visitor_user;
  int mutation_direct;
  int mutation_ready;
  int mutation_tape_ready;
  int mutation_deferred;
  int mutation_abandoned;
} lql_stream_state;

struct lql_stream_member_context {
  lql_stream_state *state;
  size_t field_index;
  size_t positions[sizeof(unsigned long) * CHAR_BIT];
  size_t container_depth;
  char number[128];
  size_t number_len;
  char temporal[128];
  size_t temporal_len;
  unsigned long active_terms;
  unsigned long active_number_terms;
  unsigned long active_temporal_terms;
  unsigned char path_containers[128];
  int active_string;
  int number_overflow;
  int temporal_overflow;
  unsigned long mismatches;
  int full_path;
  int root_object;
};

struct lql_mapped_projection_field {
  lql_stream_state *state;
  const lql_stream_field *field;
  lonejson_path_segment segments[sizeof(unsigned long) * CHAR_BIT];
};

#define LQL_STREAM_TERM_CAPACITY (sizeof(unsigned long) * CHAR_BIT)
#define LQL_STREAM_PATH_CAPACITY 128u
#define LQL_STREAM_PATH_OBJECT 1u
#define LQL_STREAM_PATH_ARRAY 2u

static int stream_field_root(const char *field, const char **key,
                             size_t *key_len) {
  const char *out;
  const char *end;
  if (field == NULL || field[0] != '/' || field[1] == '\0' ||
      strchr(field + 1, '~') != NULL) {
    return 0;
  }
  out = field + 1;
  end = strchr(out, '/');
  if (end == out ||
      (end != NULL && memchr(out, '*', (size_t)(end - out)) != NULL)) {
    return 0;
  }
  if (key != NULL) {
    *key = out;
  }
  if (key_len != NULL) {
    *key_len = end == NULL ? strlen(out) : (size_t)(end - out);
  }
  return 1;
}

static int stream_field_has_root_wildcard(const char *field) {
  const char *end;
  size_t len;
  if (field == NULL || field[0] != '/') {
    return 0;
  }
  end = strchr(field + 1, '/');
  len = end == NULL ? strlen(field + 1) : (size_t)(end - (field + 1));
  return (len == 1u && field[1] == '*') ||
         (len == 2u && memcmp(field + 1, "**", 2u) == 0) ||
         (len == 3u && memcmp(field + 1, "...", 3u) == 0);
}

static int stream_path_segment_matches(const lql_stream_member_context *member,
                                       const lonejson_value_path *path,
                                       size_t index, const char *pattern,
                                       size_t pattern_len) {
  if (index == path->segment_count) {
    return 0;
  }
  if (pattern_len == 2u && memcmp(pattern, "[]", 2u) == 0) {
    return index < LQL_STREAM_PATH_CAPACITY &&
           member->path_containers[index] == LQL_STREAM_PATH_ARRAY;
  }
  if (pattern_len == 1u && pattern[0] == '*') {
    return index < LQL_STREAM_PATH_CAPACITY &&
           member->path_containers[index] == LQL_STREAM_PATH_OBJECT;
  }
  if (pattern_len == 2u && memcmp(pattern, "**", 2u) == 0) {
    return 1;
  }
  return path->segments[index].len == pattern_len &&
         memcmp(path->segments[index].data, pattern, pattern_len) == 0;
}

static int stream_path_tail_matches(const lql_stream_member_context *member,
                                    const lonejson_value_path *path,
                                    const char *pattern, size_t index) {
  const char *end;
  const char *next;
  size_t len;
  size_t i;
  if (*pattern == '\0') {
    return index == path->segment_count;
  }
  end = strchr(pattern, '/');
  len = end == NULL ? strlen(pattern) : (size_t)(end - pattern);
  next = end == NULL ? pattern + len : end + 1;
  if (len == 3u && memcmp(pattern, "...", 3u) == 0) {
    for (i = index; i <= path->segment_count; ++i) {
      if (stream_path_tail_matches(member, path, next, i)) {
        return 1;
      }
    }
    return 0;
  }
  if (!stream_path_segment_matches(member, path, index, pattern, len)) {
    return 0;
  }
  return stream_path_tail_matches(member, path, next, index + 1u);
}

static int stream_term_path_matches(const lql_stream_term *term,
                                    const lonejson_value_path *path,
                                    const lql_stream_member_context *member) {
  const char *field;
  const char *segment;
  const char *end;
  field = term->selector->field;
  if (field == NULL || field[0] != '/') {
    return 0;
  }
  segment = field + 1;
  if (member->full_path) {
    return stream_path_tail_matches(member, path, segment, 0u);
  }
  end = strchr(segment, '/');
  if (end == NULL) {
    return path->segment_count == 0u;
  }
  segment = end + 1;
  return stream_path_tail_matches(member, path, segment, 0u);
}

static int stream_term_matches_selector(const lql_stream_program *program,
                                        const lql_selector *selector,
                                        unsigned long hits) {
  size_t i;
  int found;
  found = 0;
  for (i = 0u; i < program->term_count; ++i) {
    if (program->terms[i].selector == selector) {
      found = 1;
      if ((hits & program->terms[i].bit) != 0ul) {
        return 1;
      }
    }
  }
  return found ? 0 : -1;
}

static void stream_term_failure_table(lql_allocator *allocator,
                                      lql_stream_term *term) {
  size_t i;
  size_t matched;
  if (term->kind != LQL_SELECTOR_KIND_CONTAINS || term->value_len == 0u) {
    return;
  }
  term->failure = (size_t *)allocator->calloc(allocator, term->value_len,
                                              sizeof(*term->failure));
  if (term->failure == NULL) {
    return;
  }
  matched = 0u;
  for (i = 1u; i < term->value_len; ++i) {
    while (matched != 0u && term->value[i] != term->value[matched]) {
      matched = term->failure[matched - 1u];
    }
    if (term->value[i] == term->value[matched]) {
      ++matched;
    }
    term->failure[i] = matched;
  }
}

static lql_status stream_field_get(lql_stream_program *program, const char *key,
                                   size_t key_len, size_t *out,
                                   lql_error *error) {
  size_t i;
  for (i = 0u; i < program->field_count; ++i) {
    if (program->fields[i].key_len == key_len &&
        memcmp(program->fields[i].key, key, key_len) == 0) {
      *out = i;
      return LQL_STATUS_OK;
    }
  }
  if (program->field_count == LQL_STREAM_TERM_CAPACITY) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream selector has too many mapped fields");
    return LQL_STATUS_UNSUPPORTED;
  }
  i = program->field_count;
  program->fields[i].key = key;
  program->fields[i].key_len = key_len;
  ++program->field_count;
  *out = i;
  return LQL_STATUS_OK;
}

static lql_status stream_append_term(lql_stream_program *program,
                                     lql_allocator *allocator,
                                     const lql_selector *selector,
                                     lql_selector_kind kind, const char *key,
                                     size_t key_len, const char *value,
                                     size_t value_len, lql_error *error) {
  lql_stream_term *term;
  lql_status status;
  size_t field_index;
  if (program->term_count == LQL_STREAM_TERM_CAPACITY) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream selector has too many predicate terms");
    return LQL_STATUS_UNSUPPORTED;
  }
  status = stream_field_get(program, key, key_len, &field_index, error);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  term = &program->terms[program->term_count];
  term->selector = selector;
  term->kind = kind;
  term->field_index = field_index;
  term->value = value;
  term->value_len = value_len;
  term->bit = 1ul << program->term_count;
  if (kind == LQL_SELECTOR_KIND_CONTAINS && value_len != 0u) {
    stream_term_failure_table(allocator, term);
    if (term->failure == NULL) {
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      return LQL_STATUS_NO_MEMORY;
    }
  }
  ++program->term_count;
  return LQL_STATUS_OK;
}

static void stream_lonejson_error(lonejson_error *out, const char *message) {
  if (out == NULL) {
    return;
  }
  lonejson_error_init(out);
  out->code = LONEJSON_STATUS_CALLBACK_FAILED;
  if (message != NULL) {
    strncpy(out->message, message, sizeof(out->message) - 1u);
    out->message[sizeof(out->message) - 1u] = '\0';
  }
}

static void stream_fail(lql_stream_state *state, lql_status status,
                        const char *message) {
  if (state == NULL || state->failure.code != LQL_STATUS_OK) {
    return;
  }
  lql_set_error(&state->failure, status, message);
}

static lql_status stream_compile_term(lql_stream_program *program,
                                      lql_allocator *allocator,
                                      const lql_selector *selector,
                                      lql_error *error) {
  const char *key;
  const char *value;
  lql_selector_kind kind;
  lql_status status;
  size_t i;
  if (selector == NULL) {
    return LQL_STATUS_OK;
  }
  if (selector->kind == LQL_SELECTOR_KIND_AND ||
      selector->kind == LQL_SELECTOR_KIND_OR ||
      selector->kind == LQL_SELECTOR_KIND_NOT) {
    for (i = 0u; i < selector->child_count; ++i) {
      status = stream_compile_term(program, allocator, &selector->children[i],
                                   error);
      if (status != LQL_STATUS_OK) {
        return status;
      }
    }
    return LQL_STATUS_OK;
  }
  if (!stream_field_root(selector->field, &key, &i)) {
    if (!stream_field_has_root_wildcard(selector->field)) {
      lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                    "direct stream selector path is not implemented");
      return LQL_STATUS_UNSUPPORTED;
    }
    key = "";
    i = 0u;
    program->requires_generic_path = 1;
  }
  kind = selector->kind;
  if (strchr(selector->field + 1, '/') != NULL) {
    program->has_nested_paths = 1;
  }
  if (kind == LQL_SELECTOR_KIND_EQ) {
    if (selector->value == NULL) {
      lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                    "direct stream equality requires a string value");
      return LQL_STATUS_UNSUPPORTED;
    }
    return stream_append_term(program, allocator, selector, kind, key, i,
                              selector->value, strlen(selector->value), error);
  }
  if (kind == LQL_SELECTOR_KIND_IN) {
    if (selector->any_count == 0u) {
      lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                    "direct stream in selector requires values");
      return LQL_STATUS_UNSUPPORTED;
    }
    for (i = 0u; i < selector->any_count; ++i) {
      status = stream_append_term(
          program, allocator, selector, LQL_SELECTOR_KIND_EQ, key, strlen(key),
          selector->any[i], selector->any_lens[i], error);
      if (status != LQL_STATUS_OK) {
        return status;
      }
    }
    return LQL_STATUS_OK;
  }
  if (kind == LQL_SELECTOR_KIND_EXISTS) {
    return stream_append_term(program, allocator, selector, kind, key, i, NULL,
                              0u, error);
  }
  if (kind == LQL_SELECTOR_KIND_RANGE) {
    return stream_append_term(program, allocator, selector, kind, key, i, NULL,
                              0u, error);
  }
  if (kind == LQL_SELECTOR_KIND_DATE) {
    if (selector->since_macro != LQL_SINCE_NONE) {
      program->needs_execution_clock = 1;
    }
    return stream_append_term(program, allocator, selector, kind, key, i, NULL,
                              0u, error);
  }
  if (kind != LQL_SELECTOR_KIND_CONTAINS && kind != LQL_SELECTOR_KIND_PREFIX) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream selector predicate is not implemented");
    return LQL_STATUS_UNSUPPORTED;
  }
  if (selector->ignore_case) {
    lql_set_error(
        error, LQL_STATUS_UNSUPPORTED,
        "direct stream case-insensitive string predicates are not implemented");
    return LQL_STATUS_UNSUPPORTED;
  }
  if (!selector->value_set && selector->any_count == 0u &&
      (selector->value == NULL || selector->value[0] == '\0')) {
    lql_set_error(
        error, LQL_STATUS_UNSUPPORTED,
        "direct stream omitted string predicate values are not implemented");
    return LQL_STATUS_UNSUPPORTED;
  }
  if (selector->any_count != 0u) {
    if (kind != LQL_SELECTOR_KIND_CONTAINS) {
      lql_set_error(
          error, LQL_STATUS_UNSUPPORTED,
          "direct stream string predicate does not accept any values");
      return LQL_STATUS_UNSUPPORTED;
    }
    for (i = 0u; i < selector->any_count; ++i) {
      status = stream_append_term(program, allocator, selector, kind, key,
                                  strlen(key), selector->any[i],
                                  selector->any_lens[i], error);
      if (status != LQL_STATUS_OK) {
        return status;
      }
    }
    return LQL_STATUS_OK;
  }
  value = selector->value == NULL ? "" : selector->value;
  return stream_append_term(program, allocator, selector, kind, key,
                            strlen(key), value, strlen(value), error);
}

static int stream_selector_matches(const lql_stream_program *program,
                                   const lql_selector *selector,
                                   unsigned long hits) {
  size_t i;
  int matched;
  if (selector == NULL || selector->kind == LQL_SELECTOR_KIND_ALL) {
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_AND) {
    for (i = 0u; i < selector->child_count; ++i) {
      if (!stream_selector_matches(program, &selector->children[i], hits)) {
        return 0;
      }
    }
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_OR) {
    for (i = 0u; i < selector->child_count; ++i) {
      if (stream_selector_matches(program, &selector->children[i], hits)) {
        return 1;
      }
    }
    return 0;
  }
  if (selector->kind == LQL_SELECTOR_KIND_NOT) {
    return selector->child_count == 1u &&
           !stream_selector_matches(program, &selector->children[0], hits);
  }
  matched = stream_term_matches_selector(program, selector, hits);
  return matched > 0;
}

static int stream_temporal_bounds_match(const lql_selector *selector,
                                        const lql_temporal *candidate) {
  if (selector->has_temporal_gt &&
      lql_temporal_compare(candidate, &selector->temporal_gt) <= 0) {
    return 0;
  }
  if (selector->has_temporal_gte &&
      lql_temporal_compare(candidate, &selector->temporal_gte) < 0) {
    return 0;
  }
  if (selector->has_temporal_lt &&
      lql_temporal_compare(candidate, &selector->temporal_lt) >= 0) {
    return 0;
  }
  if (selector->has_temporal_lte &&
      lql_temporal_compare(candidate, &selector->temporal_lte) > 0) {
    return 0;
  }
  return 1;
}

static int stream_date_matches(const lql_stream_state *state,
                               const lql_selector *selector,
                               const lql_temporal *candidate) {
  const lql_temporal *since;
  since = NULL;
  if (selector->has_temporal_eq &&
      !lql_temporal_equal(candidate, &selector->temporal_eq)) {
    return 0;
  }
  if (selector->since_macro == LQL_SINCE_NOW) {
    since = state->execution_now_ready ? &state->execution_now : NULL;
  } else if (selector->since_macro == LQL_SINCE_TODAY) {
    since = state->execution_today_ready ? &state->execution_today : NULL;
  } else if (selector->since_macro == LQL_SINCE_YESTERDAY) {
    since =
        state->execution_yesterday_ready ? &state->execution_yesterday : NULL;
  }
  if (selector->since_macro != LQL_SINCE_NONE &&
      (since == NULL || lql_temporal_compare(candidate, since) < 0)) {
    return 0;
  }
  return stream_temporal_bounds_match(selector, candidate);
}

static lql_status stream_program_get(lql *self, const lql_selector *selector,
                                     const lql_stream_program **out,
                                     lql_error *error) {
  lql_selector *mutable_selector;
  lql_stream_program *program;
  lql_allocator *allocator;
  lql_status status;
  size_t i;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "stream program out required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = NULL;
  if (selector == NULL || selector->kind == LQL_SELECTOR_KIND_ALL) {
    return LQL_STATUS_OK;
  }
  mutable_selector = (lql_selector *)selector;
  if (mutable_selector->stream_program != NULL) {
    *out = mutable_selector->stream_program;
    return LQL_STATUS_OK;
  }
  allocator = lql_allocator_from_receiver(self);
  program =
      (lql_stream_program *)allocator->calloc(allocator, 1u, sizeof(*program));
  if (program == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  status = stream_compile_term(program, allocator, selector, error);
  if (status != LQL_STATUS_OK) {
    for (i = 0u; i < program->term_count; ++i) {
      allocator->destroy(allocator, program->terms[i].failure);
    }
    allocator->destroy(allocator, program);
    return status;
  }
  if (program->term_count == 0u) {
    program->match_all = 1;
  } else {
    for (i = 0u; i < program->field_count; ++i) {
      lonejson_field *field = &program->mapped_fields[i];
      const lql_stream_field *stream_field = &program->fields[i];
      memset(field, 0, sizeof(*field));
      field->json_key = stream_field->key;
      field->json_key_len = stream_field->key_len;
      field->json_key_first = stream_field->key_len == 0u
                                  ? 0u
                                  : (unsigned char)stream_field->key[0];
      field->json_key_last =
          stream_field->key_len == 0u
              ? 0u
              : (unsigned char)stream_field->key[stream_field->key_len - 1u];
      field->struct_offset = i * sizeof(lonejson_json_value);
      field->kind = LONEJSON_FIELD_KIND_JSON_VALUE;
      field->storage = LONEJSON_STORAGE_FIXED;
      field->overflow_policy = LONEJSON_OVERFLOW_FAIL;
      field->spool_class = LONEJSON_SPOOL_CLASS_DEFAULT;
    }
    memset(&program->mapped_map, 0, sizeof(program->mapped_map));
    program->mapped_map.name = "lql_stream_program";
    program->mapped_map.struct_size =
        program->field_count * sizeof(lonejson_json_value);
    program->mapped_map.fields = program->mapped_fields;
    program->mapped_map.field_count = program->field_count;
  }
  mutable_selector->stream_program = program;
  *out = program;
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL void lql_stream_program_destroy(lql *self,
                                                    lql_selector *selector) {
  lql_allocator *allocator;
  if (self == NULL || selector == NULL || selector->stream_program == NULL) {
    return;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator != NULL) {
    size_t i;
    for (i = 0u; i < selector->stream_program->term_count; ++i) {
      allocator->destroy(allocator, selector->stream_program->terms[i].failure);
    }
    allocator->destroy(allocator, selector->stream_program);
  }
  selector->stream_program = NULL;
}

static lonejson_read_result stream_read(void *user, unsigned char *buffer,
                                        size_t capacity) {
  lql_stream_state *state;
  lonejson_read_result out;
  size_t len;
  lql_status status;
  state = (lql_stream_state *)user;
  out = lonejson_default_read_result();
  if (state == NULL || state->request == NULL ||
      state->request->reader == NULL) {
    out.error_code = 1;
    return out;
  }
  len = 0u;
  status = state->request->reader(state->request->reader_user, buffer, capacity,
                                  &len, &state->failure);
  if (status != LQL_STATUS_OK) {
    if (state->failure.code == LQL_STATUS_OK) {
      stream_fail(state, status, "stream reader failed");
    }
    out.error_code = 1;
    return out;
  }
  if (len > capacity || len > (size_t)-1 - state->result->bytes_consumed) {
    stream_fail(state, LQL_STATUS_CALLBACK_ERROR,
                "stream reader returned an invalid byte count");
    out.error_code = 1;
    return out;
  }
  out.bytes_read = len;
  out.eof = len == 0u;
  state->result->bytes_consumed += len;
  return out;
}

static lonejson_status stream_payload_sink(void *user, const void *data,
                                           size_t len, lonejson_error *error) {
  lql_stream_state *state;
  lql_status status;
  state = (lql_stream_state *)user;
  status = state->request->writer(state->request->writer_user, data, len,
                                  &state->failure);
  if (status != LQL_STATUS_OK) {
    if (state->failure.code == LQL_STATUS_OK) {
      stream_fail(state, status, "stream writer failed");
    }
    stream_lonejson_error(error, state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
stream_emit_selected(lql_stream_state *state,
                     const lonejson_candidate_info *candidate,
                     lonejson_error *error) {
  lonejson_status status;
  static const char newline[] = "\n";
  if (candidate == NULL || candidate->payload_spool == NULL) {
    stream_fail(state, LQL_STATUS_CALLBACK_ERROR,
                "selected record capture is unavailable");
    stream_lonejson_error(error, state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  status = lonejson_spooled_write_to_sink(candidate->payload_spool,
                                          stream_payload_sink, state, error);
  if (status != LONEJSON_STATUS_OK) {
    if (state->failure.code == LQL_STATUS_OK) {
      stream_fail(state, LQL_STATUS_CALLBACK_ERROR,
                  "selected record write failed");
      stream_lonejson_error(error, state->failure.message);
    }
    return status;
  }
  return stream_payload_sink(state, newline, 1u, error);
}

static lonejson_status
stream_emit_projection(lql_stream_state *state,
                       const lonejson_candidate_info *candidate,
                       lonejson_error *error) {
  lql_status status;
  int emitted;
  static const char newline[] = "\n";
  if (state->projection_capture != NULL) {
    emitted = 0;
    status = lql_projection_capture_render(state->projection_capture,
                                           stream_payload_sink, state, &emitted,
                                           &state->failure);
    if (status != LQL_STATUS_OK) {
      if (state->failure.code == LQL_STATUS_OK) {
        stream_fail(state, status, "projection rendering failed");
      }
      stream_lonejson_error(error, state->failure.message);
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    return emitted ? stream_payload_sink(state, newline, 1u, error)
                   : LONEJSON_STATUS_OK;
  }
  if (candidate == NULL || candidate->payload_spool == NULL) {
    stream_fail(state, LQL_STATUS_CALLBACK_ERROR,
                "projection record capture is unavailable");
    stream_lonejson_error(error, state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  emitted = 0;
  status = lql_projection_render_top_level(
      state->receiver, state->request->projection, candidate->payload_spool,
      stream_payload_sink, state, &emitted, &state->failure);
  if (status != LQL_STATUS_OK) {
    if (state->failure.code == LQL_STATUS_OK) {
      stream_fail(state, status, "projection rendering failed");
    }
    stream_lonejson_error(error, state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (!emitted) {
    return LONEJSON_STATUS_OK;
  }
  return stream_payload_sink(state, newline, 1u, error);
}

static lonejson_status
stream_emit_mutation(lql_stream_state *state,
                     const lonejson_candidate_info *candidate,
                     lonejson_error *error) {
  lql_status status;
  static const char newline[] = "\n";
  if (state->mutation_ready) {
    lonejson_status lonejson_status;
    lonejson_status = lonejson_spooled_write_to_sink(
        &state->mutation_spool, stream_payload_sink, state, error);
    if (lonejson_status != LONEJSON_STATUS_OK) {
      if (state->failure.code == LQL_STATUS_OK) {
        stream_fail(state, LQL_STATUS_CALLBACK_ERROR,
                    "direct mutation output write failed");
      }
      return lonejson_status;
    }
    return stream_payload_sink(state, newline, 1u, error);
  }
  if (candidate == NULL || candidate->payload_spool == NULL) {
    stream_fail(state, LQL_STATUS_CALLBACK_ERROR,
                "mutation record capture is unavailable");
    stream_lonejson_error(error, state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  status = lql_mutation_render(state->receiver, state->request->mutation,
                               candidate->payload_spool, stream_payload_sink,
                               state, &state->failure);
  if (status != LQL_STATUS_OK) {
    if (state->failure.code == LQL_STATUS_OK) {
      stream_fail(state, status, "mutation rendering failed");
    }
    stream_lonejson_error(error, state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return stream_payload_sink(state, newline, 1u, error);
}

static lonejson_status stream_projection_spool_sink(void *user,
                                                    const void *data,
                                                    size_t len,
                                                    lonejson_error *error) {
  return lonejson_spooled_append((lonejson_spooled *)user, data, len, error);
}

static int stream_mutation_direct_eligible(const lql_mutation *mutation) {
  return mutation != NULL && mutation->action_count == 1u;
}

static lonejson_status
stream_mutation_emit_value(lonejson_writer *writer, void *user,
                           lonejson_error *error) {
  const lql_mutation_action *action;
  action = (const lql_mutation_action *)user;
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
stream_mutation_emit_increment(
    lonejson_writer *writer, const lonejson_value_rewrite_old_value *old_value,
    void *user, lonejson_error *error) {
  const lql_mutation_action *action;
  char *end;
  double current;
  double next;
  action = (const lql_mutation_action *)user;
  if (!old_value->present) {
    return lonejson_writer_f64(writer, action->delta, error);
  }
  if (old_value->type != LONEJSON_VALUE_NUMBER) {
    if (error != NULL) {
      error->code = LONEJSON_STATUS_TYPE_MISMATCH;
      strcpy(error->message, "increment target is not numeric");
    }
    return LONEJSON_STATUS_TYPE_MISMATCH;
  }
  errno = 0;
  current = strtod(old_value->number, &end);
  if (end != old_value->number + old_value->number_len || errno == ERANGE) {
    if (error != NULL) {
      error->code = LONEJSON_STATUS_TYPE_MISMATCH;
      strcpy(error->message, "increment target number is invalid");
    }
    return LONEJSON_STATUS_TYPE_MISMATCH;
  }
  next = current + action->delta;
  if (next != next || next == HUGE_VAL || next == -HUGE_VAL) {
    if (error != NULL) {
      error->code = LONEJSON_STATUS_OVERFLOW;
      strcpy(error->message, "increment result is not finite");
    }
    return LONEJSON_STATUS_OVERFLOW;
  }
  return lonejson_writer_f64(writer, next, error);
}

static lonejson_status
stream_mutation_rewriter_open(lql_stream_state *state, lonejson_error *error) {
  const lql_mutation_action *action;
  lonejson_value_rewrite_options options;
  if (state->request->mutation == NULL ||
      state->request->mutation->action_count != 1u) {
    return LONEJSON_STATUS_INVALID_ARGUMENT;
  }
  action = &state->request->mutation->actions[0];
  memset(&options, 0, sizeof(options));
  options.target_segments = (const char *const *)action->segments;
  options.target_segment_count = action->segment_count;
  if (action->kind == LQL_MUTATION_REMOVE) {
    options.action = LONEJSON_VALUE_REWRITE_DROP;
  } else if (action->kind == LQL_MUTATION_INCREMENT) {
    options.action = LONEJSON_VALUE_REWRITE_REPLACE_WITH;
    options.replace = stream_mutation_emit_increment;
    options.replace_user = (void *)action;
  } else {
    options.action = LONEJSON_VALUE_REWRITE_REPLACE;
    options.replacement.emit = stream_mutation_emit_value;
    options.replacement.emit_user = (void *)action;
  }
  return lonejson_value_rewriter_open(
      &state->mutation_rewriter, state->runtime, stream_projection_spool_sink,
      &state->mutation_spool, &options, &state->mutation_visitor,
      &state->mutation_visitor_user, error);
}

static lonejson_status
stream_mutation_tape_flush(lql_stream_state *state, lonejson_error *error) {
  lonejson_status status;
  if (!state->mutation_tape_ready) {
    return LONEJSON_STATUS_OK;
  }
  status = stream_mutation_rewriter_open(state, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  state->mutation_ready = 1;
  status = lonejson_value_event_tape_replay(
      &state->mutation_tape, &state->mutation_visitor,
      state->mutation_visitor_user, error);
  lonejson_value_event_tape_reset(&state->mutation_tape);
  state->mutation_tape_ready = 0;
  if (status != LONEJSON_STATUS_OK) {
    lonejson_value_rewriter_cleanup(&state->mutation_rewriter);
    state->mutation_ready = 0;
  }
  return status;
}

static lonejson_status
stream_emit_projection_then_mutation(lql_stream_state *state,
                                     const lonejson_candidate_info *candidate,
                                     lonejson_error *error) {
  lonejson *runtime;
  lonejson_error local_error;
  lonejson_spooled projection;
  lql_status status;
  int emitted;
  static const char newline[] = "\n";

  if (candidate == NULL || candidate->payload_spool == NULL) {
    stream_fail(state, LQL_STATUS_CALLBACK_ERROR,
                "combined output capture is unavailable");
    stream_lonejson_error(error, state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  lonejson_error_init(&local_error);
  runtime = lql_lonejson_new(state->receiver, &local_error);
  if (runtime == NULL) {
    stream_fail(state, LQL_STATUS_NO_MEMORY,
                "combined output allocation failed");
    stream_lonejson_error(error, state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  lonejson_spooled_init_class(runtime, &projection,
                              LONEJSON_SPOOL_CLASS_LARGE_TEXT);
  emitted = 0;
  status = lql_projection_render_top_level(
      state->receiver, state->request->projection, candidate->payload_spool,
      stream_projection_spool_sink, &projection, &emitted, &state->failure);
  if (status == LQL_STATUS_OK && emitted) {
    status = lql_mutation_render(state->receiver, state->request->mutation,
                                 &projection, stream_payload_sink, state,
                                 &state->failure);
  }
  lonejson_spooled_cleanup(&projection);
  lonejson_free(runtime);
  if (status != LQL_STATUS_OK) {
    if (state->failure.code == LQL_STATUS_OK) {
      stream_fail(state, status, "combined output rendering failed");
    }
    stream_lonejson_error(error, state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (!emitted) {
    return LONEJSON_STATUS_OK;
  }
  return stream_payload_sink(state, newline, 1u, error);
}

static lonejson_candidate_callback_result
stream_candidate_begin(void *user, const lonejson_candidate_info *candidate,
                       lonejson_error *error) {
  lql_stream_state *state;
  lonejson_status status;
  (void)candidate;
  state = (lql_stream_state *)user;
  if (state->request->limits.max_records != 0u &&
      state->result->records_seen >= state->request->limits.max_records) {
    state->result->stopped_early = 1;
    state->result->stop_reason = LQL_STREAM_STOP_RECORD_LIMIT;
    return LONEJSON_CANDIDATE_STOP;
  }
  state->hits = 0ul;
  state->object_depth = 0u;
  state->array_depth = 0u;
  state->key_pos = 0u;
  state->key_candidates = 0ul;
  state->current_term = (size_t)-1;
  state->active = 0;
  state->key_active = 0;
  state->active_mismatch = 0;
  state->active_pos = 0u;
  state->root_value_started = 0;
  if (state->projection_capture != NULL) {
    lql_projection_capture_reset(state->projection_capture);
  }
  if (state->mutation_direct) {
    lonejson_spooled_reset(&state->mutation_spool);
    state->mutation_ready = 0;
    state->mutation_tape_ready = 0;
    state->mutation_abandoned = 0;
    if (state->mutation_deferred) {
      status = lonejson_value_event_tape_open(
          &state->mutation_tape, state->runtime,
          LQL_STREAM_MUTATION_TAPE_MAX_BYTES, &state->mutation_tape_visitor,
          &state->mutation_tape_visitor_user, error);
      if (status == LONEJSON_STATUS_OK) {
        state->mutation_tape_ready = 1;
      }
    } else {
      status = stream_mutation_rewriter_open(state, error);
      if (status == LONEJSON_STATUS_OK) {
        state->mutation_ready = 1;
      }
    }
    if (status != LONEJSON_STATUS_OK) {
      lonejson_value_rewriter_cleanup(&state->mutation_rewriter);
      stream_fail(state, status == LONEJSON_STATUS_ALLOCATION_FAILED
                             ? LQL_STATUS_NO_MEMORY
                             : LQL_STATUS_JSON_ERROR,
                  error != NULL && error->message[0] != '\0'
                      ? error->message
                      : "direct mutation initialization failed");
      return LONEJSON_CANDIDATE_ERROR;
    }
  }
  if (state->generic_member != NULL) {
    lql_stream_member_context *member;
    member = state->generic_member;
    memset(member, 0, sizeof(*member));
    member->state = state;
    member->full_path = 1;
  }
  ++state->result->records_seen;
  (void)error;
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lonejson_candidate_callback_result
stream_candidate_end(void *user, const lonejson_candidate_info *candidate,
                     lonejson_error *error) {
  lql_stream_state *state;
  lql_stream_decision decision;
  lql_stream_callback_result callback_result;
  int matched;
  state = (lql_stream_state *)user;
  if (state->mutation_ready) {
    lonejson_status status;
    status = lonejson_value_rewriter_close(&state->mutation_rewriter, error);
    if (status != LONEJSON_STATUS_OK) {
      state->mutation_ready = 0;
      stream_fail(state, status == LONEJSON_STATUS_ALLOCATION_FAILED
                             ? LQL_STATUS_NO_MEMORY
                             : LQL_STATUS_JSON_ERROR,
                  error != NULL && error->message[0] != '\0'
                      ? error->message
                      : "direct mutation finalization failed");
      return LONEJSON_CANDIDATE_ERROR;
    }
  }
  if (state->mutation_tape_ready) {
    lonejson_value_event_tape_reset(&state->mutation_tape);
    state->mutation_tape_ready = 0;
  }
  matched = state->program == NULL || state->program->match_all ||
            stream_selector_matches(state->program, state->request->selector,
                                    state->hits);
  if (matched) {
    ++state->result->records_matched;
  }
  if (state->request->on_decision != NULL) {
    decision.record_index = state->result->records_seen - 1u;
    decision.matched = matched;
    callback_result = state->request->on_decision(state->request->decision_user,
                                                  &decision, &state->failure);
    if (callback_result == LQL_STREAM_CALLBACK_ERROR) {
      if (state->failure.code == LQL_STATUS_OK) {
        stream_fail(state, LQL_STATUS_CALLBACK_ERROR,
                    "stream decision callback failed");
      }
      stream_lonejson_error(error, state->failure.message);
      return LONEJSON_CANDIDATE_ERROR;
    }
    if (callback_result == LQL_STREAM_CALLBACK_STOP) {
      state->result->stopped_early = 1;
      state->result->stop_reason = LQL_STREAM_STOP_CALLBACK;
      return LONEJSON_CANDIDATE_STOP;
    }
  }
  if (state->request->output_mode == LQL_STREAM_OUTPUT_SELECTED_RECORD &&
      (matched || !state->request->matched_only)) {
    if (stream_emit_selected(state, candidate, error) != LONEJSON_STATUS_OK) {
      return LONEJSON_CANDIDATE_ERROR;
    }
  }
  if (state->request->output_mode == LQL_STREAM_OUTPUT_PROJECTION && matched) {
    if (stream_emit_projection(state, candidate, error) != LONEJSON_STATUS_OK) {
      return LONEJSON_CANDIDATE_ERROR;
    }
  }
  if (state->request->output_mode == LQL_STREAM_OUTPUT_MUTATION) {
    if (matched) {
      if (stream_emit_mutation(state, candidate, error) != LONEJSON_STATUS_OK) {
        return LONEJSON_CANDIDATE_ERROR;
      }
    } else if (!state->request->matched_only &&
               stream_emit_selected(state, candidate, error) !=
                   LONEJSON_STATUS_OK) {
      return LONEJSON_CANDIDATE_ERROR;
    }
  }
  if (state->request->output_mode ==
      LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION) {
    if (matched) {
      if (stream_emit_projection_then_mutation(state, candidate, error) !=
          LONEJSON_STATUS_OK) {
        return LONEJSON_CANDIDATE_ERROR;
      }
    } else if (!state->request->matched_only &&
               stream_emit_selected(state, candidate, error) !=
                   LONEJSON_STATUS_OK) {
      return LONEJSON_CANDIDATE_ERROR;
    }
  }
  if (matched && state->request->limits.max_matches != 0u &&
      state->result->records_matched >= state->request->limits.max_matches) {
    state->result->stopped_early = 1;
    state->result->stop_reason = LQL_STREAM_STOP_MATCH_LIMIT;
    return LONEJSON_CANDIDATE_STOP;
  }
  return LONEJSON_CANDIDATE_CONTINUE;
}

static void stream_clear_current(lql_stream_state *state) {
  state->current_term = (size_t)-1;
}

static void stream_finish_active(lql_stream_state *state) {
  const lql_stream_term *term;
  if (!state->active) {
    return;
  }
  term = &state->program->terms[state->active_term];
  if (!state->active_mismatch && state->active_pos == term->value_len) {
    state->hits |= term->bit;
  }
  state->active = 0;
}

static void stream_finish_key(lql_stream_state *state) {
  size_t i;
  if (!state->key_active) {
    return;
  }
  for (i = 0u; state->program != NULL && i < state->program->field_count; ++i) {
    const lql_stream_field *field = &state->program->fields[i];
    if ((state->key_candidates & (1ul << i)) != 0ul &&
        state->key_pos == field->key_len) {
      state->current_term = i;
      break;
    }
  }
  state->key_active = 0;
}

static lonejson_status stream_key_end(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  stream_finish_key(state);
  if (state->object_depth == 1u && state->array_depth == 0u &&
      state->current_term == (size_t)-1) {
    return LONEJSON_STATUS_SKIP_VALUE;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_object_begin(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  if (state->object_depth == 1u && state->array_depth == 0u) {
    stream_finish_key(state);
    stream_finish_active(state);
    stream_clear_current(state);
  }
  ++state->object_depth;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_match_all_object_begin(void *user,
                                                     lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  state->root_value_started = 1;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_match_all_array_begin(void *user,
                                                    lonejson_error *error) {
  lql_stream_state *state;
  state = (lql_stream_state *)user;
  if (!state->root_value_started) {
    stream_fail(state, LQL_STATUS_JSON_ERROR,
                "root JSON arrays are not valid NDJSON records");
    stream_lonejson_error(error, state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_object_end(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  if (state->object_depth == 1u && state->array_depth == 0u) {
    stream_finish_key(state);
    stream_finish_active(state);
  }
  if (state->object_depth != 0u) {
    --state->object_depth;
  }
  stream_clear_current(state);
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_array_begin(void *user, lonejson_error *error) {
  lql_stream_state *state;
  state = (lql_stream_state *)user;
  if (state->object_depth == 0u && state->array_depth == 0u) {
    stream_fail(state, LQL_STATUS_JSON_ERROR,
                "root JSON arrays are not valid NDJSON records");
    stream_lonejson_error(error, state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (state->object_depth == 1u && state->array_depth == 0u) {
    stream_finish_key(state);
    stream_finish_active(state);
    stream_clear_current(state);
  }
  ++state->array_depth;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_array_end(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  if (state->array_depth != 0u) {
    --state->array_depth;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_key_begin(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  stream_finish_active(state);
  stream_clear_current(state);
  state->key_active = state->object_depth == 1u && state->array_depth == 0u;
  state->key_pos = 0u;
  if (state->program == NULL) {
    state->key_candidates = 0ul;
  } else if (state->program->field_count == LQL_STREAM_TERM_CAPACITY) {
    state->key_candidates = ~0ul;
  } else {
    state->key_candidates = (1ul << state->program->field_count) - 1ul;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_key_chunk(void *user, const char *data,
                                        size_t len, lonejson_error *error) {
  lql_stream_state *state;
  size_t i;
  (void)error;
  state = (lql_stream_state *)user;
  if (!state->key_active) {
    return LONEJSON_STATUS_OK;
  }
  for (i = 0u; state->program != NULL && i < state->program->field_count; ++i) {
    const lql_stream_field *field = &state->program->fields[i];
    if ((state->key_candidates & (1ul << i)) != 0ul &&
        (state->key_pos > field->key_len ||
         len > field->key_len - state->key_pos ||
         memcmp(data, field->key + state->key_pos, len) != 0)) {
      state->key_candidates &= ~(1ul << i);
    }
  }
  state->key_pos += len;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_string_begin(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  stream_finish_key(state);
  state->active = 0;
  if (state->current_term != (size_t)-1 && state->object_depth == 1u &&
      state->array_depth == 0u) {
    state->active_term = state->current_term;
    state->active_pos = 0u;
    state->active_mismatch = 0;
    state->active = 1;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_string_chunk(void *user, const char *data,
                                           size_t len, lonejson_error *error) {
  lql_stream_state *state;
  const lql_stream_term *term;
  (void)error;
  state = (lql_stream_state *)user;
  if (!state->active) {
    return LONEJSON_STATUS_OK;
  }
  term = &state->program->terms[state->active_term];
  if (!state->active_mismatch &&
      (state->active_pos > term->value_len ||
       len > term->value_len - state->active_pos ||
       memcmp(data, term->value + state->active_pos, len) != 0)) {
    state->active_mismatch = 1;
  }
  state->active_pos += len;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_scalar_value(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  stream_finish_key(state);
  stream_finish_active(state);
  stream_clear_current(state);
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_boolean_value(void *user, int value,
                                            lonejson_error *error) {
  (void)value;
  return stream_scalar_value(user, error);
}

static lonejson_status
stream_mapped_path_object_begin(void *user, const lonejson_value_path *path,
                                lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  (void)error;
  member = (lql_stream_member_context *)user;
  if (member->full_path && path->segment_count == 0u) {
    member->root_object = 1;
  }
  if (path->segment_count < LQL_STREAM_PATH_CAPACITY) {
    member->path_containers[path->segment_count] = LQL_STREAM_PATH_OBJECT;
  }
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    if ((member->full_path || term->field_index == member->field_index) &&
        term->kind == LQL_SELECTOR_KIND_EXISTS &&
        stream_term_path_matches(term, path, member)) {
      member->state->hits |= term->bit;
    }
  }
  ++member->container_depth;
  return LONEJSON_STATUS_OK;
}

static lonejson_status
stream_mapped_path_object_end(void *user, const lonejson_value_path *path,
                              lonejson_error *error) {
  lql_stream_member_context *member;
  (void)path;
  (void)error;
  member = (lql_stream_member_context *)user;
  if (path->segment_count < LQL_STREAM_PATH_CAPACITY) {
    member->path_containers[path->segment_count] = 0u;
  }
  if (member->container_depth != 0u) {
    --member->container_depth;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
stream_mapped_path_array_begin(void *user, const lonejson_value_path *path,
                               lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  member = (lql_stream_member_context *)user;
  if (member->full_path && path->segment_count == 0u) {
    stream_fail(member->state, LQL_STATUS_JSON_ERROR,
                "root JSON arrays are not valid NDJSON records");
    stream_lonejson_error(error, member->state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (path->segment_count < LQL_STREAM_PATH_CAPACITY) {
    member->path_containers[path->segment_count] = LQL_STREAM_PATH_ARRAY;
  }
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    if ((member->full_path || term->field_index == member->field_index) &&
        term->kind == LQL_SELECTOR_KIND_EXISTS &&
        stream_term_path_matches(term, path, member)) {
      member->state->hits |= term->bit;
    }
  }
  ++member->container_depth;
  return LONEJSON_STATUS_OK;
}

static lonejson_status
stream_mapped_path_array_end(void *user, const lonejson_value_path *path,
                             lonejson_error *error) {
  return stream_mapped_path_object_end(user, path, error);
}

static lonejson_status
stream_mapped_path_string_begin(void *user, const lonejson_value_path *path,
                                lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  (void)error;
  member = (lql_stream_member_context *)user;
  if (member->full_path && path->segment_count == 0u && !member->root_object) {
    member->active_string = 0;
    member->active_terms = 0ul;
    return LONEJSON_STATUS_OK;
  }
  member->active_string = 1;
  member->active_terms = 0ul;
  member->active_temporal_terms = 0ul;
  member->mismatches = 0ul;
  member->temporal_len = 0u;
  member->temporal_overflow = 0;
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    if ((!member->full_path && term->field_index != member->field_index) ||
        !stream_term_path_matches(term, path, member)) {
      continue;
    }
    member->active_terms |= term->bit;
    if (term->kind == LQL_SELECTOR_KIND_DATE ||
        term->kind == LQL_SELECTOR_KIND_RANGE ||
        (term->kind == LQL_SELECTOR_KIND_EQ &&
         term->selector->value_is_temporal)) {
      member->active_temporal_terms |= term->bit;
    }
    member->positions[i] = 0u;
    if (term->kind == LQL_SELECTOR_KIND_EXISTS) {
      member->state->hits |= term->bit;
    } else if (term->kind == LQL_SELECTOR_KIND_CONTAINS &&
               term->value_len == 0u) {
      member->state->hits |= term->bit;
    }
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
stream_mapped_path_string_chunk(void *user, const lonejson_value_path *path,
                                const char *data, size_t len,
                                lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  size_t pos;
  size_t offset;
  (void)error;
  member = (lql_stream_member_context *)user;
  (void)path;
  if (!member->active_string) {
    return LONEJSON_STATUS_OK;
  }
  if (member->active_temporal_terms != 0ul) {
    if (len > sizeof(member->temporal) - 1u - member->temporal_len) {
      member->temporal_overflow = 1;
    } else {
      memcpy(member->temporal + member->temporal_len, data, len);
      member->temporal_len += len;
    }
  }
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    if ((member->active_terms & term->bit) == 0ul ||
        (member->state->hits & term->bit) != 0ul ||
        term->kind == LQL_SELECTOR_KIND_EXISTS) {
      continue;
    }
    if (term->kind == LQL_SELECTOR_KIND_EQ ||
        term->kind == LQL_SELECTOR_KIND_PREFIX) {
      pos = member->positions[i];
      if ((member->mismatches & term->bit) == 0ul) {
        if (term->kind == LQL_SELECTOR_KIND_EQ &&
            (pos > term->value_len || len > term->value_len - pos ||
             memcmp(data, term->value + pos, len) != 0)) {
          member->mismatches |= term->bit;
        }
        if (term->kind == LQL_SELECTOR_KIND_PREFIX && pos < term->value_len) {
          size_t compare_len;
          compare_len = term->value_len - pos;
          if (compare_len > len) {
            compare_len = len;
          }
          if (memcmp(data, term->value + pos, compare_len) != 0) {
            member->mismatches |= term->bit;
          }
        }
      }
      member->positions[i] = pos + len;
      continue;
    }
    if (term->kind == LQL_SELECTOR_KIND_CONTAINS) {
      pos = member->positions[i];
      for (offset = 0u; offset < len; ++offset) {
        while (pos != 0u && data[offset] != term->value[pos]) {
          pos = term->failure[pos - 1u];
        }
        if (data[offset] == term->value[pos]) {
          ++pos;
        }
        if (pos == term->value_len) {
          member->state->hits |= term->bit;
          pos = 0u;
          break;
        }
      }
      member->positions[i] = pos;
    }
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
stream_mapped_path_string_end(void *user, const lonejson_value_path *path,
                              lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  (void)error;
  member = (lql_stream_member_context *)user;
  if (member->full_path && path->segment_count == 0u && !member->root_object) {
    return LONEJSON_STATUS_OK;
  }
  (void)path;
  if (!member->active_string) {
    return LONEJSON_STATUS_OK;
  }
  if (member->active_temporal_terms != 0ul && !member->temporal_overflow) {
    lql_temporal candidate;
    member->temporal[member->temporal_len] = '\0';
    if (lql_parse_temporal_literal(member->temporal, &candidate)) {
      for (i = 0u; i < member->state->program->term_count; ++i) {
        const lql_stream_term *term = &member->state->program->terms[i];
        const lql_selector *selector;
        if ((member->active_temporal_terms & term->bit) == 0ul) {
          continue;
        }
        selector = term->selector;
        if ((term->kind == LQL_SELECTOR_KIND_EQ &&
             selector->value_is_temporal &&
             lql_temporal_equal(&candidate, &selector->temporal_eq)) ||
            (term->kind == LQL_SELECTOR_KIND_RANGE &&
             selector->range_is_temporal &&
             stream_temporal_bounds_match(selector, &candidate)) ||
            (term->kind == LQL_SELECTOR_KIND_DATE &&
             stream_date_matches(member->state, selector, &candidate))) {
          member->state->hits |= term->bit;
        }
      }
    }
  }
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    if ((member->active_terms & term->bit) == 0ul ||
        (member->state->hits & term->bit) != 0ul) {
      continue;
    }
    if (term->kind == LQL_SELECTOR_KIND_EQ &&
        (member->mismatches & term->bit) == 0ul &&
        member->positions[i] == term->value_len) {
      member->state->hits |= term->bit;
    }
    if (term->kind == LQL_SELECTOR_KIND_PREFIX &&
        (member->mismatches & term->bit) == 0ul &&
        member->positions[i] >= term->value_len) {
      member->state->hits |= term->bit;
    }
  }
  member->active_string = 0;
  member->active_terms = 0ul;
  member->active_temporal_terms = 0ul;
  return LONEJSON_STATUS_OK;
}

static lonejson_status
stream_mapped_path_present(void *user, const lonejson_value_path *path,
                           lonejson_error *error);

static lonejson_status
stream_mapped_path_number_begin(void *user, const lonejson_value_path *path,
                                lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  lonejson_status status;
  status = stream_mapped_path_present(user, path, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  member = (lql_stream_member_context *)user;
  if (member->full_path && path->segment_count == 0u && !member->root_object) {
    member->active_number_terms = 0ul;
    return LONEJSON_STATUS_OK;
  }
  member->number_len = 0u;
  member->number_overflow = 0;
  member->active_number_terms = 0ul;
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    if ((member->full_path || term->field_index == member->field_index) &&
        (term->kind == LQL_SELECTOR_KIND_RANGE ||
         (term->kind == LQL_SELECTOR_KIND_EQ &&
          !term->selector->value_is_temporal)) &&
        stream_term_path_matches(term, path, member)) {
      member->active_number_terms |= term->bit;
    }
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
stream_mapped_path_number_chunk(void *user, const lonejson_value_path *path,
                                const char *data, size_t len,
                                lonejson_error *error) {
  lql_stream_member_context *member;
  (void)path;
  (void)error;
  member = (lql_stream_member_context *)user;
  if (member->full_path && path->segment_count == 0u && !member->root_object) {
    return LONEJSON_STATUS_OK;
  }
  if (member->active_number_terms == 0ul) {
    return LONEJSON_STATUS_OK;
  }
  if (len > sizeof(member->number) - 1u - member->number_len) {
    member->number_overflow = 1;
    return LONEJSON_STATUS_OK;
  }
  memcpy(member->number + member->number_len, data, len);
  member->number_len += len;
  return LONEJSON_STATUS_OK;
}

static lonejson_status
stream_mapped_path_number_end(void *user, const lonejson_value_path *path,
                              lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  char *end;
  double value;
  (void)path;
  (void)error;
  member = (lql_stream_member_context *)user;
  if (member->active_number_terms == 0ul || member->number_overflow) {
    member->active_number_terms = 0ul;
    return LONEJSON_STATUS_OK;
  }
  member->number[member->number_len] = '\0';
  errno = 0;
  end = NULL;
  value = strtod(member->number, &end);
  if (errno != 0 || end == member->number || *end != '\0') {
    member->active_number_terms = 0ul;
    return LONEJSON_STATUS_OK;
  }
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    const lql_selector *selector;
    if ((member->active_number_terms & term->bit) == 0ul) {
      continue;
    }
    selector = term->selector;
    if (term->kind == LQL_SELECTOR_KIND_EQ) {
      if (term->value_len == member->number_len &&
          memcmp(term->value, member->number, member->number_len) == 0) {
        member->state->hits |= term->bit;
      }
      continue;
    }
    if ((!selector->has_range_gt || value > selector->range_gt) &&
        (!selector->has_range_gte || value >= selector->range_gte) &&
        (!selector->has_range_lt || value < selector->range_lt) &&
        (!selector->has_range_lte || value <= selector->range_lte)) {
      member->state->hits |= term->bit;
    }
  }
  member->active_number_terms = 0ul;
  return LONEJSON_STATUS_OK;
}

static lonejson_status
stream_mapped_path_present(void *user, const lonejson_value_path *path,
                           lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  (void)error;
  member = (lql_stream_member_context *)user;
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    if ((member->full_path || term->field_index == member->field_index) &&
        term->kind == LQL_SELECTOR_KIND_EXISTS &&
        stream_term_path_matches(term, path, member)) {
      member->state->hits |= term->bit;
    }
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
stream_mapped_path_boolean_value(void *user, const lonejson_value_path *path,
                                 int value, lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  lonejson_status status;
  status = stream_mapped_path_present(user, path, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  member = (lql_stream_member_context *)user;
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    if ((member->full_path || term->field_index == member->field_index) &&
        term->kind == LQL_SELECTOR_KIND_EQ &&
        !term->selector->value_is_temporal &&
        stream_term_path_matches(term, path, member) &&
        ((value && strcmp(term->value, "true") == 0) ||
         (!value && strcmp(term->value, "false") == 0))) {
      member->state->hits |= term->bit;
    }
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_projection_event(
    lql_stream_member_context *member, lonejson_path_value_event_fn event,
    const lonejson_value_path *path, lonejson_error *error) {
  if (member->state->projection_capture == NULL ||
      member->state->projection_visitor == NULL || event == NULL) {
    return LONEJSON_STATUS_OK;
  }
  return event(
      lql_projection_capture_visitor_user(member->state->projection_capture),
      path, error);
}

static lonejson_status
stream_projection_chunk(lql_stream_member_context *member,
                        lonejson_path_value_chunk_fn event,
                        const lonejson_value_path *path, const char *data,
                        size_t len, lonejson_error *error) {
  if (member->state->projection_capture == NULL ||
      member->state->projection_visitor == NULL || event == NULL) {
    return LONEJSON_STATUS_OK;
  }
  return event(
      lql_projection_capture_visitor_user(member->state->projection_capture),
      path, data, len, error);
}

static lonejson_status
stream_mutation_event(lql_stream_member_context *member,
                      lonejson_value_event_fn event, lonejson_error *error) {
  lql_stream_state *state;
  state = member->state;
  if (!state->mutation_ready || event == NULL) {
    return LONEJSON_STATUS_OK;
  }
  return event(state->mutation_visitor_user, error);
}

static lonejson_status
stream_mutation_chunk(lql_stream_member_context *member,
                      lonejson_value_chunk_fn event, const char *data,
                      size_t len, lonejson_error *error) {
  lql_stream_state *state;
  state = member->state;
  if (!state->mutation_ready || event == NULL) {
    return LONEJSON_STATUS_OK;
  }
  return event(state->mutation_visitor_user, data, len, error);
}

static lonejson_status
stream_mutation_boolean(lql_stream_member_context *member, int value,
                        lonejson_error *error) {
  lql_stream_state *state;
  state = member->state;
  if (!state->mutation_ready || state->mutation_visitor.boolean_value == NULL) {
    return LONEJSON_STATUS_OK;
  }
  return state->mutation_visitor.boolean_value(state->mutation_visitor_user,
                                               value, error);
}

#define STREAM_COMPOSE_PATH_EVENT(name)                                        \
  static lonejson_status stream_composed_path_##name(                          \
      void *user, const lonejson_value_path *path, lonejson_error *error) {    \
    lql_stream_member_context *member;                                         \
    lonejson_status status;                                                    \
    member = (lql_stream_member_context *)user;                                \
    status = stream_mapped_path_##name(user, path, error);                     \
    if (status != LONEJSON_STATUS_OK) {                                        \
      return status;                                                           \
    }                                                                          \
    status = stream_projection_event(                                          \
        member,                                                                \
        member->state->projection_visitor == NULL                              \
            ? NULL                                                             \
            : member->state->projection_visitor->name,                         \
        path, error);                                                          \
    if (status != LONEJSON_STATUS_OK) {                                        \
      return status;                                                           \
    }                                                                          \
    return stream_mutation_event(                                              \
        member, member->state->mutation_ready                                  \
                    ? member->state->mutation_visitor.name                    \
                    : NULL,                                                    \
        error);                                                                \
  }

#define STREAM_COMPOSE_PATH_CHUNK(name)                                        \
  static lonejson_status stream_composed_path_##name(                          \
      void *user, const lonejson_value_path *path, const char *data,           \
      size_t len, lonejson_error *error) {                                     \
    lql_stream_member_context *member;                                         \
    lonejson_status status;                                                    \
    member = (lql_stream_member_context *)user;                                \
    status = stream_mapped_path_##name(user, path, data, len, error);          \
    if (status != LONEJSON_STATUS_OK) {                                        \
      return status;                                                           \
    }                                                                          \
    status = stream_projection_chunk(                                          \
        member,                                                                \
        member->state->projection_visitor == NULL                              \
            ? NULL                                                             \
            : member->state->projection_visitor->name,                         \
        path, data, len, error);                                               \
    if (status != LONEJSON_STATUS_OK) {                                        \
      return status;                                                           \
    }                                                                          \
    return stream_mutation_chunk(                                              \
        member, member->state->mutation_ready                                  \
                    ? member->state->mutation_visitor.name                    \
                    : NULL,                                                    \
        data, len, error);                                                     \
  }

STREAM_COMPOSE_PATH_EVENT(object_begin)
STREAM_COMPOSE_PATH_EVENT(object_end)
STREAM_COMPOSE_PATH_EVENT(array_begin)
STREAM_COMPOSE_PATH_EVENT(array_end)
STREAM_COMPOSE_PATH_EVENT(string_begin)
STREAM_COMPOSE_PATH_EVENT(string_end)
STREAM_COMPOSE_PATH_EVENT(number_begin)
STREAM_COMPOSE_PATH_EVENT(number_end)
STREAM_COMPOSE_PATH_CHUNK(string_chunk)
STREAM_COMPOSE_PATH_CHUNK(number_chunk)

static lonejson_status
stream_composed_path_boolean_value(void *user, const lonejson_value_path *path,
                                   int value, lonejson_error *error) {
  lql_stream_member_context *member;
  lonejson_status status;
  member = (lql_stream_member_context *)user;
  status = stream_mapped_path_boolean_value(user, path, value, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  if (member->state->projection_capture != NULL &&
      member->state->projection_visitor != NULL &&
      member->state->projection_visitor->boolean_value != NULL) {
    status = member->state->projection_visitor->boolean_value(
        lql_projection_capture_visitor_user(member->state->projection_capture),
        path, value, error);
    if (status != LONEJSON_STATUS_OK) {
      return status;
    }
  }
  return stream_mutation_boolean(member, value, error);
}

static lonejson_status
stream_composed_path_null_value(void *user, const lonejson_value_path *path,
                                lonejson_error *error) {
  lql_stream_member_context *member;
  lonejson_status status;
  member = (lql_stream_member_context *)user;
  status = stream_projection_event(
      member,
      member->state->projection_visitor == NULL
          ? NULL
          : member->state->projection_visitor->null_value,
      path, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  return stream_mutation_event(
      member, member->state->mutation_ready
                  ? member->state->mutation_visitor.null_value
                  : NULL,
      error);
}

static lonejson_status stream_composed_path_object_key_begin(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  lql_stream_member_context *member;
  lonejson_status status;
  member = (lql_stream_member_context *)user;
  status = stream_projection_event(
      member,
      member->state->projection_visitor == NULL
          ? NULL
          : member->state->projection_visitor->object_key_begin,
      path, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  return stream_mutation_event(
      member, member->state->mutation_ready
                  ? member->state->mutation_visitor.object_key_begin
                  : NULL,
      error);
}

static lonejson_status stream_composed_path_object_key_chunk(
    void *user, const lonejson_value_path *path, const char *data, size_t len,
    lonejson_error *error) {
  lql_stream_member_context *member;
  lonejson_status status;
  member = (lql_stream_member_context *)user;
  status = stream_projection_chunk(
      member,
      member->state->projection_visitor == NULL
          ? NULL
          : member->state->projection_visitor->object_key_chunk,
      path, data, len, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  return stream_mutation_chunk(
      member, member->state->mutation_ready
                  ? member->state->mutation_visitor.object_key_chunk
                  : NULL,
      data, len, error);
}

static lonejson_status
stream_composed_path_object_key_end(void *user, const lonejson_value_path *path,
                                    lonejson_error *error) {
  lql_stream_member_context *member;
  lonejson_status status;
  member = (lql_stream_member_context *)user;
  status = stream_projection_event(
      member,
      member->state->projection_visitor == NULL
          ? NULL
          : member->state->projection_visitor->object_key_end,
      path, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  return stream_mutation_event(
      member, member->state->mutation_ready
                  ? member->state->mutation_visitor.object_key_end
                  : NULL,
      error);
}

#undef STREAM_COMPOSE_PATH_CHUNK
#undef STREAM_COMPOSE_PATH_EVENT

#define STREAM_MAPPED_PROJECTION_EVENT(name)                                  \
  static lonejson_status stream_mapped_projection_##name(                     \
      void *user, const lonejson_value_path *path, lonejson_error *error) {   \
    lql_mapped_projection_field *field;                                       \
    lonejson_value_path prefixed;                                             \
    size_t i;                                                                 \
    field = (lql_mapped_projection_field *)user;                              \
    if (field->state->projection_capture == NULL ||                           \
        field->state->projection_visitor == NULL ||                           \
        field->state->projection_visitor->name == NULL) {                     \
      return LONEJSON_STATUS_OK;                                              \
    }                                                                         \
    if (path->segment_count >= LQL_STREAM_PATH_CAPACITY) {                    \
      stream_lonejson_error(error, "projection path depth exceeded");        \
      return LONEJSON_STATUS_CALLBACK_FAILED;                                 \
    }                                                                         \
    field->segments[0].data = field->field->key;                              \
    field->segments[0].len = field->field->key_len;                           \
    for (i = 0u; i < path->segment_count; ++i) {                              \
      field->segments[i + 1u] = path->segments[i];                            \
    }                                                                         \
    prefixed.segments = field->segments;                                      \
    prefixed.segment_count = path->segment_count + 1u;                        \
    return field->state->projection_visitor->name(                            \
        lql_projection_capture_visitor_user(field->state->projection_capture), \
        &prefixed, error);                                                    \
  }

#define STREAM_MAPPED_PROJECTION_CHUNK(name)                                  \
  static lonejson_status stream_mapped_projection_##name(                     \
      void *user, const lonejson_value_path *path, const char *data,          \
      size_t len, lonejson_error *error) {                                    \
    lql_mapped_projection_field *field;                                       \
    lonejson_value_path prefixed;                                             \
    size_t i;                                                                 \
    field = (lql_mapped_projection_field *)user;                              \
    if (field->state->projection_capture == NULL ||                           \
        field->state->projection_visitor == NULL ||                           \
        field->state->projection_visitor->name == NULL) {                     \
      return LONEJSON_STATUS_OK;                                              \
    }                                                                         \
    if (path->segment_count >= LQL_STREAM_PATH_CAPACITY) {                    \
      stream_lonejson_error(error, "projection path depth exceeded");        \
      return LONEJSON_STATUS_CALLBACK_FAILED;                                 \
    }                                                                         \
    field->segments[0].data = field->field->key;                              \
    field->segments[0].len = field->field->key_len;                           \
    for (i = 0u; i < path->segment_count; ++i) {                              \
      field->segments[i + 1u] = path->segments[i];                            \
    }                                                                         \
    prefixed.segments = field->segments;                                      \
    prefixed.segment_count = path->segment_count + 1u;                        \
    return field->state->projection_visitor->name(                            \
        lql_projection_capture_visitor_user(field->state->projection_capture), \
        &prefixed, data, len, error);                                         \
  }

STREAM_MAPPED_PROJECTION_EVENT(object_begin)
STREAM_MAPPED_PROJECTION_EVENT(object_end)
STREAM_MAPPED_PROJECTION_EVENT(object_key_begin)
STREAM_MAPPED_PROJECTION_EVENT(object_key_end)
STREAM_MAPPED_PROJECTION_EVENT(array_begin)
STREAM_MAPPED_PROJECTION_EVENT(array_end)
STREAM_MAPPED_PROJECTION_EVENT(string_begin)
STREAM_MAPPED_PROJECTION_EVENT(string_end)
STREAM_MAPPED_PROJECTION_EVENT(number_begin)
STREAM_MAPPED_PROJECTION_EVENT(number_end)
STREAM_MAPPED_PROJECTION_EVENT(null_value)
STREAM_MAPPED_PROJECTION_CHUNK(object_key_chunk)
STREAM_MAPPED_PROJECTION_CHUNK(string_chunk)
STREAM_MAPPED_PROJECTION_CHUNK(number_chunk)

static lonejson_status stream_mapped_projection_boolean_value(
    void *user, const lonejson_value_path *path, int value,
    lonejson_error *error) {
  lql_mapped_projection_field *field;
  lonejson_value_path prefixed;
  size_t i;
  field = (lql_mapped_projection_field *)user;
  if (field->state->projection_capture == NULL ||
      field->state->projection_visitor == NULL ||
      field->state->projection_visitor->boolean_value == NULL) {
    return LONEJSON_STATUS_OK;
  }
  if (path->segment_count >= LQL_STREAM_PATH_CAPACITY) {
    stream_lonejson_error(error, "projection path depth exceeded");
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  field->segments[0].data = field->field->key;
  field->segments[0].len = field->field->key_len;
  for (i = 0u; i < path->segment_count; ++i) {
    field->segments[i + 1u] = path->segments[i];
  }
  prefixed.segments = field->segments;
  prefixed.segment_count = path->segment_count + 1u;
  return field->state->projection_visitor->boolean_value(
      lql_projection_capture_visitor_user(field->state->projection_capture),
      &prefixed, value, error);
}

#undef STREAM_MAPPED_PROJECTION_CHUNK
#undef STREAM_MAPPED_PROJECTION_EVENT

static void stream_mapped_projection_visitor(
    lonejson_path_value_visitor *visitor) {
  *visitor = lonejson_default_path_value_visitor();
  visitor->object_begin = stream_mapped_projection_object_begin;
  visitor->object_end = stream_mapped_projection_object_end;
  visitor->object_key_begin = stream_mapped_projection_object_key_begin;
  visitor->object_key_chunk = stream_mapped_projection_object_key_chunk;
  visitor->object_key_end = stream_mapped_projection_object_key_end;
  visitor->array_begin = stream_mapped_projection_array_begin;
  visitor->array_end = stream_mapped_projection_array_end;
  visitor->string_begin = stream_mapped_projection_string_begin;
  visitor->string_chunk = stream_mapped_projection_string_chunk;
  visitor->string_end = stream_mapped_projection_string_end;
  visitor->number_begin = stream_mapped_projection_number_begin;
  visitor->number_chunk = stream_mapped_projection_number_chunk;
  visitor->number_end = stream_mapped_projection_number_end;
  visitor->boolean_value = stream_mapped_projection_boolean_value;
  visitor->null_value = stream_mapped_projection_null_value;
}

static const lonejson_value_path stream_mapped_root_path = {NULL, 0u};

static lonejson_status stream_mapped_top_object_begin(void *user,
                                                      lonejson_error *error) {
  return stream_mapped_path_object_begin(user, &stream_mapped_root_path, error);
}

static lonejson_status stream_mapped_top_object_end(void *user,
                                                    lonejson_error *error) {
  return stream_mapped_path_object_end(user, &stream_mapped_root_path, error);
}

static lonejson_status stream_mapped_top_string_begin(void *user,
                                                      lonejson_error *error) {
  lql_stream_member_context *member;
  member = (lql_stream_member_context *)user;
  if (member->container_depth != 0u) {
    member->active_string = 0;
    member->active_terms = 0ul;
    return LONEJSON_STATUS_OK;
  }
  return stream_mapped_path_string_begin(user, &stream_mapped_root_path, error);
}

static lonejson_status stream_mapped_top_string_chunk(void *user,
                                                      const char *data,
                                                      size_t len,
                                                      lonejson_error *error) {
  return stream_mapped_path_string_chunk(user, &stream_mapped_root_path, data,
                                         len, error);
}

static lonejson_status stream_mapped_top_string_end(void *user,
                                                    lonejson_error *error) {
  return stream_mapped_path_string_end(user, &stream_mapped_root_path, error);
}

static lonejson_status stream_mapped_top_number_begin(void *user,
                                                      lonejson_error *error) {
  lql_stream_member_context *member;
  member = (lql_stream_member_context *)user;
  if (member->container_depth != 0u) {
    member->active_number_terms = 0ul;
    return LONEJSON_STATUS_OK;
  }
  return stream_mapped_path_number_begin(user, &stream_mapped_root_path, error);
}

static lonejson_status stream_mapped_top_number_chunk(void *user,
                                                      const char *data,
                                                      size_t len,
                                                      lonejson_error *error) {
  return stream_mapped_path_number_chunk(user, &stream_mapped_root_path, data,
                                         len, error);
}

static lonejson_status stream_mapped_top_number_end(void *user,
                                                    lonejson_error *error) {
  return stream_mapped_path_number_end(user, &stream_mapped_root_path, error);
}

static lonejson_status stream_mapped_top_boolean_value(void *user, int value,
                                                       lonejson_error *error) {
  lql_stream_member_context *member;
  member = (lql_stream_member_context *)user;
  if (member->container_depth != 0u) {
    return LONEJSON_STATUS_OK;
  }
  return stream_mapped_path_boolean_value(user, &stream_mapped_root_path, value,
                                          error);
}

static lql_status stream_status(lql_stream_state *state, lonejson_status status,
                                const lonejson_error *error, lql_error *out) {
  if (state->failure.code != LQL_STATUS_OK) {
    if (out != NULL) {
      *out = state->failure;
    }
    return state->failure.code;
  }
  if (status == LONEJSON_STATUS_OK) {
    return LQL_STATUS_OK;
  }
  if (status == LONEJSON_STATUS_ALLOCATION_FAILED) {
    lql_set_error(out, LQL_STATUS_NO_MEMORY,
                  error != NULL && error->message[0] != '\0'
                      ? error->message
                      : "lonejson allocation failed");
    return LQL_STATUS_NO_MEMORY;
  }
  if (status == LONEJSON_STATUS_CALLBACK_FAILED) {
    lql_set_error(out, LQL_STATUS_CALLBACK_ERROR,
                  "lonejson stream callback failed");
    return LQL_STATUS_CALLBACK_ERROR;
  }
  lql_set_error(out, LQL_STATUS_JSON_ERROR,
                error != NULL && error->message[0] != '\0'
                    ? error->message
                    : "invalid JSON stream");
  return LQL_STATUS_JSON_ERROR;
}

static void stream_program_configure_mapped_fields(lql_stream_program *program) {
  size_t i;
  for (i = 0u; i < program->field_count; ++i) {
    lonejson_field *field;
    const lql_stream_field *stream_field;
    field = &program->mapped_fields[i];
    stream_field = &program->fields[i];
    memset(field, 0, sizeof(*field));
    field->json_key = stream_field->key;
    field->json_key_len = stream_field->key_len;
    field->json_key_first = stream_field->key_len == 0u
                                ? 0u
                                : (unsigned char)stream_field->key[0];
    field->json_key_last =
        stream_field->key_len == 0u
            ? 0u
            : (unsigned char)stream_field->key[stream_field->key_len - 1u];
    field->struct_offset = i * sizeof(lonejson_json_value);
    field->kind = LONEJSON_FIELD_KIND_JSON_VALUE;
    field->storage = LONEJSON_STORAGE_FIXED;
    field->overflow_policy = LONEJSON_OVERFLOW_FAIL;
    field->spool_class = LONEJSON_SPOOL_CLASS_DEFAULT;
  }
  memset(&program->mapped_map, 0, sizeof(program->mapped_map));
  program->mapped_map.name = "lql_stream_program";
  program->mapped_map.struct_size =
      program->field_count * sizeof(lonejson_json_value);
  program->mapped_map.fields = program->mapped_fields;
  program->mapped_map.field_count = program->field_count;
}

static int stream_projection_mapped_eligible(const lql_stream_state *state) {
  const lql_projection *projection;
  size_t i;
  size_t j;
  if (state->request->output_mode != LQL_STREAM_OUTPUT_PROJECTION ||
      state->program == NULL || state->program->match_all ||
      state->program->requires_generic_path || state->program->has_nested_paths) {
    return 0;
  }
  projection = state->request->projection;
  if (projection == NULL ||
      projection->path_count > LQL_STREAM_TERM_CAPACITY - state->program->field_count) {
    return 0;
  }
  for (i = 0u; i < projection->path_count; ++i) {
    const lql_projection_path *path;
    path = &projection->compiled_paths[i];
    if (path->segment_count != 1u) {
      return 0;
    }
    for (j = 0u; j < state->program->field_count; ++j) {
      if (strlen(path->segments[0]) == state->program->fields[j].key_len &&
          memcmp(path->segments[0], state->program->fields[j].key,
                 state->program->fields[j].key_len) == 0) {
        return 0;
      }
    }
  }
  return 1;
}

static int stream_mutation_flat_selector_eligible(
    const lql_stream_state *state) {
  const lql_stream_term *term;
  char *end;
  if (!state->mutation_direct || state->program == NULL ||
      state->program->match_all || state->program->requires_generic_path ||
      state->program->has_nested_paths || state->program->term_count != 1u ||
      state->program->field_count != 1u) {
    return 0;
  }
  term = &state->program->terms[0];
  if (term->kind != LQL_SELECTOR_KIND_EQ || term->selector->value_is_temporal ||
      term->value == NULL || term->value[0] == '\0' ||
      strcmp(term->value, "true") == 0 || strcmp(term->value, "false") == 0 ||
      strcmp(term->value, "null") == 0) {
    return 0;
  }
  errno = 0;
  end = NULL;
  (void)strtod(term->value, &end);
  return errno != 0 || end == term->value || *end != '\0';
}

static lonejson_status
stream_flat_mutation_event(lql_stream_state *state,
                           lonejson_value_event_fn rewriter_event,
                           lonejson_value_event_fn tape_event,
                           lonejson_error *error) {
  lonejson_status status;
  if (state->mutation_ready) {
    return rewriter_event != NULL
               ? rewriter_event(state->mutation_visitor_user, error)
               : LONEJSON_STATUS_OK;
  }
  if (!state->mutation_tape_ready) {
    return LONEJSON_STATUS_OK;
  }
  if (tape_event == NULL) {
    return LONEJSON_STATUS_OK;
  }
  status = tape_event(state->mutation_tape_visitor_user, error);
  if (status != LONEJSON_STATUS_OVERFLOW) {
    return status;
  }
  status = stream_mutation_tape_flush(state, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  return rewriter_event != NULL
             ? rewriter_event(state->mutation_visitor_user, error)
             : LONEJSON_STATUS_OK;
}

static lonejson_status
stream_flat_mutation_chunk(lql_stream_state *state,
                           lonejson_value_chunk_fn rewriter_event,
                           lonejson_value_chunk_fn tape_event,
                           const char *data, size_t len,
                           lonejson_error *error) {
  lonejson_status status;
  if (state->mutation_ready) {
    return rewriter_event != NULL
               ? rewriter_event(state->mutation_visitor_user, data, len, error)
               : LONEJSON_STATUS_OK;
  }
  if (!state->mutation_tape_ready) {
    return LONEJSON_STATUS_OK;
  }
  if (tape_event == NULL) {
    return LONEJSON_STATUS_OK;
  }
  status = tape_event(state->mutation_tape_visitor_user, data, len, error);
  if (status != LONEJSON_STATUS_OVERFLOW) {
    return status;
  }
  status = stream_mutation_tape_flush(state, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  return rewriter_event != NULL
             ? rewriter_event(state->mutation_visitor_user, data, len, error)
             : LONEJSON_STATUS_OK;
}

#define STREAM_FLAT_MUTATION_EVENT(name, selector_event)                      \
  static lonejson_status stream_flat_mutation_##name(                         \
      void *user, lonejson_error *error) {                                    \
    lql_stream_state *state;                                                  \
    lonejson_status status;                                                   \
    state = (lql_stream_state *)user;                                         \
    if (state->mutation_abandoned) {                                          \
      return LONEJSON_STATUS_OK;                                              \
    }                                                                         \
    status = selector_event(user, error);                                     \
    if (status != LONEJSON_STATUS_OK) {                                       \
      return status;                                                          \
    }                                                                         \
    return stream_flat_mutation_event(                                        \
        state, state->mutation_visitor.name,                                  \
        state->mutation_tape_visitor.name, error);                            \
  }

#define STREAM_FLAT_MUTATION_CHUNK(name, selector_event)                      \
  static lonejson_status stream_flat_mutation_##name(                         \
      void *user, const char *data, size_t len, lonejson_error *error) {      \
    lql_stream_state *state;                                                  \
    lonejson_status status;                                                   \
    state = (lql_stream_state *)user;                                         \
    if (state->mutation_abandoned) {                                          \
      return LONEJSON_STATUS_OK;                                              \
    }                                                                         \
    status = selector_event(user, data, len, error);                          \
    if (status != LONEJSON_STATUS_OK) {                                       \
      return status;                                                          \
    }                                                                         \
    return stream_flat_mutation_chunk(                                        \
        state, state->mutation_visitor.name,                                  \
        state->mutation_tape_visitor.name, data, len, error);                 \
  }

STREAM_FLAT_MUTATION_EVENT(object_begin, stream_object_begin)
STREAM_FLAT_MUTATION_EVENT(object_end, stream_object_end)
STREAM_FLAT_MUTATION_EVENT(array_begin, stream_array_begin)
STREAM_FLAT_MUTATION_EVENT(array_end, stream_array_end)
STREAM_FLAT_MUTATION_EVENT(object_key_begin, stream_key_begin)
STREAM_FLAT_MUTATION_CHUNK(object_key_chunk, stream_key_chunk)
STREAM_FLAT_MUTATION_EVENT(number_begin, stream_scalar_value)

static lonejson_status stream_flat_mutation_string_begin(void *user,
                                                         lonejson_error *error) {
  lql_stream_state *state;
  lonejson_status status;
  state = (lql_stream_state *)user;
  if (state->mutation_abandoned) {
    return LONEJSON_STATUS_OK;
  }
  status = stream_string_begin(user, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  if (state->active && state->active_term == 0u) {
    return LONEJSON_STATUS_OK;
  }
  return stream_flat_mutation_event(
      state, state->mutation_visitor.string_begin,
      state->mutation_tape_visitor.string_begin, error);
}

static lonejson_status stream_flat_mutation_string_chunk(
    void *user, const char *data, size_t len, lonejson_error *error) {
  lql_stream_state *state;
  lonejson_status status;
  state = (lql_stream_state *)user;
  if (state->mutation_abandoned) {
    return LONEJSON_STATUS_OK;
  }
  status = stream_string_chunk(user, data, len, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  if (state->active && state->active_term == 0u) {
    return LONEJSON_STATUS_OK;
  }
  return stream_flat_mutation_chunk(
      state, state->mutation_visitor.string_chunk,
      state->mutation_tape_visitor.string_chunk, data, len, error);
}

static lonejson_status stream_flat_mutation_string_end(void *user,
                                                       lonejson_error *error) {
  lql_stream_state *state;
  lonejson_status status;
  int selector_value;
  state = (lql_stream_state *)user;
  selector_value = state->active && state->active_term == 0u;
  status = stream_scalar_value(user, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  if (selector_value) {
    if ((state->hits & state->program->terms[0].bit) == 0ul) {
      if (state->mutation_ready) {
        lonejson_value_rewriter_cleanup(&state->mutation_rewriter);
        state->mutation_ready = 0;
      }
      if (state->mutation_tape_ready) {
        lonejson_value_event_tape_reset(&state->mutation_tape);
        state->mutation_tape_ready = 0;
      }
      state->mutation_abandoned = 1;
      return LONEJSON_STATUS_OK;
    }
    if (state->mutation_tape_ready) {
      status = stream_mutation_tape_flush(state, error);
      if (status != LONEJSON_STATUS_OK) {
        return status;
      }
    }
    status = state->mutation_visitor.string_begin(state->mutation_visitor_user,
                                                   error);
    if (status == LONEJSON_STATUS_OK) {
      status = state->mutation_visitor.string_chunk(
          state->mutation_visitor_user, state->program->terms[0].value,
          state->program->terms[0].value_len, error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = state->mutation_visitor.string_end(state->mutation_visitor_user,
                                                   error);
    }
    return status;
  }
  return stream_flat_mutation_event(
      state, state->mutation_visitor.string_end,
      state->mutation_tape_visitor.string_end, error);
}

static lonejson_status
stream_flat_mutation_object_key_end(void *user, lonejson_error *error) {
  lql_stream_state *state;
  lonejson_status status;
  state = (lql_stream_state *)user;
  if (state->mutation_abandoned) {
    return LONEJSON_STATUS_SKIP_VALUE;
  }
  stream_finish_key(state);
  status = stream_flat_mutation_event(
      state, state->mutation_visitor.object_key_end,
      state->mutation_tape_visitor.object_key_end, error);
  return status;
}

static lonejson_status
stream_flat_mutation_number_chunk(void *user, const char *data, size_t len,
                                  lonejson_error *error) {
  lql_stream_state *state;
  state = (lql_stream_state *)user;
  if (state->mutation_abandoned) {
    return LONEJSON_STATUS_OK;
  }
  return stream_flat_mutation_chunk(
      state, state->mutation_visitor.number_chunk,
      state->mutation_tape_visitor.number_chunk, data, len, error);
}

static lonejson_status
stream_flat_mutation_number_end(void *user, lonejson_error *error) {
  lql_stream_state *state;
  state = (lql_stream_state *)user;
  if (state->mutation_abandoned) {
    return LONEJSON_STATUS_OK;
  }
  return stream_flat_mutation_event(
      state, state->mutation_visitor.number_end,
      state->mutation_tape_visitor.number_end, error);
}

static lonejson_status
stream_flat_mutation_boolean(void *user, int value, lonejson_error *error) {
  lql_stream_state *state;
  lonejson_status status;
  state = (lql_stream_state *)user;
  if (state->mutation_abandoned) {
    return LONEJSON_STATUS_OK;
  }
  status = stream_boolean_value(user, value, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  if (state->mutation_ready) {
    return state->mutation_visitor.boolean_value(
        state->mutation_visitor_user, value, error);
  }
  if (!state->mutation_tape_ready) {
    return LONEJSON_STATUS_OK;
  }
  status = state->mutation_tape_visitor.boolean_value(
      state->mutation_tape_visitor_user, value, error);
  if (status != LONEJSON_STATUS_OVERFLOW) {
    return status;
  }
  status = stream_mutation_tape_flush(state, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  return state->mutation_visitor.boolean_value(state->mutation_visitor_user,
                                               value, error);
}

static lonejson_status stream_flat_mutation_null(void *user,
                                                  lonejson_error *error) {
  lql_stream_state *state;
  lonejson_status status;
  state = (lql_stream_state *)user;
  if (state->mutation_abandoned) {
    return LONEJSON_STATUS_OK;
  }
  status = stream_scalar_value(user, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  return stream_flat_mutation_event(
      state, state->mutation_visitor.null_value,
      state->mutation_tape_visitor.null_value, error);
}

#undef STREAM_FLAT_MUTATION_CHUNK
#undef STREAM_FLAT_MUTATION_EVENT

static lql_status stream_execute_mapped(lql_stream_state *state,
                                        lonejson *runtime, lql_error *error) {
  lonejson_json_value values[sizeof(unsigned long) * CHAR_BIT];
  lql_stream_member_context members[sizeof(unsigned long) * CHAR_BIT];
  lql_mapped_projection_field projection_fields[
      sizeof(unsigned long) * CHAR_BIT];
  lql_stream_program projection_program;
  const lql_stream_program *saved_program;
  lonejson_value_visitor top_visitor;
  lonejson_path_value_visitor path_visitor;
  lonejson_path_value_visitor capture_visitor;
  lonejson_path_value_visitor projection_visitor;
  lonejson_stream *stream;
  lonejson_stream_result stream_result;
  lonejson_error lonejson_error;
  lonejson_status lonejson_status;
  lonejson_candidate_callback_result callback_result;
  lql_status status;
  size_t selector_field_count;
  int mapped_projection;
  size_t i;

  mapped_projection = stream_projection_mapped_eligible(state);
  saved_program = state->program;
  selector_field_count = state->program->field_count;
  if (mapped_projection) {
    const lql_projection *projection;
    projection = state->request->projection;
    projection_program = *state->program;
    for (i = 0u; i < projection->path_count; ++i) {
      lql_stream_field *field;
      field = &projection_program.fields[projection_program.field_count++];
      field->key = projection->compiled_paths[i].segments[0];
      field->key_len = strlen(field->key);
    }
    stream_program_configure_mapped_fields(&projection_program);
    state->program = &projection_program;
    status = lql_projection_capture_create(
        state->receiver, state->request->projection, runtime,
        &state->projection_capture, error);
    if (status != LQL_STATUS_OK) {
      state->program = saved_program;
      return status;
    }
    lql_projection_capture_visitor(&capture_visitor);
    state->projection_visitor = &capture_visitor;
  }
  top_visitor = lonejson_default_value_visitor();
  top_visitor.object_begin = stream_mapped_top_object_begin;
  top_visitor.object_end = stream_mapped_top_object_end;
  top_visitor.array_begin = stream_mapped_top_object_begin;
  top_visitor.array_end = stream_mapped_top_object_end;
  top_visitor.string_begin = stream_mapped_top_string_begin;
  top_visitor.string_chunk = stream_mapped_top_string_chunk;
  top_visitor.string_end = stream_mapped_top_string_end;
  top_visitor.number_begin = stream_mapped_top_number_begin;
  top_visitor.number_chunk = stream_mapped_top_number_chunk;
  top_visitor.number_end = stream_mapped_top_number_end;
  top_visitor.boolean_value = stream_mapped_top_boolean_value;
  path_visitor = lonejson_default_path_value_visitor();
  path_visitor.object_begin = stream_mapped_path_object_begin;
  path_visitor.object_end = stream_mapped_path_object_end;
  path_visitor.array_begin = stream_mapped_path_array_begin;
  path_visitor.array_end = stream_mapped_path_array_end;
  path_visitor.string_begin = stream_mapped_path_string_begin;
  path_visitor.string_chunk = stream_mapped_path_string_chunk;
  path_visitor.string_end = stream_mapped_path_string_end;
  path_visitor.number_begin = stream_mapped_path_number_begin;
  path_visitor.number_chunk = stream_mapped_path_number_chunk;
  path_visitor.number_end = stream_mapped_path_number_end;
  path_visitor.boolean_value = stream_mapped_path_boolean_value;
  if (mapped_projection) {
    stream_mapped_projection_visitor(&projection_visitor);
  }
  lonejson_error_init(&lonejson_error);
  for (i = 0u; i < state->program->field_count; ++i) {
    lonejson_json_value_init(runtime, &values[i]);
    if (mapped_projection && i >= selector_field_count) {
      size_t projection_index;
      projection_index = i - selector_field_count;
      projection_fields[projection_index].state = state;
      projection_fields[projection_index].field = &state->program->fields[i];
      lonejson_status = lonejson_json_value_set_parse_path_visitor(
          &values[i], &projection_visitor, &projection_fields[projection_index],
          &lonejson_error);
      if (lonejson_status != LONEJSON_STATUS_OK) {
        while (i != 0u) {
          --i;
          lonejson_json_value_cleanup(&values[i]);
        }
        lql_projection_capture_destroy(state->projection_capture);
        state->projection_capture = NULL;
        state->projection_visitor = NULL;
        state->program = saved_program;
        return stream_status(state, lonejson_status, &lonejson_error, error);
      }
      continue;
    }
    memset(&members[i], 0, sizeof(members[i]));
    members[i].state = state;
    members[i].field_index = i;
    if (state->program->has_nested_paths) {
      lonejson_status = lonejson_json_value_set_parse_path_visitor(
          &values[i], &path_visitor, &members[i], &lonejson_error);
      if (lonejson_status != LONEJSON_STATUS_OK) {
        while (i != 0u) {
          --i;
          lonejson_json_value_cleanup(&values[i]);
        }
        return stream_status(state, lonejson_status, &lonejson_error, error);
      }
    } else {
      lonejson_status = lonejson_json_value_set_parse_visitor(
          &values[i], &top_visitor, &members[i], &lonejson_error);
      if (lonejson_status != LONEJSON_STATUS_OK) {
        while (i != 0u) {
          --i;
          lonejson_json_value_cleanup(&values[i]);
        }
        return stream_status(state, lonejson_status, &lonejson_error, error);
      }
    }
  }
  stream = lonejson_stream_open_candidates_reader(
      runtime, &state->program->mapped_map, stream_read, state,
      &lonejson_error);
  if (stream == NULL) {
    for (i = 0u; i < state->program->field_count; ++i) {
      lonejson_json_value_cleanup(&values[i]);
    }
    status = stream_status(state, lonejson_error.code, &lonejson_error, error);
    lql_projection_capture_destroy(state->projection_capture);
    state->projection_capture = NULL;
    state->projection_visitor = NULL;
    state->program = saved_program;
    return status;
  }
  status = LQL_STATUS_OK;
  for (;;) {
    callback_result = stream_candidate_begin(state, NULL, &lonejson_error);
    if (callback_result == LONEJSON_CANDIDATE_STOP) {
      break;
    }
    if (callback_result != LONEJSON_CANDIDATE_CONTINUE) {
      status = stream_status(state, LONEJSON_STATUS_CALLBACK_FAILED,
                             &lonejson_error, error);
      break;
    }
    stream_result = stream->next(stream, values, &lonejson_error);
    if (stream_result == LONEJSON_STREAM_EOF) {
      --state->result->records_seen;
      break;
    }
    if (stream_result == LONEJSON_STREAM_ERROR) {
      status = stream_status(state, stream->error.code, &stream->error, error);
      break;
    }
    if (stream_result == LONEJSON_STREAM_VALUE &&
        stream->root_type == LONEJSON_VALUE_ARRAY) {
      stream_fail(state, LQL_STATUS_JSON_ERROR,
                  "root JSON arrays are not valid NDJSON records");
      if (error != NULL) {
        *error = state->failure;
      }
      status = state->failure.code;
      break;
    }
    if (mapped_projection &&
        (stream_result == LONEJSON_STREAM_OBJECT ||
         (stream_result == LONEJSON_STREAM_VALUE &&
          stream->root_type == LONEJSON_VALUE_OBJECT))) {
      lql_projection_capture_set_root_object(state->projection_capture);
    }
    callback_result = stream_candidate_end(state, NULL, &lonejson_error);
    if (callback_result == LONEJSON_CANDIDATE_STOP) {
      break;
    }
    if (callback_result != LONEJSON_CANDIDATE_CONTINUE) {
      status = stream_status(state, LONEJSON_STATUS_CALLBACK_FAILED,
                             &lonejson_error, error);
      break;
    }
  }
  stream->close(stream);
  for (i = 0u; i < state->program->field_count; ++i) {
    lonejson_json_value_cleanup(&values[i]);
  }
  lql_projection_capture_destroy(state->projection_capture);
  state->projection_capture = NULL;
  state->projection_visitor = NULL;
  state->program = saved_program;
  if (status == LQL_STATUS_OK && state->failure.code != LQL_STATUS_OK) {
    if (error != NULL) {
      *error = state->failure;
    }
    status = state->failure.code;
  }
  return status;
}

static lql_status stream_execute_generic_path(lql_stream_state *state,
                                              lonejson *runtime,
                                              lql_error *error) {
  lql_stream_member_context member;
  lonejson_candidate_stream_options options;
  lonejson_value_visitor flat_visitor;
  lonejson_path_value_visitor visitor;
  lonejson_path_value_visitor projection_visitor;
  lonejson_error lonejson_error;
  lql_status status;
  const lql_stream_program *saved_program;
  lql_stream_program match_all_program;
  int flat_mutation_selector;

  memset(&member, 0, sizeof(member));
  saved_program = state->program;
  if (state->program == NULL) {
    memset(&match_all_program, 0, sizeof(match_all_program));
    match_all_program.match_all = 1;
    state->program = &match_all_program;
  }
  member.state = state;
  member.full_path = 1;
  state->generic_member = &member;
  state->projection_capture = NULL;
  state->projection_visitor = NULL;
  state->runtime = runtime;
  state->mutation_direct =
      state->request->output_mode == LQL_STREAM_OUTPUT_MUTATION &&
      state->request->matched_only &&
      stream_mutation_direct_eligible(state->request->mutation);
  state->mutation_ready = 0;
  state->mutation_tape_ready = 0;
  if (state->mutation_direct) {
    lonejson_spooled_init_class(runtime, &state->mutation_spool,
                                LONEJSON_SPOOL_CLASS_LARGE_TEXT);
    lonejson_value_rewriter_init(&state->mutation_rewriter);
    lonejson_value_event_tape_init(&state->mutation_tape);
  }
  flat_mutation_selector = stream_mutation_flat_selector_eligible(state);
  state->mutation_deferred = flat_mutation_selector;
  if (state->request->output_mode == LQL_STREAM_OUTPUT_PROJECTION) {
    status = lql_projection_capture_create(state->receiver,
                                           state->request->projection, runtime,
                                           &state->projection_capture, error);
    if (status != LQL_STATUS_OK) {
      if (state->mutation_direct) {
        lonejson_value_event_tape_cleanup(&state->mutation_tape);
        lonejson_spooled_cleanup(&state->mutation_spool);
        state->mutation_direct = 0;
      }
      state->generic_member = NULL;
      state->program = saved_program;
      return status;
    }
    lql_projection_capture_visitor(&projection_visitor);
    state->projection_visitor = &projection_visitor;
  }
  options = lonejson_default_candidate_stream_options();
  options.framing = LONEJSON_CANDIDATE_FRAMING_NDJSON;
  options.capture_mode =
      (state->request->output_mode == LQL_STREAM_OUTPUT_SELECTED_RECORD ||
       (state->request->output_mode == LQL_STREAM_OUTPUT_MUTATION &&
        !state->mutation_direct) ||
       state->request->output_mode ==
           LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION)
          ? LONEJSON_CANDIDATE_CAPTURE_SPOOLED
          : LONEJSON_CANDIDATE_CAPTURE_NONE;
  options.spool_class = LONEJSON_SPOOL_CLASS_LARGE_TEXT;
  if (flat_mutation_selector) {
    flat_visitor = lonejson_default_value_visitor();
    flat_visitor.object_begin = stream_flat_mutation_object_begin;
    flat_visitor.object_end = stream_flat_mutation_object_end;
    flat_visitor.object_key_begin = stream_flat_mutation_object_key_begin;
    flat_visitor.object_key_chunk = stream_flat_mutation_object_key_chunk;
    flat_visitor.object_key_end = stream_flat_mutation_object_key_end;
    flat_visitor.array_begin = stream_flat_mutation_array_begin;
    flat_visitor.array_end = stream_flat_mutation_array_end;
    flat_visitor.string_begin = stream_flat_mutation_string_begin;
    flat_visitor.string_chunk = stream_flat_mutation_string_chunk;
    flat_visitor.string_end = stream_flat_mutation_string_end;
    flat_visitor.number_begin = stream_flat_mutation_number_begin;
    flat_visitor.number_chunk = stream_flat_mutation_number_chunk;
    flat_visitor.number_end = stream_flat_mutation_number_end;
    flat_visitor.boolean_value = stream_flat_mutation_boolean;
    flat_visitor.null_value = stream_flat_mutation_null;
    options.visitor = &flat_visitor;
    options.visitor_user = state;
  } else {
    visitor = lonejson_default_path_value_visitor();
    if (state->projection_capture != NULL || state->mutation_direct) {
    visitor.object_begin = stream_composed_path_object_begin;
    visitor.object_end = stream_composed_path_object_end;
    visitor.object_key_begin = stream_composed_path_object_key_begin;
    visitor.object_key_chunk = stream_composed_path_object_key_chunk;
    visitor.object_key_end = stream_composed_path_object_key_end;
    visitor.array_begin = stream_composed_path_array_begin;
    visitor.array_end = stream_composed_path_array_end;
    visitor.string_begin = stream_composed_path_string_begin;
    visitor.string_chunk = stream_composed_path_string_chunk;
    visitor.string_end = stream_composed_path_string_end;
    visitor.number_begin = stream_composed_path_number_begin;
    visitor.number_chunk = stream_composed_path_number_chunk;
    visitor.number_end = stream_composed_path_number_end;
    visitor.boolean_value = stream_composed_path_boolean_value;
    visitor.null_value = stream_composed_path_null_value;
    } else {
    visitor.object_begin = stream_mapped_path_object_begin;
    visitor.object_end = stream_mapped_path_object_end;
    visitor.array_begin = stream_mapped_path_array_begin;
    visitor.array_end = stream_mapped_path_array_end;
    visitor.string_begin = stream_mapped_path_string_begin;
    visitor.string_chunk = stream_mapped_path_string_chunk;
    visitor.string_end = stream_mapped_path_string_end;
    visitor.number_begin = stream_mapped_path_number_begin;
    visitor.number_chunk = stream_mapped_path_number_chunk;
    visitor.number_end = stream_mapped_path_number_end;
    visitor.boolean_value = stream_mapped_path_boolean_value;
    }
    options.path_visitor = &visitor;
    options.visitor_user = &member;
  }
  options.candidate_begin = stream_candidate_begin;
  options.candidate_end = stream_candidate_end;
  options.candidate_user = state;
  lonejson_error_init(&lonejson_error);
  status =
      stream_status(state,
                    lonejson_visit_candidates_reader(
                        runtime, stream_read, state, &options, &lonejson_error),
                    &lonejson_error, error);
  lonejson_value_rewriter_cleanup(&state->mutation_rewriter);
  lonejson_value_event_tape_cleanup(&state->mutation_tape);
  if (state->mutation_direct) {
    lonejson_spooled_cleanup(&state->mutation_spool);
  }
  state->mutation_direct = 0;
  state->mutation_ready = 0;
  lql_projection_capture_destroy(state->projection_capture);
  state->projection_capture = NULL;
  state->projection_visitor = NULL;
  state->generic_member = NULL;
  state->program = saved_program;
  return status;
}

lql_status lql_stream_execute(lql *self, const lql_stream_request *request,
                              lql_stream_result *result, lql_error *error) {
  lql_stream_state state;
  lonejson *runtime;
  lonejson_error lonejson_error;
  lonejson_candidate_stream_options options;
  lonejson_value_visitor visitor;
  lql_status status;
  int mapped_projection;
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
  }
  lql_error_init(error);
  if (self == NULL || request == NULL || result == NULL ||
      request->reader == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "stream receiver, request, result, and reader are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if ((request->output_mode != LQL_STREAM_OUTPUT_DECISION_ONLY &&
       request->output_mode != LQL_STREAM_OUTPUT_SELECTED_RECORD &&
       request->output_mode != LQL_STREAM_OUTPUT_PROJECTION &&
       request->output_mode != LQL_STREAM_OUTPUT_MUTATION &&
       request->output_mode != LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION) ||
      (request->mutation != NULL &&
       request->output_mode != LQL_STREAM_OUTPUT_MUTATION &&
       request->output_mode != LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION)) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream output mode is not implemented");
    return LQL_STATUS_UNSUPPORTED;
  }
  if ((request->output_mode == LQL_STREAM_OUTPUT_SELECTED_RECORD ||
       request->output_mode == LQL_STREAM_OUTPUT_PROJECTION ||
       request->output_mode == LQL_STREAM_OUTPUT_MUTATION ||
       request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION) &&
      request->writer == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "record output requires a writer");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (request->output_mode == LQL_STREAM_OUTPUT_PROJECTION &&
      request->projection == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection output requires a projection handle");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (request->output_mode == LQL_STREAM_OUTPUT_MUTATION &&
      request->mutation == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "mutation output requires a mutation handle");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION &&
      (request->projection == NULL || request->mutation == NULL)) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "combined output requires projection and mutation handles");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (request->limits.max_bytes != 0u) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream probe does not yet support byte limits");
    return LQL_STATUS_UNSUPPORTED;
  }
  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.request = request;
  state.result = result;
  lql_error_init(&state.failure);
  status = stream_program_get(self, request->selector, &state.program,
                              &state.failure);
  if (status != LQL_STATUS_OK) {
    if (error != NULL) {
      *error = state.failure;
    }
    return status;
  }
  if (state.program != NULL && state.program->needs_execution_clock) {
    state.execution_now_ready = lql_temporal_now(&state.execution_now);
    state.execution_today_ready = lql_temporal_today(&state.execution_today);
    state.execution_yesterday_ready =
        lql_temporal_yesterday(&state.execution_yesterday);
  }
  mapped_projection = stream_projection_mapped_eligible(&state);
  lonejson_error_init(&lonejson_error);
  runtime = state.program != NULL && !state.program->match_all &&
                !state.program->requires_generic_path &&
                (request->output_mode == LQL_STREAM_OUTPUT_DECISION_ONLY ||
                 mapped_projection)
                ? lql_lonejson_new_mapped_stream(self, &lonejson_error)
                : lql_lonejson_new(self, &lonejson_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY,
                  lonejson_error.message[0] == '\0'
                      ? "lonejson initialization failed"
                      : lonejson_error.message);
    return LQL_STATUS_NO_MEMORY;
  }
  if ((state.program != NULL && state.program->requires_generic_path) ||
      request->output_mode == LQL_STREAM_OUTPUT_SELECTED_RECORD ||
      (request->output_mode == LQL_STREAM_OUTPUT_PROJECTION &&
       !mapped_projection) ||
      request->output_mode == LQL_STREAM_OUTPUT_MUTATION ||
      request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION) {
    status = stream_execute_generic_path(&state, runtime, error);
    lonejson_free(runtime);
    return status;
  }
  if (state.program != NULL && !state.program->match_all) {
    status = stream_execute_mapped(&state, runtime, error);
    lonejson_free(runtime);
    return status;
  }
  options = lonejson_default_candidate_stream_options();
  options.framing = LONEJSON_CANDIDATE_FRAMING_NDJSON;
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_NONE;
  visitor = lonejson_default_value_visitor();
  if (state.program == NULL || state.program->match_all) {
    visitor.object_begin = stream_match_all_object_begin;
    visitor.array_begin = stream_match_all_array_begin;
  } else {
    visitor.object_begin = stream_object_begin;
    visitor.object_end = stream_object_end;
    visitor.object_key_begin = stream_key_begin;
    visitor.object_key_chunk = stream_key_chunk;
    visitor.object_key_end = stream_key_end;
    visitor.array_begin = stream_array_begin;
    visitor.array_end = stream_array_end;
    visitor.string_begin = stream_string_begin;
    visitor.string_chunk = stream_string_chunk;
    visitor.number_begin = stream_scalar_value;
    visitor.boolean_value = stream_boolean_value;
    visitor.null_value = stream_scalar_value;
  }
  options.visitor = &visitor;
  options.visitor_user = &state;
  options.candidate_begin = stream_candidate_begin;
  options.candidate_end = stream_candidate_end;
  options.candidate_user = &state;
  status = stream_status(&state,
                         lonejson_visit_candidates_reader(runtime, stream_read,
                                                          &state, &options,
                                                          &lonejson_error),
                         &lonejson_error, error);
  lonejson_free(runtime);
  return status;
}
