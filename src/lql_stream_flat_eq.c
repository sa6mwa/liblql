#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "lql_internal.h"
#include "lql_json_scan.h"
#include "lql_unicode_lower.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LQL_FLAT_EQ_TERM_CAPACITY (sizeof(unsigned long) * CHAR_BIT)
typedef struct lql_flat_eq_selector_group {
  const lql_selector *selector;
  size_t first;
  size_t count;
} lql_flat_eq_selector_group;

typedef struct lql_flat_eq_program {
  lql_allocator *allocator;
  lql_json_flat_eq_term *terms;
  const lql_selector **selectors;
  lql_json_flat_eq_term *scan_terms;
  size_t *term_sources;
  size_t scan_term_count;
  lql_flat_eq_selector_group *selector_groups;
  size_t *selector_group_table;
  size_t selector_group_count;
  size_t selector_group_table_size;
  lql_json_capture_key *capture_keys;
  lql_json_capture_span *capture_spans;
  size_t capture_key_count;
  size_t **contains_failures;
  char **icontains_needles;
  size_t term_count;
  int stop_matching_on_hit;
  unsigned long stop_hit_mask;
  const lql_mutation_action **direct_mutation_actions;
  unsigned char *direct_mutation_found;
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
  lql_flat_eq_program *program;
  lql_stream_writer_fn writer;
  void *writer_user;
} lql_flat_eq_state;

static void lql_flat_eq_program_cleanup(lql_flat_eq_program *program) {
  if (program == NULL) {
    return;
  }
  if (program->allocator != NULL) {
    size_t i;
    for (i = 0u; i < program->term_count; ++i) {
      if (program->contains_failures != NULL) {
        program->allocator->destroy(program->allocator,
                                    program->contains_failures[i]);
      }
      if (program->icontains_needles != NULL) {
        program->allocator->destroy(program->allocator,
                                    program->icontains_needles[i]);
      }
    }
    program->allocator->destroy(program->allocator,
                                program->direct_mutation_actions);
    program->allocator->destroy(program->allocator,
                                program->direct_mutation_found);
    program->allocator->destroy(program->allocator,
                                program->selector_group_table);
    program->allocator->destroy(program->allocator, program->selector_groups);
    program->allocator->destroy(program->allocator, program->term_sources);
    program->allocator->destroy(program->allocator, program->scan_terms);
    program->allocator->destroy(program->allocator, program->icontains_needles);
    program->allocator->destroy(program->allocator, program->contains_failures);
    program->allocator->destroy(program->allocator, program->capture_spans);
    program->allocator->destroy(program->allocator, program->capture_keys);
    program->allocator->destroy(program->allocator, program->selectors);
    program->allocator->destroy(program->allocator, program->terms);
  }
  memset(program, 0, sizeof(*program));
}

static int lql_flat_eq_program_init(lql_flat_eq_program *program,
                                    lql_allocator *allocator, size_t term_count,
                                    size_t capture_count,
                                    size_t mutation_count) {
  if (program == NULL || allocator == NULL) {
    return 0;
  }
  memset(program, 0, sizeof(*program));
  program->allocator = allocator;
  if (term_count != 0u) {
    program->terms =
        allocator->calloc(allocator, term_count, sizeof(*program->terms));
    program->selectors =
        allocator->calloc(allocator, term_count, sizeof(*program->selectors));
    program->contains_failures = allocator->calloc(
        allocator, term_count, sizeof(*program->contains_failures));
    program->icontains_needles = allocator->calloc(
        allocator, term_count, sizeof(*program->icontains_needles));
    if (program->terms == NULL || program->selectors == NULL ||
        program->contains_failures == NULL ||
        program->icontains_needles == NULL) {
      lql_flat_eq_program_cleanup(program);
      return 0;
    }
  }
  if (capture_count != 0u) {
    program->capture_keys = allocator->calloc(allocator, capture_count,
                                              sizeof(*program->capture_keys));
    program->capture_spans = allocator->calloc(allocator, capture_count,
                                               sizeof(*program->capture_spans));
    if (program->capture_keys == NULL || program->capture_spans == NULL) {
      lql_flat_eq_program_cleanup(program);
      return 0;
    }
  }
  if (mutation_count != 0u) {
    program->direct_mutation_actions = allocator->calloc(
        allocator, mutation_count, sizeof(*program->direct_mutation_actions));
    program->direct_mutation_found = allocator->calloc(
        allocator, mutation_count, sizeof(*program->direct_mutation_found));
    if (program->direct_mutation_actions == NULL ||
        program->direct_mutation_found == NULL) {
      lql_flat_eq_program_cleanup(program);
      return 0;
    }
  }
  return 1;
}

static int lql_flat_eq_selector_term_count(const lql_selector *selector,
                                           size_t *out) {
  size_t count;
  size_t i;
  if (out == NULL) {
    return 0;
  }
  if (selector == NULL || selector->kind == LQL_SELECTOR_KIND_ALL) {
    *out = 0u;
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_AND ||
      selector->kind == LQL_SELECTOR_KIND_OR ||
      selector->kind == LQL_SELECTOR_KIND_NOT) {
    count = 0u;
    for (i = 0u; i < selector->child_count; ++i) {
      size_t child_count;
      if (!lql_flat_eq_selector_term_count(&selector->children[i],
                                           &child_count) ||
          child_count > (size_t)-1 - count) {
        return 0;
      }
      count += child_count;
    }
    *out = count;
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_IN) {
    *out = selector->any_count;
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_CONTAINS ||
      selector->kind == LQL_SELECTOR_KIND_ICONTAINS) {
    *out = selector->any_count == 0u ? 1u : selector->any_count;
    return 1;
  }
  *out = 1u;
  return 1;
}

static int lql_flat_eq_text_equal(const char *left, size_t left_len,
                                  const char *right, size_t right_len) {
  return left_len == right_len &&
         (left_len == 0u || (left != NULL && right != NULL &&
                             memcmp(left, right, left_len) == 0));
}

static int
lql_flat_eq_scan_term_equivalent(const lql_json_flat_eq_term *left,
                                 const lql_json_flat_eq_term *right) {
  if (left == NULL || right == NULL || left->kind != right->kind ||
      left->kind == LQL_JSON_FLAT_TERM_NUMBER_RANGE ||
      left->kind == LQL_JSON_FLAT_TERM_TEMPORAL_RANGE ||
      left->kind == LQL_JSON_FLAT_TERM_CONTAINS ||
      left->kind == LQL_JSON_FLAT_TERM_ICONTAINS ||
      left->kind == LQL_JSON_FLAT_TERM_IPREFIX) {
    return 0;
  }
  return left->path_segment_count == right->path_segment_count &&
         left->path_array_segments == right->path_array_segments &&
         left->path_object_wildcards == right->path_object_wildcards &&
         left->path_array_wildcards == right->path_array_wildcards &&
         left->path_any_wildcards == right->path_any_wildcards &&
         left->path_recursive_segments == right->path_recursive_segments &&
         lql_flat_eq_text_equal(left->field, left->field_len, right->field,
                                right->field_len) &&
         lql_flat_eq_text_equal(left->path, left->path_len, right->path,
                                right->path_len) &&
         lql_flat_eq_text_equal(left->value, left->value_len, right->value,
                                right->value_len);
}

static int lql_flat_eq_program_build_scan_terms(lql_flat_eq_program *program) {
  size_t i;
  if (program == NULL || program->term_count == 0u) {
    return 1;
  }
  program->scan_terms = program->allocator->calloc(
      program->allocator, program->term_count, sizeof(*program->scan_terms));
  program->term_sources = program->allocator->calloc(
      program->allocator, program->term_count, sizeof(*program->term_sources));
  if (program->scan_terms == NULL || program->term_sources == NULL) {
    return 0;
  }
  for (i = 0u; i < program->term_count; ++i) {
    size_t source;
    for (source = 0u; source < program->scan_term_count; ++source) {
      if (lql_flat_eq_scan_term_equivalent(&program->terms[i],
                                           &program->scan_terms[source])) {
        break;
      }
    }
    if (source == program->scan_term_count) {
      program->scan_terms[source] = program->terms[i];
      ++program->scan_term_count;
    }
    program->term_sources[i] = source;
  }
  return 1;
}

static size_t lql_flat_eq_selector_group_hash(const lql_selector *selector) {
  return (size_t)((uintptr_t)selector >> 4u);
}

static int
lql_flat_eq_program_build_selector_index(lql_flat_eq_program *program) {
  size_t table_size;
  size_t i;
  if (program == NULL || program->term_count == 0u) {
    return 1;
  }
  if (program->term_count > (size_t)-1 / 2u) {
    return 0;
  }
  table_size = 1u;
  while (table_size < program->term_count * 2u) {
    if (table_size > (size_t)-1 / 2u) {
      return 0;
    }
    table_size *= 2u;
  }
  program->selector_groups =
      program->allocator->calloc(program->allocator, program->term_count,
                                 sizeof(*program->selector_groups));
  program->selector_group_table = program->allocator->alloc(
      program->allocator, table_size * sizeof(*program->selector_group_table));
  if (program->selector_groups == NULL ||
      program->selector_group_table == NULL) {
    return 0;
  }
  for (i = 0u; i < table_size; ++i) {
    program->selector_group_table[i] = (size_t)-1;
  }
  program->selector_group_table_size = table_size;
  for (i = 0u; i < program->term_count; ++i) {
    size_t slot;
    size_t group;
    slot = lql_flat_eq_selector_group_hash(program->selectors[i]) &
           (table_size - 1u);
    while (program->selector_group_table[slot] != (size_t)-1 &&
           program->selector_groups[program->selector_group_table[slot]]
                   .selector != program->selectors[i]) {
      slot = (slot + 1u) & (table_size - 1u);
    }
    group = program->selector_group_table[slot];
    if (group == (size_t)-1) {
      group = program->selector_group_count++;
      program->selector_group_table[slot] = group;
      program->selector_groups[group].selector = program->selectors[i];
      program->selector_groups[group].first = i;
    }
    ++program->selector_groups[group].count;
  }
  return 1;
}

static const lql_flat_eq_selector_group *
lql_flat_eq_selector_group_find(const lql_flat_eq_program *program,
                                const lql_selector *selector) {
  size_t slot;
  if (program == NULL || selector == NULL ||
      program->selector_group_table == NULL ||
      program->selector_group_table_size == 0u) {
    return NULL;
  }
  slot = lql_flat_eq_selector_group_hash(selector) &
         (program->selector_group_table_size - 1u);
  while (program->selector_group_table[slot] != (size_t)-1) {
    const lql_flat_eq_selector_group *group;
    group = &program->selector_groups[program->selector_group_table[slot]];
    if (group->selector == selector) {
      return group;
    }
    slot = (slot + 1u) & (program->selector_group_table_size - 1u);
  }
  return NULL;
}

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
        segment_count >= sizeof(unsigned long) * CHAR_BIT) {
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
  *out_recursive_segments = recursive_segments;
  return 1;
}

static int lql_flat_eq_contains_failure(lql_flat_eq_program *program,
                                        lql_json_flat_eq_term *term) {
  size_t i;
  size_t matched;
  size_t *failure;
  if (program == NULL || term == NULL || term->value == NULL) {
    return 0;
  }
  if (term->value_len == 0u) {
    term->contains_failure = NULL;
    return 1;
  }
  failure = (size_t *)program->allocator->calloc(
      program->allocator, term->value_len, sizeof(*failure));
  if (failure == NULL) {
    return 0;
  }
  program->contains_failures[program->term_count] = failure;
  term->contains_failure = failure;
  matched = 0u;
  for (i = 1u; i < term->value_len; ++i) {
    while (matched != 0u && term->value[i] != term->value[matched]) {
      matched = failure[matched - 1u];
    }
    if (term->value[i] == term->value[matched]) {
      ++matched;
    }
    failure[i] = matched;
  }
  return 1;
}

static int lql_flat_eq_lower_value(lql_flat_eq_program *program,
                                   const char *value, size_t value_len,
                                   const char **out_value,
                                   size_t *out_value_len) {
  char *lowered;
  size_t capacity;
  size_t lowered_len;

  if (program == NULL || out_value == NULL || out_value_len == NULL ||
      (value == NULL && value_len != 0u)) {
    return 0;
  }
  if (value_len > ((size_t)-1 - 1u) / 2u) {
    return 0;
  }
  capacity = value_len * 2u + 1u;
  lowered = (char *)program->allocator->alloc(program->allocator, capacity);
  if (lowered == NULL) {
    return 0;
  }
  if (!lql_unicode_utf8_lower(value == NULL ? "" : value, value_len, lowered,
                              capacity, &lowered_len)) {
    program->allocator->destroy(program->allocator, lowered);
    return 0;
  }
  program->icontains_needles[program->term_count] = lowered;
  *out_value = lowered;
  *out_value_len = lowered_len;
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

static int lql_flat_eq_parse_unsigned_integer_token(const char *text,
                                                    size_t len,
                                                    unsigned long *out) {
  unsigned long value;
  unsigned long digit;
  size_t i;
  if (text == NULL || len == 0u || out == NULL) {
    return 0;
  }
  if (text[0] == '0') {
    if (len != 1u) {
      return 0;
    }
    *out = 0ul;
    return 1;
  }
  if (text[0] < '1' || text[0] > '9') {
    return 0;
  }
  value = 0ul;
  for (i = 0u; i < len; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      return 0;
    }
    digit = (unsigned long)(text[i] - '0');
    if (value > (ULONG_MAX - digit) / 10ul) {
      return 0;
    }
    value = value * 10ul + digit;
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
  size_t match_segment;
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
  match_segment = recursive_segment + 1u;
  while (match_segment < term->path_segment_count &&
         match_segment < sizeof(unsigned long) * CHAR_BIT &&
         (term->path_recursive_segments & (1ul << match_segment)) != 0ul) {
    slash = strchr(segment, '/');
    if (slash == NULL) {
      term->path_recursive_match = NULL;
      term->path_recursive_match_len = 0u;
      term->path_recursive_match_segment = 0u;
      return;
    }
    segment = slash + 1;
    ++match_segment;
  }
  if (match_segment >= term->path_segment_count) {
    term->path_recursive_match = NULL;
    term->path_recursive_match_len = 0u;
    term->path_recursive_match_segment = 0u;
    return;
  }
  slash = strchr(segment, '/');
  term->path_recursive_match = segment;
  term->path_recursive_match_len =
      slash == NULL ? strlen(segment) : (size_t)(slash - segment);
  term->path_recursive_match_segment = match_segment;
}

static int lql_flat_eq_plain_segment(const char *segment, size_t segment_len) {
  size_t i;
  if (segment == NULL)
    return 0;
  for (i = 0u; i < segment_len; ++i) {
    if (segment[i] == '~')
      return 0;
  }
  return 1;
}

static void lql_flat_eq_cache_paths(lql_json_flat_eq_term *term) {
  term->field_plain = lql_flat_eq_plain_segment(term->field, term->field_len);
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
  if ((selector->any_count != 0u &&
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
      value_len = selector->value_len;
    }
    if (value == NULL) {
      return 0;
    }
    term = &program->terms[program->term_count];
    term->kind = ignore_case ? LQL_JSON_FLAT_TERM_ICONTAINS
                             : LQL_JSON_FLAT_TERM_CONTAINS;
    term->field = field + 1;
    term->field_len = first_segment_len;
    if (ignore_case) {
      if (!lql_flat_eq_lower_value(program, value, value_len, &term->value,
                                   &term->value_len)) {
        return 0;
      }
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
      if (program->icontains_needles[program->term_count] != NULL) {
        program->allocator->destroy(
            program->allocator,
            program->icontains_needles[program->term_count]);
        program->icontains_needles[program->term_count] = NULL;
      }
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
              &term_kind)) {
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
    term->range_gt_text = selector->range_gt_text;
    term->range_gt_text_len =
        selector->range_gt_text == NULL ? 0u : strlen(selector->range_gt_text);
    term->range_gt_unsigned_ready = lql_flat_eq_parse_unsigned_integer_token(
        term->range_gt_text, term->range_gt_text_len, &term->range_gt_unsigned);
    term->range_gte_text = selector->range_gte_text;
    term->range_gte_text_len = selector->range_gte_text == NULL
                                   ? 0u
                                   : strlen(selector->range_gte_text);
    term->range_gte_unsigned_ready = lql_flat_eq_parse_unsigned_integer_token(
        term->range_gte_text, term->range_gte_text_len,
        &term->range_gte_unsigned);
    term->range_lt_text = selector->range_lt_text;
    term->range_lt_text_len =
        selector->range_lt_text == NULL ? 0u : strlen(selector->range_lt_text);
    term->range_lt_unsigned_ready = lql_flat_eq_parse_unsigned_integer_token(
        term->range_lt_text, term->range_lt_text_len, &term->range_lt_unsigned);
    term->range_lte_text = selector->range_lte_text;
    term->range_lte_text_len = selector->range_lte_text == NULL
                                   ? 0u
                                   : strlen(selector->range_lte_text);
    term->range_lte_unsigned_ready = lql_flat_eq_parse_unsigned_integer_token(
        term->range_lte_text, term->range_lte_text_len,
        &term->range_lte_unsigned);
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
       selector->kind != LQL_SELECTOR_KIND_NE &&
       selector->kind != LQL_SELECTOR_KIND_EXISTS &&
       selector->kind != LQL_SELECTOR_KIND_PREFIX &&
       selector->kind != LQL_SELECTOR_KIND_IPREFIX) ||
      selector->field == NULL) {
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
  } else if ((selector->kind == LQL_SELECTOR_KIND_EQ ||
              selector->kind == LQL_SELECTOR_KIND_NE) &&
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
      if (selector->kind == LQL_SELECTOR_KIND_EQ ||
          selector->kind == LQL_SELECTOR_KIND_NE) {
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
      if (!lql_flat_eq_lower_value(program, selector->value,
                                   selector->value_len, &term->value,
                                   &term->value_len)) {
        return 0;
      }
    } else {
      term->value = selector->value;
      term->value_len = selector->value_len;
    }
  }
  lql_flat_eq_cache_paths(term);
  program->selectors[program->term_count] = selector;
  ++program->term_count;
  return 1;
}

static int lql_flat_eq_hit(const unsigned long *hits, size_t index) {
  if (hits == NULL) {
    return 0;
  }
  return (hits[index / LQL_FLAT_EQ_TERM_CAPACITY] &
          (1ul << (index % LQL_FLAT_EQ_TERM_CAPACITY))) != 0ul;
}

static int lql_flat_eq_matches(const lql_flat_eq_program *program,
                               const lql_selector *selector,
                               const unsigned long *hits) {
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
  {
    const lql_flat_eq_selector_group *group;
    int hit;
    group = lql_flat_eq_selector_group_find(program, selector);
    if (group == NULL) {
      return selector->kind == LQL_SELECTOR_KIND_NE;
    }
    hit = 0;
    for (i = group->first; i < group->first + group->count; ++i) {
      size_t source;
      source = program->term_sources == NULL ? i : program->term_sources[i];
      if (lql_flat_eq_hit(hits, source)) {
        hit = 1;
        break;
      }
    }
    if (selector->kind == LQL_SELECTOR_KIND_NE)
      return !hit;
    return hit;
  }
}

static int lql_flat_eq_matches_record(const lql_flat_eq_program *program,
                                      const lql_selector *selector,
                                      const unsigned long *hits,
                                      int root_is_object) {
  size_t i;
  if (selector == NULL || selector->kind == LQL_SELECTOR_KIND_ALL)
    return 1;
  if (selector->kind == LQL_SELECTOR_KIND_AND) {
    for (i = 0u; i < selector->child_count; ++i) {
      if (!lql_flat_eq_matches_record(program, &selector->children[i], hits,
                                      root_is_object))
        return 0;
    }
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_OR) {
    for (i = 0u; i < selector->child_count; ++i) {
      if (lql_flat_eq_matches_record(program, &selector->children[i], hits,
                                     root_is_object))
        return 1;
    }
    return 0;
  }
  if (selector->kind == LQL_SELECTOR_KIND_NOT) {
    return selector->child_count == 1u &&
           !lql_flat_eq_matches_record(program, &selector->children[0], hits,
                                       root_is_object);
  }
  /*
   * Field inequality is a field predicate, not a scalar-root selector. Keep
   * explicit logical NOT independent so "not./a=x" retains its selector logic.
   */
  if (!root_is_object && selector->kind == LQL_SELECTOR_KIND_NE)
    return 0;
  return lql_flat_eq_matches(program, selector, hits);
}

static int lql_flat_eq_required_eq_count(const lql_selector *selector,
                                         size_t *count) {
  size_t i;
  if (selector == NULL || count == NULL) {
    return 0;
  }
  if (selector->kind == LQL_SELECTOR_KIND_AND) {
    for (i = 0u; i < selector->child_count; ++i) {
      if (!lql_flat_eq_required_eq_count(&selector->children[i], count)) {
        return 0;
      }
    }
    return selector->child_count != 0u;
  }
  if (selector->kind != LQL_SELECTOR_KIND_EQ || selector->any_count != 0u ||
      *count == (size_t)-1) {
    return 0;
  }
  ++*count;
  return 1;
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
lql_flat_eq_projection_child_shape(const lql_flat_eq_program *program,
                                   size_t anchor, size_t depth,
                                   int *out_array) {
  size_t i;
  size_t index;
  int have_array;
  int have_object;
  have_array = 0;
  have_object = 0;
  for (i = 0u; i < program->capture_key_count; ++i) {
    if (program->capture_spans[i].found &&
        program->capture_keys[i].segment_count > depth &&
        lql_flat_eq_projection_prefix(program, anchor, i, depth)) {
      if (lql_flat_eq_projection_segment_index(
              program->capture_keys[i].segments[depth], &index)) {
        have_array = 1;
      } else {
        have_object = 1;
      }
    }
  }
  if (have_array && have_object)
    return 0;
  *out_array = have_array;
  return 1;
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

typedef struct lql_flat_eq_file_stream {
  lql_stream_reader_fn reader;
  void *reader_user;
  lql_mutation_file_close_fn close;
  void *close_user;
} lql_flat_eq_file_stream;

static lql_status lql_flat_eq_local_file_read(void *user, unsigned char *buffer,
                                              size_t capacity, size_t *out_len,
                                              lql_error *error) {
  FILE *file;
  size_t amount;
  if (out_len == NULL || buffer == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  *out_len = 0u;
  file = (FILE *)user;
  if (file == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  amount = fread(buffer, 1u, capacity, file);
  if (amount == 0u && ferror(file)) {
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "unable to read file-backed mutation value");
    return LQL_STATUS_IO_ERROR;
  }
  *out_len = amount;
  return LQL_STATUS_OK;
}

static void lql_flat_eq_local_file_close(void *user, void *reader_user) {
  (void)user;
  if (reader_user != NULL)
    fclose((FILE *)reader_user);
}

static lql_status lql_flat_eq_file_open(const lql_mutation_action *action,
                                        lql_flat_eq_file_stream *out,
                                        lql_error *error) {
  FILE *file;
  lql_string_view path;
  lql_status status;
  if (action == NULL || out == NULL || action->value == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  /* Reopen per pass: auto mode inspects before emitting, and a mutation may
   * apply to many records. This preserves bounded streaming without a seek
   * requirement or a cached descriptor on the immutable mutation handle. */
  if (action->file_value_open == NULL) {
    file = fopen(action->value, "rb");
    if (file == NULL) {
      lql_set_error(error, LQL_STATUS_IO_ERROR,
                    "unable to open file-backed mutation value");
      return LQL_STATUS_IO_ERROR;
    }
    out->reader = lql_flat_eq_local_file_read;
    out->reader_user = file;
    out->close = lql_flat_eq_local_file_close;
    return LQL_STATUS_OK;
  }
  path.data = action->value;
  path.len = strlen(action->value);
  status = action->file_value_open(action->file_value_user, path, &out->reader,
                                   &out->reader_user, error);
  if (status != LQL_STATUS_OK) {
    if (error != NULL && error->code == LQL_STATUS_OK)
      lql_set_error(error, status, "file value open callback failed");
    return status;
  }
  out->close = action->file_value_close;
  out->close_user = action->file_value_user;
  if (out->reader == NULL || out->close == NULL) {
    if (out->close != NULL)
      out->close(out->close_user, out->reader_user);
    memset(out, 0, sizeof(*out));
    lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                  "file value open callback returned no reader");
    return LQL_STATUS_CALLBACK_ERROR;
  }
  return LQL_STATUS_OK;
}

static void lql_flat_eq_file_close(lql_flat_eq_file_stream *stream) {
  if (stream != NULL && stream->close != NULL)
    stream->close(stream->close_user, stream->reader_user);
  if (stream != NULL)
    memset(stream, 0, sizeof(*stream));
}

static lql_status lql_flat_eq_file_read(lql_flat_eq_file_stream *stream,
                                        unsigned char *buffer, size_t capacity,
                                        size_t *out_len, lql_error *error) {
  lql_status status;
  if (stream == NULL || stream->reader == NULL || out_len == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  *out_len = 0u;
  status =
      stream->reader(stream->reader_user, buffer, capacity, out_len, error);
  if (status != LQL_STATUS_OK) {
    if (error != NULL && error->code == LQL_STATUS_OK)
      lql_set_error(error, status, "file value reader failed");
    return status;
  }
  if (*out_len > capacity) {
    lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                  "file value reader exceeded its buffer capacity");
    return LQL_STATUS_CALLBACK_ERROR;
  }
  return LQL_STATUS_OK;
}

static lql_status lql_flat_eq_file_textlike(const lql_mutation_action *action,
                                            int strict, int *out_textlike,
                                            lql_error *error) {
  unsigned char buffer[4096];
  lql_flat_eq_utf8_state utf8;
  lql_flat_eq_file_stream stream;
  size_t amount;
  size_t i;
  lql_status status;

  *out_textlike = 0;
  memset(&utf8, 0, sizeof(utf8));
  status = lql_flat_eq_file_open(action, &stream, error);
  if (status != LQL_STATUS_OK)
    return status;
  for (;;) {
    status =
        lql_flat_eq_file_read(&stream, buffer, sizeof(buffer), &amount, error);
    if (status != LQL_STATUS_OK)
      break;
    if (amount == 0u)
      break;
    for (i = 0u; i < amount; ++i) {
      if (buffer[i] == 0u) {
        if (strict) {
          lql_set_error(error, LQL_STATUS_JSON_ERROR,
                        "textfile mutation value contains NUL byte");
          status = LQL_STATUS_JSON_ERROR;
          goto done;
        }
        goto done;
      }
      if (!lql_flat_eq_utf8_accept(&utf8, buffer[i])) {
        if (strict) {
          lql_set_error(error, LQL_STATUS_JSON_ERROR,
                        "textfile mutation value is not valid UTF-8");
          status = LQL_STATUS_JSON_ERROR;
          goto done;
        }
        goto done;
      }
    }
  }
  if (status == LQL_STATUS_OK && utf8.remaining != 0) {
    if (strict) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "textfile mutation value is not valid UTF-8");
      status = LQL_STATUS_JSON_ERROR;
      goto done;
    }
    goto done;
  }
  if (status == LQL_STATUS_OK)
    *out_textlike = 1;
done:
  lql_flat_eq_file_close(&stream);
  return status;
}

static lql_status
lql_flat_eq_file_text_json_string(lql_flat_eq_state *state,
                                  const lql_mutation_action *action,
                                  int validate_utf8, lql_error *error) {
  static const char hex[] = "0123456789abcdef";
  unsigned char buffer[4096];
  lql_flat_eq_utf8_state utf8;
  lql_flat_eq_file_stream stream;
  size_t amount;
  size_t i;
  lql_status status;

  memset(&utf8, 0, sizeof(utf8));
  status = lql_flat_eq_file_open(action, &stream, error);
  if (status != LQL_STATUS_OK)
    return status;
  status = lql_flat_eq_write(state, "\"", 1u, error);
  if (status != LQL_STATUS_OK)
    goto done;
  for (;;) {
    status =
        lql_flat_eq_file_read(&stream, buffer, sizeof(buffer), &amount, error);
    if (status != LQL_STATUS_OK || amount == 0u)
      break;
    for (i = 0u; i < amount; ++i) {
      unsigned char ch;
      ch = buffer[i];
      if (validate_utf8) {
        if (ch == 0u) {
          lql_set_error(error, LQL_STATUS_JSON_ERROR,
                        "textfile mutation value contains NUL byte");
          status = LQL_STATUS_JSON_ERROR;
          goto done;
        }
        if (!lql_flat_eq_utf8_accept(&utf8, ch)) {
          lql_set_error(error, LQL_STATUS_JSON_ERROR,
                        "textfile mutation value is not valid UTF-8");
          status = LQL_STATUS_JSON_ERROR;
          goto done;
        }
      }
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
        goto done;
    }
  }
  if (status == LQL_STATUS_OK && validate_utf8 && utf8.remaining != 0) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "textfile mutation value is not valid UTF-8");
    status = LQL_STATUS_JSON_ERROR;
  }
  if (status == LQL_STATUS_OK)
    status = lql_flat_eq_write(state, "\"", 1u, error);
done:
  lql_flat_eq_file_close(&stream);
  return status;
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
  lql_flat_eq_file_stream stream;
  size_t amount;
  size_t i;
  size_t carry_len;
  size_t out_len;
  lql_status status;

  status = lql_flat_eq_file_open(action, &stream, error);
  if (status != LQL_STATUS_OK)
    return status;
  status = lql_flat_eq_write(state, "\"", 1u, error);
  if (status != LQL_STATUS_OK)
    goto done;
  carry_len = 0u;
  out_len = 0u;
  for (;;) {
    status =
        lql_flat_eq_file_read(&stream, buffer, sizeof(buffer), &amount, error);
    if (status != LQL_STATUS_OK || amount == 0u)
      break;
    i = 0u;
    if (carry_len != 0u) {
      while (carry_len < 3u && i < amount) {
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
    while (i + 3u <= amount) {
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
        goto done;
      out_len = 0u;
    }
    while (i < amount) {
      carry[carry_len++] = buffer[i++];
    }
  }
  if (status == LQL_STATUS_OK && carry_len != 0u) {
    unsigned long triple;
    if (out_len + 4u > sizeof(outbuf)) {
      status = lql_flat_eq_write(state, outbuf, out_len, error);
      if (status != LQL_STATUS_OK)
        goto done;
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
      goto done;
  }
  if (status == LQL_STATUS_OK)
    status = lql_flat_eq_write(state, "\"", 1u, error);
done:
  lql_flat_eq_file_close(&stream);
  return status;
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
    /*
     * Explicit textfile: values must fail before any record bytes are emitted.
     * The emit pass still validates as a race guard if the file changes between
     * validation and streaming, but the ordinary invalid-input path is clean.
     */
    return lql_flat_eq_file_text_json_string(state, action, 1, error);
  }
  status = lql_flat_eq_file_textlike(action, 0, &textlike, error);
  if (status != LQL_STATUS_OK)
    return status;
  return textlike ? lql_flat_eq_file_text_json_string(state, action, 1, error)
                  : lql_flat_eq_file_base64_json_string(state, action, error);
}

static lql_status
lql_flat_eq_preflight_file_mutations(const lql_flat_eq_program *program,
                                     lql_error *error) {
  size_t i;
  int textlike;
  lql_status status;

  if (program == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *action;
    action = program->direct_mutation_actions[i];
    if (action->kind == LQL_MUTATION_SET &&
        action->value_kind == LQL_MUTATION_VALUE_FILE_TEXT) {
      /*
       * Explicit textfile: mutations are deterministic input validation, so
       * fail before scanning starts. That preserves the stream contract:
       * invalid mutation inputs never leave consumers with a valid-looking
       * prefix.
       */
      status = lql_flat_eq_file_textlike(action, 1, &textlike, error);
      if (status != LQL_STATUS_OK)
        return status;
    }
  }
  return LQL_STATUS_OK;
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
lql_flat_eq_mutation_increment(lql_flat_eq_state *state,
                               const lql_mutation_action *action,
                               const lql_json_spool *spool, size_t value_start,
                               size_t value_end, int present, lql_error *error);

static int lql_flat_eq_mutation_segment_is_wildcard(const char *segment) {
  return segment != NULL &&
         (strcmp(segment, "*") == 0 || strcmp(segment, "[]") == 0 ||
          strcmp(segment, "**") == 0 || strcmp(segment, "...") == 0);
}

static int
lql_flat_eq_mutation_segment_matches_any_object_key(const char *segment) {
  return segment != NULL &&
         (strcmp(segment, "*") == 0 || strcmp(segment, "**") == 0 ||
          strcmp(segment, "...") == 0);
}

static int
lql_flat_eq_mutation_segment_matches_any_array_index(const char *segment) {
  return segment != NULL &&
         (strcmp(segment, "[]") == 0 || strcmp(segment, "**") == 0 ||
          strcmp(segment, "...") == 0);
}

static lql_status lql_flat_eq_mutation_object_key_matches_segment(
    const lql_json_spool *spool, size_t key_start, size_t size,
    const char *segment, int *out_match, lql_error *error) {
  if (lql_flat_eq_mutation_segment_matches_any_object_key(segment)) {
    *out_match = 1;
    return LQL_STATUS_OK;
  }
  if (segment != NULL && strcmp(segment, "[]") == 0) {
    *out_match = 0;
    return LQL_STATUS_OK;
  }
  return lql_flat_eq_key_equals(spool, key_start, size, segment, out_match,
                                error);
}

static int
lql_flat_eq_mutation_suffix_has_wildcard(const lql_mutation_action *action,
                                         size_t depth) {
  size_t i;
  if (action == NULL) {
    return 0;
  }
  for (i = depth; i < action->segment_count; ++i) {
    if (lql_flat_eq_mutation_segment_is_wildcard(action->segments[i])) {
      return 1;
    }
  }
  return 0;
}

static size_t
lql_flat_eq_mutation_collapse_ellipsis(const lql_mutation_action *action,
                                       size_t depth) {
  if (action == NULL)
    return depth;
  /*
   * The language treats repeated recursive segments as one recursive segment:
   * /.../.../b reaches the same set of b fields as /.../b. Keep the collapse
   * in the executor so public parsing can stay syntax-preserving.
   */
  while (
      depth + 1u < action->segment_count &&
      strcmp(action->segments[depth], "...") == 0 &&
      strcmp(action->segments[depth + 1u], "...") == 0 &&
      depth + 2u < action->segment_count &&
      !lql_flat_eq_mutation_segment_is_wildcard(action->segments[depth + 2u])) {
    ++depth;
  }
  return depth;
}

static int lql_flat_eq_mutation_terminal_repeated_ellipsis(
    const lql_mutation_action *action, size_t depth) {
  return action != NULL && depth + 2u == action->segment_count &&
         strcmp(action->segments[depth], "...") == 0 &&
         strcmp(action->segments[depth + 1u], "...") == 0;
}

static int lql_flat_eq_spooled_value_is_container(const lql_json_spool *spool,
                                                  size_t value_start,
                                                  lql_error *error) {
  unsigned char ch;
  if (spool == NULL)
    return 0;
  if (lql_flat_eq_spool_byte(spool, value_start, &ch, error) != LQL_STATUS_OK)
    return 0;
  return ch == (unsigned char)'{' || ch == (unsigned char)'[';
}

static int lql_flat_eq_spooled_value_accepts_segment(
    const lql_json_spool *spool, size_t value_start, const char *segment,
    lql_error *error) {
  unsigned char ch;
  size_t index;
  if (spool == NULL || segment == NULL)
    return 0;
  if (lql_flat_eq_spool_byte(spool, value_start, &ch, error) != LQL_STATUS_OK)
    return 0;
  if (ch == (unsigned char)'{')
    return strcmp(segment, "[]") != 0;
  if (ch == (unsigned char)'[')
    return lql_flat_eq_mutation_segment_matches_any_array_index(segment) ||
           lql_flat_eq_projection_segment_index(segment, &index);
  return 0;
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
    if (action->kind == LQL_MUTATION_INCREMENT) {
      status =
          lql_flat_eq_mutation_increment(state, action, NULL, 0u, 0u, 0, error);
    } else if (action->kind == LQL_MUTATION_SET) {
      status = lql_flat_eq_mutation_value(state, action, error);
    } else {
      status = LQL_STATUS_INVALID_ARGUMENT;
    }
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
    number = (char *)state->program->allocator->alloc(state->program->allocator,
                                                      len + 1u);
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
          state->program->allocator->destroy(state->program->allocator, number);
        return copy_status;
      }
      number[i] = (char)ch;
    }
  }
  number[len] = '\0';
  if (!lql_number_parse_json(number, len, &current)) {
    if (number != stack_number)
      state->program->allocator->destroy(state->program->allocator, number);
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment target number is invalid");
    return LQL_STATUS_JSON_ERROR;
  }
  if (number != stack_number)
    state->program->allocator->destroy(state->program->allocator, number);
  next = current + action->delta;
  return lql_flat_eq_mutation_number(state, next, error);
}

static lql_status lql_flat_eq_mutation_validate_increment_path(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error);

static int lql_flat_eq_mutation_same_path(const lql_mutation_action *left,
                                          const lql_mutation_action *right) {
  size_t i;
  if (left == NULL || right == NULL ||
      left->segment_count != right->segment_count)
    return 0;
  for (i = 0u; i < left->segment_count; ++i)
    if (strcmp(left->segments[i], right->segments[i]) != 0)
      return 0;
  return 1;
}

static int lql_flat_eq_mutation_path_prefix(const lql_mutation_action *prefix,
                                            const lql_mutation_action *action) {
  size_t i;
  if (prefix == NULL || action == NULL ||
      prefix->segment_count >= action->segment_count)
    return 0;
  for (i = 0u; i < prefix->segment_count; ++i)
    if (strcmp(prefix->segments[i], action->segments[i]) != 0)
      return 0;
  return 1;
}

static int lql_flat_eq_mutation_segment_may_overlap(const char *left,
                                                    const char *right) {
  if (left == NULL || right == NULL)
    return 0;
  return strcmp(left, right) == 0 ||
         lql_flat_eq_mutation_segment_is_wildcard(left) ||
         lql_flat_eq_mutation_segment_is_wildcard(right);
}

static int
lql_flat_eq_mutation_path_may_overlap(const lql_mutation_action *left,
                                      const lql_mutation_action *right) {
  size_t i;
  size_t count;
  if (left == NULL || right == NULL || left->segment_count == 0u ||
      right->segment_count == 0u)
    return 0;
  count = left->segment_count < right->segment_count ? left->segment_count
                                                     : right->segment_count;
  for (i = 0u; i < count; ++i) {
    if (!lql_flat_eq_mutation_segment_may_overlap(left->segments[i],
                                                  right->segments[i]))
      return 0;
    if (strcmp(left->segments[i], "...") == 0 ||
        strcmp(right->segments[i], "...") == 0)
      return 1;
  }
  return 1;
}

static lql_status lql_flat_eq_mutation_increment_replaced_before(
    lql_flat_eq_state *state, size_t action_index, int *out_replaced,
    lql_error *error) {
  const lql_mutation_action *action;
  size_t i;
  if (state == NULL || state->program == NULL || out_replaced == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  *out_replaced = 0;
  action = state->program->direct_mutation_actions[action_index];
  for (i = action_index; i > 0u; --i) {
    const lql_mutation_action *previous;
    previous = state->program->direct_mutation_actions[i - 1u];
    if (!lql_flat_eq_mutation_same_path(previous, action) &&
        !lql_flat_eq_mutation_path_prefix(previous, action) &&
        !lql_flat_eq_mutation_path_may_overlap(previous, action))
      continue;
    if (previous->kind == LQL_MUTATION_SET &&
        lql_flat_eq_mutation_same_path(previous, action)) {
      *out_replaced = 1;
      if (previous->value_kind != LQL_MUTATION_VALUE_NUMBER) {
        lql_set_error(error, LQL_STATUS_JSON_ERROR,
                      "increment target number is invalid");
        return LQL_STATUS_JSON_ERROR;
      }
      return LQL_STATUS_OK;
    }
    if (previous->kind == LQL_MUTATION_SET ||
        previous->kind == LQL_MUTATION_REMOVE) {
      *out_replaced = 1;
      return LQL_STATUS_OK;
    }
  }
  return LQL_STATUS_OK;
}

static lql_status lql_flat_eq_mutation_validate_increment_number(
    lql_flat_eq_state *state, const lql_json_spool *spool, size_t value_start,
    size_t value_end, lql_error *error) {
  char stack_number[128];
  char *number;
  double parsed;
  size_t len;
  size_t i;
  unsigned char ch;
  lql_status status;
  if (state == NULL || spool == NULL || value_end <= value_start) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment target number is invalid");
    return LQL_STATUS_JSON_ERROR;
  }
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (ch != (unsigned char)'-' &&
      (ch < (unsigned char)'0' || ch > (unsigned char)'9')) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment target number is invalid");
    return LQL_STATUS_JSON_ERROR;
  }
  len = value_end - value_start;
  number = len < sizeof(stack_number) ? stack_number : NULL;
  if (number == NULL) {
    number = (char *)state->program->allocator->alloc(state->program->allocator,
                                                      len + 1u);
    if (number == NULL) {
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      return LQL_STATUS_NO_MEMORY;
    }
  }
  for (i = 0u; i < len; ++i) {
    status = lql_flat_eq_spool_byte(spool, value_start + i, &ch, error);
    if (status != LQL_STATUS_OK) {
      if (number != stack_number)
        state->program->allocator->destroy(state->program->allocator, number);
      return status;
    }
    number[i] = (char)ch;
  }
  number[len] = '\0';
  if (!lql_number_parse_json(number, len, &parsed)) {
    if (number != stack_number)
      state->program->allocator->destroy(state->program->allocator, number);
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "increment target number is invalid");
    return LQL_STATUS_JSON_ERROR;
  }
  if (number != stack_number)
    state->program->allocator->destroy(state->program->allocator, number);
  return LQL_STATUS_OK;
}

static lql_status lql_flat_eq_mutation_validate_increment_object(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error) {
  size_t pos;
  unsigned char ch;
  lql_status status;
  const char *segment;
  int terminal_repeated_ellipsis;
  segment = action->segments[depth];
  terminal_repeated_ellipsis =
      lql_flat_eq_mutation_terminal_repeated_ellipsis(action, depth);
  pos = value_start + 1u;
  status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  while (pos + 1u < value_end && ch != (unsigned char)'}') {
    size_t key_start;
    size_t key_end;
    size_t child_value_end;
    int same_key;
    key_start = pos;
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
    if (strcmp(segment, "...") == 0 && depth + 1u < action->segment_count &&
        !terminal_repeated_ellipsis) {
      status = lql_flat_eq_mutation_object_key_matches_segment(
          spool, key_start, value_end, action->segments[depth + 1u], &same_key,
          error);
      if (status != LQL_STATUS_OK)
        return status;
      if (same_key) {
        if (depth + 2u == action->segment_count)
          status = lql_flat_eq_mutation_validate_increment_number(
              state, spool, key_end + 1u, child_value_end, error);
        else
          status = lql_flat_eq_mutation_validate_increment_path(
              state, action, spool, key_end + 1u, child_value_end, depth + 2u,
              error);
        if (status != LQL_STATUS_OK)
          return status;
      }
      status = lql_flat_eq_mutation_validate_increment_path(
          state, action, spool, key_end + 1u, child_value_end, depth, error);
      if (status != LQL_STATUS_OK)
        return status;
    } else {
      status = lql_flat_eq_mutation_object_key_matches_segment(
          spool, key_start, value_end, segment, &same_key, error);
      if (status != LQL_STATUS_OK)
        return status;
      if (same_key) {
        if (depth + 1u == action->segment_count || terminal_repeated_ellipsis)
          status = lql_flat_eq_mutation_validate_increment_number(
              state, spool, key_end + 1u, child_value_end, error);
        else
          status = lql_flat_eq_mutation_validate_increment_path(
              state, action, spool, key_end + 1u, child_value_end, depth + 1u,
              error);
        if (status != LQL_STATUS_OK)
          return status;
      }
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
  return LQL_STATUS_OK;
}

static lql_status lql_flat_eq_mutation_validate_increment_array(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error) {
  size_t pos;
  size_t index;
  unsigned char ch;
  lql_status status;
  const char *segment;
  int all_indexes;
  int terminal_repeated_ellipsis;
  size_t target_index;
  segment = action->segments[depth];
  terminal_repeated_ellipsis =
      lql_flat_eq_mutation_terminal_repeated_ellipsis(action, depth);
  target_index = 0u;
  all_indexes = lql_flat_eq_mutation_segment_matches_any_array_index(segment);
  pos = value_start + 1u;
  index = 0u;
  status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  while (pos + 1u < value_end && ch != (unsigned char)']') {
    size_t element_start;
    size_t element_end;
    int selected;
    element_start = pos;
    element_end = pos;
    status = lql_flat_eq_skip_value(spool, &element_end, value_end, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (strcmp(segment, "...") == 0 && depth + 1u < action->segment_count &&
        !terminal_repeated_ellipsis) {
      selected = lql_flat_eq_mutation_segment_matches_any_array_index(
                     action->segments[depth + 1u]) ||
                 (lql_flat_eq_projection_segment_index(
                      action->segments[depth + 1u], &target_index) &&
                  target_index == index);
      if (selected) {
        if (depth + 2u == action->segment_count)
          status = lql_flat_eq_mutation_validate_increment_number(
              state, spool, element_start, element_end, error);
        else
          status = lql_flat_eq_mutation_validate_increment_path(
              state, action, spool, element_start, element_end, depth + 2u,
              error);
        if (status != LQL_STATUS_OK)
          return status;
      }
      status = lql_flat_eq_mutation_validate_increment_path(
          state, action, spool, element_start, element_end, depth, error);
      if (status != LQL_STATUS_OK)
        return status;
    } else if (all_indexes) {
      if (depth + 1u == action->segment_count || terminal_repeated_ellipsis)
        status = lql_flat_eq_mutation_validate_increment_number(
            state, spool, element_start, element_end, error);
      else
        status = lql_flat_eq_mutation_validate_increment_path(
            state, action, spool, element_start, element_end, depth + 1u,
            error);
      if (status != LQL_STATUS_OK)
        return status;
    }
    pos = element_end;
    status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (ch == (unsigned char)',') {
      ++pos;
      status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
      if (status != LQL_STATUS_OK)
        return status;
    }
    ++index;
  }
  return LQL_STATUS_OK;
}

static lql_status lql_flat_eq_mutation_validate_increment_path(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error) {
  unsigned char ch;
  lql_status status;
  if (action == NULL || action->kind != LQL_MUTATION_INCREMENT ||
      depth >= action->segment_count)
    return LQL_STATUS_INVALID_ARGUMENT;
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (ch == (unsigned char)'{')
    return lql_flat_eq_mutation_validate_increment_object(
        state, action, spool, value_start, value_end, depth, error);
  if (ch == (unsigned char)'[')
    return lql_flat_eq_mutation_validate_increment_array(
        state, action, spool, value_start, value_end, depth, error);
  return LQL_STATUS_OK;
}

static lql_status lql_flat_eq_mutation_validate_increment_targets(
    lql_flat_eq_state *state, const lql_json_spool *spool, lql_error *error) {
  size_t i;
  lql_status status;
  if (state == NULL || state->program == NULL || spool == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  for (i = 0u; i < state->program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *action;
    int replaced;
    action = state->program->direct_mutation_actions[i];
    if (action->kind != LQL_MUTATION_INCREMENT)
      continue;
    status = lql_flat_eq_mutation_increment_replaced_before(state, i, &replaced,
                                                            error);
    if (status != LQL_STATUS_OK)
      return status;
    if (replaced)
      continue;
    status = lql_flat_eq_mutation_validate_increment_path(
        state, action, spool, 0u, lql_json_spool_size(spool), 0u, error);
    if (status != LQL_STATUS_OK)
      return status;
  }
  return LQL_STATUS_OK;
}

static lql_status lql_flat_eq_mutation_set_existing_path(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error);
static lql_status lql_flat_eq_mutation_increment_existing_path(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error);
static lql_status lql_flat_eq_mutation_nested_remove(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error);

static lql_status lql_flat_eq_mutation_set_recursive_key(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t key_depth, lql_error *error) {
  size_t pos;
  int first;
  int terminal_repeated_ellipsis;
  unsigned char ch;
  lql_status status;
  key_depth = lql_flat_eq_mutation_collapse_ellipsis(action, key_depth);
  terminal_repeated_ellipsis =
      lql_flat_eq_mutation_terminal_repeated_ellipsis(action, key_depth);
  if (key_depth >= action->segment_count)
    return lql_json_spool_write_slice(spool, value_start,
                                      value_end - value_start, state->writer,
                                      state->writer_user, error);
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if ((key_depth + 1u == action->segment_count || terminal_repeated_ellipsis) &&
      lql_flat_eq_mutation_segment_matches_any_array_index(
          action->segments[key_depth])) {
    return lql_flat_eq_mutation_value(state, action, error);
  }
  if (ch == (unsigned char)'[') {
    size_t index;
    size_t target_index;
    int all_indexes;
    int has_target_index;
    all_indexes = lql_flat_eq_mutation_segment_matches_any_array_index(
        action->segments[key_depth]);
    target_index = 0u;
    has_target_index = lql_flat_eq_projection_segment_index(
        action->segments[key_depth], &target_index);
    status = lql_flat_eq_write(state, "[", 1u, error);
    if (status != LQL_STATUS_OK)
      return status;
    pos = value_start + 1u;
    index = 0u;
    first = 1;
    status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    while (pos + 1u < value_end && ch != (unsigned char)']') {
      size_t element_start;
      size_t element_end;
      int selected;
      element_start = pos;
      element_end = pos;
      status = lql_flat_eq_skip_value(spool, &element_end, value_end, error);
      if (status != LQL_STATUS_OK)
        return status;
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      selected = all_indexes || (has_target_index && index == target_index);
      if (selected && (key_depth + 1u == action->segment_count ||
                       terminal_repeated_ellipsis)) {
        status = lql_flat_eq_mutation_value(state, action, error);
      } else if (selected) {
        if (all_indexes) {
          status = lql_flat_eq_mutation_set_recursive_key(
              state, action, spool, element_start, element_end, key_depth + 1u,
              error);
        } else {
          status = lql_flat_eq_mutation_set_existing_path(
              state, action, spool, element_start, element_end, key_depth + 1u,
              error);
        }
      } else {
        status = lql_flat_eq_mutation_set_recursive_key(
            state, action, spool, element_start, element_end, key_depth, error);
      }
      if (status != LQL_STATUS_OK)
        return status;
      first = 0;
      pos = element_end;
      status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
      if (status != LQL_STATUS_OK)
        return status;
      if (ch == (unsigned char)',') {
        ++pos;
        status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
        if (status != LQL_STATUS_OK)
          return status;
      }
      ++index;
    }
    return lql_flat_eq_write(state, "]", 1u, error);
  }
  if (ch != (unsigned char)'{') {
    if ((key_depth + 1u == action->segment_count ||
         terminal_repeated_ellipsis) &&
        lql_flat_eq_mutation_segment_matches_any_array_index(
            action->segments[key_depth])) {
      return lql_flat_eq_mutation_value(state, action, error);
    }
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
    if (lql_flat_eq_mutation_segment_matches_any_object_key(
            action->segments[key_depth])) {
      same_key = 1;
    } else if (strcmp(action->segments[key_depth], "[]") == 0) {
      same_key = 0;
    } else {
      status =
          lql_flat_eq_key_equals(spool, key_start, value_end,
                                 action->segments[key_depth], &same_key, error);
      if (status != LQL_STATUS_OK)
        return status;
    }
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
    if (status == LQL_STATUS_OK) {
      if (same_key && (key_depth + 1u == action->segment_count ||
                       terminal_repeated_ellipsis)) {
        status = lql_flat_eq_mutation_value(state, action, error);
      } else if (same_key) {
        if (strcmp(action->segments[key_depth], "...") == 0) {
          status = lql_flat_eq_mutation_set_recursive_key(
              state, action, spool, key_end + 1u, child_value_end,
              key_depth + 1u, error);
        } else {
          status = lql_flat_eq_mutation_set_existing_path(
              state, action, spool, key_end + 1u, child_value_end,
              key_depth + 1u, error);
        }
      } else {
        status = lql_flat_eq_mutation_set_recursive_key(
            state, action, spool, key_end + 1u, child_value_end, key_depth,
            error);
      }
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
  return lql_flat_eq_write(state, "}", 1u, error);
}

static lql_status lql_flat_eq_mutation_set_existing_path(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error) {
  size_t target_index;
  size_t index;
  size_t pos;
  int first;
  int all_indexes;
  int terminal_repeated_ellipsis;
  unsigned char ch;
  lql_status status;
  if (depth >= action->segment_count)
    return LQL_STATUS_INVALID_ARGUMENT;
  depth = lql_flat_eq_mutation_collapse_ellipsis(action, depth);
  terminal_repeated_ellipsis =
      lql_flat_eq_mutation_terminal_repeated_ellipsis(action, depth);
  if (strcmp(action->segments[depth], "...") == 0 &&
      depth + 1u < action->segment_count && !terminal_repeated_ellipsis) {
    return lql_flat_eq_mutation_set_recursive_key(
        state, action, spool, value_start, value_end, depth + 1u, error);
  }
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (ch == (unsigned char)'{') {
    if (strcmp(action->segments[depth], "[]") == 0)
      return lql_json_spool_write_slice(spool, value_start,
                                        value_end - value_start, state->writer,
                                        state->writer_user, error);
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
      same_key = 0;
      if (strcmp(action->segments[depth], "*") == 0 ||
          strcmp(action->segments[depth], "**") == 0 ||
          strcmp(action->segments[depth], "...") == 0) {
        same_key = 1;
      } else {
        status =
            lql_flat_eq_key_equals(spool, key_start, value_end,
                                   action->segments[depth], &same_key, error);
        if (status != LQL_STATUS_OK)
          return status;
      }
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
      status =
          lql_flat_eq_skip_value(spool, &child_value_end, value_end, error);
      if (status != LQL_STATUS_OK)
        return status;
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status =
          lql_json_spool_write_slice(spool, key_start, key_end - key_start + 1u,
                                     state->writer, state->writer_user, error);
      if (status == LQL_STATUS_OK) {
        if (same_key && (depth + 1u == action->segment_count ||
                         terminal_repeated_ellipsis)) {
          status = lql_flat_eq_mutation_value(state, action, error);
        } else if (same_key) {
          status = lql_flat_eq_mutation_set_existing_path(
              state, action, spool, key_end + 1u, child_value_end, depth + 1u,
              error);
        } else {
          status = lql_json_spool_write_slice(
              spool, key_end + 1u, child_value_end - key_end - 1u,
              state->writer, state->writer_user, error);
        }
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
    return lql_flat_eq_write(state, "}", 1u, error);
  }
  if (ch != (unsigned char)'[' || strcmp(action->segments[depth], "*") == 0) {
    return lql_json_spool_write_slice(spool, value_start,
                                      value_end - value_start, state->writer,
                                      state->writer_user, error);
  }
  all_indexes = strcmp(action->segments[depth], "[]") == 0 ||
                strcmp(action->segments[depth], "**") == 0 ||
                strcmp(action->segments[depth], "...") == 0;
  target_index = 0u;
  if (!all_indexes && !lql_flat_eq_projection_segment_index(
                          action->segments[depth], &target_index)) {
    return lql_json_spool_write_slice(spool, value_start,
                                      value_end - value_start, state->writer,
                                      state->writer_user, error);
  }
  status = lql_flat_eq_write(state, "[", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  pos = value_start + 1u;
  index = 0u;
  first = 1;
  status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  while (pos + 1u < value_end && ch != (unsigned char)']') {
    size_t element_start;
    size_t element_end;
    int selected;
    element_start = pos;
    element_end = pos;
    status = lql_flat_eq_skip_value(spool, &element_end, value_end, error);
    if (status != LQL_STATUS_OK)
      return status;
    selected = all_indexes || index == target_index;
    if (!first &&
        (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
      return status;
    if (selected &&
        (depth + 1u == action->segment_count || terminal_repeated_ellipsis)) {
      status = lql_flat_eq_mutation_value(state, action, error);
    } else if (selected) {
      status = lql_flat_eq_mutation_set_existing_path(
          state, action, spool, element_start, element_end, depth + 1u, error);
    } else {
      status = lql_json_spool_write_slice(
          spool, element_start, element_end - element_start, state->writer,
          state->writer_user, error);
    }
    if (status != LQL_STATUS_OK)
      return status;
    first = 0;
    pos = element_end;
    status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (ch == (unsigned char)',') {
      ++pos;
      status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
      if (status != LQL_STATUS_OK)
        return status;
    }
    ++index;
  }
  return lql_flat_eq_write(state, "]", 1u, error);
}

static lql_status lql_flat_eq_mutation_increment_existing_key(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t key_depth, lql_error *error) {
  size_t pos;
  int first;
  unsigned char ch;
  lql_status status;
  if (key_depth >= action->segment_count)
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
    status = lql_flat_eq_mutation_object_key_matches_segment(
        spool, key_start, value_end, action->segments[key_depth], &same_key,
        error);
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
    if (status == LQL_STATUS_OK) {
      if (same_key) {
        status = lql_flat_eq_mutation_increment(
            state, action, spool, key_end + 1u, child_value_end, 1, error);
      } else {
        status = lql_json_spool_write_slice(
            spool, key_end + 1u, child_value_end - key_end - 1u, state->writer,
            state->writer_user, error);
      }
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
  return lql_flat_eq_write(state, "}", 1u, error);
}

static lql_status lql_flat_eq_mutation_increment_recursive_key(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t key_depth, lql_error *error) {
  size_t pos;
  int first;
  unsigned char ch;
  lql_status status;
  key_depth = lql_flat_eq_mutation_collapse_ellipsis(action, key_depth);
  if (key_depth >= action->segment_count)
    return lql_json_spool_write_slice(spool, value_start,
                                      value_end - value_start, state->writer,
                                      state->writer_user, error);
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (key_depth + 1u == action->segment_count &&
      lql_flat_eq_mutation_segment_matches_any_array_index(
          action->segments[key_depth])) {
    return lql_flat_eq_mutation_increment(state, action, spool, value_start,
                                          value_end, 1, error);
  }
  if (ch == (unsigned char)'[') {
    size_t index;
    size_t target_index;
    int all_indexes;
    int has_target_index;
    all_indexes = lql_flat_eq_mutation_segment_matches_any_array_index(
        action->segments[key_depth]);
    target_index = 0u;
    has_target_index = lql_flat_eq_projection_segment_index(
        action->segments[key_depth], &target_index);
    status = lql_flat_eq_write(state, "[", 1u, error);
    if (status != LQL_STATUS_OK)
      return status;
    pos = value_start + 1u;
    index = 0u;
    first = 1;
    status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    while (pos + 1u < value_end && ch != (unsigned char)']') {
      size_t element_start;
      size_t element_end;
      int selected;
      element_start = pos;
      element_end = pos;
      status = lql_flat_eq_skip_value(spool, &element_end, value_end, error);
      if (status != LQL_STATUS_OK)
        return status;
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      selected = all_indexes || (has_target_index && index == target_index);
      if (selected && key_depth + 1u == action->segment_count) {
        status = lql_flat_eq_mutation_increment(
            state, action, spool, element_start, element_end, 1, error);
      } else if (selected) {
        if (all_indexes) {
          status = lql_flat_eq_mutation_increment_recursive_key(
              state, action, spool, element_start, element_end, key_depth + 1u,
              error);
        } else {
          status = lql_flat_eq_mutation_increment_existing_path(
              state, action, spool, element_start, element_end, key_depth + 1u,
              error);
        }
      } else {
        status = lql_flat_eq_mutation_increment_recursive_key(
            state, action, spool, element_start, element_end, key_depth, error);
      }
      if (status != LQL_STATUS_OK)
        return status;
      first = 0;
      pos = element_end;
      status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
      if (status != LQL_STATUS_OK)
        return status;
      if (ch == (unsigned char)',') {
        ++pos;
        status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
        if (status != LQL_STATUS_OK)
          return status;
      }
      ++index;
    }
    return lql_flat_eq_write(state, "]", 1u, error);
  }
  if (ch != (unsigned char)'{') {
    if (key_depth + 1u == action->segment_count &&
        lql_flat_eq_mutation_segment_matches_any_array_index(
            action->segments[key_depth])) {
      return lql_flat_eq_mutation_increment(state, action, spool, value_start,
                                            value_end, 1, error);
    }
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
    if (lql_flat_eq_mutation_segment_matches_any_object_key(
            action->segments[key_depth])) {
      same_key = 1;
    } else if (strcmp(action->segments[key_depth], "[]") == 0) {
      same_key = 0;
    } else {
      status =
          lql_flat_eq_key_equals(spool, key_start, value_end,
                                 action->segments[key_depth], &same_key, error);
      if (status != LQL_STATUS_OK)
        return status;
    }
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
    if (status == LQL_STATUS_OK) {
      if (same_key && key_depth + 1u == action->segment_count) {
        status = lql_flat_eq_mutation_increment(
            state, action, spool, key_end + 1u, child_value_end, 1, error);
      } else if (same_key) {
        if (lql_flat_eq_mutation_segment_matches_any_object_key(
                action->segments[key_depth])) {
          status = lql_flat_eq_mutation_increment_recursive_key(
              state, action, spool, key_end + 1u, child_value_end,
              key_depth + 1u, error);
        } else {
          status = lql_flat_eq_mutation_increment_existing_path(
              state, action, spool, key_end + 1u, child_value_end,
              key_depth + 1u, error);
        }
      } else {
        status = lql_flat_eq_mutation_increment_recursive_key(
            state, action, spool, key_end + 1u, child_value_end, key_depth,
            error);
      }
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
  return lql_flat_eq_write(state, "}", 1u, error);
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
  depth = lql_flat_eq_mutation_collapse_ellipsis(action, depth);
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (strcmp(action->segments[depth], "...") == 0 &&
      depth + 1u < action->segment_count) {
    return lql_flat_eq_mutation_set_recursive_key(
        state, action, spool, value_start, value_end, depth + 1u, error);
  }
  if (ch == (unsigned char)'[' &&
      (strcmp(action->segments[depth], "[]") == 0 ||
       strcmp(action->segments[depth], "**") == 0 ||
       strcmp(action->segments[depth], "...") == 0)) {
    size_t pos;
    int first;
    status = lql_flat_eq_write(state, "[", 1u, error);
    if (status != LQL_STATUS_OK)
      return status;
    pos = value_start + 1u;
    first = 1;
    status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    while (pos + 1u < value_end && ch != (unsigned char)']') {
      size_t element_start;
      size_t element_end;
      element_start = pos;
      element_end = pos;
      status = lql_flat_eq_skip_value(spool, &element_end, value_end, error);
      if (status != LQL_STATUS_OK)
        return status;
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      /*
       * Once [] selected the parent element, Go lql only mutates compatible
       * existing descendants. It does not synthesize wildcard-selected parents.
       */
      if (depth + 1u == action->segment_count) {
        status = lql_flat_eq_mutation_value(state, action, error);
      } else {
        status = lql_flat_eq_mutation_set_existing_path(
            state, action, spool, element_start, element_end, depth + 1u,
            error);
      }
      if (status != LQL_STATUS_OK)
        return status;
      first = 0;
      pos = element_end;
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
    return lql_flat_eq_write(state, "]", 1u, error);
  }
  if (ch == (unsigned char)'[' && strcmp(action->segments[depth], "*") == 0) {
    return lql_json_spool_write_slice(spool, value_start,
                                      value_end - value_start, state->writer,
                                      state->writer_user, error);
  }
  if (ch != (unsigned char)'{') {
    if (lql_flat_eq_mutation_suffix_has_wildcard(action, depth)) {
      return lql_json_spool_write_slice(spool, value_start,
                                        value_end - value_start, state->writer,
                                        state->writer_user, error);
    }
    return lql_flat_eq_mutation_object_chain(state, action, depth, error);
  }
  if (strcmp(action->segments[depth], "[]") == 0) {
    return lql_json_spool_write_slice(spool, value_start,
                                      value_end - value_start, state->writer,
                                      state->writer_user, error);
  }
  if (strcmp(action->segments[depth], "*") == 0 ||
      strcmp(action->segments[depth], "**") == 0 ||
      strcmp(action->segments[depth], "...") == 0) {
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
      key_start = pos;
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
      status =
          lql_flat_eq_skip_value(spool, &child_value_end, value_end, error);
      if (status != LQL_STATUS_OK)
        return status;
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status =
          lql_json_spool_write_slice(spool, key_start, key_end - key_start + 1u,
                                     state->writer, state->writer_user, error);
      if (status == LQL_STATUS_OK) {
        if (depth + 1u == action->segment_count) {
          status = lql_flat_eq_mutation_value(state, action, error);
        } else {
          status = lql_flat_eq_mutation_set_existing_path(
              state, action, spool, key_end + 1u, child_value_end, depth + 1u,
              error);
        }
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
    return lql_flat_eq_write(state, "}", 1u, error);
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
    if (lql_flat_eq_mutation_suffix_has_wildcard(action, depth + 1u)) {
      return lql_flat_eq_write(state, "}", 1u, error);
    }
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
    size_t depth, lql_error *error);

static lql_status lql_flat_eq_mutation_remove_recursive_key(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t key_depth, lql_error *error) {
  size_t pos;
  int first;
  unsigned char ch;
  lql_status status;
  key_depth = lql_flat_eq_mutation_collapse_ellipsis(action, key_depth);
  if (key_depth >= action->segment_count)
    return lql_json_spool_write_slice(spool, value_start,
                                      value_end - value_start, state->writer,
                                      state->writer_user, error);
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (key_depth + 1u == action->segment_count &&
      lql_flat_eq_mutation_segment_matches_any_array_index(
          action->segments[key_depth])) {
    return lql_flat_eq_write(state, "null", 4u, error);
  }
  if (ch == (unsigned char)'[') {
    size_t index;
    size_t target_index;
    int all_indexes;
    int has_target_index;
    all_indexes = lql_flat_eq_mutation_segment_matches_any_array_index(
        action->segments[key_depth]);
    target_index = 0u;
    has_target_index = lql_flat_eq_projection_segment_index(
        action->segments[key_depth], &target_index);
    status = lql_flat_eq_write(state, "[", 1u, error);
    if (status != LQL_STATUS_OK)
      return status;
    pos = value_start + 1u;
    index = 0u;
    first = 1;
    status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    while (pos + 1u < value_end && ch != (unsigned char)']') {
      size_t element_start;
      size_t element_end;
      int selected;
      element_start = pos;
      element_end = pos;
      status = lql_flat_eq_skip_value(spool, &element_end, value_end, error);
      if (status != LQL_STATUS_OK)
        return status;
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      selected = all_indexes || (has_target_index && index == target_index);
      if (selected && key_depth + 1u == action->segment_count) {
        status = lql_flat_eq_write(state, "null", 4u, error);
      } else if (selected) {
        if (all_indexes) {
          status = lql_flat_eq_mutation_remove_recursive_key(
              state, action, spool, element_start, element_end, key_depth + 1u,
              error);
        } else {
          status = lql_flat_eq_mutation_nested_remove(
              state, action, spool, element_start, element_end, key_depth + 1u,
              error);
        }
      } else {
        status = lql_flat_eq_mutation_remove_recursive_key(
            state, action, spool, element_start, element_end, key_depth, error);
      }
      if (status != LQL_STATUS_OK)
        return status;
      first = 0;
      pos = element_end;
      status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
      if (status != LQL_STATUS_OK)
        return status;
      if (ch == (unsigned char)',') {
        ++pos;
        status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
        if (status != LQL_STATUS_OK)
          return status;
      }
      ++index;
    }
    return lql_flat_eq_write(state, "]", 1u, error);
  }
  if (ch != (unsigned char)'{') {
    if (key_depth + 1u == action->segment_count &&
        lql_flat_eq_mutation_segment_matches_any_array_index(
            action->segments[key_depth])) {
      return lql_flat_eq_write(state, "null", 4u, error);
    }
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
    if (lql_flat_eq_mutation_segment_matches_any_object_key(
            action->segments[key_depth])) {
      same_key = 1;
    } else if (strcmp(action->segments[key_depth], "[]") == 0) {
      same_key = 0;
    } else {
      status =
          lql_flat_eq_key_equals(spool, key_start, value_end,
                                 action->segments[key_depth], &same_key, error);
      if (status != LQL_STATUS_OK)
        return status;
    }
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
    if (!same_key || key_depth + 1u < action->segment_count) {
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status =
          lql_json_spool_write_slice(spool, key_start, key_end - key_start + 1u,
                                     state->writer, state->writer_user, error);
      if (status == LQL_STATUS_OK) {
        if (same_key) {
          if (lql_flat_eq_mutation_segment_matches_any_object_key(
                  action->segments[key_depth])) {
            status = lql_flat_eq_mutation_remove_recursive_key(
                state, action, spool, key_end + 1u, child_value_end,
                key_depth + 1u, error);
          } else {
            status = lql_flat_eq_mutation_nested_remove(
                state, action, spool, key_end + 1u, child_value_end,
                key_depth + 1u, error);
          }
        } else {
          status = lql_flat_eq_mutation_remove_recursive_key(
              state, action, spool, key_end + 1u, child_value_end, key_depth,
              error);
        }
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

static lql_status lql_flat_eq_mutation_nested_remove_array(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error) {
  size_t target_index;
  size_t index;
  size_t pos;
  int first;
  int all_indexes;
  unsigned char ch;
  lql_status status;
  all_indexes = strcmp(action->segments[depth], "[]") == 0 ||
                strcmp(action->segments[depth], "**") == 0 ||
                strcmp(action->segments[depth], "...") == 0;
  target_index = 0u;
  if (!all_indexes && !lql_flat_eq_projection_segment_index(
                          action->segments[depth], &target_index)) {
    return lql_json_spool_write_slice(spool, value_start,
                                      value_end - value_start, state->writer,
                                      state->writer_user, error);
  }
  status = lql_flat_eq_write(state, "[", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  pos = value_start + 1u;
  index = 0u;
  first = 1;
  status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  while (pos + 1u < value_end && ch != (unsigned char)']') {
    size_t element_start;
    size_t element_end;
    int wrote_element;
    element_start = pos;
    element_end = pos;
    status = lql_flat_eq_skip_value(spool, &element_end, value_end, error);
    if (status != LQL_STATUS_OK)
      return status;
    wrote_element = 0;
    if (!all_indexes && index != target_index) {
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status = lql_json_spool_write_slice(
          spool, element_start, element_end - element_start, state->writer,
          state->writer_user, error);
      wrote_element = 1;
    } else if (depth + 1u == action->segment_count) {
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status = lql_flat_eq_write(state, "null", 4u, error);
      wrote_element = 1;
    } else if (depth + 1u < action->segment_count) {
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status = lql_flat_eq_mutation_nested_remove(
          state, action, spool, element_start, element_end, depth + 1u, error);
      wrote_element = 1;
    }
    if (status != LQL_STATUS_OK)
      return status;
    if (wrote_element)
      first = 0;
    pos = element_end;
    status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (ch == (unsigned char)',') {
      ++pos;
      status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
      if (status != LQL_STATUS_OK)
        return status;
    }
    ++index;
  }
  return lql_flat_eq_write(state, "]", 1u, error);
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
  depth = lql_flat_eq_mutation_collapse_ellipsis(action, depth);
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (strcmp(action->segments[depth], "...") == 0 &&
      depth + 1u < action->segment_count) {
    return lql_flat_eq_mutation_remove_recursive_key(
        state, action, spool, value_start, value_end, depth + 1u, error);
  }
  if (ch == (unsigned char)'[')
    return lql_flat_eq_mutation_nested_remove_array(
        state, action, spool, value_start, value_end, depth, error);
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
  if (strcmp(action->segments[depth], "*") == 0 ||
      strcmp(action->segments[depth], "**") == 0 ||
      strcmp(action->segments[depth], "...") == 0) {
    if (depth + 1u == action->segment_count) {
      while (pos + 1u < value_end && ch != (unsigned char)'}') {
        size_t child_value_end;
        child_value_end = pos;
        status =
            lql_flat_eq_skip_string(spool, &child_value_end, value_end, error);
        if (status != LQL_STATUS_OK)
          return status;
        status = lql_flat_eq_spool_byte(spool, child_value_end, &ch, error);
        if (status != LQL_STATUS_OK)
          return status;
        if (ch != (unsigned char)':')
          return LQL_STATUS_JSON_ERROR;
        ++child_value_end;
        status =
            lql_flat_eq_skip_value(spool, &child_value_end, value_end, error);
        if (status != LQL_STATUS_OK)
          return status;
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
    while (pos + 1u < value_end && ch != (unsigned char)'}') {
      size_t key_start;
      size_t key_end;
      size_t child_value_end;
      key_start = pos;
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
      status =
          lql_flat_eq_skip_value(spool, &child_value_end, value_end, error);
      if (status != LQL_STATUS_OK)
        return status;
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

static lql_status lql_flat_eq_mutation_increment_existing_path(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error) {
  size_t target_index;
  size_t index;
  size_t pos;
  int first;
  int all_indexes;
  unsigned char ch;
  lql_status status;
  if (depth >= action->segment_count)
    return LQL_STATUS_INVALID_ARGUMENT;
  depth = lql_flat_eq_mutation_collapse_ellipsis(action, depth);
  if (strcmp(action->segments[depth], "...") == 0 &&
      depth + 1u < action->segment_count) {
    return lql_flat_eq_mutation_increment_recursive_key(
        state, action, spool, value_start, value_end, depth + 1u, error);
  }
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  if (ch == (unsigned char)'{') {
    if (strcmp(action->segments[depth], "[]") == 0)
      return lql_json_spool_write_slice(spool, value_start,
                                        value_end - value_start, state->writer,
                                        state->writer_user, error);
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
      same_key = 0;
      if (strcmp(action->segments[depth], "*") == 0 ||
          strcmp(action->segments[depth], "**") == 0 ||
          strcmp(action->segments[depth], "...") == 0) {
        same_key = 1;
      } else {
        status =
            lql_flat_eq_key_equals(spool, key_start, value_end,
                                   action->segments[depth], &same_key, error);
        if (status != LQL_STATUS_OK)
          return status;
      }
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
      status =
          lql_flat_eq_skip_value(spool, &child_value_end, value_end, error);
      if (status != LQL_STATUS_OK)
        return status;
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status =
          lql_json_spool_write_slice(spool, key_start, key_end - key_start + 1u,
                                     state->writer, state->writer_user, error);
      if (status == LQL_STATUS_OK) {
        if (same_key && depth + 1u == action->segment_count) {
          status = lql_flat_eq_mutation_increment(
              state, action, spool, key_end + 1u, child_value_end, 1, error);
        } else if (same_key) {
          status = lql_flat_eq_mutation_increment_existing_path(
              state, action, spool, key_end + 1u, child_value_end, depth + 1u,
              error);
        } else {
          status = lql_json_spool_write_slice(
              spool, key_end + 1u, child_value_end - key_end - 1u,
              state->writer, state->writer_user, error);
        }
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
    return lql_flat_eq_write(state, "}", 1u, error);
  }
  if (ch != (unsigned char)'[' || strcmp(action->segments[depth], "*") == 0) {
    return lql_json_spool_write_slice(spool, value_start,
                                      value_end - value_start, state->writer,
                                      state->writer_user, error);
  }
  all_indexes = strcmp(action->segments[depth], "[]") == 0 ||
                strcmp(action->segments[depth], "**") == 0 ||
                strcmp(action->segments[depth], "...") == 0;
  target_index = 0u;
  if (!all_indexes && !lql_flat_eq_projection_segment_index(
                          action->segments[depth], &target_index)) {
    return lql_json_spool_write_slice(spool, value_start,
                                      value_end - value_start, state->writer,
                                      state->writer_user, error);
  }
  status = lql_flat_eq_write(state, "[", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  pos = value_start + 1u;
  index = 0u;
  first = 1;
  status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  while (pos + 1u < value_end && ch != (unsigned char)']') {
    size_t element_start;
    size_t element_end;
    int selected;
    element_start = pos;
    element_end = pos;
    status = lql_flat_eq_skip_value(spool, &element_end, value_end, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (!first &&
        (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
      return status;
    selected = all_indexes || index == target_index;
    if (selected && depth + 1u == action->segment_count) {
      status = lql_flat_eq_mutation_increment(
          state, action, spool, element_start, element_end, 1, error);
    } else if (selected) {
      status = lql_flat_eq_mutation_increment_existing_path(
          state, action, spool, element_start, element_end, depth + 1u, error);
    } else {
      status = lql_json_spool_write_slice(
          spool, element_start, element_end - element_start, state->writer,
          state->writer_user, error);
    }
    if (status != LQL_STATUS_OK)
      return status;
    first = 0;
    pos = element_end;
    status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (ch == (unsigned char)',') {
      ++pos;
      status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
      if (status != LQL_STATUS_OK)
        return status;
    }
    ++index;
  }
  return lql_flat_eq_write(state, "]", 1u, error);
}

static lql_status lql_flat_eq_mutation_nested_increment_array(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error);

static lql_status lql_flat_eq_mutation_nested_increment(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error) {
  size_t pos;
  int first;
  int found;
  int terminal_repeated_ellipsis;
  unsigned char ch;
  lql_status status;
  if (action == NULL || action->kind != LQL_MUTATION_INCREMENT ||
      depth >= action->segment_count || spool == NULL ||
      value_start >= value_end)
    return LQL_STATUS_INVALID_ARGUMENT;
  depth = lql_flat_eq_mutation_collapse_ellipsis(action, depth);
  terminal_repeated_ellipsis =
      lql_flat_eq_mutation_terminal_repeated_ellipsis(action, depth);
  status = lql_flat_eq_spool_byte(spool, value_start, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  /*
   * Go lql materializes missing nested increment parents for ordinary path
   * segments, but wildcard segments only visit existing containers. Keep that
   * split explicit so missing wildcard targets stay no-op instead of creating
   * literal "*" or "[]" object keys.
   */
  if (strcmp(action->segments[depth], "...") == 0 &&
      depth + 1u < action->segment_count && !terminal_repeated_ellipsis) {
    return lql_flat_eq_mutation_increment_recursive_key(
        state, action, spool, value_start, value_end, depth + 1u, error);
  }
  if (ch == (unsigned char)'[')
    return lql_flat_eq_mutation_nested_increment_array(
        state, action, spool, value_start, value_end, depth, error);
  if (ch != (unsigned char)'{') {
    if (lql_flat_eq_mutation_suffix_has_wildcard(action, depth)) {
      return lql_json_spool_write_slice(spool, value_start,
                                        value_end - value_start, state->writer,
                                        state->writer_user, error);
    }
    return lql_flat_eq_mutation_object_chain(state, action, depth, error);
  }
  if (strcmp(action->segments[depth], "[]") == 0) {
    return lql_json_spool_write_slice(spool, value_start,
                                      value_end - value_start, state->writer,
                                      state->writer_user, error);
  }
  if (strcmp(action->segments[depth], "*") == 0 ||
      strcmp(action->segments[depth], "**") == 0 ||
      strcmp(action->segments[depth], "...") == 0) {
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
      key_start = pos;
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
      status =
          lql_flat_eq_skip_value(spool, &child_value_end, value_end, error);
      if (status != LQL_STATUS_OK)
        return status;
      if (!first &&
          (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
        return status;
      status =
          lql_json_spool_write_slice(spool, key_start, key_end - key_start + 1u,
                                     state->writer, state->writer_user, error);
      if (status == LQL_STATUS_OK) {
        if (depth + 1u == action->segment_count || terminal_repeated_ellipsis) {
          status = lql_flat_eq_mutation_increment(
              state, action, spool, key_end + 1u, child_value_end, 1, error);
        } else if (depth + 2u == action->segment_count) {
          status = lql_flat_eq_mutation_increment_existing_key(
              state, action, spool, key_end + 1u, child_value_end, depth + 1u,
              error);
        } else {
          status = lql_json_spool_write_slice(
              spool, key_end + 1u, child_value_end - key_end - 1u,
              state->writer, state->writer_user, error);
        }
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
    return lql_flat_eq_write(state, "}", 1u, error);
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
    if (lql_flat_eq_mutation_suffix_has_wildcard(action, depth + 1u)) {
      /*
       * Missing ordinary parents may be synthesized for increments, but a
       * wildcard in the remaining suffix means the request can only visit an
       * existing compatible child. Preserve the object unchanged.
       */
      return lql_flat_eq_write(state, "}", 1u, error);
    }
    if (!first &&
        (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
      return status;
    status = lql_flat_eq_projection_key(state, action->segments[depth], error);
    if (status != LQL_STATUS_OK)
      return status;
    if (depth + 1u == action->segment_count) {
      status =
          lql_flat_eq_mutation_increment(state, action, NULL, 0u, 0u, 0, error);
    } else {
      status =
          lql_flat_eq_mutation_object_chain(state, action, depth + 1u, error);
    }
    if (status != LQL_STATUS_OK)
      return status;
  }
  return lql_flat_eq_write(state, "}", 1u, error);
}

static lql_status lql_flat_eq_mutation_nested_increment_array(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *spool, size_t value_start, size_t value_end,
    size_t depth, lql_error *error) {
  size_t pos;
  int first;
  int all_indexes;
  int terminal_repeated_ellipsis;
  unsigned char ch;
  lql_status status;
  terminal_repeated_ellipsis =
      lql_flat_eq_mutation_terminal_repeated_ellipsis(action, depth);
  all_indexes = strcmp(action->segments[depth], "[]") == 0 ||
                strcmp(action->segments[depth], "**") == 0 ||
                strcmp(action->segments[depth], "...") == 0;
  if (!all_indexes) {
    if (lql_flat_eq_mutation_suffix_has_wildcard(action, depth)) {
      return lql_json_spool_write_slice(spool, value_start,
                                        value_end - value_start, state->writer,
                                        state->writer_user, error);
    }
    if (strcmp(action->segments[depth], "*") != 0)
      return lql_flat_eq_mutation_object_chain(state, action, depth, error);
    return lql_json_spool_write_slice(spool, value_start,
                                      value_end - value_start, state->writer,
                                      state->writer_user, error);
  }
  status = lql_flat_eq_write(state, "[", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  pos = value_start + 1u;
  first = 1;
  status = lql_flat_eq_spool_byte(spool, pos, &ch, error);
  if (status != LQL_STATUS_OK)
    return status;
  while (pos + 1u < value_end && ch != (unsigned char)']') {
    size_t element_start;
    size_t element_end;
    element_start = pos;
    element_end = pos;
    status = lql_flat_eq_skip_value(spool, &element_end, value_end, error);
    if (status != LQL_STATUS_OK)
      return status;
    if (!first &&
        (status = lql_flat_eq_write(state, ",", 1u, error)) != LQL_STATUS_OK)
      return status;
    if (depth + 1u == action->segment_count || terminal_repeated_ellipsis) {
      status = lql_flat_eq_mutation_increment(
          state, action, spool, element_start, element_end, 1, error);
    } else if (!all_indexes) {
      status = lql_flat_eq_mutation_nested_increment(
          state, action, spool, element_start, element_end, depth + 1u, error);
    } else {
      /*
       * Wildcards only visit compatible existing children. Preserve scalars and
       * incompatible containers; do not synthesize object parents under
       * wildcard-selected elements.
       */
      status = lql_flat_eq_mutation_increment_existing_path(
          state, action, spool, element_start, element_end, depth + 1u, error);
    }
    if (status != LQL_STATUS_OK)
      return status;
    first = 0;
    pos = element_end;
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
  return lql_flat_eq_write(state, "]", 1u, error);
}

static int
lql_flat_eq_mutation_nonrecursive_wildcard(const lql_mutation_action *action);
static int
lql_flat_eq_mutation_top_matches_object_key(const lql_mutation_action *action);

static int
lql_flat_eq_mutation_exact_top_key(const lql_mutation_action *action) {
  return action != NULL && action->segment_count != 0u &&
         action->segments[0] != NULL &&
         !lql_flat_eq_mutation_segment_matches_any_object_key(
             action->segments[0]);
}

static int lql_flat_eq_mutation_program_has_top_object_wildcard(
    const lql_flat_eq_program *program) {
  size_t i;
  if (program == NULL)
    return 0;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    if (lql_flat_eq_mutation_top_matches_object_key(
            program->direct_mutation_actions[i])) {
      return 1;
    }
  }
  return 0;
}

static int
lql_flat_eq_mutation_top_matches_object_key(const lql_mutation_action *action) {
  if (action == NULL || action->segment_count == 0u ||
      action->segments[0] == NULL) {
    return 0;
  }
  /*
   * Top-level NDJSON records are objects. "*" selects each object member, and
   * recursive aliases also select existing object members at this boundary.
   * "[]" is intentionally not included because it is an array wildcard.
   */
  return lql_flat_eq_mutation_segment_matches_any_object_key(
      action->segments[0]);
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
    if (lql_flat_eq_mutation_top_matches_object_key(action)) {
      same_key = 1;
    } else {
      status = lql_flat_eq_key_equals(spool, key_start, size,
                                      action->segments[0], &same_key, error);
      if (status != LQL_STATUS_OK)
        return status;
    }
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
  if (action->kind == LQL_MUTATION_REMOVE && action->segment_count == 1u)
    return 1;
  return action->segment_count > 1u && (action->kind == LQL_MUTATION_SET ||
                                        action->kind == LQL_MUTATION_REMOVE ||
                                        action->kind == LQL_MUTATION_INCREMENT);
}

static int
lql_flat_eq_mutation_nonrecursive_wildcard(const lql_mutation_action *action) {
  size_t i;
  if (action == NULL)
    return 0;
  for (i = 0u; i < action->segment_count; ++i) {
    if (strcmp(action->segments[i], "*") == 0 ||
        strcmp(action->segments[i], "[]") == 0 ||
        strcmp(action->segments[i], "**") == 0 ||
        strcmp(action->segments[i], "...") == 0)
      return 1;
  }
  return 0;
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
  return 1;
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
                                           const unsigned char *found,
                                           size_t before) {
  size_t i;
  if (program == NULL || action == NULL || found == NULL)
    return 0;
  for (i = 0u; i < before; ++i) {
    if (lql_flat_eq_mutation_same_top(program->direct_mutation_actions[i],
                                      action) &&
        found[i] != 0u)
      return 1;
  }
  return 0;
}

static int lql_flat_eq_mutation_group_has_value_creator(
    const lql_flat_eq_program *program, const lql_mutation_action *action) {
  size_t i;
  if (program == NULL || action == NULL)
    return 0;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *candidate;
    candidate = program->direct_mutation_actions[i];
    if (lql_flat_eq_mutation_same_top(candidate, action) &&
        (candidate->kind == LQL_MUTATION_SET ||
         candidate->kind == LQL_MUTATION_INCREMENT))
      return 1;
  }
  return 0;
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
lql_flat_eq_mutation_group_is_exact_nested(const lql_flat_eq_program *program,
                                           const lql_mutation_action *action) {
  size_t i;
  if (program == NULL || action == NULL ||
      !lql_flat_eq_mutation_exact_top_key(action) ||
      action->segment_count <= 1u)
    return 0;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *candidate;
    candidate = program->direct_mutation_actions[i];
    if (!lql_flat_eq_mutation_same_top(candidate, action))
      continue;
    if (!lql_flat_eq_mutation_exact_top_key(candidate) ||
        candidate->segment_count <= 1u ||
        (candidate->kind != LQL_MUTATION_SET &&
         candidate->kind != LQL_MUTATION_REMOVE &&
         candidate->kind != LQL_MUTATION_INCREMENT))
      return 0;
  }
  return 1;
}

static lql_status
lql_flat_eq_mutation_group_increment_action(const lql_flat_eq_program *program,
                                            const lql_mutation_action *group,
                                            lql_mutation_action *out) {
  size_t i;
  double delta;
  if (program == NULL || group == NULL || out == NULL)
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

static const lql_mutation_action *
lql_flat_eq_mutation_group_last_set(const lql_flat_eq_program *program,
                                    const lql_mutation_action *group) {
  size_t i;
  const lql_mutation_action *last;
  if (program == NULL || group == NULL)
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

static int lql_flat_eq_mutation_program_needs_atomic_output(
    const lql_flat_eq_program *program) {
  size_t i;
  size_t j;
  if (program == NULL)
    return 0;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *creator;
    creator = program->direct_mutation_actions[i];
    if (creator == NULL || creator->segment_count <= 1u ||
        creator->kind == LQL_MUTATION_REMOVE)
      continue;
    for (j = i + 1u; j < program->direct_mutation_action_count; ++j) {
      const lql_mutation_action *increment;
      increment = program->direct_mutation_actions[j];
      if (increment != NULL && increment->kind == LQL_MUTATION_INCREMENT &&
          increment->segment_count == 1u &&
          lql_flat_eq_mutation_same_top(creator, increment))
        return 1;
    }
  }
  return 0;
}

static lql_status lql_flat_eq_mutation_action_matches_spooled_key(
    const lql_mutation_action *action, const lql_json_spool *spool,
    size_t key_start, size_t size, int *out_matches, lql_error *error) {
  lql_status status;
  if (out_matches != NULL)
    *out_matches = 0;
  if (action == NULL || spool == NULL || out_matches == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  if (lql_flat_eq_mutation_top_matches_object_key(action)) {
    *out_matches = 1;
    return LQL_STATUS_OK;
  }
  status = lql_flat_eq_key_equals(spool, key_start, size, action->segments[0],
                                  out_matches, error);
  return status;
}

static int lql_flat_eq_mutation_action_matches_missing_key(
    const lql_mutation_action *action, const char *key) {
  if (action == NULL || key == NULL || action->segment_count == 0u ||
      action->segments[0] == NULL)
    return 0;
  if (lql_flat_eq_mutation_top_matches_object_key(action))
    return 1;
  return strcmp(action->segments[0], key) == 0;
}

static void lql_flat_eq_mutation_key_mark_spooled(
    const lql_flat_eq_program *program, const lql_json_spool *spool,
    size_t key_start, size_t size, unsigned char *found, lql_error *error) {
  size_t i;
  if (program == NULL || spool == NULL || found == NULL)
    return;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    int matches;
    if (lql_flat_eq_mutation_action_matches_spooled_key(
            program->direct_mutation_actions[i], spool, key_start, size,
            &matches, error) == LQL_STATUS_OK &&
        matches) {
      found[i] = 1u;
    }
  }
}

static void
lql_flat_eq_mutation_key_mark_missing(const lql_flat_eq_program *program,
                                      const char *key, unsigned char *found) {
  size_t i;
  if (program == NULL || key == NULL || found == NULL)
    return;
  for (i = 0u; i < program->direct_mutation_action_count; ++i) {
    if (lql_flat_eq_mutation_action_matches_missing_key(
            program->direct_mutation_actions[i], key)) {
      found[i] = 1u;
    }
  }
}

static lql_status lql_flat_eq_mutation_apply_nested_to_spool(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *source, size_t value_start, size_t value_end,
    int top_wildcard_consumed, lql_json_spool *target, lql_error *error) {
  lql_stream_writer_fn saved_writer;
  void *saved_writer_user;
  lql_status status;
  int top_wildcard_suffix;
  if (state == NULL || action == NULL || source == NULL || target == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  lql_json_spool_reset(target);
  saved_writer = state->writer;
  saved_writer_user = state->writer_user;
  state->writer = lql_flat_eq_spool_writer;
  state->writer_user = target;
  top_wildcard_suffix = top_wildcard_consumed && action->segment_count > 1u &&
                        strcmp(action->segments[0], "...") != 0 &&
                        lql_flat_eq_mutation_top_matches_object_key(action);
  if (top_wildcard_suffix &&
      !lql_flat_eq_spooled_value_accepts_segment(source, value_start,
                                                 action->segments[1], error)) {
    status =
        lql_json_spool_write_slice(source, value_start, value_end - value_start,
                                   state->writer, state->writer_user, error);
    goto done;
  }
  if (action->kind == LQL_MUTATION_SET) {
    if (action->segment_count > 1u && strcmp(action->segments[0], "...") == 0) {
      status = lql_flat_eq_mutation_set_recursive_key(
          state, action, source, value_start, value_end, 1u, error);
    } else if (action->segment_count > 1u &&
               lql_flat_eq_mutation_top_matches_object_key(action)) {
      if (top_wildcard_consumed) {
        status = lql_flat_eq_mutation_nested_set(
            state, action, source, value_start, value_end, 1u, error);
      } else {
        status = lql_flat_eq_mutation_set_existing_path(
            state, action, source, value_start, value_end, 1u, error);
      }
    } else {
      status = lql_flat_eq_mutation_nested_set(
          state, action, source, value_start, value_end, 1u, error);
    }
  } else if (action->kind == LQL_MUTATION_REMOVE) {
    if (action->segment_count > 1u && strcmp(action->segments[0], "...") == 0) {
      status = lql_flat_eq_mutation_remove_recursive_key(
          state, action, source, value_start, value_end, 1u, error);
    } else if (action->segment_count > 1u &&
               lql_flat_eq_mutation_top_matches_object_key(action)) {
      status = lql_flat_eq_mutation_nested_remove(
          state, action, source, value_start, value_end, 1u, error);
    } else {
      status = lql_flat_eq_mutation_nested_remove(
          state, action, source, value_start, value_end, 1u, error);
    }
  } else if (action->kind == LQL_MUTATION_INCREMENT) {
    if (action->segment_count > 1u && strcmp(action->segments[0], "...") == 0) {
      status = lql_flat_eq_mutation_increment_recursive_key(
          state, action, source, value_start, value_end, 1u, error);
    } else if (action->segment_count > 1u &&
               lql_flat_eq_mutation_top_matches_object_key(action)) {
      if (top_wildcard_consumed) {
        status = lql_flat_eq_mutation_nested_increment(
            state, action, source, value_start, value_end, 1u, error);
      } else {
        status = lql_flat_eq_mutation_increment_existing_path(
            state, action, source, value_start, value_end, 1u, error);
      }
    } else {
      status = lql_flat_eq_mutation_nested_increment(
          state, action, source, value_start, value_end, 1u, error);
    }
  } else {
    status = LQL_STATUS_INVALID_ARGUMENT;
  }
done:
  state->writer = saved_writer;
  state->writer_user = saved_writer_user;
  return status;
}

static lql_status lql_flat_eq_mutation_apply_ordered_key_action(
    lql_flat_eq_state *state, const lql_mutation_action *action,
    const lql_json_spool *source, size_t source_start, size_t source_end,
    int present, int allow_create, int top_wildcard_consumed,
    lql_json_spool *target, int *out_present, lql_error *error) {
  lql_stream_writer_fn saved_writer;
  void *saved_writer_user;
  lql_status status;
  if (state == NULL || action == NULL || target == NULL || out_present == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  *out_present = present;
  if (!present && !allow_create)
    return LQL_STATUS_OK;
  if (action->segment_count == 1u) {
    if (action->kind == LQL_MUTATION_REMOVE) {
      *out_present = 0;
      return LQL_STATUS_OK;
    }
    lql_json_spool_reset(target);
    saved_writer = state->writer;
    saved_writer_user = state->writer_user;
    state->writer = lql_flat_eq_spool_writer;
    state->writer_user = target;
    if (action->kind == LQL_MUTATION_SET) {
      status = lql_flat_eq_mutation_value(state, action, error);
    } else if (action->kind == LQL_MUTATION_INCREMENT) {
      status = lql_flat_eq_mutation_increment(
          state, action, present ? source : NULL, present ? source_start : 0u,
          present ? source_end : 0u, present, error);
    } else {
      status = LQL_STATUS_INVALID_ARGUMENT;
    }
    state->writer = saved_writer;
    state->writer_user = saved_writer_user;
  } else {
    if (!present) {
      lql_json_spool created;
      int created_initialized;
      if (action->kind == LQL_MUTATION_REMOVE)
        return LQL_STATUS_OK;
      if (lql_flat_eq_mutation_suffix_has_wildcard(action, 1u))
        return LQL_STATUS_OK;
      created_initialized = 0;
      status = lql_json_spool_init_with_allocator(
          &created, state->program->allocator, error);
      if (status != LQL_STATUS_OK)
        return status;
      created_initialized = 1;
      status = lql_json_spool_append(&created, "{}", 2u, error);
      if (status != LQL_STATUS_OK) {
        lql_json_spool_cleanup(&created);
        return status;
      }
      source = &created;
      source_start = 0u;
      source_end = lql_json_spool_size(source);
      status = lql_flat_eq_mutation_apply_nested_to_spool(
          state, action, source, source_start, source_end,
          top_wildcard_consumed, target, error);
      if (created_initialized)
        lql_json_spool_cleanup(&created);
      if (status == LQL_STATUS_OK)
        *out_present = 1;
      return status;
    }
    status = lql_flat_eq_mutation_apply_nested_to_spool(
        state, action, source, source_start, source_end, top_wildcard_consumed,
        target, error);
  }
  if (status == LQL_STATUS_OK)
    *out_present = 1;
  return status;
}

static lql_status lql_flat_eq_mutation_emit_existing_nested_group(
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
  if (state == NULL || spool == NULL || group == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  initialized_first = 0;
  initialized_second = 0;
  status = lql_json_spool_init_with_allocator(&first, state->program->allocator,
                                              error);
  if (status != LQL_STATUS_OK)
    return status;
  initialized_first = 1;
  status = lql_json_spool_init_with_allocator(&second,
                                              state->program->allocator, error);
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
        state, action, source, source_start, source_end, 0, target, error);
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

static lql_status lql_flat_eq_mutation_emit_existing_key_group(
    lql_flat_eq_state *state, const lql_json_spool *spool, size_t key_start,
    size_t size, size_t value_start, size_t value_end, lql_json_spool *out,
    int *out_present, lql_error *error) {
  lql_json_spool first;
  lql_json_spool second;
  const lql_json_spool *source;
  lql_json_spool *target;
  size_t source_start;
  size_t source_end;
  size_t i;
  int initialized_first;
  int initialized_second;
  int present;
  lql_status status;
  if (out_present != NULL)
    *out_present = 0;
  if (state == NULL || spool == NULL || out == NULL || out_present == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  initialized_first = 0;
  initialized_second = 0;
  status = lql_json_spool_init_with_allocator(&first, state->program->allocator,
                                              error);
  if (status != LQL_STATUS_OK)
    return status;
  initialized_first = 1;
  status = lql_json_spool_init_with_allocator(&second,
                                              state->program->allocator, error);
  if (status != LQL_STATUS_OK)
    goto done;
  initialized_second = 1;
  source = spool;
  source_start = value_start;
  source_end = value_end;
  target = &first;
  present = 1;
  for (i = 0u; i < state->program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *action;
    const lql_mutation_action *recursive_after_action;
    lql_mutation_action root_action;
    int apply_recursive_after;
    int original_top_wildcard;
    int matches;
    action = state->program->direct_mutation_actions[i];
    recursive_after_action = action;
    apply_recursive_after = 0;
    original_top_wildcard = strcmp(action->segments[0], "...") != 0 &&
                            lql_flat_eq_mutation_top_matches_object_key(action);
    status = lql_flat_eq_mutation_action_matches_spooled_key(
        action, spool, key_start, size, &matches, error);
    if (status != LQL_STATUS_OK)
      goto done;
    if (!matches)
      continue;
    if (action->segment_count > 1u && strcmp(action->segments[0], "...") == 0) {
      int same_recursive_key;
      size_t target_depth;
      target_depth = 1u;
      while (target_depth < action->segment_count &&
             strcmp(action->segments[target_depth], "...") == 0) {
        ++target_depth;
      }
      if (target_depth >= action->segment_count) {
        root_action = *action;
        root_action.segment_count = 1u;
        action = &root_action;
        goto apply_action;
      }
      status = lql_flat_eq_mutation_object_key_matches_segment(
          spool, key_start, size, action->segments[target_depth],
          &same_recursive_key, error);
      if (status != LQL_STATUS_OK)
        goto done;
      if (same_recursive_key) {
        root_action = *action;
        if (target_depth + 1u == action->segment_count) {
          root_action.segment_count = 1u;
        } else {
          if (!lql_flat_eq_spooled_value_is_container(source, source_start,
                                                      error)) {
            continue;
          }
          root_action.segments = action->segments + target_depth;
          root_action.segment_count = action->segment_count - target_depth;
          apply_recursive_after = 1;
        }
        action = &root_action;
      }
    }
  apply_action:
    status = lql_flat_eq_mutation_apply_ordered_key_action(
        state, action, source, source_start, source_end, present,
        present || !original_top_wildcard, original_top_wildcard, target,
        &present, error);
    if (status != LQL_STATUS_OK)
      goto done;
    if (present) {
      source = target;
      source_start = 0u;
      source_end = lql_json_spool_size(source);
      target = target == &first ? &second : &first;
    }
    if (apply_recursive_after && present) {
      status = lql_flat_eq_mutation_apply_ordered_key_action(
          state, recursive_after_action, source, source_start, source_end,
          present, 1, 0, target, &present, error);
      if (status != LQL_STATUS_OK)
        goto done;
      if (present) {
        source = target;
        source_start = 0u;
        source_end = lql_json_spool_size(source);
        target = target == &first ? &second : &first;
      }
    }
  }
  if (present) {
    lql_json_spool_reset(out);
    status = lql_json_spool_write_slice(source, source_start,
                                        source_end - source_start,
                                        lql_flat_eq_spool_writer, out, error);
    if (status != LQL_STATUS_OK)
      goto done;
  }
  *out_present = present;
  status = LQL_STATUS_OK;
done:
  if (initialized_second)
    lql_json_spool_cleanup(&second);
  if (initialized_first)
    lql_json_spool_cleanup(&first);
  return status;
}

static lql_status lql_flat_eq_mutation_emit_missing_key_group(
    lql_flat_eq_state *state, const lql_mutation_action *group,
    lql_json_spool *out, int *out_present, lql_error *error) {
  lql_json_spool first;
  lql_json_spool second;
  const lql_json_spool *source;
  lql_json_spool *target;
  size_t source_start;
  size_t source_end;
  size_t i;
  int initialized_first;
  int initialized_second;
  int present;
  lql_status status;
  if (out_present != NULL)
    *out_present = 0;
  if (state == NULL || group == NULL || group->segment_count == 0u ||
      out == NULL || out_present == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  initialized_first = 0;
  initialized_second = 0;
  status = lql_json_spool_init_with_allocator(&first, state->program->allocator,
                                              error);
  if (status != LQL_STATUS_OK)
    return status;
  initialized_first = 1;
  status = lql_json_spool_init_with_allocator(&second,
                                              state->program->allocator, error);
  if (status != LQL_STATUS_OK)
    goto done;
  initialized_second = 1;
  source = &first;
  source_start = 0u;
  source_end = 0u;
  target = &first;
  present = 0;
  for (i = 0u; i < state->program->direct_mutation_action_count; ++i) {
    const lql_mutation_action *action;
    const lql_mutation_action *recursive_after_action;
    lql_mutation_action root_action;
    int apply_recursive_after;
    int allow_create;
    action = state->program->direct_mutation_actions[i];
    recursive_after_action = action;
    apply_recursive_after = 0;
    if (!lql_flat_eq_mutation_action_matches_missing_key(action,
                                                         group->segments[0]))
      continue;
    if (present && action->segment_count > 1u &&
        strcmp(action->segments[0], "...") == 0) {
      int same_recursive_key;
      size_t target_depth;
      target_depth = 1u;
      while (target_depth < action->segment_count &&
             strcmp(action->segments[target_depth], "...") == 0) {
        ++target_depth;
      }
      if (target_depth >= action->segment_count) {
        root_action = *action;
        root_action.segment_count = 1u;
        action = &root_action;
        goto apply_missing_action;
      }
      same_recursive_key =
          strcmp(action->segments[target_depth], group->segments[0]) == 0 ||
          lql_flat_eq_mutation_segment_matches_any_object_key(
              action->segments[target_depth]);
      if (same_recursive_key) {
        root_action = *action;
        if (target_depth + 1u == action->segment_count) {
          root_action.segment_count = 1u;
        } else {
          if (!lql_flat_eq_spooled_value_is_container(source, source_start,
                                                      error)) {
            continue;
          }
          root_action.segments = action->segments + target_depth;
          root_action.segment_count = action->segment_count - target_depth;
          apply_recursive_after = 1;
        }
        action = &root_action;
      }
    }
  apply_missing_action:
    allow_create = !lql_flat_eq_mutation_top_matches_object_key(action);
    status = lql_flat_eq_mutation_apply_ordered_key_action(
        state, action, source, source_start, source_end, present, allow_create,
        0, target, &present, error);
    if (status != LQL_STATUS_OK)
      goto done;
    if (present) {
      source = target;
      source_start = 0u;
      source_end = lql_json_spool_size(source);
      target = target == &first ? &second : &first;
    }
    if (apply_recursive_after && present) {
      status = lql_flat_eq_mutation_apply_ordered_key_action(
          state, recursive_after_action, source, source_start, source_end,
          present, 1, 0, target, &present, error);
      if (status != LQL_STATUS_OK)
        goto done;
      if (present) {
        source = target;
        source_start = 0u;
        source_end = lql_json_spool_size(source);
        target = target == &first ? &second : &first;
      }
    }
  }
  if (present) {
    lql_json_spool_reset(out);
    status =
        lql_json_spool_write_to(source, lql_flat_eq_spool_writer, out, error);
    if (status != LQL_STATUS_OK)
      goto done;
  }
  *out_present = present;
  status = LQL_STATUS_OK;
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
  unsigned char *found;
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
  status = lql_flat_eq_mutation_validate_increment_targets(state, spool, error);
  if (status != LQL_STATUS_OK)
    return status;
  status = lql_flat_eq_write(state, "{", 1u, error);
  if (status != LQL_STATUS_OK)
    return status;
  pos = 1u;
  found = state->program->direct_mutation_found;
  memset(found, 0, state->program->direct_mutation_action_count);
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
      if (group_count > 1u && lql_flat_eq_mutation_exact_top_key(action) &&
          !lql_flat_eq_mutation_program_has_top_object_wildcard(
              state->program) &&
          lql_flat_eq_mutation_group_is_top_increment(state->program, action)) {
        lql_mutation_action grouped_increment;
        status = lql_flat_eq_mutation_group_increment_action(
            state->program, action, &grouped_increment);
        if (status != LQL_STATUS_OK)
          return status;
        if (!first && (status = lql_flat_eq_write(state, ",", 1u, error)) !=
                          LQL_STATUS_OK)
          return status;
        status = lql_json_spool_write_slice(
            spool, key_start, key_end - key_start + 1u, state->writer,
            state->writer_user, error);
        if (status == LQL_STATUS_OK)
          status =
              lql_flat_eq_mutation_increment(state, &grouped_increment, spool,
                                             key_end + 1u, value_end, 1, error);
        if (status != LQL_STATUS_OK)
          return status;
        lql_flat_eq_mutation_key_mark_spooled(state->program, spool, key_start,
                                              size, found, error);
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
        continue;
      }
      if (group_count > 1u && lql_flat_eq_mutation_exact_top_key(action) &&
          !lql_flat_eq_mutation_program_has_top_object_wildcard(
              state->program) &&
          lql_flat_eq_mutation_group_is_top_set(state->program, action)) {
        const lql_mutation_action *last_set;
        last_set = lql_flat_eq_mutation_group_last_set(state->program, action);
        if (last_set == NULL)
          return LQL_STATUS_INVALID_ARGUMENT;
        if (!first && (status = lql_flat_eq_write(state, ",", 1u, error)) !=
                          LQL_STATUS_OK)
          return status;
        status = lql_json_spool_write_slice(
            spool, key_start, key_end - key_start + 1u, state->writer,
            state->writer_user, error);
        if (status == LQL_STATUS_OK)
          status = lql_flat_eq_mutation_value(state, last_set, error);
        if (status != LQL_STATUS_OK)
          return status;
        lql_flat_eq_mutation_key_mark_spooled(state->program, spool, key_start,
                                              size, found, error);
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
        continue;
      }
      if (group_count > 1u && lql_flat_eq_mutation_exact_top_key(action) &&
          !lql_flat_eq_mutation_program_has_top_object_wildcard(
              state->program) &&
          lql_flat_eq_mutation_group_is_exact_nested(state->program, action)) {
        /*
         * Ordered key groups are required when top wildcards can interleave
         * with concrete keys. Exact same-top nested groups have no such
         * cross-key interaction, so they can keep the streaming direct path.
         */
        if (!first && (status = lql_flat_eq_write(state, ",", 1u, error)) !=
                          LQL_STATUS_OK)
          return status;
        status = lql_json_spool_write_slice(
            spool, key_start, key_end - key_start + 1u, state->writer,
            state->writer_user, error);
        if (status == LQL_STATUS_OK)
          status = lql_flat_eq_mutation_emit_existing_nested_group(
              state, spool, action, key_end + 1u, value_end, error);
        if (status != LQL_STATUS_OK)
          return status;
        lql_flat_eq_mutation_key_mark_spooled(state->program, spool, key_start,
                                              size, found, error);
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
        continue;
      }
      if (group_count == 1u && lql_flat_eq_mutation_exact_top_key(action) &&
          !lql_flat_eq_mutation_program_has_top_object_wildcard(
              state->program)) {
        found[action_index] = 1u;
        if (action->kind == LQL_MUTATION_REMOVE &&
            action->segment_count == 1u) {
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
        if (!first && (status = lql_flat_eq_write(state, ",", 1u, error)) !=
                          LQL_STATUS_OK)
          return status;
        status = lql_json_spool_write_slice(
            spool, key_start, key_end - key_start + 1u, state->writer,
            state->writer_user, error);
        if (status == LQL_STATUS_OK) {
          if (action->kind == LQL_MUTATION_INCREMENT &&
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
        continue;
      }
      {
        lql_json_spool group_value;
        int group_value_present;
        (void)action_index;
        status = lql_json_spool_init_with_allocator(
            &group_value, state->program->allocator, error);
        if (status != LQL_STATUS_OK)
          return status;
        status = lql_flat_eq_mutation_emit_existing_key_group(
            state, spool, key_start, size, key_end + 1u, value_end,
            &group_value, &group_value_present, error);
        if (status == LQL_STATUS_OK && group_value_present) {
          if (!first)
            status = lql_flat_eq_write(state, ",", 1u, error);
          if (status == LQL_STATUS_OK)
            status = lql_json_spool_write_slice(
                spool, key_start, key_end - key_start + 1u, state->writer,
                state->writer_user, error);
          if (status == LQL_STATUS_OK)
            status = lql_json_spool_write_to(&group_value, state->writer,
                                             state->writer_user, error);
          if (status == LQL_STATUS_OK)
            first = 0;
        }
        if (status == LQL_STATUS_OK)
          lql_flat_eq_mutation_key_mark_spooled(state->program, spool,
                                                key_start, size, found, error);
        lql_json_spool_cleanup(&group_value);
        if (status != LQL_STATUS_OK)
          return status;
      }
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
      if (lql_flat_eq_mutation_nonrecursive_wildcard(action) ||
          found[i] != 0u ||
          (group_count <= 1u && action->kind == LQL_MUTATION_REMOVE) ||
          (group_count > 1u &&
           lql_flat_eq_mutation_group_seen(state->program, action, found, i)) ||
          (group_count > 1u && !lql_flat_eq_mutation_group_has_value_creator(
                                   state->program, action)))
        continue;
      if (group_count == 1u && lql_flat_eq_mutation_exact_top_key(action) &&
          !lql_flat_eq_mutation_program_has_top_object_wildcard(
              state->program)) {
        if (!first && (status = lql_flat_eq_write(state, ",", 1u, error)) !=
                          LQL_STATUS_OK)
          return status;
        status = lql_flat_eq_projection_key(state, action->segments[0], error);
        if (status == LQL_STATUS_OK) {
          if ((action->kind == LQL_MUTATION_SET ||
               action->kind == LQL_MUTATION_INCREMENT) &&
              action->segment_count > 1u) {
            status =
                lql_flat_eq_mutation_object_chain(state, action, 1u, error);
          } else if (action->kind == LQL_MUTATION_INCREMENT) {
            status = lql_flat_eq_mutation_increment(state, action, NULL, 0u, 0u,
                                                    0, error);
          } else {
            status = lql_flat_eq_mutation_value(state, action, error);
          }
        }
        if (status != LQL_STATUS_OK)
          return status;
        found[i] = 1u;
        first = 0;
        continue;
      }
      if (group_count > 1u && lql_flat_eq_mutation_exact_top_key(action) &&
          !lql_flat_eq_mutation_program_has_top_object_wildcard(
              state->program) &&
          lql_flat_eq_mutation_group_is_top_increment(state->program, action)) {
        lql_mutation_action grouped_increment;
        status = lql_flat_eq_mutation_group_increment_action(
            state->program, action, &grouped_increment);
        if (status != LQL_STATUS_OK)
          return status;
        if (!first && (status = lql_flat_eq_write(state, ",", 1u, error)) !=
                          LQL_STATUS_OK)
          return status;
        status = lql_flat_eq_projection_key(state, action->segments[0], error);
        if (status == LQL_STATUS_OK)
          status = lql_flat_eq_mutation_increment(state, &grouped_increment,
                                                  NULL, 0u, 0u, 0, error);
        if (status != LQL_STATUS_OK)
          return status;
        lql_flat_eq_mutation_key_mark_missing(state->program,
                                              action->segments[0], found);
        first = 0;
        continue;
      }
      if (group_count > 1u && lql_flat_eq_mutation_exact_top_key(action) &&
          !lql_flat_eq_mutation_program_has_top_object_wildcard(
              state->program) &&
          lql_flat_eq_mutation_group_is_top_set(state->program, action)) {
        const lql_mutation_action *last_set;
        last_set = lql_flat_eq_mutation_group_last_set(state->program, action);
        if (last_set == NULL)
          return LQL_STATUS_INVALID_ARGUMENT;
        if (!first && (status = lql_flat_eq_write(state, ",", 1u, error)) !=
                          LQL_STATUS_OK)
          return status;
        status = lql_flat_eq_projection_key(state, action->segments[0], error);
        if (status == LQL_STATUS_OK)
          status = lql_flat_eq_mutation_value(state, last_set, error);
        if (status != LQL_STATUS_OK)
          return status;
        lql_flat_eq_mutation_key_mark_missing(state->program,
                                              action->segments[0], found);
        first = 0;
        continue;
      }
      {
        lql_json_spool group_value;
        int group_value_present;
        status = lql_json_spool_init_with_allocator(
            &group_value, state->program->allocator, error);
        if (status != LQL_STATUS_OK)
          return status;
        status = lql_flat_eq_mutation_emit_missing_key_group(
            state, action, &group_value, &group_value_present, error);
        if (status == LQL_STATUS_OK && group_value_present) {
          if (!first)
            status = lql_flat_eq_write(state, ",", 1u, error);
          if (status == LQL_STATUS_OK)
            status =
                lql_flat_eq_projection_key(state, action->segments[0], error);
          if (status == LQL_STATUS_OK)
            status = lql_json_spool_write_to(&group_value, state->writer,
                                             state->writer_user, error);
          if (status == LQL_STATUS_OK)
            first = 0;
        }
        if (status == LQL_STATUS_OK)
          lql_flat_eq_mutation_key_mark_missing(state->program,
                                                action->segments[0], found);
        lql_json_spool_cleanup(&group_value);
        if (status != LQL_STATUS_OK)
          return status;
      }
    }
  }
  status = lql_flat_eq_write(state, "}\n", 2u, error);
  return status;
}

static lql_status lql_flat_eq_mutation_emit_atomic(lql_flat_eq_state *state,
                                                   const lql_json_spool *spool,
                                                   lql_error *error) {
  lql_json_spool rendered;
  lql_stream_writer_fn saved_writer;
  void *saved_writer_user;
  lql_status status;
  if (state == NULL || spool == NULL)
    return LQL_STATUS_INVALID_ARGUMENT;
  status = lql_json_spool_init_with_allocator(&rendered,
                                              state->program->allocator, error);
  if (status != LQL_STATUS_OK)
    return status;
  saved_writer = state->writer;
  saved_writer_user = state->writer_user;
  state->writer = lql_flat_eq_spool_writer;
  state->writer_user = &rendered;
  status = lql_flat_eq_mutation_emit(state, spool, error);
  state->writer = saved_writer;
  state->writer_user = saved_writer_user;
  if (status == LQL_STATUS_OK)
    status = lql_json_spool_write_to(&rendered, state->writer,
                                     state->writer_user, error);
  lql_json_spool_cleanup(&rendered);
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
  int child_array;
  if (!lql_flat_eq_projection_child_shape(state->program, anchor, depth,
                                          &child_array)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "projection paths conflict for JSON object");
    return LQL_STATUS_JSON_ERROR;
  }
  if (child_array)
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
  status = lql_json_spool_init_with_allocator(&projected,
                                              state->program->allocator, error);
  if (status != LQL_STATUS_OK)
    return status;
  saved_writer = state->writer;
  saved_writer_user = state->writer_user;
  state->writer = lql_flat_eq_spool_writer;
  state->writer_user = &projected;
  status = lql_flat_eq_projection_object(state, spool, 0u, 0u, error);
  state->writer = saved_writer;
  state->writer_user = saved_writer_user;
  if (status == LQL_STATUS_OK) {
    if (lql_flat_eq_mutation_program_needs_atomic_output(state->program))
      status = lql_flat_eq_mutation_emit_atomic(state, &projected, error);
    else
      status = lql_flat_eq_mutation_emit(state, &projected, error);
  }
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
        if (left_array && right_array && left_index == right_index &&
            strcmp(left->segments[depth], right->segments[depth]) != 0)
          return 0;
      }
    }
  }
  return 1;
}

static lql_status
lql_flat_eq_record_words(void *user, size_t record_index, int root_is_object,
                         const unsigned long *hits, const lql_json_spool *spool,
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
  matched = lql_flat_eq_matches_record(state->program, state->request->selector,
                                       hits, root_is_object);
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
    if (lql_flat_eq_mutation_program_needs_atomic_output(state->program))
      status = lql_flat_eq_mutation_emit_atomic(state, spool, error);
    else
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

static lql_status lql_flat_eq_record(void *user, size_t record_index,
                                     int root_is_object, unsigned long hits,
                                     const lql_json_spool *spool,
                                     size_t source_offset, size_t source_len,
                                     int source_compact, lql_error *error) {
  return lql_flat_eq_record_words(user, record_index, root_is_object, &hits,
                                  spool, source_offset, source_len,
                                  source_compact, error);
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
  if (program == NULL || mutation == NULL || mutation->action_count == 0u)
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

typedef struct lql_flat_eq_batched_state {
  lql_flat_eq_state *state;
  unsigned long *scan_hits;
  size_t scan_hit_word_count;
  int all_terms_required;
} lql_flat_eq_batched_state;

typedef struct lql_flat_eq_alias_state {
  lql_flat_eq_state *state;
} lql_flat_eq_alias_state;

typedef struct lql_flat_eq_term_batch {
  unsigned long *hits;
  size_t term_offset;
  size_t term_count;
} lql_flat_eq_term_batch;

typedef struct lql_flat_eq_capture_batch {
  lql_json_capture_span *target;
  const lql_json_capture_span *source;
  size_t count;
} lql_flat_eq_capture_batch;

static lql_status
lql_flat_eq_alias_record(void *user, size_t record_index, int root_is_object,
                         unsigned long hits, const lql_json_spool *spool,
                         size_t source_offset, size_t source_len,
                         int source_compact, lql_error *error) {
  lql_flat_eq_alias_state *alias;
  alias = (lql_flat_eq_alias_state *)user;
  if (alias == NULL || alias->state == NULL) {
    return LQL_STATUS_CALLBACK_ERROR;
  }
  return lql_flat_eq_record_words(alias->state, record_index, root_is_object,
                                  &hits, spool, source_offset, source_len,
                                  source_compact, error);
}

static lql_status lql_flat_eq_term_batch_record(
    void *user, size_t record_index, int root_is_object, unsigned long hits,
    const lql_json_spool *spool, size_t source_offset, size_t source_len,
    int source_compact, lql_error *error) {
  lql_flat_eq_term_batch *batch;
  size_t i;
  (void)record_index;
  (void)root_is_object;
  (void)spool;
  (void)source_offset;
  (void)source_len;
  (void)source_compact;
  (void)error;
  batch = (lql_flat_eq_term_batch *)user;
  if (batch == NULL || batch->hits == NULL) {
    return LQL_STATUS_CALLBACK_ERROR;
  }
  for (i = 0u; i < batch->term_count; ++i) {
    if ((hits & (1ul << i)) != 0ul) {
      size_t index;
      index = batch->term_offset + i;
      batch->hits[index / LQL_FLAT_EQ_TERM_CAPACITY] |=
          1ul << (index % LQL_FLAT_EQ_TERM_CAPACITY);
    }
  }
  return LQL_STATUS_OK;
}

static lql_status lql_flat_eq_capture_batch_record(
    void *user, size_t record_index, int root_is_object, unsigned long hits,
    const lql_json_spool *spool, size_t source_offset, size_t source_len,
    int source_compact, lql_error *error) {
  lql_flat_eq_capture_batch *batch;
  (void)record_index;
  (void)root_is_object;
  (void)hits;
  (void)spool;
  (void)source_offset;
  (void)source_len;
  (void)source_compact;
  (void)error;
  batch = (lql_flat_eq_capture_batch *)user;
  if (batch == NULL || batch->target == NULL || batch->source == NULL) {
    return LQL_STATUS_CALLBACK_ERROR;
  }
  memcpy(batch->target, batch->source, batch->count * sizeof(*batch->target));
  return LQL_STATUS_OK;
}

static lql_status lql_flat_eq_scan_term_batch(
    const lql_json_spool *source, const lql_flat_eq_program *program,
    size_t offset, size_t count, unsigned long *hits, lql_error *error) {
  lql_json_flat_eq_request request;
  lql_json_spool_reader reader;
  lql_flat_eq_term_batch batch;
  lql_status status;
  if (source == NULL || program == NULL || hits == NULL || count == 0u ||
      count > LQL_FLAT_EQ_TERM_CAPACITY) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&request, 0, sizeof(request));
  lql_json_spool_reader_init(&reader, source);
  batch.hits = hits;
  batch.term_offset = offset;
  batch.term_count = count;
  request.allocator = program->allocator;
  request.reader = lql_json_spool_read;
  request.reader_user = &reader;
  request.terms = program->scan_terms + offset;
  request.term_count = count;
  request.max_records = 1u;
  request.record = lql_flat_eq_term_batch_record;
  request.record_user = &batch;
  status = lql_json_scan_flat_eq_ndjson(&request, NULL, NULL, error);
  return status;
}

static lql_status lql_flat_eq_scan_capture_batch(const lql_json_spool *source,
                                                 lql_flat_eq_program *program,
                                                 size_t offset, size_t count,
                                                 lql_error *error) {
  lql_json_flat_eq_request request;
  lql_json_spool_reader reader;
  lql_json_spool capture_spool;
  lql_json_capture_span spans[LQL_FLAT_EQ_TERM_CAPACITY];
  lql_flat_eq_capture_batch batch;
  lql_status status;
  if (source == NULL || program == NULL || count == 0u ||
      count > LQL_FLAT_EQ_TERM_CAPACITY) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  status = lql_json_spool_init_with_allocator(&capture_spool,
                                              program->allocator, error);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  memset(&request, 0, sizeof(request));
  memset(spans, 0, sizeof(spans));
  lql_json_spool_reader_init(&reader, source);
  batch.target = program->capture_spans + offset;
  batch.source = spans;
  batch.count = count;
  request.allocator = program->allocator;
  request.reader = lql_json_spool_read;
  request.reader_user = &reader;
  request.spool = &capture_spool;
  request.capture = 1;
  request.capture_keys = program->capture_keys + offset;
  request.capture_key_count = count;
  request.capture_spans = spans;
  request.max_records = 1u;
  request.record = lql_flat_eq_capture_batch_record;
  request.record_user = &batch;
  status = lql_json_scan_flat_eq_ndjson(&request, NULL, NULL, error);
  if (status == LQL_STATUS_OK &&
      lql_json_spool_size(&capture_spool) != lql_json_spool_size(source)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "batched capture changed compact JSON record size");
    status = LQL_STATUS_JSON_ERROR;
  }
  lql_json_spool_cleanup(&capture_spool);
  return status;
}

static int lql_flat_eq_required_term_missed(const lql_flat_eq_program *program,
                                            const unsigned long *hits,
                                            size_t scanned) {
  size_t i;
  if (program == NULL || hits == NULL || program->term_sources == NULL) {
    return 0;
  }
  for (i = 0u; i < program->term_count; ++i) {
    size_t source;
    source = program->term_sources[i];
    if (source < scanned && !lql_flat_eq_hit(hits, source)) {
      return 1;
    }
  }
  return 0;
}

static lql_status
lql_flat_eq_batched_record(void *user, size_t record_index, int root_is_object,
                           unsigned long hits, const lql_json_spool *spool,
                           size_t source_offset, size_t source_len,
                           int source_compact, lql_error *error) {
  lql_flat_eq_batched_state *batch;
  lql_flat_eq_program *program;
  size_t offset;
  lql_status status;
  (void)hits;
  batch = (lql_flat_eq_batched_state *)user;
  if (batch == NULL || batch->state == NULL || batch->scan_hits == NULL ||
      spool == NULL) {
    return LQL_STATUS_CALLBACK_ERROR;
  }
  program = batch->state->program;
  memset(batch->scan_hits, 0,
         batch->scan_hit_word_count * sizeof(*batch->scan_hits));
  for (offset = 0u; offset < program->scan_term_count;
       offset += LQL_FLAT_EQ_TERM_CAPACITY) {
    size_t count;
    count = program->scan_term_count - offset;
    if (count > LQL_FLAT_EQ_TERM_CAPACITY) {
      count = LQL_FLAT_EQ_TERM_CAPACITY;
    }
    status = lql_flat_eq_scan_term_batch(spool, program, offset, count,
                                         batch->scan_hits, error);
    if (status != LQL_STATUS_OK) {
      return status;
    }
    if (batch->all_terms_required &&
        (program->capture_key_count == 0u ||
         batch->state->request->matched_only) &&
        lql_flat_eq_required_term_missed(program, batch->scan_hits,
                                         offset + count)) {
      return lql_flat_eq_record_words(
          batch->state, record_index, root_is_object, batch->scan_hits, spool,
          source_offset, source_len, source_compact, error);
    }
  }
  for (offset = 0u; offset < program->capture_key_count;
       offset += LQL_FLAT_EQ_TERM_CAPACITY) {
    size_t count;
    count = program->capture_key_count - offset;
    if (count > LQL_FLAT_EQ_TERM_CAPACITY) {
      count = LQL_FLAT_EQ_TERM_CAPACITY;
    }
    status =
        lql_flat_eq_scan_capture_batch(spool, program, offset, count, error);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  return lql_flat_eq_record_words(batch->state, record_index, root_is_object,
                                  batch->scan_hits, spool, source_offset,
                                  source_len, source_compact, error);
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
  size_t term_capacity;
  size_t capture_capacity;
  size_t mutation_capacity;
  size_t scan_hit_word_count;
  size_t required_eq_count;
  unsigned long *batched_scan_hits;
  lql_flat_eq_batched_state batched_state;
  lql_flat_eq_alias_state alias_state;
  int cancelled;
  int limit_stop;
  int batched;
  int aliased;
  int all_terms_required;
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
  time_bounds_ptr = NULL;
  if (lql_flat_eq_selector_needs_time(request->selector)) {
    time_t now;
    lql_int64 now_seconds;
    int default_clock_failed;
    memset(&time_bounds, 0, sizeof(time_bounds));
    if (request->time_now != NULL) {
      now = request->time_now(request->time_user);
      default_clock_failed = 0;
    } else {
      now = time(NULL);
      default_clock_failed = now == (time_t)-1;
    }
    time_bounds.now_ready = !default_clock_failed &&
                            lql_temporal_from_time_t(now, 0, &time_bounds.now);
    time_bounds.today_ready =
        !default_clock_failed &&
        lql_temporal_from_time_t(now, 1, &time_bounds.today);
    now_seconds = (lql_int64)now;
    time_bounds.yesterday_ready =
        !default_clock_failed &&
        now_seconds >= LQL_INT64_MIN_VALUE + (lql_int64)86400 &&
        lql_temporal_from_seconds(now_seconds - (lql_int64)86400, 1,
                                  &time_bounds.yesterday);
    time_bounds_ptr = &time_bounds;
  }
  if (!lql_flat_eq_selector_term_count(request->selector, &term_capacity)) {
    return LQL_STATUS_OK;
  }
  capture_capacity =
      (request->output_mode == LQL_STREAM_OUTPUT_PROJECTION ||
       request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION) &&
              request->projection != NULL
          ? request->projection->path_count
          : 0u;
  mutation_capacity =
      (request->output_mode == LQL_STREAM_OUTPUT_MUTATION ||
       request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION) &&
              request->mutation != NULL
          ? request->mutation->action_count
          : 0u;
  /*
   * The direct API has one pass over a non-rewindable reader. Its scanner is
   * deliberately word-bounded, so reject wider plans before allocating or
   * consuming input rather than disguising replay or disk spooling as
   * streaming. The separately named compatibility API handles wide plans.
   */
  if (!allow_spool && (term_capacity > LQL_FLAT_EQ_TERM_CAPACITY ||
                       capture_capacity > LQL_FLAT_EQ_TERM_CAPACITY)) {
    return LQL_STATUS_OK;
  }
  if (!lql_flat_eq_program_init(&program, lql_allocator_from_receiver(self),
                                term_capacity, capture_capacity,
                                mutation_capacity)) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY,
                  "unable to allocate stream selector state");
    return LQL_STATUS_NO_MEMORY;
  }
  if (request->selector != NULL &&
      !lql_flat_eq_append(&program, request->selector, time_bounds_ptr)) {
    lql_flat_eq_program_cleanup(&program);
    return LQL_STATUS_OK;
  }
  if ((request->output_mode == LQL_STREAM_OUTPUT_PROJECTION ||
       request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION) &&
      !lql_flat_eq_projection_append(&program, request->projection)) {
    lql_flat_eq_program_cleanup(&program);
    return LQL_STATUS_OK;
  }
  if ((request->output_mode == LQL_STREAM_OUTPUT_MUTATION ||
       request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION) &&
      !lql_flat_eq_mutation_append(&program, request->mutation)) {
    lql_flat_eq_program_cleanup(&program);
    return LQL_STATUS_OK;
  }
  if (mutation_capacity != 0u) {
    status = lql_flat_eq_preflight_file_mutations(&program, error);
    if (status != LQL_STATUS_OK) {
      *out_handled = 1;
      lql_flat_eq_program_cleanup(&program);
      return status;
    }
  }
  if (!lql_flat_eq_program_build_selector_index(&program)) {
    lql_flat_eq_program_cleanup(&program);
    lql_set_error(error, LQL_STATUS_NO_MEMORY,
                  "unable to allocate stream selector index");
    return LQL_STATUS_NO_MEMORY;
  }
  batched = program.term_count > LQL_FLAT_EQ_TERM_CAPACITY ||
            program.capture_key_count > LQL_FLAT_EQ_TERM_CAPACITY;
  /*
   * The scanner's word mask is a private fast-path detail, not an API limit.
   * Deduplicate equivalent predicates first; if their unique set still exceeds
   * one word, the spooled executor scans each compact record in word batches
   * and merges those hits before evaluating the original selector tree.
   */
  if (batched && !lql_flat_eq_program_build_scan_terms(&program)) {
    lql_flat_eq_program_cleanup(&program);
    lql_set_error(error, LQL_STATUS_NO_MEMORY,
                  "unable to allocate stream selector scan state");
    return LQL_STATUS_NO_MEMORY;
  }
  required_eq_count = 0u;
  all_terms_required =
      batched &&
      lql_flat_eq_required_eq_count(request->selector, &required_eq_count) &&
      required_eq_count == program.term_count;
  aliased = batched && program.scan_term_count <= LQL_FLAT_EQ_TERM_CAPACITY &&
            program.capture_key_count <= LQL_FLAT_EQ_TERM_CAPACITY;
  if (batched && !aliased && !allow_spool) {
    lql_flat_eq_program_cleanup(&program);
    return LQL_STATUS_OK;
  }
  if (!batched) {
    program.stop_hit_mask = lql_flat_eq_stop_mask(&program, request->selector);
    program.stop_matching_on_hit = program.stop_hit_mask != 0ul;
  }
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
  if (batched && !aliased) {
    /* Oversized public requests are scanned per compact record in word batches.
     */
    capture = 1;
  }
  memset(&spool, 0, sizeof(spool));
  if (capture) {
    status =
        lql_json_spool_init_with_allocator(&spool, program.allocator, error);
    if (status != LQL_STATUS_OK) {
      lql_flat_eq_program_cleanup(&program);
      return status;
    }
  }
  memset(&state, 0, sizeof(state));
  state.request = request;
  state.result = result;
  state.program = &program;
  state.writer = request->writer;
  state.writer_user = request->writer_user;
  batched_scan_hits = NULL;
  memset(&batched_state, 0, sizeof(batched_state));
  memset(&alias_state, 0, sizeof(alias_state));
  scan_hit_word_count = 0u;
  if (batched) {
    if (!aliased) {
      scan_hit_word_count =
          (program.scan_term_count + LQL_FLAT_EQ_TERM_CAPACITY - 1u) /
          LQL_FLAT_EQ_TERM_CAPACITY;
      if (scan_hit_word_count == 0u) {
        scan_hit_word_count = 1u;
      }
      batched_scan_hits = program.allocator->calloc(
          program.allocator, scan_hit_word_count, sizeof(*batched_scan_hits));
    }
    if (!aliased && batched_scan_hits == NULL) {
      if (capture) {
        lql_json_spool_cleanup(&spool);
      }
      program.allocator->destroy(program.allocator, batched_scan_hits);
      lql_flat_eq_program_cleanup(&program);
      lql_set_error(error, LQL_STATUS_NO_MEMORY,
                    "unable to allocate stream selector hit state");
      return LQL_STATUS_NO_MEMORY;
    }
    if (aliased) {
      alias_state.state = &state;
    } else {
      batched_state.state = &state;
      batched_state.scan_hits = batched_scan_hits;
      batched_state.scan_hit_word_count = scan_hit_word_count;
      batched_state.all_terms_required = all_terms_required;
    }
  }
  memset(&scan_request, 0, sizeof(scan_request));
  cancelled = 0;
  scan_request.allocator = program.allocator;
  scan_request.reader = request->reader;
  scan_request.reader_user = request->reader_user;
  scan_request.terms =
      batched ? (aliased ? program.scan_terms : NULL) : program.terms;
  scan_request.term_count =
      batched ? (aliased ? program.scan_term_count : 0u) : program.term_count;
  scan_request.spool = capture ? &spool : NULL;
  scan_request.capture = capture;
  scan_request.stop_matching_on_hit =
      batched ? 0 : program.stop_matching_on_hit;
  scan_request.stop_hit_mask = batched ? 0ul : program.stop_hit_mask;
  scan_request.capture_keys = batched && !aliased ? NULL : program.capture_keys;
  scan_request.capture_key_count =
      batched && !aliased ? 0u : program.capture_key_count;
  scan_request.capture_spans =
      batched && !aliased ? NULL : program.capture_spans;
  scan_request.max_records = request->limits.max_records;
  scan_request.max_bytes = request->limits.max_bytes;
  scan_request.cancelled = request->cancelled;
  scan_request.cancel_user = request->cancel_user;
  scan_request.out_cancelled = &cancelled;
  limit_stop = 0;
  scan_request.out_limit_stop = &limit_stop;
  scan_request.record = batched ? (aliased ? lql_flat_eq_alias_record
                                           : lql_flat_eq_batched_record)
                                : lql_flat_eq_record;
  scan_request.record_user =
      batched ? (aliased ? (void *)&alias_state : (void *)&batched_state)
              : (void *)&state;
  status =
      lql_json_scan_flat_eq_ndjson(&scan_request, &records, &bytes_read, error);
  result->bytes_consumed = bytes_read;
  if (capture) {
    lql_json_spool_cleanup(&spool);
  }
  program.allocator->destroy(program.allocator, batched_scan_hits);
  lql_flat_eq_program_cleanup(&program);
  if (status == LQL_STATUS_STOP && !result->stopped_early && limit_stop &&
      request->limits.max_records != 0u &&
      result->records_seen >= request->limits.max_records) {
    result->stopped_early = 1;
    result->stop_reason = LQL_STREAM_STOP_RECORD_LIMIT;
  }
  if (status == LQL_STATUS_STOP && !result->stopped_early && limit_stop &&
      request->limits.max_bytes != 0u &&
      result->bytes_consumed >= request->limits.max_bytes) {
    result->stopped_early = 1;
    result->stop_reason = LQL_STREAM_STOP_BYTE_LIMIT;
  }
  if (status == LQL_STATUS_STOP && !result->stopped_early && cancelled) {
    result->stopped_early = 1;
    result->stop_reason = LQL_STREAM_STOP_CANCELLED;
  }
  if (status == LQL_STATUS_STOP && result->stopped_early) {
    return LQL_STATUS_OK;
  }
  return status;
}
