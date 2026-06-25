#include "lql_internal.h"

#include <ctype.h>
#include <lonejson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct eval_doc {
  const lql_selector *selector;
  unsigned char *hits;
  char *val_buf;
  size_t val_len;
  int *container_types;
  size_t *container_depths;
  size_t container_count;
  size_t container_cap;
} eval_doc;

static void free_doc(eval_doc *doc) {
  free(doc->hits);
  free(doc->val_buf);
  free(doc->container_types);
  free(doc->container_depths);
  memset(doc, 0, sizeof(*doc));
}

static int init_doc(eval_doc *doc, const lql_selector *selector) {
  memset(doc, 0, sizeof(*doc));
  doc->selector = selector;
  if (selector != NULL && selector->hit_count != 0u) {
    doc->hits = (unsigned char *)calloc(selector->hit_count, 1u);
    if (doc->hits == NULL) {
      return 0;
    }
  }
  return 1;
}

static void reset_doc(eval_doc *doc) {
  if (doc->selector != NULL && doc->selector->hit_count != 0u) {
    memset(doc->hits, 0, doc->selector->hit_count);
  }
  free(doc->val_buf);
  doc->val_buf = NULL;
  doc->val_len = 0u;
  doc->container_count = 0u;
}

static int append_buf(char **buf, size_t *len, const char *data, size_t n) {
  char *next;
  next = (char *)realloc(*buf, *len + n + 1u);
  if (next == NULL) {
    return 0;
  }
  *buf = next;
  memcpy(*buf + *len, data, n);
  *len += n;
  (*buf)[*len] = '\0';
  return 1;
}

static lonejson_status push_container(eval_doc *doc,
                                      const lonejson_value_path *path,
                                      int type) {
  int *next_types;
  size_t *next_depths;
  size_t next_cap;
  if (doc->container_count == doc->container_cap) {
    next_cap = doc->container_cap == 0u ? 8u : doc->container_cap * 2u;
    next_types = (int *)realloc(doc->container_types, sizeof(int) * next_cap);
    if (next_types == NULL) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    doc->container_types = next_types;
    next_depths =
        (size_t *)realloc(doc->container_depths, sizeof(size_t) * next_cap);
    if (next_depths == NULL) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    doc->container_depths = next_depths;
    doc->container_cap = next_cap;
  }
  doc->container_types[doc->container_count] = type;
  doc->container_depths[doc->container_count] = path->segment_count;
  ++doc->container_count;
  return LONEJSON_STATUS_OK;
}

static void pop_container(eval_doc *doc, const lonejson_value_path *path) {
  if (doc->container_count == 0u) {
    return;
  }
  if (doc->container_depths[doc->container_count - 1u] ==
      path->segment_count) {
    --doc->container_count;
  }
}

static int ascii_case_equal_prefix(const char *a, const char *b, size_t n) {
  size_t i;
  for (i = 0u; i < n; ++i) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
      return 0;
    }
  }
  return 1;
}

static int contains_case(const char *haystack, const char *needle,
                         int ignore_case) {
  size_t h;
  size_t n;
  size_t i;
  h = strlen(haystack);
  n = strlen(needle);
  if (n == 0u) {
    return 1;
  }
  if (n > h) {
    return 0;
  }
  for (i = 0u; i + n <= h; ++i) {
    if (ignore_case ? ascii_case_equal_prefix(haystack + i, needle, n)
                    : memcmp(haystack + i, needle, n) == 0) {
      return 1;
    }
  }
  return 0;
}

static int path_segment_matches(const char *start, size_t len,
                                const lonejson_path_segment *segment) {
  size_t i;
  size_t j;
  char ch;
  i = 0u;
  j = 0u;
  while (i < len && j < segment->len) {
    ch = start[i++];
    if (ch == '~' && i < len) {
      if (start[i] == '0') {
        ch = '~';
        ++i;
      } else if (start[i] == '1') {
        ch = '/';
        ++i;
      }
    }
    if (ch != segment->data[j++]) {
      return 0;
    }
  }
  return i == len && j == segment->len;
}

static int parent_container_type(const eval_doc *doc, size_t depth,
                                 int *out_type) {
  size_t i;
  for (i = doc->container_count; i > 0u; --i) {
    if (doc->container_depths[i - 1u] == depth) {
      *out_type = doc->container_types[i - 1u];
      return 1;
    }
  }
  return 0;
}

static int pattern_segment_is(const char *start, size_t len, const char *lit) {
  return strlen(lit) == len && memcmp(start, lit, len) == 0;
}

static int pattern_segment_matches(const eval_doc *doc, const char *start,
                                   size_t len,
                                   const lonejson_value_path *path,
                                   size_t path_idx) {
  int parent_type;
  if (path_idx >= path->segment_count ||
      !parent_container_type(doc, path_idx, &parent_type)) {
    return 0;
  }
  if (pattern_segment_is(start, len, "*")) {
    return parent_type == '{';
  }
  if (pattern_segment_is(start, len, "[]")) {
    return parent_type == '[';
  }
  if (pattern_segment_is(start, len, "**")) {
    return parent_type == '{' || parent_type == '[';
  }
  return path_segment_matches(start, len, &path->segments[path_idx]);
}

static int path_matches_from(const eval_doc *doc, const char *seg,
                             const lonejson_value_path *path,
                             size_t path_idx) {
  const char *slash;
  size_t len;
  size_t i;
  if (*seg == '\0') {
    return path_idx == path->segment_count;
  }
  slash = strchr(seg, '/');
  len = slash == NULL ? strlen(seg) : (size_t)(slash - seg);
  if (pattern_segment_is(seg, len, "...")) {
    if (slash == NULL) {
      return 1;
    }
    for (i = path_idx; i <= path->segment_count; ++i) {
      if (path_matches_from(doc, slash + 1, path, i)) {
        return 1;
      }
    }
    return 0;
  }
  if (!pattern_segment_matches(doc, seg, len, path, path_idx)) {
    return 0;
  }
  if (slash == NULL) {
    return path_idx + 1u == path->segment_count;
  }
  return path_matches_from(doc, slash + 1, path, path_idx + 1u);
}

static int path_matches(const eval_doc *doc, const char *pattern,
                        const lonejson_value_path *path) {
  const char *seg;
  if (pattern == NULL || path == NULL || pattern[0] != '/') {
    return 0;
  }
  if (pattern[1] == '\0') {
    return path->segment_count == 0u;
  }
  seg = pattern + 1;
  return path_matches_from(doc, seg, path, 0u);
}

static int node_is_term(const lql_node *node) {
  switch (node->kind) {
  case LQL_NODE_EQ:
  case LQL_NODE_NE:
  case LQL_NODE_CONTAINS:
  case LQL_NODE_ICONTAINS:
  case LQL_NODE_PREFIX:
  case LQL_NODE_IPREFIX:
  case LQL_NODE_RANGE:
  case LQL_NODE_IN:
  case LQL_NODE_EXISTS:
    return 1;
  default:
    return 0;
  }
}

static void observe_node(eval_doc *doc, const lql_node *node,
                         const lonejson_value_path *path, const char *value,
                         int is_number, int is_container) {
  size_t i;
  size_t n;
  size_t j;
  double number;
  const char *needle;
  if (node == NULL) {
    return;
  }
  if (!node_is_term(node)) {
    for (i = 0u; i < node->child_count; ++i) {
      observe_node(doc, &node->children[i], path, value, is_number,
                   is_container);
    }
    return;
  }
  if (doc->hits == NULL || !path_matches(doc, node->term.field, path)) {
    return;
  }
  switch (node->kind) {
  case LQL_NODE_EQ:
    if (is_container) {
      break;
    }
    if (strcmp(value, node->term.value == NULL ? "" : node->term.value) == 0) {
      doc->hits[node->hit_index] = 1u;
    }
    break;
  case LQL_NODE_NE:
    if (is_container) {
      break;
    }
    if (strcmp(value, node->term.value == NULL ? "" : node->term.value) != 0) {
      doc->hits[node->hit_index] = 1u;
    }
    break;
  case LQL_NODE_CONTAINS:
  case LQL_NODE_ICONTAINS:
    if (is_container) {
      break;
    }
    if (node->term.any_count == 0u) {
      if (contains_case(value, node->term.value == NULL ? "" : node->term.value,
                        node->kind == LQL_NODE_ICONTAINS ||
                            node->term.ignore_case)) {
        doc->hits[node->hit_index] = 1u;
      }
    } else {
      for (j = 0u; j < node->term.any_count; ++j) {
        if (contains_case(value, node->term.any[j],
                          node->kind == LQL_NODE_ICONTAINS ||
                              node->term.ignore_case)) {
          doc->hits[node->hit_index] = 1u;
          break;
        }
      }
    }
    break;
  case LQL_NODE_PREFIX:
  case LQL_NODE_IPREFIX:
    if (is_container) {
      break;
    }
    n = strlen(node->term.value == NULL ? "" : node->term.value);
    if (strlen(value) >= n &&
        (node->kind == LQL_NODE_IPREFIX || node->term.ignore_case
             ? ascii_case_equal_prefix(value, node->term.value, n)
             : memcmp(value, node->term.value, n) == 0)) {
      doc->hits[node->hit_index] = 1u;
    }
    break;
  case LQL_NODE_RANGE:
    if (!is_container && is_number && node->term.has_number) {
      number = strtod(value, NULL);
      if ((node->term.range_op == '>' && number > node->term.number) ||
          (node->term.range_op == 'G' && number >= node->term.number) ||
          (node->term.range_op == '<' && number < node->term.number) ||
          (node->term.range_op == 'L' && number <= node->term.number)) {
        doc->hits[node->hit_index] = 1u;
      }
    }
    break;
  case LQL_NODE_IN:
    if (is_container) {
      break;
    }
    for (j = 0u; j < node->term.any_count; ++j) {
      needle = node->term.any[j];
      if (strcmp(value, needle) == 0) {
        doc->hits[node->hit_index] = 1u;
        break;
      }
    }
    break;
  case LQL_NODE_EXISTS:
    doc->hits[node->hit_index] = 1u;
    break;
  default:
    break;
  }
}

static void observe_value(eval_doc *doc, const lonejson_value_path *path,
                          const char *value, int is_number, int is_container) {
  if (doc->selector == NULL || doc->selector->root.kind == LQL_NODE_ALL) {
    return;
  }
  observe_node(doc, &doc->selector->root, path, value, is_number, is_container);
}

static int eval_node(const lql_node *node, const eval_doc *doc) {
  size_t i;
  switch (node->kind) {
  case LQL_NODE_ALL:
    return 1;
  case LQL_NODE_AND:
    for (i = 0u; i < node->child_count; ++i) {
      if (!eval_node(&node->children[i], doc)) {
        return 0;
      }
    }
    return 1;
  case LQL_NODE_OR:
    for (i = 0u; i < node->child_count; ++i) {
      if (eval_node(&node->children[i], doc)) {
        return 1;
      }
    }
    return 0;
  case LQL_NODE_NOT:
    return node->child_count == 0u ? 1 : !eval_node(&node->children[0], doc);
  default:
    return doc->hits != NULL && doc->hits[node->hit_index] != 0u;
  }
}

static lonejson_status on_object_begin(void *user,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  observe_value(doc, path, "", 0, 1);
  return push_container(doc, path, '{');
}

static lonejson_status on_object_end(void *user, const lonejson_value_path *path,
                                     lonejson_error *error) {
  (void)error;
  pop_container((eval_doc *)user, path);
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_array_begin(void *user,
                                      const lonejson_value_path *path,
                                      lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  observe_value(doc, path, "", 0, 1);
  return push_container(doc, path, '[');
}

static lonejson_status on_array_end(void *user, const lonejson_value_path *path,
                                    lonejson_error *error) {
  (void)error;
  pop_container((eval_doc *)user, path);
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_string_begin(void *user,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)path;
  (void)error;
  free(doc->val_buf);
  doc->val_buf = NULL;
  doc->val_len = 0u;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_string_chunk(void *user,
                                       const lonejson_value_path *path,
                                       const char *data, size_t len,
                                       lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)path;
  (void)error;
  return append_buf(&doc->val_buf, &doc->val_len, data, len)
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_ALLOCATION_FAILED;
}

static lonejson_status on_string_end(void *user,
                                     const lonejson_value_path *path,
                                     lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  observe_value(doc, path, doc->val_buf == NULL ? "" : doc->val_buf, 0, 0);
  free(doc->val_buf);
  doc->val_buf = NULL;
  doc->val_len = 0u;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_number_begin(void *user,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  return on_string_begin(user, path, error);
}

static lonejson_status on_number_chunk(void *user,
                                       const lonejson_value_path *path,
                                       const char *data, size_t len,
                                       lonejson_error *error) {
  return on_string_chunk(user, path, data, len, error);
}

static lonejson_status on_number_end(void *user,
                                     const lonejson_value_path *path,
                                     lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  observe_value(doc, path, doc->val_buf == NULL ? "" : doc->val_buf, 1, 0);
  free(doc->val_buf);
  doc->val_buf = NULL;
  doc->val_len = 0u;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_boolean(void *user, const lonejson_value_path *path,
                                  int value, lonejson_error *error) {
  (void)error;
  observe_value((eval_doc *)user, path, value ? "true" : "false", 0, 0);
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_null(void *user, const lonejson_value_path *path,
                               lonejson_error *error) {
  (void)error;
  observe_value((eval_doc *)user, path, "", 0, 0);
  return LONEJSON_STATUS_OK;
}

static void init_eval_visitor(lonejson_path_value_visitor *visitor) {
  *visitor = lonejson_default_path_value_visitor();
  visitor->object_begin = on_object_begin;
  visitor->object_end = on_object_end;
  visitor->array_begin = on_array_begin;
  visitor->array_end = on_array_end;
  visitor->string_begin = on_string_begin;
  visitor->string_chunk = on_string_chunk;
  visitor->string_end = on_string_end;
  visitor->number_begin = on_number_begin;
  visitor->number_chunk = on_number_chunk;
  visitor->number_end = on_number_end;
  visitor->boolean_value = on_boolean;
  visitor->null_value = on_null;
}

typedef struct query_stream_state {
  const lql_selector *selector;
  lql_query_decision_fn on_decision;
  void *user;
  lql_query_result result;
  lql_status callback_status;
  eval_doc doc;
} query_stream_state;

static lonejson_candidate_callback_result
on_candidate_begin(void *user, const lonejson_candidate_info *candidate,
                   lonejson_error *error) {
  query_stream_state *state = (query_stream_state *)user;
  (void)candidate;
  (void)error;
  reset_doc(&state->doc);
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lonejson_candidate_callback_result
on_candidate_end(void *user, const lonejson_candidate_info *candidate,
                 lonejson_error *error) {
  query_stream_state *state = (query_stream_state *)user;
  lql_query_decision decision;
  lql_status st;
  int matched;
  (void)error;
  matched = state->selector == NULL ||
            state->selector->root.kind == LQL_NODE_ALL ||
            eval_node(&state->selector->root, &state->doc);
  if (matched) {
    state->result.candidates_matched++;
  }
  state->result.candidates_seen++;
  state->result.bytes_read =
      (lql_uint64)(candidate->stream_offset + candidate->byte_size);
  decision.matched = matched;
  decision.index = (lql_uint64)candidate->index;
  decision.offset = (lql_uint64)candidate->stream_offset;
  decision.size = (lql_uint64)candidate->byte_size;
  st = state->on_decision(state->user, &decision);
  reset_doc(&state->doc);
  if (st != LQL_STATUS_OK) {
    state->callback_status = st;
    return LONEJSON_CANDIDATE_ERROR;
  }
  return LONEJSON_CANDIDATE_CONTINUE;
}

lql_status lql_eval_selector(const lql_selector *selector, const char *json,
                             size_t json_len, int *out_matched,
                             lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_status st;
  eval_doc doc;

  if (!init_doc(&doc, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime = lonejson_new(NULL, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    free_doc(&doc);
    return LQL_STATUS_JSON_ERROR;
  }
  init_eval_visitor(&visitor);
  st = lonejson_visit_path_value_buffer(runtime, json, json_len, &visitor, &doc,
                                        &lj_error);
  if (st != LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    free_doc(&doc);
    lonejson_free(runtime);
    return LQL_STATUS_JSON_ERROR;
  }
  *out_matched = selector == NULL || selector->root.kind == LQL_NODE_ALL ||
                 eval_node(&selector->root, &doc);
  free_doc(&doc);
  lonejson_free(runtime);
  return LQL_STATUS_OK;
}

lql_status
lql_eval_query_file_decisions(const lql_selector *selector, FILE *file,
                              lql_query_decision_fn on_decision, void *user,
                              lql_query_result *out_result, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  query_stream_state state;

  memset(&state, 0, sizeof(state));
  state.selector = selector;
  state.on_decision = on_decision;
  state.user = user;
  state.callback_status = LQL_STATUS_OK;
  if (!init_doc(&state.doc, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime = lonejson_new(NULL, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    free_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_NONE;
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_candidate_begin;
  options.candidate_end = on_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_filep(runtime, file, &options, &lj_error);
  if (st != LONEJSON_STATUS_OK) {
    free_doc(&state.doc);
    lonejson_free(runtime);
    if (state.callback_status != LQL_STATUS_OK) {
      lql_set_error(error, state.callback_status,
                    "query decision callback failed");
      return state.callback_status;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  free_doc(&state.doc);
  lonejson_free(runtime);
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}
