#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "lql_internal.h"
#include "lql_json_scan.h"
#include "lql_unicode_lower.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
  unsigned long stop_hit_mask;
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

static int lql_flat_eq_literal_kind(const char *value,
                                    lql_selector_literal_kind literal_kind,
                                    int from_json,
                                    lql_json_flat_term_kind *out) {
  (void)value;
  (void)from_json;
  if (out == NULL) {
    return 0;
  }
  /*
   * Intentional liblql semantics: unquoted selector scalars stay typed instead
   * of following Go lql's scalar-to-string coercion. Numeric equality is
   * handled by the scanner as numeric JSON equality, not source text equality.
   */
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
  case LQL_SELECTOR_LITERAL_NULL:
    *out = LQL_JSON_FLAT_TERM_NULL_EQ;
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
    if (segment_len == 0u ||
        segment_count >= sizeof(unsigned long) * CHAR_BIT ||
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

static int lql_flat_eq_path_segment_index(const char *path, size_t segment,
                                          size_t *out) {
  const char *start;
  const char *end;
  size_t i;
  size_t value;
  if (path == NULL || path[0] != '/' || out == NULL)
    return 0;
  start = path + 1;
  for (i = 0u; i < segment; ++i) {
    end = strchr(start, '/');
    if (end == NULL)
      return 0;
    start = end + 1;
  }
  end = strchr(start, '/');
  value = 0u;
  for (; end == NULL ? *start != '\0' : start < end; ++start) {
    size_t digit;
    if (*start < '0' || *start > '9')
      return 0;
    digit = (size_t)(*start - '0');
    if (value > ((size_t)-1 - digit) / 10u)
      return 0;
    value = value * 10u + digit;
  }
  *out = value;
  return 1;
}

static void lql_flat_eq_cache_array_indexes(lql_json_flat_eq_term *term) {
  size_t i;
  if (term == NULL)
    return;
  term->path_array_index_cache = 0ul;
  for (i = 0u; i < LQL_JSON_PATH_SEGMENT_CAPACITY; ++i) {
    size_t value;
    if ((term->path_array_segments & (1ul << i)) == 0ul)
      continue;
    if (lql_flat_eq_path_segment_index(term->path, i, &value)) {
      term->path_array_index_values[i] = value;
      term->path_array_index_cache |= 1ul << i;
    }
  }
}

static void lql_flat_eq_cache_recursive_match(lql_json_flat_eq_term *term) {
  const char *segment;
  const char *slash;
  size_t recursive_segment;
  size_t i;
  if (term == NULL || term->path == NULL ||
      term->path_recursive_segments == 0ul) {
    return;
  }
  recursive_segment = 0u;
  while ((term->path_recursive_segments & (1ul << recursive_segment)) == 0ul) {
    ++recursive_segment;
  }
  segment = term->path + 1;
  for (i = 0u; i < recursive_segment + 1u; ++i) {
    slash = strchr(segment, '/');
    if (slash == NULL) {
      return;
    }
    segment = slash + 1;
  }
  slash = strchr(segment, '/');
  term->path_recursive_match = segment;
  term->path_recursive_match_len =
      slash == NULL ? strlen(segment) : (size_t)(slash - segment);
  term->path_recursive_match_segment = recursive_segment + 1u;
}

static void lql_flat_eq_cache_paths(lql_json_flat_eq_term *term) {
  lql_flat_eq_cache_array_indexes(term);
  lql_flat_eq_cache_recursive_match(term);
}

static int lql_flat_eq_append_contains(
    lql_flat_eq_program *program, const lql_selector *selector,
    const char *field, size_t path_len, size_t path_segment_count,
    size_t first_segment_len, unsigned long path_array_segments,
    unsigned long path_object_wildcards, unsigned long path_array_wildcards,
    unsigned long path_any_wildcards, unsigned long path_recursive_segments,
    int ignore_case) {
  lql_json_flat_eq_term *term;
  size_t count;
  size_t i;
  if (program == NULL || selector == NULL || field == NULL) {
    return 0;
  }
  count = selector->any_count == 0u ? 1u : selector->any_count;
  if (selector->any_count == 0u && !selector->value_set) {
    if (program->term_count == LQL_FLAT_EQ_TERM_CAPACITY) {
      return 0;
    }
    term = &program->terms[program->term_count];
    term->kind = LQL_JSON_FLAT_TERM_EXISTS;
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
    lql_flat_eq_cache_paths(term);
    program->selectors[program->term_count] = selector;
    ++program->term_count;
    return 1;
  }
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
    lql_flat_eq_cache_paths(term);
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
    return 1;
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
      if (!lql_flat_eq_literal_kind(
              selector->any[i], selector->any_kinds[i],
              selector->any_from_json != NULL ? selector->any_from_json[i] : 0,
              &term_kind) ||
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
      lql_flat_eq_cache_paths(term);
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
    term->kind = selector->range_is_temporal ? LQL_JSON_FLAT_TERM_TEMPORAL_RANGE
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
    lql_flat_eq_cache_paths(term);
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
    lql_flat_eq_cache_paths(term);
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
  } else if (selector->kind == LQL_SELECTOR_KIND_EQ &&
             selector->value_is_temporal) {
    term->kind = LQL_JSON_FLAT_TERM_TEMPORAL_RANGE;
    term->temporal_eq = selector->temporal_eq;
    term->has_temporal_eq = 1;
  } else if (selector->kind == LQL_SELECTOR_KIND_PREFIX ||
             selector->kind == LQL_SELECTOR_KIND_IPREFIX) {
    term->kind =
        selector->kind == LQL_SELECTOR_KIND_IPREFIX || selector->ignore_case
            ? LQL_JSON_FLAT_TERM_IPREFIX
            : LQL_JSON_FLAT_TERM_PREFIX;
  } else if (!lql_flat_eq_literal_kind(selector->value, selector->value_kind,
                                       selector->value_from_json,
                                       &term->kind)) {
    return 0;
  }
  if (term->kind != LQL_JSON_FLAT_TERM_EXISTS) {
    if (!selector->value_set) {
      if (selector->kind == LQL_SELECTOR_KIND_EQ) {
        return 1;
      }
      term->kind = LQL_JSON_FLAT_TERM_EXISTS;
    } else if ((selector->value_is_temporal &&
                term->kind != LQL_JSON_FLAT_TERM_TEMPORAL_RANGE) ||
               selector->value == NULL ||
               ((term->kind == LQL_JSON_FLAT_TERM_PREFIX ||
                 term->kind == LQL_JSON_FLAT_TERM_IPREFIX) &&
                selector->value_kind != LQL_SELECTOR_LITERAL_STRING)) {
      return 0;
    }
    if (term->kind == LQL_JSON_FLAT_TERM_EXISTS) {
      term->value = NULL;
      term->value_len = 0u;
    } else if (term->kind == LQL_JSON_FLAT_TERM_IPREFIX) {
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
  lql_flat_eq_cache_paths(term);
  program->selectors[program->term_count] = selector;
  ++program->term_count;
  return 1;
}

static int lql_flat_eq_matches(const lql_flat_eq_program *program,
                               const lql_selector *selector,
                               unsigned long hits) {
  size_t i;
  if (selector == NULL) {
    return 1;
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

static unsigned long
lql_flat_eq_leaf_stop_mask(const lql_flat_eq_program *program,
                           const lql_selector *selector) {
  unsigned long mask;
  size_t i;
  if (program == NULL || selector == NULL) {
    return 0ul;
  }
  mask = 0ul;
  for (i = 0u; i < program->term_count; ++i) {
    if (program->selectors[i] == selector) {
      if (mask != 0ul) {
        return 0ul;
      }
      mask = 1ul << i;
    }
  }
  return mask;
}

static unsigned long lql_flat_eq_stop_mask(const lql_flat_eq_program *program,
                                           const lql_selector *selector) {
  unsigned long mask;
  size_t i;
  if (program == NULL || selector == NULL) {
    return 0ul;
  }
  if (selector->kind == LQL_SELECTOR_KIND_AND) {
    mask = 0ul;
    for (i = 0u; i < selector->child_count; ++i) {
      unsigned long child;
      child = lql_flat_eq_stop_mask(program, &selector->children[i]);
      if (child == 0ul) {
        return 0ul;
      }
      mask |= child;
    }
    return mask;
  }
  if (selector->kind == LQL_SELECTOR_KIND_EQ ||
      selector->kind == LQL_SELECTOR_KIND_EXISTS ||
      selector->kind == LQL_SELECTOR_KIND_PREFIX ||
      selector->kind == LQL_SELECTOR_KIND_IPREFIX ||
      selector->kind == LQL_SELECTOR_KIND_CONTAINS ||
      selector->kind == LQL_SELECTOR_KIND_ICONTAINS ||
      selector->kind == LQL_SELECTOR_KIND_RANGE ||
      selector->kind == LQL_SELECTOR_KIND_DATE) {
    return lql_flat_eq_leaf_stop_mask(program, selector);
  }
  return 0ul;
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

static lql_status lql_flat_eq_emit_range(lql_flat_eq_state *state,
                                         size_t source_offset,
                                         size_t source_len, lql_error *error) {
  lql_status status;
  static const char newline[] = "\n";
  if (state == NULL || state->request == NULL ||
      state->request->range_writer == NULL || state->writer == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "streaming selected record range is unavailable");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  status = state->request->range_writer(
      state->request->range_user, source_offset, source_len, state->writer,
      state->writer_user, error);
  if (status != LQL_STATUS_OK) {
    if (error != NULL && error->code == LQL_STATUS_OK)
      lql_set_error(error, status, "stream range writer failed");
    return status;
  }
  status =
      state->writer(state->writer_user, newline, sizeof(newline) - 1u, error);
  if (status != LQL_STATUS_OK && error != NULL && error->code == LQL_STATUS_OK)
    lql_set_error(error, status, "stream writer failed");
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

typedef struct lql_flat_eq_utf8_state {
  int remaining;
  unsigned char min_next;
  unsigned char max_next;
} lql_flat_eq_utf8_state;

static int lql_flat_eq_utf8_accept(lql_flat_eq_utf8_state *state,
                                   unsigned char ch) {
  if (state->remaining == 0) {
    if (ch <= 0x7fu)
      return 1;
    state->min_next = 0x80u;
    state->max_next = 0xbfu;
    if (ch >= 0xc2u && ch <= 0xdfu) {
      state->remaining = 1;
      return 1;
    }
    if (ch == 0xe0u) {
      state->remaining = 2;
      state->min_next = 0xa0u;
      return 1;
    }
    if ((ch >= 0xe1u && ch <= 0xecu) || (ch >= 0xeeu && ch <= 0xefu)) {
      state->remaining = 2;
      return 1;
    }
    if (ch == 0xedu) {
      state->remaining = 2;
      state->max_next = 0x9fu;
      return 1;
    }
    if (ch == 0xf0u) {
      state->remaining = 3;
      state->min_next = 0x90u;
      return 1;
    }
    if (ch >= 0xf1u && ch <= 0xf3u) {
      state->remaining = 3;
      return 1;
    }
    if (ch == 0xf4u) {
      state->remaining = 3;
      state->max_next = 0x8fu;
      return 1;
    }
    return 0;
  }
  if (ch < state->min_next || ch > state->max_next)
    return 0;
  --state->remaining;
  state->min_next = 0x80u;
  state->max_next = 0xbfu;
  return 1;
}

static lql_status lql_flat_eq_file_open(const lql_mutation_action *action,
                                        FILE **out, lql_error *error) {
  lql_mutation_action *mutable_action;
  FILE *file;

  *out = NULL;
  /*
   * The public request borrows a const mutation handle, but the handle owns
   * this file descriptor cache. Reusing the descriptor avoids per-record open
   * overhead without caching or materializing file contents.
   */
  mutable_action = (lql_mutation_action *)action;
  file = mutable_action->file_value_handle;
  if (file == NULL) {
    file = fopen(action->value, "rb");
    if (file == NULL) {
      lql_set_error(error, LQL_STATUS_IO_ERROR,
                    "unable to open file-backed mutation value");
      return LQL_STATUS_IO_ERROR;
    }
    mutable_action->file_value_handle = file;
  } else if (fseek(file, 0L, SEEK_SET) != 0) {
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "unable to rewind file-backed mutation value");
    return LQL_STATUS_IO_ERROR;
  }
  clearerr(file);
  *out = file;
  return LQL_STATUS_OK;
}

static lql_status lql_flat_eq_file_textlike(const lql_mutation_action *action,
                                            int strict, int *out_textlike,
                                            lql_error *error) {
  unsigned char buffer[4096];
  lql_flat_eq_utf8_state utf8;
  FILE *file;
  size_t amount;
  size_t i;

  *out_textlike = 0;
  memset(&utf8, 0, sizeof(utf8));
  if (lql_flat_eq_file_open(action, &file, error) != LQL_STATUS_OK)
    return error == NULL ? LQL_STATUS_IO_ERROR : error->code;
  while ((amount = fread(buffer, 1u, sizeof(buffer), file)) != 0u) {
    for (i = 0u; i < amount; ++i) {
      if (buffer[i] == 0u) {
        if (strict) {
          lql_set_error(error, LQL_STATUS_JSON_ERROR,
                        "textfile mutation value contains NUL byte");
          return LQL_STATUS_JSON_ERROR;
        }
        return LQL_STATUS_OK;
      }
      if (!lql_flat_eq_utf8_accept(&utf8, buffer[i])) {
        if (strict) {
          lql_set_error(error, LQL_STATUS_JSON_ERROR,
                        "textfile mutation value is not valid UTF-8");
          return LQL_STATUS_JSON_ERROR;
        }
        return LQL_STATUS_OK;
      }
    }
  }
  if (ferror(file)) {
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "unable to read file-backed mutation value");
    return LQL_STATUS_IO_ERROR;
  }
  if (utf8.remaining != 0) {
    if (strict) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "textfile mutation value is not valid UTF-8");
      return LQL_STATUS_JSON_ERROR;
    }
    return LQL_STATUS_OK;
  }
  *out_textlike = 1;
  return LQL_STATUS_OK;
}

static lql_status
lql_flat_eq_file_text_json_string(lql_flat_eq_state *state,
                                  const lql_mutation_action *action,
                                  lql_error *error) {
  static const char hex[] = "0123456789abcdef";
  unsigned char buffer[4096];
  FILE *file;
  size_t amount;
  size_t i;
  lql_status status;

  status = lql_flat_eq_file_open(action, &file, error);
  if (status != LQL_STATUS_OK)
    return status;
  status = lql_flat_eq_write(state, "\"", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  while ((amount = fread(buffer, 1u, sizeof(buffer), file)) != 0u) {
    for (i = 0u; i < amount; ++i) {
      unsigned char ch;
      ch = buffer[i];
      if (ch == (unsigned char)'"' || ch == (unsigned char)'\\') {
        status = lql_flat_eq_write(state, "\\", 1u, error);
        if (status == LQL_STATUS_OK)
          status = lql_flat_eq_write(state, buffer + i, 1u, error);
      } else if (ch < 0x20u) {
        char escaped[6];
        escaped[0] = '\\';
        escaped[1] = 'u';
        escaped[2] = '0';
        escaped[3] = '0';
        escaped[4] = hex[ch >> 4u];
        escaped[5] = hex[ch & 0x0fu];
        status = lql_flat_eq_write(state, escaped, sizeof(escaped), error);
      } else {
        status = lql_flat_eq_write(state, buffer + i, 1u, error);
      }
      if (status != LQL_STATUS_OK)
        return status;
    }
  }
  if (ferror(file)) {
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "unable to read file-backed mutation value");
    return LQL_STATUS_IO_ERROR;
  }
  return lql_flat_eq_write(state, "\"", 1u, error);
}

static lql_status
lql_flat_eq_file_base64_json_string(lql_flat_eq_state *state,
                                    const lql_mutation_action *action,
                                    lql_error *error) {
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  unsigned char buffer[12288];
  unsigned char carry[3];
  char outbuf[16384];
  FILE *file;
  ssize_t amount;
  size_t i;
  size_t carry_len;
  size_t out_len;
  int fd;
  lql_status status;

  status = lql_flat_eq_file_open(action, &file, error);
  if (status != LQL_STATUS_OK)
    return status;
  fd = fileno(file);
  if (fd < 0 || lseek(fd, 0, SEEK_SET) < 0) {
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "unable to rewind file-backed mutation value");
    return LQL_STATUS_IO_ERROR;
  }
  status = lql_flat_eq_write(state, "\"", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  carry_len = 0u;
  out_len = 0u;
  while ((amount = read(fd, buffer, sizeof(buffer))) > 0) {
    i = 0u;
    if (carry_len != 0u) {
      while (carry_len < 3u && i < (size_t)amount) {
        carry[carry_len++] = buffer[i++];
      }
      if (carry_len == 3u) {
        unsigned long triple;
        triple = ((unsigned long)carry[0] << 16) |
                 ((unsigned long)carry[1] << 8) | carry[2];
        outbuf[out_len++] = table[(triple >> 18) & 0x3ful];
        outbuf[out_len++] = table[(triple >> 12) & 0x3ful];
        outbuf[out_len++] = table[(triple >> 6) & 0x3ful];
        outbuf[out_len++] = table[triple & 0x3ful];
        carry_len = 0u;
      }
    }
    while (i + 3u <= (size_t)amount) {
      unsigned long triple;
      triple = ((unsigned long)buffer[i] << 16) |
               ((unsigned long)buffer[i + 1u] << 8) | buffer[i + 2u];
      outbuf[out_len++] = table[(triple >> 18) & 0x3ful];
      outbuf[out_len++] = table[(triple >> 12) & 0x3ful];
      outbuf[out_len++] = table[(triple >> 6) & 0x3ful];
      outbuf[out_len++] = table[triple & 0x3ful];
      i += 3u;
    }
    if (out_len != 0u) {
      status = lql_flat_eq_write(state, outbuf, out_len, error);
      if (status != LQL_STATUS_OK)
        return status;
      out_len = 0u;
    }
    while (i < (size_t)amount) {
      carry[carry_len++] = buffer[i++];
    }
  }
  if (amount < 0) {
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "unable to read file-backed mutation value");
    return LQL_STATUS_IO_ERROR;
  }
  if (carry_len != 0u) {
    unsigned long triple;
    if (out_len + 4u > sizeof(outbuf)) {
      status = lql_flat_eq_write(state, outbuf, out_len, error);
      if (status != LQL_STATUS_OK)
        return status;
      out_len = 0u;
    }
    triple = (unsigned long)carry[0] << 16;
    if (carry_len == 2u)
      triple |= (unsigned long)carry[1] << 8;
    outbuf[out_len++] = table[(triple >> 18) & 0x3ful];
    outbuf[out_len++] = table[(triple >> 12) & 0x3ful];
    outbuf[out_len++] = carry_len == 2u ? table[(triple >> 6) & 0x3ful] : '=';
    outbuf[out_len++] = '=';
  }
  if (out_len != 0u) {
    status = lql_flat_eq_write(state, outbuf, out_len, error);
    if (status != LQL_STATUS_OK)
      return status;
  }
  return lql_flat_eq_write(state, "\"", 1u, error);
}

static lql_status lql_flat_eq_file_value(lql_flat_eq_state *state,
                                         const lql_mutation_action *action,
                                         lql_error *error) {
  int textlike;
  lql_status status;

  if (action->value_kind == LQL_MUTATION_VALUE_FILE_BASE64)
    return lql_flat_eq_file_base64_json_string(state, action, error);
  if (action->value_kind == LQL_MUTATION_VALUE_FILE_TEXT) {
    status = lql_flat_eq_file_textlike(action, 1, &textlike, error);
    if (status != LQL_STATUS_OK)
      return status;
    return lql_flat_eq_file_text_json_string(state, action, error);
  }
  status = lql_flat_eq_file_textlike(action, 0, &textlike, error);
  if (status != LQL_STATUS_OK)
    return status;
  return textlike ? lql_flat_eq_file_text_json_string(state, action, error)
                  : lql_flat_eq_file_base64_json_string(state, action, error);
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
  lql_status status;
  if (offset == NULL || *offset >= end)
    return LQL_STATUS_JSON_ERROR;
  status = lql_json_spool_find_string_end(spool, *offset, end, offset, error);
  return status;
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
  if (action->value_kind == LQL_MUTATION_VALUE_FILE_AUTO ||
      action->value_kind == LQL_MUTATION_VALUE_FILE_TEXT ||
      action->value_kind == LQL_MUTATION_VALUE_FILE_BASE64)
    return lql_flat_eq_file_value(state, action, error);
  return LQL_STATUS_INVALID_ARGUMENT;
}

static lql_status
lql_flat_eq_mutation_object_chain(lql_flat_eq_state *state,
                                  const lql_mutation_action *action,
                                  size_t depth, lql_error *error) {
  lql_status status;
  status = lql_flat_eq_write(state, "{", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  status = lql_flat_eq_projection_key(state, action->segments[depth], error);
  if (status != LQL_STATUS_OK)
    return status;
  if (depth + 1u == action->segment_count) {
    status = lql_flat_eq_mutation_value(state, action, error);
  } else {
    status =
        lql_flat_eq_mutation_object_chain(state, action, depth + 1u, error);
  }
  if (status != LQL_STATUS_OK)
    return status;
  return lql_flat_eq_write(state, "}", 1u, error);
}

static lql_status lql_flat_eq_mutation_number(lql_flat_eq_state *state,
                                              double value, lql_error *error) {
  char buffer[64];
  if (value != value || value == HUGE_VAL || value == -HUGE_VAL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment result is not finite");
    return LQL_STATUS_JSON_ERROR;
  }
  if (!lql_number_format_json(value, buffer, sizeof(buffer))) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment result could not be emitted");
    return LQL_STATUS_JSON_ERROR;
  }
  return lql_flat_eq_write(state, buffer, strlen(buffer), error);
}

static lql_status lql_flat_eq_mutation_increment(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    int present, lql_error *error) {
  char stack_number[128];
  char *number;
  double current;
  double next;
  size_t len;
  (void)state;
  if (action == NULL || action->kind != LQL_MUTATION_INCREMENT)
    return LQL_STATUS_INVALID_ARGUMENT;
  if (!present)
    return lql_flat_eq_mutation_number(state, action->delta, error);
  if (value_end < value_start) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment target number is invalid");
    return LQL_STATUS_JSON_ERROR;
  }
  len = value_end - value_start;
  if (len == 0u) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment target number is invalid");
    return LQL_STATUS_JSON_ERROR;
  }
  number = len < sizeof(stack_number) ? stack_number : NULL;
  if (number == NULL) {
    number = (char *)malloc(len + 1u);
    if (number == NULL) {
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      return LQL_STATUS_NO_MEMORY;
    }
  }
  {
    size_t i;
    lql_status copy_status;
    unsigned char ch;
    for (i = 0u; i < len; ++i) {
      copy_status = lql_flat_eq_spool_byte(spool, value_start + i, &ch, error);
      if (copy_status != LQL_STATUS_OK) {
        if (number != stack_number)
          free(number);
        return copy_status;
      }
      number[i] = (char)ch;
    }
  }
  number[len] = '\0';
  if (!lql_number_parse_json(number, len, &current)) {
    if (number != stack_number)
      free(number);
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment target number is invalid");
    return LQL_STATUS_JSON_ERROR;
  }
  if (number != stack_number)
    free(number);
  next = current + action->delta;
  return lql_flat_eq_mutation_number(state, next, error);
}

static lql_status lql_flat_eq_mutation_nested_set(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error) {
  size_t pos;
  int first;
  int found;
  unsigned char ch;
  lql_status status;
  if (action == NULL || action->kind != LQL_MUTATION_SET ||
      depth >= action->segment_count || spool == NULL ||
      value_start >= value_end)
    return LQL_STATUS_INVALID_ARGUMENT;
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (ch != (unsigned char)'{')
    return lql_flat_eq_mutation_object_chain(state, action, depth, error);
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
                                    action->segments[depth], &same_key, error);
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
      if (status == LQL_STATUS_OK) {
        if (depth + 1u == action->segment_count) {
          status = lql_flat_eq_mutation_value(state, action, error);
        } else {
          status = lql_flat_eq_mutation_nested_set(
              state, action, spool, key_end + 1u, child_value_end, depth + 1u,
              error);
        }
      }
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
    status = lql_flat_eq_projection_key(state, action->segments[depth], error);
    if (status != LQL_STATUS_OK)
      return status;
    if (depth + 1u == action->segment_count) {
      status = lql_flat_eq_mutation_value(state, action, error);
    } else {
      status =
          lql_flat_eq_mutation_object_chain(state, action, depth + 1u, error);
    }
    if (status != LQL_STATUS_OK)
      return status;
  }
  return lql_flat_eq_write(state, "}", 1u, error);
}

static lql_status lql_flat_eq_mutation_nested_remove(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error) {
  size_t pos;
  int first;
  unsigned char ch;
  lql_status status;
  if (action == NULL || action->kind != LQL_MUTATION_REMOVE ||
      depth >= action->segment_count || spool == NULL ||
      value_start >= value_end)
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
                                    action->segments[depth], &same_key, error);
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
    } else if (depth + 1u < action->segment_count) {
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status =
          lql_json_spool_write_slice(spool, key_start, key_end - key_start + 1u,
                                     state->writer, state->writer_user, error);
      if (status == LQL_STATUS_OK) {
        status = lql_flat_eq_mutation_nested_remove(
            state, action, spool, key_end + 1u, child_value_end, depth + 1u,
            error);
      }
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

static lql_status lql_flat_eq_mutation_nested_increment(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error) {
  size_t pos;
  int first;
  int found;
  unsigned char ch;
  lql_status status;
  if (action == NULL || action->kind != LQL_MUTATION_INCREMENT ||
      depth >= action->segment_count || spool == NULL ||
      value_start >= value_end)
    return LQL_STATUS_INVALID_ARGUMENT;
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (ch != (unsigned char)'{') {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment target was not found");
    return LQL_STATUS_JSON_ERROR;
  }
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
                                    action->segments[depth], &same_key, error);
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
    status =
        lql_json_spool_write_slice(spool, key_start, key_end - key_start + 1u,
                                   state->writer, state->writer_user, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (same_key) {
      found = 1;
      if (depth + 1u == action->segment_count) {
        status = lql_flat_eq_mutation_increment(
            state, action, spool, key_end + 1u, child_value_end, 1, error);
      } else {
        status = lql_flat_eq_mutation_nested_increment(
            state, action, spool, key_end + 1u, child_value_end, depth + 1u,
            error);
      }
    } else {
      status = lql_json_spool_write_slice(
          spool, key_end + 1u, child_value_end - key_end - 1u, state->writer,
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
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment target was not found");
    return LQL_STATUS_JSON_ERROR;
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
    return lql_flat_eq_mutation_object_chain(state, action, 1u, error);
  if (action->kind == LQL_MUTATION_SET && action->segment_count > 2u)
    return lql_flat_eq_mutation_object_chain(state, action, 1u, error);
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

static int lql_flat_eq_mutation_groupable(const lql_mutation_action *action) {
  if (action == NULL)
    return 0;
  if (action->kind == LQL_MUTATION_SET && action->segment_count == 1u)
    return 1;
  if (action->kind == LQL_MUTATION_INCREMENT && action->segment_count == 1u)
    return 1;
  return action->segment_count > 1u && (action->kind == LQL_MUTATION_SET ||
                                        action->kind == LQL_MUTATION_REMOVE ||
                                        action->kind == LQL_MUTATION_INCREMENT);
}

static int lql_flat_eq_mutation_same_top(const lql_mutation_action *left,
                                         const lql_mutation_action *right) {
  return left != NULL && right != NULL && left->segment_count != 0u &&
         right->segment_count != 0u && left->segments[0] != NULL &&
         right->segments[0] != NULL &&
         strcmp(left->segments[0], right->segments[0]) == 0;
}

static int
lql_flat_eq_mutation_group_compatible(const lql_mutation_action *left,
                                      const lql_mutation_action *right) {
  if (!lql_flat_eq_mutation_same_top(left, right))
    return 0;
  if (!lql_flat_eq_mutation_groupable(left) ||
      !lql_flat_eq_mutation_groupable(right))
    return 0;
  if (left->segment_count > 1u && right->segment_count > 1u)
    return 1;
  if (left->kind == LQL_MUTATION_SET && left->segment_count == 1u &&
      right->kind == LQL_MUTATION_SET && right->segment_count == 1u)
    return 1;
  if (left->kind == LQL_MUTATION_INCREMENT && left->segment_count == 1u &&
      right->kind == LQL_MUTATION_INCREMENT && right->segment_count == 1u)
    return 1;
  return 0;
}

static size_t
lql_flat_eq_mutation_group_count(const lql_flat_eq_program *program,
                                 const lql_mutation_action *action) {
  size_t i;
  size_t count;
  if (program == NULL || action == NULL)
    return 0u;
  count = 0u;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    if (lql_flat_eq_mutation_same_top(program->direct_mutation_actions[i],
                                      action))
      ++count;
  }
  return count;
}

static int lql_flat_eq_mutation_group_seen(const lql_flat_eq_program *program,
                                           const lql_mutation_action *action,
                                           size_t before) {
  size_t i;
  if (program == NULL || action == NULL)
    return 0;
  for (i = 0u; i < before; ++i) {
    if (lql_flat_eq_mutation_same_top(program->direct_mutation_actions[i],
                                      action))
      return 1;
  }
  return 0;
}

static int
lql_flat_eq_mutation_group_has_set(const lql_flat_eq_program *program,
                                   const lql_mutation_action *action) {
  size_t i;
  if (program == NULL || action == NULL)
    return 0;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *candidate;
    candidate = program->direct_mutation_actions[i];
    if (lql_flat_eq_mutation_same_top(candidate, action) &&
        candidate->kind == LQL_MUTATION_SET)
      return 1;
  }
  return 0;
}

static int
lql_flat_eq_mutation_group_is_top_increment(const lql_flat_eq_program *program,
                                            const lql_mutation_action *action) {
  size_t i;
  if (program == NULL || action == NULL ||
      action->kind != LQL_MUTATION_INCREMENT || action->segment_count != 1u)
    return 0;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *candidate;
    candidate = program->direct_mutation_actions[i];
    if (!lql_flat_eq_mutation_same_top(candidate, action))
      continue;
    if (candidate->kind != LQL_MUTATION_INCREMENT ||
        candidate->segment_count != 1u)
      return 0;
  }
  return 1;
}

static int
lql_flat_eq_mutation_group_is_top_set(const lql_flat_eq_program *program,
                                      const lql_mutation_action *action) {
  size_t i;
  if (program == NULL || action == NULL || action->kind != LQL_MUTATION_SET ||
      action->segment_count != 1u)
    return 0;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *candidate;
    candidate = program->direct_mutation_actions[i];
    if (!lql_flat_eq_mutation_same_top(candidate, action))
      continue;
    if (candidate->kind != LQL_MUTATION_SET || candidate->segment_count != 1u)
      return 0;
  }
  return 1;
}

static const lql_mutation_action *
lql_flat_eq_mutation_group_last_set(const lql_flat_eq_program *program,
                                    const lql_mutation_action *group) {
  size_t i;
  const lql_mutation_action *last;
  if (program == NULL || group == NULL ||
      !lql_flat_eq_mutation_group_is_top_set(program, group))
    return NULL;
  last = NULL;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *candidate;
    candidate = program->direct_mutation_actions[i];
    if (lql_flat_eq_mutation_same_top(candidate, group))
      last = candidate;
  }
  return last;
}

static lql_status
lql_flat_eq_mutation_group_increment_action(const lql_flat_eq_program *program,
                                            const lql_mutation_action *group,
                                            lql_mutation_action *out) {
  size_t i;
  double delta;
  if (program == NULL || group == NULL || out == NULL ||
      !lql_flat_eq_mutation_group_is_top_increment(program, group))
    return LQL_STATUS_INVALID_ARGUMENT;
  *out = *group;
  delta = 0.0;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *candidate;
    candidate = program->direct_mutation_actions[i];
    if (lql_flat_eq_mutation_same_top(candidate, group))
      delta += candidate->delta;
  }
  out->delta = delta;
  return LQL_STATUS_OK;
}

static void lql_flat_eq_mutation_group_mark(const lql_flat_eq_program *program,
                                            const lql_mutation_action *action,
                                            unsigned long *found) {
  size_t i;
  if (program == NULL || action == NULL || found == NULL)
    return;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    if (lql_flat_eq_mutation_same_top(program->direct_mutation_actions[i],
                                      action))
      *found |= 1ul << i;
  }
}

static lql_status lql_flat_eq_mutation_apply_nested_to_spool(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *source, size_t value_start, size_t value_end,
    lql_json_spool *target, lql_error *error) {
  lql_stream_writer_fn saved_writer;
  void *saved_writer_user;
  lql_status status;
  if (state == NULL || action == NULL || source == NULL || target == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  lql_json_spool_reset(target);
  saved_writer = state->writer;
  saved_writer_user = state->writer_user;
  state->writer = lql_flat_eq_spool_writer;
  state->writer_user = target;
  if (action->kind == LQL_MUTATION_SET) {
    status = lql_flat_eq_mutation_nested_set(state, action, source, value_start,
                                             value_end, 1u, error);
  } else if (action->kind == LQL_MUTATION_REMOVE) {
    status = lql_flat_eq_mutation_nested_remove(
        state, action, source, value_start, value_end, 1u, error);
  } else if (action->kind == LQL_MUTATION_INCREMENT) {
    status = lql_flat_eq_mutation_nested_increment(
        state, action, source, value_start, value_end, 1u, error);
  } else {
    status = LQL_STATUS_INVALID_ARGUMENT;
  }
  state->writer = saved_writer;
  state->writer_user = saved_writer_user;
  return status;
}

static lql_status lql_flat_eq_mutation_emit_existing_group(
    lql_flat_eq_state *state, const lql_json_spool *spool,
    const lql_mutation_action *group, size_t value_start, size_t value_end,
    lql_error *error) {
  lql_json_spool first;
  lql_json_spool second;
  const lql_json_spool *source;
  lql_json_spool *target;
  size_t source_start;
  size_t source_end;
  size_t i;
  int initialized_first;
  int initialized_second;
  lql_status status;
  initialized_first = 0;
  initialized_second = 0;
  status = lql_json_spool_init(&first, error);
  if (status != LQL_STATUS_OK)
    return status;
  initialized_first = 1;
  status = lql_json_spool_init(&second, error);
  if (status != LQL_STATUS_OK)
    goto done;
  initialized_second = 1;
  source = spool;
  source_start = value_start;
  source_end = value_end;
  target = &first;
  for (i = 0u; i < state->program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *action;
    action = state->program->direct_mutation_actions[i];
    if (!lql_flat_eq_mutation_same_top(action, group))
      continue;
    status = lql_flat_eq_mutation_apply_nested_to_spool(
        state, action, source, source_start, source_end, target, error);
    if (status != LQL_STATUS_OK)
      goto done;
    source = target;
    source_start = 0u;
    source_end = lql_json_spool_size(source);
    target = target == &first ? &second : &first;
  }
  status =
      lql_json_spool_write_to(source, state->writer, state->writer_user, error);
done:
  if (initialized_second)
    lql_json_spool_cleanup(&second);
  if (initialized_first)
    lql_json_spool_cleanup(&first);
  return status;
}

static lql_status
lql_flat_eq_mutation_emit_missing_group(lql_flat_eq_state *state,
                                        const lql_mutation_action *group,
                                        lql_error *error) {
  lql_json_spool first;
  lql_json_spool second;
  const lql_json_spool *source;
  lql_json_spool *target;
  size_t i;
  int initialized_first;
  int initialized_second;
  lql_status status;
  initialized_first = 0;
  initialized_second = 0;
  status = lql_json_spool_init(&first, error);
  if (status != LQL_STATUS_OK)
    return status;
  initialized_first = 1;
  status = lql_json_spool_init(&second, error);
  if (status != LQL_STATUS_OK)
    goto done;
  initialized_second = 1;
  status = lql_json_spool_append(&first, "{}", 2u, error);
  if (status != LQL_STATUS_OK)
    goto done;
  source = &first;
  target = &second;
  for (i = 0u; i < state->program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *action;
    action = state->program->direct_mutation_actions[i];
    if (!lql_flat_eq_mutation_same_top(action, group))
      continue;
    status = lql_flat_eq_mutation_apply_nested_to_spool(
        state, action, source, 0u, lql_json_spool_size(source), target, error);
    if (status != LQL_STATUS_OK)
      goto done;
    source = target;
    target = target == &first ? &second : &first;
  }
  status =
      lql_json_spool_write_to(source, state->writer, state->writer_user, error);
done:
  if (initialized_second)
    lql_json_spool_cleanup(&second);
  if (initialized_first)
    lql_json_spool_cleanup(&first);
  return status;
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
      size_t group_count;
      group_count = lql_flat_eq_mutation_group_count(state->program, action);
      if (group_count > 1u)
        lql_flat_eq_mutation_group_mark(state->program, action, &found);
      else
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
        if (group_count > 1u && lql_flat_eq_mutation_group_is_top_increment(
                                    state->program, action)) {
          lql_mutation_action grouped_increment;
          status = lql_flat_eq_mutation_group_increment_action(
              state->program, action, &grouped_increment);
          if (status == LQL_STATUS_OK)
            status = lql_flat_eq_mutation_increment(state, &grouped_increment,
                                                    spool, key_end + 1u,
                                                    value_end, 1, error);
        } else if (group_count > 1u && lql_flat_eq_mutation_group_is_top_set(
                                           state->program, action)) {
          const lql_mutation_action *last_set;
          last_set =
              lql_flat_eq_mutation_group_last_set(state->program, action);
          if (last_set == NULL)
            status = LQL_STATUS_INVALID_ARGUMENT;
          else
            status = lql_flat_eq_mutation_value(state, last_set, error);
        } else if (group_count > 1u) {
          status = lql_flat_eq_mutation_emit_existing_group(
              state, spool, action, key_end + 1u, value_end, error);
        } else if (action->kind == LQL_MUTATION_INCREMENT &&
                   action->segment_count == 1u) {
          status = lql_flat_eq_mutation_increment(
              state, action, spool, key_end + 1u, value_end, 1, error);
        } else if (action->kind == LQL_MUTATION_INCREMENT) {
          status = lql_flat_eq_mutation_nested_increment(
              state, action, spool, key_end + 1u, value_end, 1u, error);
        } else if (action->kind == LQL_MUTATION_SET &&
                   action->segment_count > 1u) {
          status = lql_flat_eq_mutation_nested_set(
              state, action, spool, key_end + 1u, value_end, 1u, error);
        } else if (action->kind == LQL_MUTATION_REMOVE &&
                   action->segment_count > 1u) {
          status = lql_flat_eq_mutation_nested_remove(
              state, action, spool, key_end + 1u, value_end, 1u, error);
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
      size_t group_count;
      action = state->program->direct_mutation_actions[i];
      group_count = lql_flat_eq_mutation_group_count(state->program, action);
      if ((found & (1ul << i)) != 0ul || action->kind == LQL_MUTATION_REMOVE ||
          (group_count > 1u &&
           lql_flat_eq_mutation_group_seen(state->program, action, i)) ||
          (group_count > 1u &&
           !lql_flat_eq_mutation_group_has_set(state->program, action) &&
           !lql_flat_eq_mutation_group_is_top_increment(state->program,
                                                        action)) ||
          (group_count > 1u &&
           !lql_flat_eq_mutation_group_is_top_set(state->program, action) &&
           !lql_flat_eq_mutation_group_is_top_increment(state->program,
                                                        action) &&
           action->segment_count == 1u))
        continue;
      if (action->kind == LQL_MUTATION_INCREMENT &&
          action->segment_count > 1u) {
        lql_set_error(error, LQL_STATUS_JSON_ERROR,
                      "increment target was not found");
        return LQL_STATUS_JSON_ERROR;
      }
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status = lql_flat_eq_projection_key(state, action->segments[0], error);
      if (status == LQL_STATUS_OK) {
        if (group_count > 1u && lql_flat_eq_mutation_group_is_top_increment(
                                    state->program, action)) {
          lql_mutation_action grouped_increment;
          status = lql_flat_eq_mutation_group_increment_action(
              state->program, action, &grouped_increment);
          if (status == LQL_STATUS_OK)
            status = lql_flat_eq_mutation_increment(state, &grouped_increment,
                                                    NULL, 0u, 0u, 0, error);
        } else if (group_count > 1u && lql_flat_eq_mutation_group_is_top_set(
                                           state->program, action)) {
          const lql_mutation_action *last_set;
          last_set =
              lql_flat_eq_mutation_group_last_set(state->program, action);
          if (last_set == NULL)
            status = LQL_STATUS_INVALID_ARGUMENT;
          else
            status = lql_flat_eq_mutation_value(state, last_set, error);
        } else if (group_count > 1u) {
          status =
              lql_flat_eq_mutation_emit_missing_group(state, action, error);
        } else {
          status = lql_flat_eq_mutation_key_value(state, action, 0, error);
        }
      }
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
                                     size_t source_offset, size_t source_len,
                                     int source_compact, lql_error *error) {
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
    if (spool == NULL &&
        (!source_compact || !state->request->input_is_compact ||
         state->request->range_writer == NULL)) {
      lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                    "streaming matched value range is unavailable");
      return LQL_STATUS_UNSUPPORTED;
    }
    memset(&value, 0, sizeof(value));
    if (source_compact && state->request->range_writer != NULL) {
      value.storage_kind = LQL_STREAM_VALUE_SOURCE_RANGE;
      value.range_writer = state->request->range_writer;
      value.range_user = state->request->range_user;
      value.range_offset = source_offset;
      value.range_len = source_len;
    } else {
      value.storage_kind = LQL_STREAM_VALUE_JSON_SPOOL;
      value.spool = spool;
    }
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
      if (!source_compact || !state->request->input_is_compact ||
          state->request->range_writer == NULL) {
        lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                      "streaming selected record range is unavailable");
        return LQL_STATUS_UNSUPPORTED;
      }
      status = lql_flat_eq_emit_range(state, source_offset, source_len, error);
    } else {
      status = lql_flat_eq_emit(state, spool, error);
    }
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
    if (!root_is_object) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "combined input must be a JSON object");
      return LQL_STATUS_JSON_ERROR;
    }
    if (lql_flat_eq_projection_found(state, spool)) {
      status = lql_flat_eq_projection_emit(state, spool, error);
      if (status != LQL_STATUS_OK)
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
  if (request == NULL ||
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
      if (action->segment_count == 0u || action->value == NULL)
        return 0;
    } else if (action->kind == LQL_MUTATION_REMOVE) {
      if (action->segment_count == 0u)
        return 0;
    } else if (action->kind == LQL_MUTATION_INCREMENT) {
      if (action->segment_count == 0u)
        return 0;
    } else {
      return 0;
    }
    for (j = 0u; j < action->segment_count; ++j) {
      if (action->segments[j] == NULL)
        return 0;
    }
    for (j = 0u; j < i; ++j) {
      if (strcmp(mutation->actions[j].segments[0], action->segments[0]) == 0 &&
          !lql_flat_eq_mutation_group_compatible(&mutation->actions[j], action))
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
                                      lql_error *error, int allow_spool,
                                      int *out_handled) {
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
  int range_only_value;
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
  if (request->selector != NULL &&
      !lql_flat_eq_append(&program, request->selector, time_bounds_ptr)) {
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
  program.stop_hit_mask = lql_flat_eq_stop_mask(&program, request->selector);
  program.stop_matching_on_hit = program.stop_hit_mask != 0ul;
  *out_handled = 1;
  range_only_value = !allow_spool && request->on_value != NULL &&
                     request->range_writer != NULL && request->input_is_compact;
  capture =
      allow_spool &&
      ((request->on_value != NULL && !range_only_value) ||
       request->output_mode == LQL_STREAM_OUTPUT_SELECTED_RECORD ||
       request->output_mode == LQL_STREAM_OUTPUT_PROJECTION ||
       request->output_mode == LQL_STREAM_OUTPUT_MUTATION ||
       request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION);
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
  scan_request.stop_hit_mask = program.stop_hit_mask;
  scan_request.capture_keys = program.capture_keys;
  scan_request.capture_key_count = program.capture_key_count;
  scan_request.capture_spans = program.capture_spans;
  scan_request.max_records = request->limits.max_records;
  scan_request.max_bytes = request->limits.max_bytes;
  scan_request.record = lql_flat_eq_record;
  scan_request.record_user = &state;
  status =
      lql_json_scan_flat_eq_ndjson(&scan_request, &records, &bytes_read, error);
  result->bytes_consumed = bytes_read;
  if (capture) {
    lql_json_spool_cleanup(&spool);
  }
  if (status == LQL_STATUS_STOP && !result->stopped_early &&
      request->limits.max_records != 0u &&
      result->records_seen >= request->limits.max_records) {
    result->stopped_early = 1;
    result->stop_reason = LQL_STREAM_STOP_RECORD_LIMIT;
  }
  if (status == LQL_STATUS_STOP && !result->stopped_early &&
      request->limits.max_bytes != 0u &&
      result->bytes_consumed >= request->limits.max_bytes) {
    result->stopped_early = 1;
    result->stop_reason = LQL_STREAM_STOP_BYTE_LIMIT;
  }
  if (status == LQL_STATUS_STOP && result->stopped_early) {
    return LQL_STATUS_OK;
  }
  return status;
}
