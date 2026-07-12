#include "lql_internal.h"
#include "lql_json_scan.h"
#include "lql_unicode_lower.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LQL_FLAT_EQ_TERM_CAPACITY (sizeof(unsigned long) * CHAR_BIT)
#define LQL_FLAT_CONTAINS_NEEDLE_MAX 256u
#define LQL_FLAT_ICONTAINS_NEEDLE_MAX (LQL_FLAT_CONTAINS_NEEDLE_MAX * 2u)

typedef struct lql_flat_eq_program {
  lql_json_flat_eq_term terms[LQL_FLAT_EQ_TERM_CAPACITY];
  const lql_selector *selectors[LQL_FLAT_EQ_TERM_CAPACITY];
  lql_json_capture_key capture_keys[LQL_FLAT_EQ_TERM_CAPACITY];
  lql_json_capture_span capture_spans[LQL_FLAT_EQ_TERM_CAPACITY];
  size_t capture_key_count;
  size_t contains_failures[LQL_FLAT_EQ_TERM_CAPACITY]
                          [LQL_FLAT_ICONTAINS_NEEDLE_MAX];
  char icontains_needles[LQL_FLAT_EQ_TERM_CAPACITY]
                        [LQL_FLAT_ICONTAINS_NEEDLE_MAX];
  size_t term_count;
  int stop_matching_on_hit;
  const lql_mutation_action *direct_mutation_actions[LQL_FLAT_EQ_TERM_CAPACITY];
  size_t direct_mutation_action_count;
} lql_flat_eq_program;

typedef struct lql_flat_eq_time_bounds {
  lql_temporal now;
  lql_temporal today;
  lql_temporal yesterday;
  int now_ready;
  int today_ready;
  int yesterday_ready;
} lql_flat_eq_time_bounds;

typedef struct lql_flat_eq_state {
  const lql_stream_request *request;
  lql_stream_result *result;
  const lql_flat_eq_program *program;
  lql_stream_writer_fn writer;
  void *writer_user;
} lql_flat_eq_state;

static int lql_flat_eq_selector_needs_time(const lql_selector *selector) {
  size_t i;
  if (selector == NULL) {
    return 0;
  }
  if (selector->kind == LQL_SELECTOR_KIND_DATE &&
      selector->since_macro != LQL_SINCE_NONE) {
    return 1;
  }
  for (i = 0u; i < selector->child_count; ++i) {
    if (lql_flat_eq_selector_needs_time(&selector->children[i])) {
      return 1;
    }
  }
  return 0;
}

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

static int lql_flat_eq_append_contains(
    lql_flat_eq_program *program, const lql_selector *selector,
    const char *field, size_t path_len, size_t path_segment_count,
    size_t first_segment_len, unsigned long path_array_segments,
    unsigned long path_object_wildcards, unsigned long path_array_wildcards,
    unsigned long path_any_wildcards, unsigned long path_recursive_segments,
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
                              const lql_selector *selector,
                              const lql_flat_eq_time_bounds *time_bounds) {
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
      if (!lql_flat_eq_append(program, &selector->children[i], time_bounds)) {
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
            &path_array_segments, &path_object_wildcards, &path_array_wildcards,
            &path_any_wildcards, &path_recursive_segments)) {
      return 0;
    }
    return lql_flat_eq_append_contains(
        program, selector, field, path_len, path_segment_count,
        first_segment_len, path_array_segments, path_object_wildcards,
        path_array_wildcards, path_any_wildcards, path_recursive_segments,
        selector->kind == LQL_SELECTOR_KIND_ICONTAINS || selector->ignore_case);
  }
  if (selector->kind == LQL_SELECTOR_KIND_RANGE) {
    if (selector->field == NULL ||
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
    term->kind = selector->range_is_temporal
                     ? LQL_JSON_FLAT_TERM_TEMPORAL_RANGE
                     : LQL_JSON_FLAT_TERM_NUMBER_RANGE;
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
    term->range_gt = selector->range_gt;
    term->range_gte = selector->range_gte;
    term->range_lt = selector->range_lt;
    term->range_lte = selector->range_lte;
    term->has_range_gt = selector->has_range_gt;
    term->has_range_gte = selector->has_range_gte;
    term->has_range_lt = selector->has_range_lt;
    term->has_range_lte = selector->has_range_lte;
    term->temporal_gt = selector->temporal_gt;
    term->temporal_gte = selector->temporal_gte;
    term->temporal_lt = selector->temporal_lt;
    term->temporal_lte = selector->temporal_lte;
    term->temporal_eq = selector->temporal_eq;
    term->has_temporal_gt = selector->has_temporal_gt;
    term->has_temporal_gte = selector->has_temporal_gte;
    term->has_temporal_lt = selector->has_temporal_lt;
    term->has_temporal_lte = selector->has_temporal_lte;
    term->has_temporal_eq = selector->has_temporal_eq;
    program->selectors[program->term_count] = selector;
    ++program->term_count;
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_DATE) {
    if (selector->field == NULL ||
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
    term->kind = LQL_JSON_FLAT_TERM_TEMPORAL_RANGE;
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
    term->temporal_gt = selector->temporal_gt;
    term->temporal_gte = selector->temporal_gte;
    term->temporal_lt = selector->temporal_lt;
    term->temporal_lte = selector->temporal_lte;
    term->temporal_eq = selector->temporal_eq;
    term->has_temporal_gt = selector->has_temporal_gt;
    term->has_temporal_gte = selector->has_temporal_gte;
    term->has_temporal_lt = selector->has_temporal_lt;
    term->has_temporal_lte = selector->has_temporal_lte;
    term->has_temporal_eq = selector->has_temporal_eq;
    if (selector->since_macro == LQL_SINCE_NOW) {
      if (time_bounds == NULL || !time_bounds->now_ready) {
        return 0;
      }
      term->temporal_gte = time_bounds->now;
      term->has_temporal_gte = 1;
    } else if (selector->since_macro == LQL_SINCE_TODAY) {
      if (time_bounds == NULL || !time_bounds->today_ready) {
        return 0;
      }
      term->temporal_gte = time_bounds->today;
      term->has_temporal_gte = 1;
    } else if (selector->since_macro == LQL_SINCE_YESTERDAY) {
      if (time_bounds == NULL || !time_bounds->yesterday_ready) {
        return 0;
      }
      term->temporal_gte = time_bounds->yesterday;
      term->has_temporal_gte = 1;
    }
    program->selectors[program->term_count] = selector;
    ++program->term_count;
    return 1;
  }
  if ((selector->kind != LQL_SELECTOR_KIND_EQ &&
       selector->kind != LQL_SELECTOR_KIND_EXISTS &&
       selector->kind != LQL_SELECTOR_KIND_PREFIX &&
       selector->kind != LQL_SELECTOR_KIND_IPREFIX) ||
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
  } else if (selector->kind == LQL_SELECTOR_KIND_PREFIX ||
             selector->kind == LQL_SELECTOR_KIND_IPREFIX) {
    term->kind =
        selector->kind == LQL_SELECTOR_KIND_IPREFIX || selector->ignore_case
            ? LQL_JSON_FLAT_TERM_IPREFIX
            : LQL_JSON_FLAT_TERM_PREFIX;
  } else if (!lql_flat_eq_literal_kind(selector->value_kind, &term->kind)) {
    return 0;
  }
  if (term->kind != LQL_JSON_FLAT_TERM_EXISTS) {
    if (!selector->value_set || selector->value_is_temporal ||
        selector->value == NULL ||
        ((term->kind == LQL_JSON_FLAT_TERM_PREFIX ||
          term->kind == LQL_JSON_FLAT_TERM_IPREFIX) &&
         selector->value_kind != LQL_SELECTOR_LITERAL_STRING)) {
      return 0;
    }
    if (term->kind == LQL_JSON_FLAT_TERM_IPREFIX) {
      if (!lql_unicode_utf8_lower(
              selector->value, strlen(selector->value),
              program->icontains_needles[program->term_count],
              LQL_FLAT_ICONTAINS_NEEDLE_MAX, &term->value_len) ||
          term->value_len == 0u) {
        return 0;
      }
      term->value = program->icontains_needles[program->term_count];
    } else {
      term->value = selector->value;
      term->value_len = strlen(selector->value);
    }
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
  status =
      lql_json_spool_write_to(spool, state->writer, state->writer_user, error);
  if (status != LQL_STATUS_OK) {
    if (error != NULL && error->code == LQL_STATUS_OK) {
      lql_set_error(error, status, "stream writer failed");
    }
    return status;
  }
  status =
      state->writer(state->writer_user, newline, sizeof(newline) - 1u, error);
  if (status != LQL_STATUS_OK && error != NULL &&
      error->code == LQL_STATUS_OK) {
    lql_set_error(error, status, "stream writer failed");
  }
  return status;
}

static lql_status lql_flat_eq_write(lql_flat_eq_state *state, const void *data,
                                    size_t len, lql_error *error) {
  lql_status status;
  status = state->writer(state->writer_user, data, len, error);
  if (status != LQL_STATUS_OK && error != NULL && error->code == LQL_STATUS_OK)
    lql_set_error(error, status, "stream writer failed");
  return status;
}

static int lql_flat_eq_projection_prefix(const lql_flat_eq_program *program,
                                         size_t left, size_t right,
                                         size_t depth) {
  size_t i;
  if (depth == 0u)
    return 1;
  for (i = 0u; i < depth; ++i) {
    if (strcmp(program->capture_keys[left].segments[i],
               program->capture_keys[right].segments[i]) != 0)
      return 0;
  }
  return 1;
}

static int
lql_flat_eq_projection_group_found(const lql_flat_eq_program *program,
                                   size_t anchor, size_t depth, size_t member) {
  size_t i;
  const char *segment;
  segment = program->capture_keys[member].segments[depth];
  for (i = 0u; i < program->capture_key_count; ++i) {
    if (program->capture_spans[i].found &&
        program->capture_keys[i].segment_count > depth &&
        lql_flat_eq_projection_prefix(program, anchor, i, depth) &&
        strcmp(program->capture_keys[i].segments[depth], segment) == 0)
      return 1;
  }
  return 0;
}

static int lql_flat_eq_projection_segment_index(const char *segment,
                                                size_t *out) {
  size_t i;
  size_t value;
  if (segment == NULL || segment[0] == '\0' || out == NULL)
    return 0;
  value = 0u;
  for (i = 0u; segment[i] != '\0'; ++i) {
    size_t digit;
    if (segment[i] < '0' || segment[i] > '9')
      return 0;
    digit = (size_t)(segment[i] - '0');
    if (value > ((size_t)-1 - digit) / 10u)
      return 0;
    value = value * 10u + digit;
  }
  *out = value;
  return 1;
}

static int
lql_flat_eq_projection_child_array(const lql_flat_eq_program *program,
                                   size_t anchor, size_t depth) {
  size_t i;
  size_t index;
  for (i = 0u; i < program->capture_key_count; ++i) {
    if (program->capture_spans[i].found &&
        program->capture_keys[i].segment_count > depth &&
        lql_flat_eq_projection_prefix(program, anchor, i, depth))
      return lql_flat_eq_projection_segment_index(
          program->capture_keys[i].segments[depth], &index);
  }
  return 0;
}

static lql_status lql_flat_eq_json_string(lql_flat_eq_state *state,
                                          const char *value, lql_error *error) {
  size_t i;
  lql_status status;
  status = lql_flat_eq_write(state, "\"", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  for (i = 0u; value[i] != '\0'; ++i) {
    unsigned char ch;
    ch = (unsigned char)value[i];
    if (ch == (unsigned char)'"' || ch == (unsigned char)'\\') {
      status = lql_flat_eq_write(state, "\\", 1u, error);
      if (status == LQL_STATUS_OK) {
        status = lql_flat_eq_write(state, value + i, 1u, error);
      }
    } else if (ch < 0x20u) {
      static const char hex[] = "0123456789abcdef";
      char escaped[6];
      escaped[0] = '\\';
      escaped[1] = 'u';
      escaped[2] = '0';
      escaped[3] = '0';
      escaped[4] = hex[ch >> 4u];
      escaped[5] = hex[ch & 0x0fu];
      status = lql_flat_eq_write(state, escaped, sizeof(escaped), error);
    } else {
      status = lql_flat_eq_write(state, value + i, 1u, error);
    }
    if (status != LQL_STATUS_OK)
      return status;
  }
  return lql_flat_eq_write(state, "\"", 1u, error);
}

static lql_status lql_flat_eq_projection_key(lql_flat_eq_state *state,
                                             const char *key,
                                             lql_error *error) {
  lql_status status;
  status = lql_flat_eq_json_string(state, key, error);
  if (status != LQL_STATUS_OK)
    return status;
  return lql_flat_eq_write(state, ":", 1u, error);
}

static lql_status lql_flat_eq_spool_byte(const lql_json_spool *spool,
                                         size_t offset, unsigned char *out,
                                         lql_error *error) {
  return lql_json_spool_byte_at(spool, offset, out, error);
}

static lql_status lql_flat_eq_spool_writer(void *user, const void *data,
                                           size_t len, lql_error *error) {
  return lql_json_spool_append((lql_json_spool *)user, data, len, error);
}

static lql_status lql_flat_eq_skip_string(const lql_json_spool *spool,
                                          size_t *offset, size_t end,
                                          lql_error *error) {
  unsigned char ch;
  lql_status status;
  if (offset == NULL || *offset >= end)
    return LQL_STATUS_JSON_ERROR;
  status = lql_flat_eq_spool_byte(spool, *offset, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (ch != (unsigned char)'"')
    return LQL_STATUS_JSON_ERROR;
  ++*offset;
  while (*offset < end) {
    status = lql_flat_eq_spool_byte(spool, *offset, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    ++*offset;
    if (ch == (unsigned char)'"')
      return LQL_STATUS_OK;
    if (ch == (unsigned char)'\\') {
      if (*offset >= end)
        return LQL_STATUS_JSON_ERROR;
      status = lql_flat_eq_spool_byte(spool, *offset, &ch, error);
      if (status != LQL_STATUS_OK)
        return status;
      ++*offset;
      if (ch == (unsigned char)'u') {
        if (end - *offset < 4u)
          return LQL_STATUS_JSON_ERROR;
        *offset += 4u;
      }
    }
  }
  return LQL_STATUS_JSON_ERROR;
}

static lql_status lql_flat_eq_skip_value(const lql_json_spool *spool,
                                         size_t *offset, size_t end,
                                         lql_error *error) {
  unsigned char ch;
  size_t depth;
  lql_status status;
  if (offset == NULL || *offset >= end)
    return LQL_STATUS_JSON_ERROR;
  status = lql_flat_eq_spool_byte(spool, *offset, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (ch == (unsigned char)'"')
    return lql_flat_eq_skip_string(spool, offset, end, error);
  if (ch != (unsigned char)'{' && ch != (unsigned char)'[') {
    while (*offset < end) {
      status = lql_flat_eq_spool_byte(spool, *offset, &ch, error);
      if (status != LQL_STATUS_OK)
        return status;
      if (ch == (unsigned char)',' || ch == (unsigned char)'}' ||
          ch == (unsigned char)']')
        break;
      ++*offset;
    }
    return LQL_STATUS_OK;
  }
  depth = 0u;
  while (*offset < end) {
    status = lql_flat_eq_spool_byte(spool, *offset, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (ch == (unsigned char)'"') {
      status = lql_flat_eq_skip_string(spool, offset, end, error);
      if (status != LQL_STATUS_OK)
        return status;
      continue;
    }
    ++*offset;
    if (ch == (unsigned char)'{' || ch == (unsigned char)'[')
      ++depth;
    else if (ch == (unsigned char)'}' || ch == (unsigned char)']') {
      if (depth == 0u)
        return LQL_STATUS_JSON_ERROR;
      --depth;
      if (depth == 0u)
        return LQL_STATUS_OK;
    }
  }
  return LQL_STATUS_JSON_ERROR;
}

static int lql_flat_eq_hex_value(unsigned char ch, unsigned char *out) {
  if (ch >= (unsigned char)'0' && ch <= (unsigned char)'9') {
    *out = (unsigned char)(ch - (unsigned char)'0');
    return 1;
  }
  if (ch >= (unsigned char)'a' && ch <= (unsigned char)'f') {
    *out = (unsigned char)(10u + ch - (unsigned char)'a');
    return 1;
  }
  if (ch >= (unsigned char)'A' && ch <= (unsigned char)'F') {
    *out = (unsigned char)(10u + ch - (unsigned char)'A');
    return 1;
  }
  return 0;
}

static lql_status lql_flat_eq_key_equals(const lql_json_spool *spool,
                                         size_t offset, size_t end,
                                         const char *key, int *out,
                                         lql_error *error) {
  size_t key_pos;
  unsigned char ch;
  lql_status status;
  if (out != NULL)
    *out = 0;
  if (key == NULL || out == NULL || offset >= end)
    return LQL_STATUS_JSON_ERROR;
  status = lql_flat_eq_spool_byte(spool, offset, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (ch != (unsigned char)'"')
    return LQL_STATUS_JSON_ERROR;
  ++offset;
  key_pos = 0u;
  while (offset < end) {
    unsigned char decoded;
    status = lql_flat_eq_spool_byte(spool, offset, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    ++offset;
    if (ch == (unsigned char)'"') {
      *out = key[key_pos] == '\0';
      return LQL_STATUS_OK;
    }
    if (ch == (unsigned char)'\\') {
      status = lql_flat_eq_spool_byte(spool, offset, &ch, error);
      if (status != LQL_STATUS_OK)
        return status;
      ++offset;
      if (ch == (unsigned char)'u') {
        unsigned char high;
        unsigned char low;
        unsigned char zero;
        size_t i;
        zero = 0u;
        for (i = 0u; i < 2u; ++i) {
          status = lql_flat_eq_spool_byte(spool, offset + i, &ch, error);
          if (status != LQL_STATUS_OK)
            return status;
          if (!lql_flat_eq_hex_value(ch, &high))
            return LQL_STATUS_JSON_ERROR;
          zero = (unsigned char)((zero << 4u) | high);
        }
        if (zero != 0u)
          return LQL_STATUS_OK;
        status = lql_flat_eq_spool_byte(spool, offset + 2u, &ch, error);
        if (status != LQL_STATUS_OK || !lql_flat_eq_hex_value(ch, &high))
          return status == LQL_STATUS_OK ? LQL_STATUS_JSON_ERROR : status;
        status = lql_flat_eq_spool_byte(spool, offset + 3u, &ch, error);
        if (status != LQL_STATUS_OK || !lql_flat_eq_hex_value(ch, &low))
          return status == LQL_STATUS_OK ? LQL_STATUS_JSON_ERROR : status;
        decoded = (unsigned char)((high << 4u) | low);
        offset += 4u;
      } else if (ch == (unsigned char)'n') {
        decoded = (unsigned char)'\n';
      } else if (ch == (unsigned char)'r') {
        decoded = (unsigned char)'\r';
      } else if (ch == (unsigned char)'t') {
        decoded = (unsigned char)'\t';
      } else if (ch == (unsigned char)'b') {
        decoded = (unsigned char)'\b';
      } else if (ch == (unsigned char)'f') {
        decoded = (unsigned char)'\f';
      } else {
        decoded = ch;
      }
    } else {
      decoded = ch;
    }
    if (key[key_pos] == '\0' || decoded != (unsigned char)key[key_pos])
      return LQL_STATUS_OK;
    ++key_pos;
  }
  return LQL_STATUS_JSON_ERROR;
}

static lql_status lql_flat_eq_mutation_value(lql_flat_eq_state *state,
                                             const lql_mutation_action *action,
                                             lql_error *error) {
  if (action->value_kind == LQL_MUTATION_VALUE_STRING)
    return lql_flat_eq_json_string(state, action->value, error);
  if (action->value_kind == LQL_MUTATION_VALUE_BOOL ||
      action->value_kind == LQL_MUTATION_VALUE_NUMBER ||
      action->value_kind == LQL_MUTATION_VALUE_NULL)
    return lql_flat_eq_write(state, action->value, strlen(action->value),
                             error);
  return LQL_STATUS_INVALID_ARGUMENT;
}

static lql_status
lql_flat_eq_mutation_nested_object(lql_flat_eq_state *state,
                                   const lql_mutation_action *action,
                                   lql_error *error) {
  lql_status status;
  status = lql_flat_eq_write(state, "{", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  status = lql_flat_eq_projection_key(state, action->segments[1], error);
  if (status != LQL_STATUS_OK)
    return status;
  status = lql_flat_eq_mutation_value(state, action, error);
  if (status != LQL_STATUS_OK)
    return status;
  return lql_flat_eq_write(state, "}", 1u, error);
}

static lql_status lql_flat_eq_mutation_number(lql_flat_eq_state *state,
                                              double value, lql_error *error) {
  char buffer[64];
  int len;
  if (value != value || value == HUGE_VAL || value == -HUGE_VAL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment result is not finite");
    return LQL_STATUS_JSON_ERROR;
  }
  len = sprintf(buffer, "%.17g", value);
  if (len <= 0 || (size_t)len >= sizeof(buffer)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment result could not be emitted");
    return LQL_STATUS_JSON_ERROR;
  }
  return lql_flat_eq_write(state, buffer, (size_t)len, error);
}

static lql_status lql_flat_eq_mutation_increment(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    int present, lql_error *error) {
  char number[128];
  char *end;
  double current;
  double next;
  size_t len;
  (void)state;
  if (action == NULL || action->kind != LQL_MUTATION_INCREMENT)
    return LQL_STATUS_INVALID_ARGUMENT;
  if (!present)
    return lql_flat_eq_mutation_number(state, action->delta, error);
  len = value_end - value_start;
  if (len == 0u || len >= sizeof(number)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment target number is invalid");
    return LQL_STATUS_JSON_ERROR;
  }
  {
    size_t i;
    lql_status copy_status;
    unsigned char ch;
    for (i = 0u; i < len; ++i) {
      copy_status = lql_flat_eq_spool_byte(spool, value_start + i, &ch, error);
      if (copy_status != LQL_STATUS_OK)
        return copy_status;
      number[i] = (char)ch;
    }
  }
  number[len] = '\0';
  errno = 0;
  current = strtod(number, &end);
  if (end != number + len || errno == ERANGE) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment target number is invalid");
    return LQL_STATUS_JSON_ERROR;
  }
  next = current + action->delta;
  return lql_flat_eq_mutation_number(state, next, error);
}

static lql_status
lql_flat_eq_mutation_nested_set(lql_flat_eq_state *state,
                                const lql_mutation_action *action,
                                const lql_json_spool *spool, size_t value_start,
                                size_t value_end, lql_error *error) {
  size_t pos;
  int first;
  int found;
  unsigned char ch;
  lql_status status;
  if (action == NULL || action->kind != LQL_MUTATION_SET ||
      action->segment_count != 2u || spool == NULL || value_start >= value_end)
    return LQL_STATUS_INVALID_ARGUMENT;
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (ch != (unsigned char)'{')
    return lql_flat_eq_mutation_nested_object(state, action, error);
  status = lql_flat_eq_write(state, "{", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  pos = value_start + 1u;
  first = 1;
  found = 0;
  status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  while (pos + 1u < value_end && ch != (unsigned char)'}') {
    size_t key_start;
    size_t key_end;
    size_t child_value_end;
    int same_key;
    key_start = pos;
    status = lql_flat_eq_key_equals(spool, key_start, value_end,
                                    action->segments[1], &same_key, error);
    if (status != LQL_STATUS_OK)
      return status;
    key_end = pos;
    status = lql_flat_eq_skip_string(spool, &key_end, value_end, error);
    if (status != LQL_STATUS_OK)
      return status;
    status = lql_flat_eq_spool_byte(spool, key_end, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (ch != (unsigned char)':')
      return LQL_STATUS_JSON_ERROR;
    child_value_end = key_end + 1u;
    status = lql_flat_eq_skip_value(spool, &child_value_end, value_end, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (!first &&
        (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
      return status;
    if (same_key) {
      status =
          lql_json_spool_write_slice(spool, key_start, key_end - key_start + 1u,
                                     state->writer, state->writer_user, error);
      if (status == LQL_STATUS_OK)
        status = lql_flat_eq_mutation_value(state, action, error);
      found = 1;
    } else {
      status = lql_json_spool_write_slice(
          spool, key_start, child_value_end - key_start, state->writer,
          state->writer_user, error);
    }
    if (status != LQL_STATUS_OK)
      return status;
    first = 0;
    pos = child_value_end;
    status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (ch == (unsigned char)',') {
      ++pos;
      status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
      if (status != LQL_STATUS_OK)
        return status;
    }
  }
  if (!found) {
    if (!first &&
        (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
      return status;
    status = lql_flat_eq_projection_key(state, action->segments[1], error);
    if (status != LQL_STATUS_OK)
      return status;
    status = lql_flat_eq_mutation_value(state, action, error);
    if (status != LQL_STATUS_OK)
      return status;
  }
  return lql_flat_eq_write(state, "}", 1u, error);
}

static lql_status lql_flat_eq_mutation_nested_remove(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    lql_error *error) {
  size_t pos;
  int first;
  unsigned char ch;
  lql_status status;
  if (action == NULL || action->kind != LQL_MUTATION_REMOVE ||
      action->segment_count != 2u || spool == NULL || value_start >= value_end)
    return LQL_STATUS_INVALID_ARGUMENT;
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (ch != (unsigned char)'{') {
    return lql_json_spool_write_slice(spool, value_start,
                                      value_end - value_start, state->writer,
                                      state->writer_user, error);
  }
  status = lql_flat_eq_write(state, "{", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  pos = value_start + 1u;
  first = 1;
  status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  while (pos + 1u < value_end && ch != (unsigned char)'}') {
    size_t key_start;
    size_t key_end;
    size_t child_value_end;
    int same_key;
    key_start = pos;
    status = lql_flat_eq_key_equals(spool, key_start, value_end,
                                    action->segments[1], &same_key, error);
    if (status != LQL_STATUS_OK)
      return status;
    key_end = pos;
    status = lql_flat_eq_skip_string(spool, &key_end, value_end, error);
    if (status != LQL_STATUS_OK)
      return status;
    status = lql_flat_eq_spool_byte(spool, key_end, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (ch != (unsigned char)':')
      return LQL_STATUS_JSON_ERROR;
    child_value_end = key_end + 1u;
    status = lql_flat_eq_skip_value(spool, &child_value_end, value_end, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (!same_key) {
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status = lql_json_spool_write_slice(
          spool, key_start, child_value_end - key_start, state->writer,
          state->writer_user, error);
      if (status != LQL_STATUS_OK)
        return status;
      first = 0;
    }
    pos = child_value_end;
    status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (ch == (unsigned char)',') {
      ++pos;
      status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
      if (status != LQL_STATUS_OK)
        return status;
    }
  }
  return lql_flat_eq_write(state, "}", 1u, error);
}

static lql_status
lql_flat_eq_mutation_key_value(lql_flat_eq_state *state,
                               const lql_mutation_action *action,
                               int include_key, lql_error *error) {
  lql_status status;
  if (include_key) {
    status = lql_flat_eq_projection_key(state, action->segments[0], error);
    if (status != LQL_STATUS_OK)
      return status;
  }
  if (action->kind == LQL_MUTATION_SET && action->segment_count == 2u)
    return lql_flat_eq_mutation_nested_object(state, action, error);
  if (action->kind == LQL_MUTATION_INCREMENT)
    return lql_flat_eq_mutation_increment(state, action, NULL, 0u, 0u, 0,
                                          error);
  return lql_flat_eq_mutation_value(state, action, error);
}

static lql_status lql_flat_eq_mutation_find_action(
    const lql_flat_eq_program *program, const lql_json_spool *spool,
    size_t key_start, size_t size, size_t *out_index,
    const lql_mutation_action **out_action, lql_error *error) {
  size_t i;
  lql_status status;
  int same_key;
  if (out_index != NULL)
    *out_index = 0u;
  if (out_action != NULL)
    *out_action = NULL;
  if (program == NULL || spool == NULL || out_index == NULL ||
      out_action == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *action;
    action = program->direct_mutation_actions[i];
    status = lql_flat_eq_key_equals(spool, key_start, size, action->segments[0],
                                    &same_key, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (same_key) {
      *out_index = i;
      *out_action = action;
      return LQL_STATUS_OK;
    }
  }
  return LQL_STATUS_OK;
}

static lql_status lql_flat_eq_mutation_emit(lql_flat_eq_state *state,
                                            const lql_json_spool *spool,
                                            lql_error *error) {
  size_t pos;
  size_t size;
  unsigned long found;
  int first;
  unsigned char ch;
  lql_status status;
  if (state->program->direct_mutation_action_count == 0u || spool == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  size = lql_json_spool_size(spool);
  if (size < 2u)
    return LQL_STATUS_JSON_ERROR;
  status = lql_flat_eq_spool_byte(spool, 0u, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (ch != (unsigned char)'{')
    return LQL_STATUS_JSON_ERROR;
  status = lql_flat_eq_write(state, "{", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  pos = 1u;
  found = 0;
  first = 1;
  status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  while (pos + 1u < size && ch != (unsigned char)'}') {
    size_t key_start;
    size_t key_end;
    size_t value_end;
    const lql_mutation_action *action;
    size_t action_index;
    key_start = pos;
    action = NULL;
    action_index = 0u;
    status = lql_flat_eq_mutation_find_action(
        state->program, spool, key_start, size, &action_index, &action, error);
    if (status != LQL_STATUS_OK)
      return status;
    key_end = pos;
    status = lql_flat_eq_skip_string(spool, &key_end, size, error);
    if (status != LQL_STATUS_OK)
      return status;
    status = lql_flat_eq_spool_byte(spool, key_end, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (ch != (unsigned char)':')
      return LQL_STATUS_JSON_ERROR;
    value_end = key_end + 1u;
    status = lql_flat_eq_skip_value(spool, &value_end, size, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (action != NULL) {
      found |= 1ul << action_index;
      if (action->kind == LQL_MUTATION_REMOVE && action->segment_count == 1u) {
        pos = value_end;
        status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
        if (status != LQL_STATUS_OK)
          return status;
        if (ch == (unsigned char)',') {
          ++pos;
          status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
          if (status != LQL_STATUS_OK)
            return status;
        }
        continue;
      }
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status =
          lql_json_spool_write_slice(spool, key_start, key_end - key_start + 1u,
                                     state->writer, state->writer_user, error);
      if (status == LQL_STATUS_OK) {
        if (action->kind == LQL_MUTATION_INCREMENT) {
          status = lql_flat_eq_mutation_increment(
              state, action, spool, key_end + 1u, value_end, 1, error);
        } else if (action->kind == LQL_MUTATION_SET &&
                   action->segment_count == 2u) {
          status = lql_flat_eq_mutation_nested_set(
              state, action, spool, key_end + 1u, value_end, error);
        } else if (action->kind == LQL_MUTATION_REMOVE &&
                   action->segment_count == 2u) {
          status = lql_flat_eq_mutation_nested_remove(
              state, action, spool, key_end + 1u, value_end, error);
        } else {
          status = lql_flat_eq_mutation_value(state, action, error);
        }
      }
    } else {
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status =
          lql_json_spool_write_slice(spool, key_start, value_end - key_start,
                                     state->writer, state->writer_user, error);
    }
    if (status != LQL_STATUS_OK)
      return status;
    first = 0;
    pos = value_end;
    status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (ch == (unsigned char)',') {
      ++pos;
      status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
      if (status != LQL_STATUS_OK)
        return status;
    }
  }
  {
    size_t i;
    for (i = 0u; i < state->program->direct_mutation_action_count; ++i) {
      const lql_mutation_action *action;
      action = state->program->direct_mutation_actions[i];
      if ((found & (1ul << i)) != 0ul || action->kind == LQL_MUTATION_REMOVE)
        continue;
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status = lql_flat_eq_mutation_key_value(state, action, 1, error);
      if (status != LQL_STATUS_OK)
        return status;
      first = 0;
    }
  }
  status = lql_flat_eq_write(state, "}\n", 2u, error);
  return status;
}

static lql_status lql_flat_eq_projection_node(lql_flat_eq_state *state,
                                              const lql_json_spool *spool,
                                              size_t anchor, size_t depth,
                                              lql_error *error);

static lql_status lql_flat_eq_projection_object(lql_flat_eq_state *state,
                                                const lql_json_spool *spool,
                                                size_t anchor, size_t depth,
                                                lql_error *error) {
  size_t i;
  size_t selected;
  int first;
  int have_previous;
  const char *previous;
  lql_status status;
  status = lql_flat_eq_write(state, "{", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  first = 1;
  have_previous = 0;
  previous = NULL;
  for (;;) {
    const lql_json_capture_key *key;
    selected = state->program->capture_key_count;
    for (i = 0u; i < state->program->capture_key_count; ++i) {
      const lql_json_capture_key *candidate;
      candidate = &state->program->capture_keys[i];
      if (candidate->segment_count <= depth ||
          !lql_flat_eq_projection_prefix(state->program, anchor, i, depth) ||
          !lql_flat_eq_projection_group_found(state->program, anchor, depth,
                                              i) ||
          (have_previous && strcmp(candidate->segments[depth], previous) <= 0))
        continue;
      if (selected == state->program->capture_key_count ||
          strcmp(candidate->segments[depth],
                 state->program->capture_keys[selected].segments[depth]) < 0)
        selected = i;
    }
    if (selected == state->program->capture_key_count)
      break;
    key = &state->program->capture_keys[selected];
    if (!first &&
        (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
      return status;
    status = lql_flat_eq_projection_key(state, key->segments[depth], error);
    if (status != LQL_STATUS_OK)
      return status;
    if (key->segment_count == depth + 1u) {
      status = lql_json_spool_write_slice(
          spool, state->program->capture_spans[selected].offset,
          state->program->capture_spans[selected].len, state->writer,
          state->writer_user, error);
    } else {
      status = lql_flat_eq_projection_node(state, spool, selected, depth + 1u,
                                           error);
    }
    if (status != LQL_STATUS_OK)
      return status;
    first = 0;
    previous = key->segments[depth];
    have_previous = 1;
  }
  return lql_flat_eq_write(state, "}", 1u, error);
}

static lql_status lql_flat_eq_projection_array(lql_flat_eq_state *state,
                                               const lql_json_spool *spool,
                                               size_t anchor, size_t depth,
                                               lql_error *error) {
  size_t i;
  size_t next_index;
  size_t selected;
  int first;
  lql_status status;
  status = lql_flat_eq_write(state, "[", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  first = 1;
  next_index = 0u;
  for (;;) {
    size_t selected_index;
    selected = state->program->capture_key_count;
    selected_index = 0u;
    for (i = 0u; i < state->program->capture_key_count; ++i) {
      const lql_json_capture_key *candidate;
      size_t candidate_index;
      candidate = &state->program->capture_keys[i];
      if (candidate->segment_count <= depth ||
          !lql_flat_eq_projection_prefix(state->program, anchor, i, depth) ||
          !lql_flat_eq_projection_group_found(state->program, anchor, depth,
                                              i) ||
          !lql_flat_eq_projection_segment_index(candidate->segments[depth],
                                                &candidate_index) ||
          candidate_index < next_index)
        continue;
      if (selected == state->program->capture_key_count ||
          candidate_index < selected_index) {
        selected = i;
        selected_index = candidate_index;
      }
    }
    if (selected == state->program->capture_key_count)
      break;
    while (next_index < selected_index) {
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status = lql_flat_eq_write(state, "null", 4u, error);
      if (status != LQL_STATUS_OK)
        return status;
      first = 0;
      ++next_index;
    }
    if (!first &&
        (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
      return status;
    if (state->program->capture_keys[selected].segment_count == depth + 1u) {
      status = lql_json_spool_write_slice(
          spool, state->program->capture_spans[selected].offset,
          state->program->capture_spans[selected].len, state->writer,
          state->writer_user, error);
    } else {
      status = lql_flat_eq_projection_node(state, spool, selected, depth + 1u,
                                           error);
    }
    if (status != LQL_STATUS_OK)
      return status;
    first = 0;
    next_index = selected_index + 1u;
  }
  return lql_flat_eq_write(state, "]", 1u, error);
}

static lql_status lql_flat_eq_projection_node(lql_flat_eq_state *state,
                                              const lql_json_spool *spool,
                                              size_t anchor, size_t depth,
                                              lql_error *error) {
  if (lql_flat_eq_projection_child_array(state->program, anchor, depth))
    return lql_flat_eq_projection_array(state, spool, anchor, depth, error);
  return lql_flat_eq_projection_object(state, spool, anchor, depth, error);
}

static lql_status lql_flat_eq_projection_emit(lql_flat_eq_state *state,
                                              const lql_json_spool *spool,
                                              lql_error *error) {
  lql_status status;
  status = lql_flat_eq_projection_object(state, spool, 0u, 0u, error);
  if (status != LQL_STATUS_OK)
    return status;
  return lql_flat_eq_write(state, "\n", 1u, error);
}

static int lql_flat_eq_projection_found(const lql_flat_eq_state *state,
                                        const lql_json_spool *spool) {
  size_t i;
  if (state == NULL || state->request->projection == NULL || spool == NULL)
    return 0;
  for (i = 0u; i < state->program->capture_key_count; ++i)
    if (state->program->capture_spans[i].found)
      return 1;
  return 0;
}

static lql_status lql_flat_eq_projection_then_mutation_emit(
    lql_flat_eq_state *state, const lql_json_spool *spool, lql_error *error) {
  lql_json_spool projected;
  lql_stream_writer_fn saved_writer;
  void *saved_writer_user;
  lql_status status;
  if (!lql_flat_eq_projection_found(state, spool))
    return LQL_STATUS_OK;
  status = lql_json_spool_init(&projected, error);
  if (status != LQL_STATUS_OK)
    return status;
  saved_writer = state->writer;
  saved_writer_user = state->writer_user;
  state->writer = lql_flat_eq_spool_writer;
  state->writer_user = &projected;
  status = lql_flat_eq_projection_object(state, spool, 0u, 0u, error);
  state->writer = saved_writer;
  state->writer_user = saved_writer_user;
  if (status == LQL_STATUS_OK)
    status = lql_flat_eq_mutation_emit(state, &projected, error);
  lql_json_spool_cleanup(&projected);
  return status;
}

static int
lql_flat_eq_projection_paths_compatible(const lql_projection *projection) {
  size_t i;
  size_t j;
  if (projection == NULL)
    return 0;
  for (i = 0u; i < projection->path_count; ++i) {
    const lql_projection_path *left;
    left = &projection->compiled_paths[i];
    for (j = i + 1u; j < projection->path_count; ++j) {
      const lql_projection_path *right;
      size_t depth;
      right = &projection->compiled_paths[j];
      depth = 0u;
      while (depth < left->segment_count && depth < right->segment_count &&
             strcmp(left->segments[depth], right->segments[depth]) == 0)
        ++depth;
      if (depth < left->segment_count && depth < right->segment_count) {
        size_t left_index;
        size_t right_index;
        int left_array;
        int right_array;
        left_array = lql_flat_eq_projection_segment_index(left->segments[depth],
                                                          &left_index);
        right_array = lql_flat_eq_projection_segment_index(
            right->segments[depth], &right_index);
        if (left_array != right_array)
          return 0;
        if (left_array && left_index == right_index &&
            strcmp(left->segments[depth], right->segments[depth]) != 0)
          return 0;
      }
    }
  }
  return 1;
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
  if (state->request->output_mode == LQL_STREAM_OUTPUT_PROJECTION &&
      (matched || !state->request->matched_only)) {
    if (!root_is_object) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "projection input must be a JSON object");
      return LQL_STATUS_JSON_ERROR;
    }
    if (lql_flat_eq_projection_found(state, spool)) {
      status = lql_flat_eq_projection_emit(state, spool, error);
      if (status != LQL_STATUS_OK)
        return status;
    }
  }
  if (state->request->output_mode == LQL_STREAM_OUTPUT_MUTATION && matched) {
    if (!root_is_object) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "mutation input must be a JSON object");
      return LQL_STATUS_JSON_ERROR;
    }
    status = lql_flat_eq_mutation_emit(state, spool, error);
    if (status != LQL_STATUS_OK)
      return status;
  } else if (state->request->output_mode == LQL_STREAM_OUTPUT_MUTATION &&
             !state->request->matched_only) {
    if (spool == NULL) {
      lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                    "mutation passthrough capture is unavailable");
      return LQL_STATUS_CALLBACK_ERROR;
    }
    status = lql_flat_eq_emit(state, spool, error);
    if (status != LQL_STATUS_OK)
      return status;
  } else if (state->request->output_mode ==
                 LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION &&
             matched) {
    if (!root_is_object) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "combined input must be a JSON object");
      return LQL_STATUS_JSON_ERROR;
    }
    status = lql_flat_eq_projection_then_mutation_emit(state, spool, error);
    if (status != LQL_STATUS_OK)
      return status;
  } else if (state->request->output_mode ==
                 LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION &&
             !state->request->matched_only) {
    if (spool == NULL) {
      lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                    "combined passthrough capture is unavailable");
      return LQL_STATUS_CALLBACK_ERROR;
    }
    status = lql_flat_eq_emit(state, spool, error);
    if (status != LQL_STATUS_OK)
      return status;
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
      request->limits.max_records != 0u ||
      (request->output_mode != LQL_STREAM_OUTPUT_DECISION_ONLY &&
       request->output_mode != LQL_STREAM_OUTPUT_SELECTED_RECORD &&
       request->output_mode != LQL_STREAM_OUTPUT_PROJECTION &&
       request->output_mode != LQL_STREAM_OUTPUT_MUTATION &&
       request->output_mode != LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION)) {
    return 0;
  }
  if (request->output_mode != LQL_STREAM_OUTPUT_MUTATION &&
      request->output_mode != LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION &&
      request->mutation != NULL)
    return 0;
  if (request->output_mode == LQL_STREAM_OUTPUT_MUTATION &&
      request->projection != NULL)
    return 0;
  if (request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION &&
      (request->projection == NULL || request->mutation == NULL))
    return 0;
  return 1;
}

static int lql_flat_eq_mutation_append(lql_flat_eq_program *program,
                                       const lql_mutation *mutation) {
  size_t i;
  if (program == NULL || mutation == NULL || mutation->action_count == 0u ||
      mutation->action_count > LQL_FLAT_EQ_TERM_CAPACITY)
    return 0;
  for (i = 0u; i < mutation->action_count; ++i) {
    const lql_mutation_action *action;
    size_t j;
    action = &mutation->actions[i];
    if (action->segments == NULL || action->segments[0] == NULL)
      return 0;
    if (action->kind == LQL_MUTATION_SET) {
      if ((action->segment_count != 1u && action->segment_count != 2u) ||
          action->value == NULL)
        return 0;
      if (action->segment_count == 2u && action->segments[1] == NULL)
        return 0;
    } else if (action->kind == LQL_MUTATION_REMOVE) {
      if (action->segment_count != 1u && action->segment_count != 2u)
        return 0;
      if (action->segment_count == 2u && action->segments[1] == NULL)
        return 0;
    } else if (action->kind == LQL_MUTATION_INCREMENT) {
      if (action->segment_count != 1u)
        return 0;
    } else {
      return 0;
    }
    for (j = 0u; j < i; ++j) {
      if (strcmp(mutation->actions[j].segments[0], action->segments[0]) == 0)
        return 0;
    }
    program->direct_mutation_actions[i] = action;
  }
  program->direct_mutation_action_count = mutation->action_count;
  return 1;
}

static int lql_flat_eq_projection_append(lql_flat_eq_program *program,
                                         const lql_projection *projection) {
  size_t i;
  if (projection == NULL || projection->path_count == 0u ||
      projection->path_count > LQL_FLAT_EQ_TERM_CAPACITY ||
      !lql_flat_eq_projection_paths_compatible(projection))
    return 0;
  for (i = 0u; i < projection->path_count; ++i) {
    const lql_projection_path *path = &projection->compiled_paths[i];
    if (path->segment_count == 0u || path->segments[0] == NULL)
      return 0;
    program->capture_keys[i].segments = (const char *const *)path->segments;
    program->capture_keys[i].segment_count = path->segment_count;
  }
  program->capture_key_count = projection->path_count;
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
  lql_flat_eq_time_bounds time_bounds;
  lql_flat_eq_time_bounds *time_bounds_ptr;
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
  time_bounds_ptr = NULL;
  if (lql_flat_eq_selector_needs_time(request->selector)) {
    memset(&time_bounds, 0, sizeof(time_bounds));
    time_bounds.now_ready = lql_temporal_now(&time_bounds.now);
    time_bounds.today_ready = lql_temporal_today(&time_bounds.today);
    time_bounds.yesterday_ready =
        lql_temporal_yesterday(&time_bounds.yesterday);
    time_bounds_ptr = &time_bounds;
  }
  if (!lql_flat_eq_append(&program, request->selector, time_bounds_ptr)) {
    return LQL_STATUS_OK;
  }
  if ((request->output_mode == LQL_STREAM_OUTPUT_PROJECTION ||
       request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION) &&
      !lql_flat_eq_projection_append(&program, request->projection)) {
    return LQL_STATUS_OK;
  }
  if ((request->output_mode == LQL_STREAM_OUTPUT_MUTATION ||
       request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION) &&
      !lql_flat_eq_mutation_append(&program, request->mutation)) {
    return LQL_STATUS_OK;
  }
  program.stop_matching_on_hit =
      request->selector->kind == LQL_SELECTOR_KIND_EQ &&
      program.term_count == 1u;
  *out_handled = 1;
  capture = request->on_value != NULL ||
            request->output_mode == LQL_STREAM_OUTPUT_SELECTED_RECORD ||
            request->output_mode == LQL_STREAM_OUTPUT_PROJECTION ||
            request->output_mode == LQL_STREAM_OUTPUT_MUTATION ||
            request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION;
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
  state.writer = request->writer;
  state.writer_user = request->writer_user;
  memset(&scan_request, 0, sizeof(scan_request));
  scan_request.reader = request->reader;
  scan_request.reader_user = request->reader_user;
  scan_request.terms = program.terms;
  scan_request.term_count = program.term_count;
  scan_request.spool = capture ? &spool : NULL;
  scan_request.capture = capture;
  scan_request.stop_matching_on_hit = program.stop_matching_on_hit;
  scan_request.capture_keys = program.capture_keys;
  scan_request.capture_key_count = program.capture_key_count;
  scan_request.capture_spans = program.capture_spans;
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
