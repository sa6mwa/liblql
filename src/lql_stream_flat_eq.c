#include "lql_internal.h"
#include "lql_json_scan.h"
#include "lql_unicode_lower.h"

#include <limits.h>
#include <string.h>

#define LQL_FLAT_EQ_TERM_CAPACITY (sizeof(unsigned long) * CHAR_BIT)
#define LQL_FLAT_CONTAINS_NEEDLE_MAX 256u
#define LQL_FLAT_ICONTAINS_NEEDLE_MAX (LQL_FLAT_CONTAINS_NEEDLE_MAX * 2u)

typedef struct lql_flat_eq_program {
  lql_json_flat_eq_term terms[LQL_FLAT_EQ_TERM_CAPACITY];
  const lql_selector *selectors[LQL_FLAT_EQ_TERM_CAPACITY];
  size_t contains_failures[LQL_FLAT_EQ_TERM_CAPACITY]
                          [LQL_FLAT_ICONTAINS_NEEDLE_MAX];
  char icontains_needles[LQL_FLAT_EQ_TERM_CAPACITY]
                        [LQL_FLAT_ICONTAINS_NEEDLE_MAX];
  size_t term_count;
  int stop_matching_on_hit;
} lql_flat_eq_program;

typedef struct lql_flat_eq_state {
  const lql_stream_request *request;
  lql_stream_result *result;
  const lql_flat_eq_program *program;
} lql_flat_eq_state;

static int lql_flat_eq_literal_kind(lql_selector_literal_kind literal_kind,
                                    lql_json_flat_term_kind *out) {
  if (out == NULL) {
    return 0;
  }
  switch (literal_kind) {
  case LQL_SELECTOR_LITERAL_STRING:
    *out = LQL_JSON_FLAT_TERM_EQ;
    return 1;
  case LQL_SELECTOR_LITERAL_NUMBER:
    *out = LQL_JSON_FLAT_TERM_NUMBER_EQ;
    return 1;
  case LQL_SELECTOR_LITERAL_BOOL:
    *out = LQL_JSON_FLAT_TERM_BOOL_EQ;
    return 1;
  default:
    return 0;
  }
}

static int lql_flat_eq_literal_object_path(
    const char *path, size_t *out_path_len, size_t *out_segment_count,
    size_t *out_first_segment_len, unsigned long *out_array_segments,
    unsigned long *out_object_wildcards, unsigned long *out_array_wildcards,
    unsigned long *out_any_wildcards, unsigned long *out_recursive_segments) {
  const char *segment;
  const char *slash;
  size_t path_len;
  size_t segment_len;
  size_t segment_count;
  size_t i;
  unsigned long array_segments;
  unsigned long object_wildcards;
  unsigned long array_wildcards;
  unsigned long any_wildcards;
  unsigned long recursive_segments;
  int numeric_segment;
  if (path == NULL || path[0] != '/' || path[1] == '\0' ||
      out_path_len == NULL || out_segment_count == NULL ||
      out_first_segment_len == NULL || out_array_segments == NULL ||
      out_object_wildcards == NULL || out_array_wildcards == NULL ||
      out_any_wildcards == NULL || out_recursive_segments == NULL) {
    return 0;
  }
  path_len = strlen(path);
  segment = path + 1;
  segment_count = 0u;
  array_segments = 0ul;
  object_wildcards = 0ul;
  array_wildcards = 0ul;
  any_wildcards = 0ul;
  recursive_segments = 0ul;
  for (;;) {
    slash = strchr(segment, '/');
    segment_len = slash == NULL ? strlen(segment) : (size_t)(slash - segment);
    numeric_segment = segment_len != 0u;
    for (i = 0u; i < segment_len; ++i) {
      if (segment[i] < '0' || segment[i] > '9') {
        numeric_segment = 0;
        break;
      }
    }
    if (segment_len == 0u || memchr(segment, '~', segment_len) != NULL ||
        segment_count >= sizeof(unsigned long) * CHAR_BIT ||
        (numeric_segment && slash == NULL) ||
        ((segment_len == 1u && segment[0] == '*') && slash == NULL) ||
        ((segment_len == 2u && segment[0] == '[' && segment[1] == ']') &&
         slash == NULL) ||
        ((segment_len == 2u && segment[0] == '*' && segment[1] == '*') &&
         slash == NULL) ||
        ((segment_len == 3u && memcmp(segment, "...", 3u) == 0) &&
         slash == NULL)) {
      return 0;
    }
    if (segment_count == 0u) {
      *out_first_segment_len = segment_len;
    }
    if (numeric_segment) {
      array_segments |= 1ul << segment_count;
    } else if (segment_len == 1u && segment[0] == '*') {
      object_wildcards |= 1ul << segment_count;
    } else if (segment_len == 2u && segment[0] == '[' && segment[1] == ']') {
      array_wildcards |= 1ul << segment_count;
    } else if (segment_len == 2u && segment[0] == '*' && segment[1] == '*') {
      any_wildcards |= 1ul << segment_count;
    } else if (segment_len == 3u && memcmp(segment, "...", 3u) == 0) {
      recursive_segments |= 1ul << segment_count;
    }
    ++segment_count;
    if (slash == NULL) {
      break;
    }
    segment = slash + 1;
  }
  *out_path_len = path_len;
  *out_segment_count = segment_count;
  *out_array_segments = array_segments;
  *out_object_wildcards = object_wildcards;
  *out_array_wildcards = array_wildcards;
  *out_any_wildcards = any_wildcards;
  if (recursive_segments != 0ul) {
    size_t recursive_index;
    recursive_index = 0u;
    while ((recursive_segments & (1ul << recursive_index)) == 0ul) {
      ++recursive_index;
    }
    if ((recursive_segments & (recursive_segments - 1ul)) != 0ul ||
        array_segments != 0ul || object_wildcards != 0ul ||
        array_wildcards != 0ul || any_wildcards != 0ul ||
        recursive_index + 2u != segment_count) {
      return 0;
    }
  }
  *out_recursive_segments = recursive_segments;
  return 1;
}

static int lql_flat_eq_contains_failure(lql_flat_eq_program *program,
                                        lql_json_flat_eq_term *term) {
  size_t i;
  size_t matched;
  if (program == NULL || term == NULL || term->value == NULL ||
      term->value_len == 0u ||
      term->value_len > LQL_FLAT_ICONTAINS_NEEDLE_MAX ||
      program->term_count == LQL_FLAT_EQ_TERM_CAPACITY) {
    return 0;
  }
  term->contains_failure = program->contains_failures[program->term_count];
  term->contains_failure[0] = 0u;
  matched = 0u;
  for (i = 1u; i < term->value_len; ++i) {
    while (matched != 0u && term->value[i] != term->value[matched]) {
      matched = term->contains_failure[matched - 1u];
    }
    if (term->value[i] == term->value[matched]) {
      ++matched;
    }
    term->contains_failure[i] = matched;
  }
  return 1;
}

static int lql_flat_eq_append_contains(lql_flat_eq_program *program,
                                       const lql_selector *selector,
                                       const char *field, size_t path_len,
                                       size_t path_segment_count,
                                       size_t first_segment_len,
                                       unsigned long path_array_segments,
                                       unsigned long path_object_wildcards,
                                       unsigned long path_array_wildcards,
                                       unsigned long path_any_wildcards,
                                       unsigned long path_recursive_segments,
                                       int ignore_case) {
  size_t count;
  size_t i;
  if (program == NULL || selector == NULL || field == NULL) {
    return 0;
  }
  count = selector->any_count == 0u ? 1u : selector->any_count;
  if (count > LQL_FLAT_EQ_TERM_CAPACITY - program->term_count ||
      (selector->any_count != 0u &&
       (selector->any == NULL || selector->any_lens == NULL ||
        selector->any_kinds == NULL)) ||
      (selector->any_count == 0u &&
       (!selector->value_set || selector->value_is_temporal ||
        selector->value_kind != LQL_SELECTOR_LITERAL_STRING ||
        selector->value == NULL))) {
    return 0;
  }
  for (i = 0u; i < count; ++i) {
    lql_json_flat_eq_term *term;
    const char *value;
    size_t value_len;
    if (selector->any_count != 0u) {
      if (selector->any_kinds[i] != LQL_SELECTOR_LITERAL_STRING) {
        return 0;
      }
      value = selector->any[i];
      value_len = selector->any_lens[i];
    } else {
      value = selector->value;
      value_len = strlen(value);
    }
    if (value == NULL || value_len == 0u ||
        value_len > LQL_FLAT_CONTAINS_NEEDLE_MAX) {
      return 0;
    }
    term = &program->terms[program->term_count];
    term->kind = ignore_case ? LQL_JSON_FLAT_TERM_ICONTAINS
                             : LQL_JSON_FLAT_TERM_CONTAINS;
    term->field = field + 1;
    term->field_len = first_segment_len;
    if (ignore_case) {
      if (!lql_unicode_utf8_lower(
              value, value_len, program->icontains_needles[program->term_count],
              LQL_FLAT_ICONTAINS_NEEDLE_MAX, &term->value_len) ||
          term->value_len == 0u) {
        return 0;
      }
      term->value = program->icontains_needles[program->term_count];
    } else {
      term->value = value;
      term->value_len = value_len;
    }
    term->path = field;
    term->path_len = path_len;
    term->path_segment_count = path_segment_count;
    term->path_array_segments = path_array_segments;
    term->path_object_wildcards = path_object_wildcards;
    term->path_array_wildcards = path_array_wildcards;
    term->path_any_wildcards = path_any_wildcards;
    term->path_recursive_segments = path_recursive_segments;
    if (!lql_flat_eq_contains_failure(program, term)) {
      return 0;
    }
    program->selectors[program->term_count] = selector;
    ++program->term_count;
  }
  return 1;
}

static int lql_flat_eq_append(lql_flat_eq_program *program,
                              const lql_selector *selector) {
  lql_json_flat_eq_term *term;
  lql_json_flat_term_kind term_kind;
  const char *field;
  size_t path_len;
  size_t path_segment_count;
  size_t first_segment_len;
  unsigned long path_array_segments;
  unsigned long path_object_wildcards;
  unsigned long path_array_wildcards;
  unsigned long path_any_wildcards;
  unsigned long path_recursive_segments;
  size_t i;
  if (selector == NULL) {
    return 0;
  }
  if (selector->kind == LQL_SELECTOR_KIND_ALL) {
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_AND ||
      selector->kind == LQL_SELECTOR_KIND_OR ||
      selector->kind == LQL_SELECTOR_KIND_NOT) {
    for (i = 0u; i < selector->child_count; ++i) {
      if (!lql_flat_eq_append(program, &selector->children[i])) {
        return 0;
      }
    }
    return selector->kind != LQL_SELECTOR_KIND_NOT ||
           selector->child_count == 1u;
  }
  if (selector->kind == LQL_SELECTOR_KIND_IN) {
    if (selector->field == NULL || selector->any_count == 0u ||
        selector->any_kinds == NULL) {
      return 0;
    }
    field = selector->field;
    if (!lql_flat_eq_literal_object_path(
            field, &path_len, &path_segment_count, &first_segment_len,
            &path_array_segments, &path_object_wildcards, &path_array_wildcards,
            &path_any_wildcards, &path_recursive_segments)) {
      return 0;
    }
    for (i = 0u; i < selector->any_count; ++i) {
      if (!lql_flat_eq_literal_kind(selector->any_kinds[i], &term_kind) ||
          program->term_count == LQL_FLAT_EQ_TERM_CAPACITY) {
        return 0;
      }
      term = &program->terms[program->term_count];
      term->kind = term_kind;
      term->field = field + 1;
      term->field_len = first_segment_len;
      term->value = selector->any[i];
      term->value_len = selector->any_lens[i];
      term->path = field;
      term->path_len = path_len;
      term->path_segment_count = path_segment_count;
      term->path_array_segments = path_array_segments;
      term->path_object_wildcards = path_object_wildcards;
      term->path_array_wildcards = path_array_wildcards;
      term->path_any_wildcards = path_any_wildcards;
      term->path_recursive_segments = path_recursive_segments;
      program->selectors[program->term_count] = selector;
      ++program->term_count;
    }
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_CONTAINS ||
      selector->kind == LQL_SELECTOR_KIND_ICONTAINS) {
    if (selector->field == NULL) {
      return 0;
    }
    field = selector->field;
    if (!lql_flat_eq_literal_object_path(
            field, &path_len, &path_segment_count, &first_segment_len,
            &path_array_segments, &path_object_wildcards,
            &path_array_wildcards, &path_any_wildcards,
            &path_recursive_segments)) {
      return 0;
    }
    return lql_flat_eq_append_contains(
        program, selector, field, path_len, path_segment_count,
        first_segment_len, path_array_segments, path_object_wildcards,
        path_array_wildcards, path_any_wildcards, path_recursive_segments,
        selector->kind == LQL_SELECTOR_KIND_ICONTAINS || selector->ignore_case);
  }
  if ((selector->kind != LQL_SELECTOR_KIND_EQ &&
       selector->kind != LQL_SELECTOR_KIND_EXISTS &&
       selector->kind != LQL_SELECTOR_KIND_PREFIX) ||
      selector->field == NULL ||
      program->term_count == LQL_FLAT_EQ_TERM_CAPACITY) {
    return 0;
  }
  field = selector->field;
  if (!lql_flat_eq_literal_object_path(
          field, &path_len, &path_segment_count, &first_segment_len,
          &path_array_segments, &path_object_wildcards, &path_array_wildcards,
          &path_any_wildcards, &path_recursive_segments)) {
    return 0;
  }
  term = &program->terms[program->term_count];
  term->field = field + 1;
  term->field_len = first_segment_len;
  term->path = field;
  term->path_len = path_len;
  term->path_segment_count = path_segment_count;
  term->path_array_segments = path_array_segments;
  term->path_object_wildcards = path_object_wildcards;
  term->path_array_wildcards = path_array_wildcards;
  term->path_any_wildcards = path_any_wildcards;
  term->path_recursive_segments = path_recursive_segments;
  if (selector->kind == LQL_SELECTOR_KIND_EXISTS) {
    term->kind = LQL_JSON_FLAT_TERM_EXISTS;
  } else if (selector->kind == LQL_SELECTOR_KIND_PREFIX) {
    term->kind = LQL_JSON_FLAT_TERM_PREFIX;
  } else if (!lql_flat_eq_literal_kind(selector->value_kind, &term->kind)) {
    return 0;
  }
  if (term->kind != LQL_JSON_FLAT_TERM_EXISTS) {
    if (!selector->value_set || selector->value_is_temporal ||
        selector->value == NULL ||
        (term->kind == LQL_JSON_FLAT_TERM_PREFIX &&
         selector->value_kind != LQL_SELECTOR_LITERAL_STRING)) {
      return 0;
    }
    term->value = selector->value;
    term->value_len = strlen(selector->value);
  }
  program->selectors[program->term_count] = selector;
  ++program->term_count;
  return 1;
}

static int lql_flat_eq_matches(const lql_flat_eq_program *program,
                               const lql_selector *selector,
                               unsigned long hits) {
  size_t i;
  if (selector == NULL) {
    return 0;
  }
  if (selector->kind == LQL_SELECTOR_KIND_ALL) {
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_AND) {
    for (i = 0u; i < selector->child_count; ++i) {
      if (!lql_flat_eq_matches(program, &selector->children[i], hits)) {
        return 0;
      }
    }
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_OR) {
    for (i = 0u; i < selector->child_count; ++i) {
      if (lql_flat_eq_matches(program, &selector->children[i], hits)) {
        return 1;
      }
    }
    return 0;
  }
  if (selector->kind == LQL_SELECTOR_KIND_NOT) {
    return selector->child_count == 1u &&
           !lql_flat_eq_matches(program, &selector->children[0], hits);
  }
  for (i = 0u; i < program->term_count; ++i) {
    if (program->selectors[i] == selector) {
      if ((hits & (1ul << i)) != 0ul) {
        return 1;
      }
    }
  }
  return 0;
}

static lql_status lql_flat_eq_emit(lql_flat_eq_state *state,
                                   const lql_json_spool *spool,
                                   lql_error *error) {
  lql_status status;
  static const char newline[] = "\n";
  status = lql_json_spool_write_to(spool, state->request->writer,
                                   state->request->writer_user, error);
  if (status != LQL_STATUS_OK) {
    if (error != NULL && error->code == LQL_STATUS_OK) {
      lql_set_error(error, status, "stream writer failed");
    }
    return status;
  }
  status = state->request->writer(state->request->writer_user, newline,
                                  sizeof(newline) - 1u, error);
  if (status != LQL_STATUS_OK && error != NULL &&
      error->code == LQL_STATUS_OK) {
    lql_set_error(error, status, "stream writer failed");
  }
  return status;
}

static lql_status lql_flat_eq_record(void *user, size_t record_index,
                                     int root_is_object, unsigned long hits,
                                     const lql_json_spool *spool,
                                     lql_error *error) {
  lql_flat_eq_state *state;
  lql_stream_decision decision;
  lql_stream_value value;
  lql_stream_callback_result callback_result;
  lql_status status;
  int matched;
  state = (lql_flat_eq_state *)user;
  if (state == NULL) {
    lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                  "flat equality scanner callback is unavailable");
    return LQL_STATUS_CALLBACK_ERROR;
  }
  matched = lql_flat_eq_matches(state->program, state->request->selector, hits);
  ++state->result->records_seen;
  if (matched) {
    ++state->result->records_matched;
  }
  if (state->request->on_decision != NULL) {
    decision.record_index = record_index;
    decision.matched = matched;
    callback_result = state->request->on_decision(state->request->decision_user,
                                                  &decision, error);
    if (callback_result == LQL_STREAM_CALLBACK_ERROR) {
      if (error != NULL && error->code == LQL_STATUS_OK) {
        lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                      "stream decision callback failed");
      }
      return LQL_STATUS_CALLBACK_ERROR;
    }
    if (callback_result == LQL_STREAM_CALLBACK_STOP) {
      state->result->stopped_early = 1;
      state->result->stop_reason = LQL_STREAM_STOP_CALLBACK;
      return LQL_STATUS_STOP;
    }
  }
  if (matched && state->request->on_value != NULL) {
    if (spool == NULL) {
      lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                    "matched value capture is unavailable");
      return LQL_STATUS_CALLBACK_ERROR;
    }
    memset(&value, 0, sizeof(value));
    value.storage_kind = LQL_STREAM_VALUE_JSON_SPOOL;
    value.spool = spool;
    callback_result =
        state->request->on_value(state->request->value_user, &value, error);
    if (callback_result == LQL_STREAM_CALLBACK_ERROR) {
      if (error != NULL && error->code == LQL_STATUS_OK) {
        lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                      "stream value callback failed");
      }
      return LQL_STATUS_CALLBACK_ERROR;
    }
    if (callback_result == LQL_STREAM_CALLBACK_STOP) {
      state->result->stopped_early = 1;
      state->result->stop_reason = LQL_STREAM_STOP_CALLBACK;
      return LQL_STATUS_STOP;
    }
  }
  if (state->request->output_mode == LQL_STREAM_OUTPUT_SELECTED_RECORD &&
      (matched || !state->request->matched_only)) {
    if (spool == NULL) {
      lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                    "selected record capture is unavailable");
      return LQL_STATUS_CALLBACK_ERROR;
    }
    status = lql_flat_eq_emit(state, spool, error);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  if (matched && state->request->limits.max_matches != 0u &&
      state->result->records_matched >= state->request->limits.max_matches) {
    state->result->stopped_early = 1;
    state->result->stop_reason = LQL_STREAM_STOP_MATCH_LIMIT;
    return LQL_STATUS_STOP;
  }
  (void)root_is_object;
  return LQL_STATUS_OK;
}

static int lql_flat_eq_eligible(const lql_stream_request *request) {
  if (request == NULL || request->selector == NULL ||
      request->limits.max_records != 0u || request->projection != NULL ||
      request->mutation != NULL ||
      (request->output_mode != LQL_STREAM_OUTPUT_DECISION_ONLY &&
       request->output_mode != LQL_STREAM_OUTPUT_SELECTED_RECORD)) {
    return 0;
  }
  return 1;
}

lql_status lql_stream_execute_flat_eq(lql *self,
                                      const lql_stream_request *request,
                                      lql_stream_result *result,
                                      lql_error *error, int *out_handled) {
  lql_json_flat_eq_request scan_request;
  lql_json_spool spool;
  lql_flat_eq_state state;
  lql_flat_eq_program program;
  lql_status status;
  size_t records;
  size_t bytes_read;
  int capture;
  (void)self;
  if (out_handled == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "flat equality state required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out_handled = 0;
  if (!lql_flat_eq_eligible(request)) {
    return LQL_STATUS_OK;
  }
  memset(&program, 0, sizeof(program));
  if (!lql_flat_eq_append(&program, request->selector)) {
    return LQL_STATUS_OK;
  }
  program.stop_matching_on_hit =
      request->selector->kind == LQL_SELECTOR_KIND_EQ &&
      program.term_count == 1u;
  *out_handled = 1;
  capture = request->on_value != NULL ||
            request->output_mode == LQL_STREAM_OUTPUT_SELECTED_RECORD;
  memset(&spool, 0, sizeof(spool));
  if (capture) {
    status = lql_json_spool_init(&spool, error);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  memset(&state, 0, sizeof(state));
  state.request = request;
  state.result = result;
  state.program = &program;
  memset(&scan_request, 0, sizeof(scan_request));
  scan_request.reader = request->reader;
  scan_request.reader_user = request->reader_user;
  scan_request.terms = program.terms;
  scan_request.term_count = program.term_count;
  scan_request.spool = capture ? &spool : NULL;
  scan_request.capture = capture;
  scan_request.stop_matching_on_hit = program.stop_matching_on_hit;
  scan_request.record = lql_flat_eq_record;
  scan_request.record_user = &state;
  status =
      lql_json_scan_flat_eq_ndjson(&scan_request, &records, &bytes_read, error);
  result->bytes_consumed = bytes_read;
  if (capture) {
    lql_json_spool_cleanup(&spool);
  }
  if (status == LQL_STATUS_STOP) {
    return LQL_STATUS_OK;
  }
  return status;
}
