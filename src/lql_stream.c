#include "lql_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

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

struct lql_stream_program {
  lql_stream_term terms[sizeof(unsigned long) * CHAR_BIT];
  size_t term_count;
  lql_stream_field fields[sizeof(unsigned long) * CHAR_BIT];
  size_t field_count;
  int has_nested_paths;
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
} lql_stream_state;

typedef struct lql_stream_member_context {
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
} lql_stream_member_context;

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

static lql_status stream_field_get(lql_stream_program *program,
                                   const char *key, size_t key_len,
                                   size_t *out, lql_error *error) {
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
                                     lql_selector_kind kind,
                                     const char *key, size_t key_len,
                                     const char *value, size_t value_len,
                                     lql_error *error) {
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
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream supports only non-temporal literal JSON pointer fields");
    return LQL_STATUS_UNSUPPORTED;
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
      status = stream_append_term(program, allocator, selector,
                                  LQL_SELECTOR_KIND_EQ, key, strlen(key),
                                  selector->any[i], selector->any_lens[i], error);
      if (status != LQL_STATUS_OK) {
        return status;
      }
    }
    return LQL_STATUS_OK;
  }
  if (kind == LQL_SELECTOR_KIND_EXISTS) {
    return stream_append_term(program, allocator, selector, kind, key, i,
                              NULL, 0u, error);
  }
  if (kind == LQL_SELECTOR_KIND_RANGE) {
    return stream_append_term(program, allocator, selector, kind, key, i,
                              NULL, 0u, error);
  }
  if (kind == LQL_SELECTOR_KIND_DATE) {
    if (selector->since_macro != LQL_SINCE_NONE) {
      program->needs_execution_clock = 1;
    }
    return stream_append_term(program, allocator, selector, kind, key, i,
                              NULL, 0u, error);
  }
  if (kind != LQL_SELECTOR_KIND_CONTAINS && kind != LQL_SELECTOR_KIND_PREFIX) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream selector predicate is not implemented");
    return LQL_STATUS_UNSUPPORTED;
  }
  if (selector->ignore_case) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream case-insensitive string predicates are not implemented");
    return LQL_STATUS_UNSUPPORTED;
  }
  if (!selector->value_set && selector->any_count == 0u &&
      (selector->value == NULL || selector->value[0] == '\0')) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream omitted string predicate values are not implemented");
    return LQL_STATUS_UNSUPPORTED;
  }
  if (selector->any_count != 0u) {
    if (kind != LQL_SELECTOR_KIND_CONTAINS) {
      lql_set_error(error, LQL_STATUS_UNSUPPORTED,
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
    since = state->execution_yesterday_ready ? &state->execution_yesterday
                                              : NULL;
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
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "stream program out required");
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
  program = (lql_stream_program *)allocator->calloc(allocator, 1u,
                                                     sizeof(*program));
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
      field->json_key_last = stream_field->key_len == 0u
                                 ? 0u
                                 : (unsigned char)stream_field->key[
                                       stream_field->key_len - 1u];
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

static lonejson_candidate_callback_result
stream_candidate_begin(void *user, const lonejson_candidate_info *candidate,
                       lonejson_error *error) {
  lql_stream_state *state;
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
  (void)candidate;
  state = (lql_stream_state *)user;
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

static lonejson_status stream_key_chunk(void *user, const char *data, size_t len,
                                        lonejson_error *error) {
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

static lonejson_status stream_mapped_path_object_begin(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  (void)error;
  member = (lql_stream_member_context *)user;
  if (path->segment_count < LQL_STREAM_PATH_CAPACITY) {
    member->path_containers[path->segment_count] = LQL_STREAM_PATH_OBJECT;
  }
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    if (term->field_index == member->field_index &&
        term->kind == LQL_SELECTOR_KIND_EXISTS &&
        stream_term_path_matches(term, path, member)) {
      member->state->hits |= term->bit;
    }
  }
  ++member->container_depth;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_mapped_path_object_end(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
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

static lonejson_status stream_mapped_path_array_begin(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  (void)error;
  member = (lql_stream_member_context *)user;
  if (path->segment_count < LQL_STREAM_PATH_CAPACITY) {
    member->path_containers[path->segment_count] = LQL_STREAM_PATH_ARRAY;
  }
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    if (term->field_index == member->field_index &&
        term->kind == LQL_SELECTOR_KIND_EXISTS &&
        stream_term_path_matches(term, path, member)) {
      member->state->hits |= term->bit;
    }
  }
  ++member->container_depth;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_mapped_path_array_end(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  return stream_mapped_path_object_end(user, path, error);
}

static lonejson_status stream_mapped_path_string_begin(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  (void)error;
  member = (lql_stream_member_context *)user;
  member->active_string = 1;
  member->active_terms = 0ul;
  member->active_temporal_terms = 0ul;
  member->mismatches = 0ul;
  member->temporal_len = 0u;
  member->temporal_overflow = 0;
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    if (term->field_index != member->field_index ||
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

static lonejson_status stream_mapped_path_string_chunk(
    void *user, const lonejson_value_path *path, const char *data, size_t len,
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

static lonejson_status stream_mapped_path_string_end(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  (void)error;
  member = (lql_stream_member_context *)user;
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

static lonejson_status stream_mapped_path_present(
    void *user, const lonejson_value_path *path, lonejson_error *error);

static lonejson_status stream_mapped_path_number_begin(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  lonejson_status status;
  status = stream_mapped_path_present(user, path, error);
  if (status != LONEJSON_STATUS_OK) {
    return status;
  }
  member = (lql_stream_member_context *)user;
  member->number_len = 0u;
  member->number_overflow = 0;
  member->active_number_terms = 0ul;
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    if (term->field_index == member->field_index &&
        (term->kind == LQL_SELECTOR_KIND_RANGE ||
         (term->kind == LQL_SELECTOR_KIND_EQ &&
          !term->selector->value_is_temporal)) &&
        stream_term_path_matches(term, path, member)) {
      member->active_number_terms |= term->bit;
    }
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_mapped_path_number_chunk(
    void *user, const lonejson_value_path *path, const char *data, size_t len,
    lonejson_error *error) {
  lql_stream_member_context *member;
  (void)path;
  (void)error;
  member = (lql_stream_member_context *)user;
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

static lonejson_status stream_mapped_path_number_end(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
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

static lonejson_status stream_mapped_path_present(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  lql_stream_member_context *member;
  size_t i;
  (void)error;
  member = (lql_stream_member_context *)user;
  for (i = 0u; i < member->state->program->term_count; ++i) {
    const lql_stream_term *term = &member->state->program->terms[i];
    if (term->field_index == member->field_index &&
        term->kind == LQL_SELECTOR_KIND_EXISTS &&
        stream_term_path_matches(term, path, member)) {
      member->state->hits |= term->bit;
    }
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_mapped_path_boolean_value(
    void *user, const lonejson_value_path *path, int value,
    lonejson_error *error) {
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
    if (term->field_index == member->field_index &&
        term->kind == LQL_SELECTOR_KIND_EQ && !term->selector->value_is_temporal &&
        stream_term_path_matches(term, path, member) &&
        ((value && strcmp(term->value, "true") == 0) ||
         (!value && strcmp(term->value, "false") == 0))) {
      member->state->hits |= term->bit;
    }
  }
  return LONEJSON_STATUS_OK;
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

static lql_status stream_execute_mapped(lql_stream_state *state,
                                        lonejson *runtime, lql_error *error) {
  lonejson_json_value values[sizeof(unsigned long) * CHAR_BIT];
  lql_stream_member_context members[sizeof(unsigned long) * CHAR_BIT];
  lonejson_value_visitor top_visitor;
  lonejson_path_value_visitor path_visitor;
  lonejson_stream *stream;
  lonejson_stream_result stream_result;
  lonejson_error lonejson_error;
  lonejson_status lonejson_status;
  lonejson_candidate_callback_result callback_result;
  lql_status status;
  size_t i;

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
  lonejson_error_init(&lonejson_error);
  for (i = 0u; i < state->program->field_count; ++i) {
    memset(&members[i], 0, sizeof(members[i]));
    members[i].state = state;
    members[i].field_index = i;
    lonejson_json_value_init(runtime, &values[i]);
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
      runtime, &state->program->mapped_map, stream_read, state, &lonejson_error);
  if (stream == NULL) {
    for (i = 0u; i < state->program->field_count; ++i) {
      lonejson_json_value_cleanup(&values[i]);
    }
    return stream_status(state, lonejson_error.code, &lonejson_error, error);
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
  if (status == LQL_STATUS_OK && state->failure.code != LQL_STATUS_OK) {
    if (error != NULL) {
      *error = state->failure;
    }
    status = state->failure.code;
  }
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
  if (request->output_mode != LQL_STREAM_OUTPUT_DECISION_ONLY ||
      request->writer != NULL || request->projection != NULL ||
      request->mutation != NULL) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream probe currently supports decision-only output");
    return LQL_STATUS_UNSUPPORTED;
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
  lonejson_error_init(&lonejson_error);
  runtime = state.program != NULL && !state.program->match_all
                ? lql_lonejson_new_mapped_stream(self, &lonejson_error)
                : lql_lonejson_new(self, &lonejson_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY,
                  lonejson_error.message[0] == '\0' ? "lonejson initialization failed"
                                                     : lonejson_error.message);
    return LQL_STATUS_NO_MEMORY;
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
