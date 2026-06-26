#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "lql_internal.h"

#include <ctype.h>
#include <lonejson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct eval_doc {
  lql_allocator *allocator;
  const lql_selector *selector;
  unsigned char *hits;
  char *val_buf;
  size_t val_len;
  int scalar_interested;
  int *container_types;
  size_t *container_depths;
  size_t container_count;
  size_t container_cap;
  char root_kind;
} eval_doc;

typedef struct lql_match_adapter {
  FILE *file;
  lql_query_match_fn on_match;
  void *user;
} lql_match_adapter;

typedef struct lql_payload_sink_adapter {
  lql_write_fn write;
  void *user;
  lql_status status;
} lql_payload_sink_adapter;

static void clear_query_result(lql_query_result *out_result) {
  if (out_result != NULL) {
    memset(out_result, 0, sizeof(*out_result));
  }
}

static int payload_seek_u64(FILE *file, lql_uint64 offset) {
  off_t seek_offset;
  seek_offset = (off_t)offset;
  if (seek_offset < (off_t)0 || (lql_uint64)seek_offset != offset) {
    return 0;
  }
  return fseeko(file, seek_offset, SEEK_SET) == 0;
}

static lql_status copy_range_to_sink(FILE *in, lql_uint64 size,
                                     lql_write_fn write, void *user) {
  char buf[8192];
  size_t want;
  size_t got;
  lql_status st;
  while (size != 0u) {
    want = size > (lql_uint64)sizeof(buf) ? sizeof(buf) : (size_t)size;
    got = fread(buf, 1u, want, in);
    if (got == 0u) {
      return LQL_STATUS_JSON_ERROR;
    }
    st = write(user, buf, got);
    if (st != LQL_STATUS_OK) {
      return st;
    }
    size -= (lql_uint64)got;
  }
  return LQL_STATUS_OK;
}

static lql_status payload_file_write(void *user, const void *data, size_t len) {
  FILE *out;
  out = (FILE *)user;
  return fwrite(data, 1u, len, out) == len ? LQL_STATUS_OK
                                           : LQL_STATUS_JSON_ERROR;
}

static lonejson_status payload_lql_sink(void *user, const void *data,
                                        size_t len, lonejson_error *error) {
  lql_payload_sink_adapter *adapter;
  (void)error;
  adapter = (lql_payload_sink_adapter *)user;
  adapter->status = adapter->write(adapter->user, data, len);
  return adapter->status == LQL_STATUS_OK ? LONEJSON_STATUS_OK
                                          : LONEJSON_STATUS_CALLBACK_FAILED;
}

static lql_status on_match_decision(void *user,
                                    const lql_query_decision *decision) {
  lql_match_adapter *adapter;
  lql_query_match match;

  if (!decision->matched) {
    return LQL_STATUS_OK;
  }
  adapter = (lql_match_adapter *)user;
  memset(&match, 0, sizeof(match));
  match.decision = *decision;
  match.payload.kind = LQL_PAYLOAD_SEEKABLE_RANGE;
  match.payload.index = decision->index;
  match.payload.offset = decision->offset;
  match.payload.size = decision->size;
  match.payload.source = adapter->file;
  return adapter->on_match(adapter->user, &match);
}

static void destroy_doc(eval_doc *doc) {
  doc->allocator->destroy(doc->allocator, doc->hits);
  doc->allocator->destroy(doc->allocator, doc->val_buf);
  doc->allocator->destroy(doc->allocator, doc->container_types);
  doc->allocator->destroy(doc->allocator, doc->container_depths);
  memset(doc, 0, sizeof(*doc));
}

static int init_doc(eval_doc *doc, lql *self, const lql_selector *selector) {
  memset(doc, 0, sizeof(*doc));
  doc->allocator = lql_allocator_from_receiver(self);
  doc->selector = selector;
  if (doc->allocator == NULL) {
    return 0;
  }
  if (selector != NULL && selector->hit_count != 0u) {
    doc->hits = (unsigned char *)doc->allocator->calloc(
        doc->allocator, selector->hit_count, 1u);
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
  doc->allocator->destroy(doc->allocator, doc->val_buf);
  doc->val_buf = NULL;
  doc->val_len = 0u;
  doc->scalar_interested = 0;
  doc->container_count = 0u;
  doc->root_kind = '\0';
}

static int append_buf(lql_allocator *allocator, char **buf, size_t *len,
                      const char *data, size_t n) {
  char *next;
  next = (char *)allocator->realloc(allocator, *buf, *len + n + 1u);
  if (next == NULL) {
    return 0;
  }
  *buf = next;
  memcpy(*buf + *len, data, n);
  *len += n;
  (*buf)[*len] = '\0';
  return 1;
}

static lonejson_status
push_container(eval_doc *doc, const lonejson_value_path *path, int type) {
  int *next_types;
  size_t *next_depths;
  size_t next_cap;
  if (doc->container_count == doc->container_cap) {
    next_cap = doc->container_cap == 0u ? 8u : doc->container_cap * 2u;
    next_types = (int *)doc->allocator->realloc(
        doc->allocator, doc->container_types, sizeof(int) * next_cap);
    if (next_types == NULL) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    doc->container_types = next_types;
    next_depths = (size_t *)doc->allocator->realloc(
        doc->allocator, doc->container_depths, sizeof(size_t) * next_cap);
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
  if (doc->container_depths[doc->container_count - 1u] == path->segment_count) {
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
                                   size_t len, const lonejson_value_path *path,
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
                             const lonejson_value_path *path, size_t path_idx) {
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
  case LQL_NODE_DATE:
  case LQL_NODE_IN:
  case LQL_NODE_EXISTS:
    return 1;
  default:
    return 0;
  }
}

static int resolve_since_macro(lql_since_macro macro, lql_temporal *out) {
  switch (macro) {
  case LQL_SINCE_NOW:
    return lql_temporal_now(out);
  case LQL_SINCE_TODAY:
    return lql_temporal_today(out);
  case LQL_SINCE_YESTERDAY:
    return lql_temporal_yesterday(out);
  default:
    return 0;
  }
}

static void observe_node(eval_doc *doc, const lql_node *node,
                         const lonejson_value_path *path, const char *value,
                         int is_number, int is_container, int is_null) {
  size_t i;
  size_t n;
  size_t j;
  double number;
  lql_temporal temporal;
  lql_temporal query_temporal;
  lql_temporal since_macro;
  const char *needle;
  if (node == NULL) {
    return;
  }
  if (!node_is_term(node)) {
    for (i = 0u; i < node->child_count; ++i) {
      observe_node(doc, &node->children[i], path, value, is_number,
                   is_container, is_null);
    }
    return;
  }
  if (doc->hits == NULL || !path_matches(doc, node->term.field, path)) {
    return;
  }
  switch (node->kind) {
  case LQL_NODE_EQ:
    if (is_container || is_null) {
      break;
    }
    if (strcmp(value, node->term.value == NULL ? "" : node->term.value) == 0) {
      doc->hits[node->hit_index] = 1u;
    } else if (lql_parse_temporal_literal(node->term.value, &query_temporal) &&
               lql_parse_temporal_literal(value, &temporal) &&
               lql_temporal_equal(&temporal, &query_temporal)) {
      doc->hits[node->hit_index] = 1u;
    }
    break;
  case LQL_NODE_NE:
    if (is_container) {
      break;
    }
    if (is_null) {
      doc->hits[node->hit_index] = 1u;
      break;
    }
    if (strcmp(value, node->term.value == NULL ? "" : node->term.value) != 0 &&
        !(lql_parse_temporal_literal(node->term.value, &query_temporal) &&
          lql_parse_temporal_literal(value, &temporal) &&
          lql_temporal_equal(&temporal, &query_temporal))) {
      doc->hits[node->hit_index] = 1u;
    }
    break;
  case LQL_NODE_CONTAINS:
  case LQL_NODE_ICONTAINS:
    if (node->term.any_count == 0u && !node->term.value_set &&
        node->term.value == NULL) {
      doc->hits[node->hit_index] = 1u;
      break;
    }
    if (is_container || is_null) {
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
    if (!node->term.value_set && node->term.value == NULL) {
      doc->hits[node->hit_index] = 1u;
      break;
    }
    if (is_container || is_null) {
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
    if (!is_container && !is_null && node->term.range_is_temporal) {
      if (lql_parse_temporal_literal(value, &temporal) &&
          (!node->term.has_temporal_gt ||
           lql_temporal_compare(&temporal, &node->term.temporal_gt) > 0) &&
          (!node->term.has_temporal_gte ||
           lql_temporal_compare(&temporal, &node->term.temporal_gte) >= 0) &&
          (!node->term.has_temporal_lt ||
           lql_temporal_compare(&temporal, &node->term.temporal_lt) < 0) &&
          (!node->term.has_temporal_lte ||
           lql_temporal_compare(&temporal, &node->term.temporal_lte) <= 0)) {
        doc->hits[node->hit_index] = 1u;
      }
    } else if (!is_container && !is_null && is_number) {
      number = strtod(value, NULL);
      if ((!node->term.has_range_gt || number > node->term.range_gt) &&
          (!node->term.has_range_gte || number >= node->term.range_gte) &&
          (!node->term.has_range_lt || number < node->term.range_lt) &&
          (!node->term.has_range_lte || number <= node->term.range_lte)) {
        doc->hits[node->hit_index] = 1u;
      }
    }
    break;
  case LQL_NODE_DATE:
    if (!is_container && !is_null &&
        (node->term.since_macro == LQL_SINCE_NONE ||
         resolve_since_macro(node->term.since_macro, &since_macro)) &&
        lql_parse_temporal_literal(value, &temporal) &&
        (!node->term.has_temporal_eq ||
         lql_temporal_equal(&temporal, &node->term.temporal_eq)) &&
        (node->term.since_macro == LQL_SINCE_NONE ||
         lql_temporal_compare(&temporal, &since_macro) >= 0) &&
        (!node->term.has_temporal_gt ||
         lql_temporal_compare(&temporal, &node->term.temporal_gt) > 0) &&
        (!node->term.has_temporal_gte ||
         lql_temporal_compare(&temporal, &node->term.temporal_gte) >= 0) &&
        (!node->term.has_temporal_lt ||
         lql_temporal_compare(&temporal, &node->term.temporal_lt) < 0) &&
        (!node->term.has_temporal_lte ||
         lql_temporal_compare(&temporal, &node->term.temporal_lte) <= 0)) {
      doc->hits[node->hit_index] = 1u;
    }
    break;
  case LQL_NODE_IN:
    if (is_container || is_null) {
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
    if (!is_null) {
      doc->hits[node->hit_index] = 1u;
    }
    break;
  default:
    break;
  }
}

static int selector_path_interested(const eval_doc *doc, const lql_node *node,
                                    const lonejson_value_path *path) {
  size_t i;

  if (node == NULL) {
    return 0;
  }
  if (!node_is_term(node)) {
    for (i = 0u; i < node->child_count; ++i) {
      if (selector_path_interested(doc, &node->children[i], path)) {
        return 1;
      }
    }
    return 0;
  }
  return path_matches(doc, node->term.field, path);
}

static int scalar_path_interested(const eval_doc *doc,
                                  const lonejson_value_path *path) {
  if (doc->selector == NULL || doc->selector->root.kind == LQL_NODE_ALL) {
    return 0;
  }
  return selector_path_interested(doc, &doc->selector->root, path);
}

static void observe_value(eval_doc *doc, const lonejson_value_path *path,
                          const char *value, int is_number, int is_container,
                          int is_null) {
  if (doc->selector == NULL || doc->selector->root.kind == LQL_NODE_ALL) {
    return;
  }
  observe_node(doc, &doc->selector->root, path, value, is_number, is_container,
               is_null);
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
  if (path->segment_count == 0u) {
    doc->root_kind = '{';
  }
  observe_value(doc, path, "", 0, 1, 0);
  return push_container(doc, path, '{');
}

static lonejson_status on_object_end(void *user,
                                     const lonejson_value_path *path,
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
  if (path->segment_count == 0u) {
    doc->root_kind = '[';
  }
  observe_value(doc, path, "", 0, 1, 0);
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
  (void)error;
  doc->allocator->destroy(doc->allocator, doc->val_buf);
  doc->val_buf = NULL;
  doc->val_len = 0u;
  doc->scalar_interested = scalar_path_interested(doc, path);
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_string_chunk(void *user,
                                       const lonejson_value_path *path,
                                       const char *data, size_t len,
                                       lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)path;
  (void)error;
  if (!doc->scalar_interested) {
    return LONEJSON_STATUS_OK;
  }
  return append_buf(doc->allocator, &doc->val_buf, &doc->val_len, data, len)
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_ALLOCATION_FAILED;
}

static lonejson_status on_string_end(void *user,
                                     const lonejson_value_path *path,
                                     lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  if (path->segment_count == 0u) {
    doc->root_kind = 's';
  }
  if (doc->scalar_interested) {
    observe_value(doc, path, doc->val_buf == NULL ? "" : doc->val_buf, 0, 0, 0);
  }
  doc->allocator->destroy(doc->allocator, doc->val_buf);
  doc->val_buf = NULL;
  doc->val_len = 0u;
  doc->scalar_interested = 0;
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
  if (path->segment_count == 0u) {
    doc->root_kind = 'n';
  }
  if (doc->scalar_interested) {
    observe_value(doc, path, doc->val_buf == NULL ? "" : doc->val_buf, 1, 0, 0);
  }
  doc->allocator->destroy(doc->allocator, doc->val_buf);
  doc->val_buf = NULL;
  doc->val_len = 0u;
  doc->scalar_interested = 0;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_boolean(void *user, const lonejson_value_path *path,
                                  int value, lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  if (path->segment_count == 0u) {
    doc->root_kind = 'b';
  }
  observe_value(doc, path, value ? "true" : "false", 0, 0, 0);
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_null(void *user, const lonejson_value_path *path,
                               lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  if (path->segment_count == 0u) {
    doc->root_kind = '0';
  }
  observe_value(doc, path, "", 0, 0, 1);
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
  lql_query_options options;
  lql_query_decision_fn on_decision;
  void *user;
  lql_query_result result;
  lql_status callback_status;
  eval_doc doc;
} query_stream_state;

typedef struct source_reader_adapter {
  lql_read_fn read;
  void *user;
  int error_code;
  lql_uint64 total_read;
} source_reader_adapter;

typedef struct eval_limited_file_reader {
  FILE *file;
  lql_uint64 remaining;
} eval_limited_file_reader;

typedef struct spooled_match_state {
  lql *receiver;
  const lql_selector *selector;
  FILE *out;
  int compact;
  const lql_projection *projection;
  const lql_mutation_plan *mutation_plan;
  int matches_only;
  int expand_arrays;
  lonejson *compact_runtime;
  lql_query_result result;
  lql_error projection_error;
  lql_error mutation_error;
  eval_doc doc;
} spooled_match_state;

typedef struct source_spooled_match_state {
  const lql_selector *selector;
  lql_query_options options;
  lql_query_match_fn on_match;
  void *user;
  lql_query_result result;
  lql_status callback_status;
  eval_doc doc;
} source_spooled_match_state;

static int query_limit_enabled(lql_uint64 limit) { return limit != 0u; }

static int eval_seek_u64(FILE *file, lql_uint64 offset) {
  off_t seek_offset;
  seek_offset = (off_t)offset;
  if (seek_offset < (off_t)0 || (lql_uint64)seek_offset != offset) {
    return 0;
  }
  return fseeko(file, seek_offset, SEEK_SET) == 0;
}

static int eval_file_size_u64(FILE *file, lql_uint64 *out) {
  off_t end;
  if (file == NULL || out == NULL) {
    return 0;
  }
  if (fseeko(file, (off_t)0, SEEK_END) != 0) {
    return 0;
  }
  end = ftello(file);
  if (end < (off_t)0) {
    return 0;
  }
  *out = (lql_uint64)end;
  return (off_t)(*out) == end;
}

static int eval_copy_range(FILE *in, FILE *out, lql_uint64 size) {
  char buf[8192];
  size_t want;
  size_t got;
  while (size != 0u) {
    want = size > (lql_uint64)sizeof(buf) ? sizeof(buf) : (size_t)size;
    got = fread(buf, 1u, want, in);
    if (got == 0u) {
      return 0;
    }
    if (fwrite(buf, 1u, got, out) != got) {
      return 0;
    }
    size -= (lql_uint64)got;
  }
  return 1;
}

static lonejson_read_result
source_reader_read(void *user, unsigned char *buffer, size_t capacity) {
  source_reader_adapter *adapter;
  lql_read_result lql_result;
  lonejson_read_result result;

  result = lonejson_default_read_result();
  adapter = (source_reader_adapter *)user;
  lql_result = adapter->read(adapter->user, buffer, capacity);
  if (lql_result.bytes_read > capacity) {
    adapter->error_code = 1;
    result.error_code = 1;
    return result;
  }
  if (lql_result.error_code != 0) {
    adapter->error_code = lql_result.error_code;
    result.error_code = lql_result.error_code;
    return result;
  }
  result.bytes_read = lql_result.bytes_read;
  adapter->total_read += (lql_uint64)lql_result.bytes_read;
  result.eof = lql_result.eof;
  return result;
}

static void query_finish_file_bytes(lql_query_result *result, FILE *file) {
  off_t pos;
  if (result == NULL || file == NULL || result->stopped_early) {
    return;
  }
  pos = ftello(file);
  if (pos >= (off_t)0 && (off_t)((lql_uint64)pos) == pos) {
    result->bytes_read = (lql_uint64)pos;
  }
}

static void query_finish_source_bytes(lql_query_result *result,
                                      const source_reader_adapter *adapter) {
  if (result == NULL || adapter == NULL || result->stopped_early) {
    return;
  }
  result->bytes_read = adapter->total_read;
}

static lonejson_read_result
eval_limited_file_read(void *user, unsigned char *buffer, size_t capacity) {
  eval_limited_file_reader *reader;
  lonejson_read_result result;
  size_t want;

  result = lonejson_default_read_result();
  reader = (eval_limited_file_reader *)user;
  if (reader->remaining == 0u) {
    result.eof = 1;
    return result;
  }
  want = reader->remaining > (lql_uint64)capacity ? capacity
                                                  : (size_t)reader->remaining;
  result.bytes_read = fread(buffer, 1u, want, reader->file);
  reader->remaining -= (lql_uint64)result.bytes_read;
  if (result.bytes_read != want) {
    result.error_code = 1;
  }
  if (reader->remaining == 0u) {
    result.eof = 1;
  }
  return result;
}

static lql_status eval_project_then_maybe_mutate_spooled(
    lql *self, const lql_projection *projection,
    const lql_mutation_plan *mutation_plan, int matched,
    const lonejson_spooled *spooled, FILE *out, int *out_projected,
    lql_error *error) {
  FILE *projected_file;
  lql_uint64 projected_size;
  lql_status st;

  if (out_projected != NULL) {
    *out_projected = 0;
  }
  projected_file = tmpfile();
  if (projected_file == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to create projection temp file");
    return LQL_STATUS_JSON_ERROR;
  }
  st = lql_project_spooled(self, projection, spooled, projected_file,
                           out_projected, error);
  if (st == LQL_STATUS_OK && out_projected != NULL && *out_projected) {
    if (!eval_file_size_u64(projected_file, &projected_size)) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "failed to size projection temp file");
      st = LQL_STATUS_JSON_ERROR;
    } else if (matched && mutation_plan != NULL) {
      st = self->mutate_file_range_paths(self, mutation_plan, projected_file,
                                         0u, projected_size, out, error);
    } else if (!eval_seek_u64(projected_file, 0u) ||
               !eval_copy_range(projected_file, out, projected_size)) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "failed to write projected candidate");
      st = LQL_STATUS_JSON_ERROR;
    }
  }
  fclose(projected_file);
  return st;
}

static void query_stop(query_stream_state *state,
                       lql_query_stop_reason reason) {
  state->result.stopped_early = 1;
  state->result.stop_reason = reason;
}

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
  if (st == LQL_STATUS_STOP) {
    query_stop(state, LQL_QUERY_STOP_CALLBACK);
    return LONEJSON_CANDIDATE_STOP;
  }
  if (st != LQL_STATUS_OK) {
    state->callback_status = st;
    return LONEJSON_CANDIDATE_ERROR;
  }
  if (query_limit_enabled(state->options.max_matches) &&
      state->result.candidates_matched >= state->options.max_matches) {
    query_stop(state, LQL_QUERY_STOP_MATCH_LIMIT);
    return LONEJSON_CANDIDATE_STOP;
  }
  if (query_limit_enabled(state->options.max_candidates) &&
      state->result.candidates_seen >= state->options.max_candidates) {
    query_stop(state, LQL_QUERY_STOP_CANDIDATE_LIMIT);
    return LONEJSON_CANDIDATE_STOP;
  }
  if (query_limit_enabled(state->options.max_bytes_read) &&
      state->result.bytes_read >= state->options.max_bytes_read) {
    query_stop(state, LQL_QUERY_STOP_BYTE_LIMIT);
    return LONEJSON_CANDIDATE_STOP;
  }
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lonejson_status file_sink(void *user, const void *data, size_t len,
                                 lonejson_error *error) {
  FILE *out;
  (void)error;
  out = (FILE *)user;
  if (len != 0u && fwrite(data, 1u, len, out) != len) {
    return LONEJSON_STATUS_IO_ERROR;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status write_spooled_payload(FILE *out,
                                             const lonejson_spooled *spooled,
                                             int compact, lonejson *runtime,
                                             lonejson_error *error) {
  lonejson_writer writer;
  lonejson_status st;
  int writer_initialized;

  if (!compact) {
    return lonejson_spooled_write_to_sink(spooled, file_sink, out, error);
  }
  writer_initialized = 0;
  st = lonejson_writer_init_sink(runtime, &writer, file_sink, out, error);
  if (st == LONEJSON_STATUS_OK) {
    writer_initialized = 1;
    st = lonejson_writer_json_value_spooled(&writer, spooled, error);
  }
  if (st == LONEJSON_STATUS_OK) {
    st = lonejson_writer_finish(&writer, error);
  }
  if (writer_initialized) {
    lonejson_writer_cleanup(&writer);
  }
  return st;
}

static lonejson_read_result eval_spooled_read(void *user, unsigned char *buffer,
                                              size_t capacity) {
  return lonejson_spooled_read((lonejson_spooled *)user, buffer, capacity);
}

static lonejson_candidate_callback_result
on_spooled_candidate_begin(void *user, const lonejson_candidate_info *candidate,
                           lonejson_error *error);
static lonejson_candidate_callback_result
on_spooled_candidate_end(void *user, const lonejson_candidate_info *candidate,
                         lonejson_error *error);

static lonejson_status write_spooled_array_candidates(
    lql *self, const lql_mutation_plan *mutation_plan,
    const lonejson_spooled *spooled, FILE *out, int compact,
    lonejson *compact_runtime, lonejson_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_spooled cursor;
  spooled_match_state state;
  lonejson_status st;

  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    *error = lj_error;
    return LONEJSON_STATUS_INTERNAL_ERROR;
  }
  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.out = out;
  state.compact = compact;
  state.mutation_plan = mutation_plan;
  state.compact_runtime = compact_runtime;
  state.expand_arrays = 0;
  lql_error_init(&state.projection_error);
  lql_error_init(&state.mutation_error);
  if (!init_doc(&state.doc, self, NULL)) {
    lonejson_free(runtime);
    error->code = LONEJSON_STATUS_ALLOCATION_FAILED;
    strcpy(error->message, "failed to initialize nested array mutation");
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  cursor = *spooled;
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_SPOOLED;
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_spooled_candidate_begin;
  options.candidate_end = on_spooled_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_reader(runtime, eval_spooled_read, &cursor,
                                        &options, &lj_error);
  destroy_doc(&state.doc);
  lonejson_free(runtime);
  if (st != LONEJSON_STATUS_OK) {
    if (state.mutation_error.code != LQL_STATUS_OK) {
      error->code = LONEJSON_STATUS_CALLBACK_FAILED;
      strncpy(error->message, state.mutation_error.message,
              sizeof(error->message) - 1u);
      error->message[sizeof(error->message) - 1u] = '\0';
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    *error = lj_error;
  }
  return st;
}

static lonejson_candidate_callback_result
on_spooled_candidate_begin(void *user, const lonejson_candidate_info *candidate,
                           lonejson_error *error) {
  spooled_match_state *state = (spooled_match_state *)user;
  (void)candidate;
  (void)error;
  reset_doc(&state->doc);
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lonejson_candidate_callback_result
on_source_spooled_candidate_begin(void *user,
                                  const lonejson_candidate_info *candidate,
                                  lonejson_error *error) {
  source_spooled_match_state *state;
  (void)candidate;
  (void)error;
  state = (source_spooled_match_state *)user;
  reset_doc(&state->doc);
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lonejson_candidate_callback_result
on_source_spooled_candidate_end(void *user,
                                const lonejson_candidate_info *candidate,
                                lonejson_error *error) {
  source_spooled_match_state *state;
  lql_query_match match;
  lql_status st;
  int matched;

  state = (source_spooled_match_state *)user;
  matched = state->selector == NULL ||
            state->selector->root.kind == LQL_NODE_ALL ||
            eval_node(&state->selector->root, &state->doc);
  state->result.candidates_seen++;
  state->result.bytes_read =
      (lql_uint64)(candidate->stream_offset + candidate->byte_size);
  if (matched) {
    if (candidate->payload_spool == NULL) {
      reset_doc(&state->doc);
      return LONEJSON_CANDIDATE_ERROR;
    }
    memset(&match, 0, sizeof(match));
    match.decision.matched = 1;
    match.decision.index = (lql_uint64)candidate->index;
    match.decision.offset = (lql_uint64)candidate->stream_offset;
    match.decision.size = (lql_uint64)candidate->byte_size;
    match.payload.kind = LQL_PAYLOAD_SPOOLED;
    match.payload.index = match.decision.index;
    match.payload.offset = match.decision.offset;
    match.payload.size = match.decision.size;
    match.payload.spooled = candidate->payload_spool;
    state->result.candidates_matched++;
    st = state->on_match(state->user, &match);
    if (st == LQL_STATUS_STOP) {
      state->result.stopped_early = 1;
      state->result.stop_reason = LQL_QUERY_STOP_CALLBACK;
      reset_doc(&state->doc);
      return LONEJSON_CANDIDATE_STOP;
    }
    if (st != LQL_STATUS_OK) {
      state->callback_status = st;
      reset_doc(&state->doc);
      return LONEJSON_CANDIDATE_ERROR;
    }
  }
  reset_doc(&state->doc);
  if (query_limit_enabled(state->options.max_matches) &&
      state->result.candidates_matched >= state->options.max_matches) {
    state->result.stopped_early = 1;
    state->result.stop_reason = LQL_QUERY_STOP_MATCH_LIMIT;
    return LONEJSON_CANDIDATE_STOP;
  }
  if (query_limit_enabled(state->options.max_candidates) &&
      state->result.candidates_seen >= state->options.max_candidates) {
    state->result.stopped_early = 1;
    state->result.stop_reason = LQL_QUERY_STOP_CANDIDATE_LIMIT;
    return LONEJSON_CANDIDATE_STOP;
  }
  if (query_limit_enabled(state->options.max_bytes_read) &&
      state->result.bytes_read >= state->options.max_bytes_read) {
    state->result.stopped_early = 1;
    state->result.stop_reason = LQL_QUERY_STOP_BYTE_LIMIT;
    return LONEJSON_CANDIDATE_STOP;
  }
  (void)error;
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lonejson_candidate_callback_result
on_spooled_candidate_end(void *user, const lonejson_candidate_info *candidate,
                         lonejson_error *error) {
  spooled_match_state *state = (spooled_match_state *)user;
  lonejson_status write_status;
  int matched;
  int projected;
  int wrote_output;
  write_status = LONEJSON_STATUS_OK;
  projected = 0;
  wrote_output = 0;
  matched = state->selector == NULL ||
            state->selector->root.kind == LQL_NODE_ALL ||
            eval_node(&state->selector->root, &state->doc);
  if (matched || state->mutation_plan != NULL) {
    if (candidate->payload_spool == NULL) {
      reset_doc(&state->doc);
      return LONEJSON_CANDIDATE_ERROR;
    }
    if (state->mutation_plan != NULL) {
      if (!matched && state->matches_only) {
        state->result.candidates_seen++;
        state->result.bytes_read =
            (lql_uint64)(candidate->stream_offset + candidate->byte_size);
        reset_doc(&state->doc);
        return LONEJSON_CANDIDATE_CONTINUE;
      }
      if (state->projection != NULL) {
        if (eval_project_then_maybe_mutate_spooled(
                state->receiver, state->projection, state->mutation_plan,
                matched, candidate->payload_spool, state->out, &projected,
                &state->mutation_error) != LQL_STATUS_OK) {
          error->code = LONEJSON_STATUS_CALLBACK_FAILED;
          strncpy(error->message, state->mutation_error.message,
                  sizeof(error->message) - 1u);
          error->message[sizeof(error->message) - 1u] = '\0';
          reset_doc(&state->doc);
          return LONEJSON_CANDIDATE_ERROR;
        }
        if (!projected) {
          state->result.candidates_seen++;
          state->result.bytes_read =
              (lql_uint64)(candidate->stream_offset + candidate->byte_size);
          reset_doc(&state->doc);
          return LONEJSON_CANDIDATE_CONTINUE;
        }
        wrote_output = 1;
      } else if (matched) {
        if (state->doc.root_kind == '[' && state->expand_arrays) {
          write_status = write_spooled_array_candidates(
              state->receiver, state->mutation_plan, candidate->payload_spool,
              state->out, state->compact, state->compact_runtime, error);
          if (write_status != LONEJSON_STATUS_OK) {
            reset_doc(&state->doc);
            return LONEJSON_CANDIDATE_ERROR;
          }
        } else if (state->doc.root_kind != '{') {
          write_status = write_spooled_payload(
              state->out, candidate->payload_spool, state->compact,
              state->compact_runtime, error);
          if (write_status != LONEJSON_STATUS_OK) {
            reset_doc(&state->doc);
            return LONEJSON_CANDIDATE_ERROR;
          }
          wrote_output = 1;
        } else if (lql_mutate_spooled_paths(
                       state->receiver, state->mutation_plan,
                       candidate->payload_spool, state->out,
                       &state->mutation_error) != LQL_STATUS_OK) {
          error->code = LONEJSON_STATUS_CALLBACK_FAILED;
          strncpy(error->message, state->mutation_error.message,
                  sizeof(error->message) - 1u);
          error->message[sizeof(error->message) - 1u] = '\0';
          reset_doc(&state->doc);
          return LONEJSON_CANDIDATE_ERROR;
        }
        if (state->doc.root_kind == '{') {
          wrote_output = 1;
        }
      } else {
        write_status = write_spooled_payload(
            state->out, candidate->payload_spool, state->compact,
            state->compact_runtime, error);
        if (write_status != LONEJSON_STATUS_OK) {
          reset_doc(&state->doc);
          return LONEJSON_CANDIDATE_ERROR;
        }
        wrote_output = 1;
      }
    } else if (state->projection != NULL) {
      if (lql_project_spooled(state->receiver, state->projection,
                              candidate->payload_spool, state->out, &projected,
                              &state->projection_error) != LQL_STATUS_OK) {
        error->code = LONEJSON_STATUS_CALLBACK_FAILED;
        strncpy(error->message, state->projection_error.message,
                sizeof(error->message) - 1u);
        error->message[sizeof(error->message) - 1u] = '\0';
        reset_doc(&state->doc);
        return LONEJSON_CANDIDATE_ERROR;
      }
      if (!projected) {
        state->result.candidates_seen++;
        state->result.bytes_read =
            (lql_uint64)(candidate->stream_offset + candidate->byte_size);
        reset_doc(&state->doc);
        return LONEJSON_CANDIDATE_CONTINUE;
      }
      wrote_output = 1;
    } else {
      write_status =
          write_spooled_payload(state->out, candidate->payload_spool,
                                state->compact, state->compact_runtime, error);
      if (write_status != LONEJSON_STATUS_OK) {
        reset_doc(&state->doc);
        return LONEJSON_CANDIDATE_ERROR;
      }
      wrote_output = 1;
    }
    if (wrote_output && fputc('\n', state->out) == EOF) {
      reset_doc(&state->doc);
      return LONEJSON_CANDIDATE_ERROR;
    }
    if (matched) {
      state->result.candidates_matched++;
    }
  }
  state->result.candidates_seen++;
  state->result.bytes_read =
      (lql_uint64)(candidate->stream_offset + candidate->byte_size);
  reset_doc(&state->doc);
  return LONEJSON_CANDIDATE_CONTINUE;
}

LQL_INTERNAL_SYMBOL lql_status
lql_eval_selector(lql *self, const lql_selector *selector, const char *json,
                  size_t json_len, int *out_matched, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_status st;
  eval_doc doc;

  if (!init_doc(&doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&doc);
    return LQL_STATUS_JSON_ERROR;
  }
  init_eval_visitor(&visitor);
  st = lonejson_visit_path_value_buffer(runtime, json, json_len, &visitor, &doc,
                                        &lj_error);
  if (st != LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&doc);
    lonejson_free(runtime);
    return LQL_STATUS_JSON_ERROR;
  }
  *out_matched = selector == NULL || selector->root.kind == LQL_NODE_ALL ||
                 eval_node(&selector->root, &doc);
  destroy_doc(&doc);
  lonejson_free(runtime);
  return LQL_STATUS_OK;
}

static lql_status query_file_decisions_method(
    lql *self, const lql_selector *selector, FILE *file,
    lql_query_decision_fn on_decision, void *user, lql_query_result *out_result,
    lql_error *error) {
  return self->query_file_decisions_with_options(
      self, selector, file, NULL, on_decision, user, out_result, error);
}

static lql_status query_file_decisions_with_options_method(
    lql *self, const lql_selector *selector, FILE *file,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error) {
  if (file == NULL || on_decision == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "file and on_decision are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return lql_eval_query_file_decisions(self, selector, file, options,
                                       on_decision, user, out_result, error);
}

static lql_status query_source_decisions_method(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_query_decision_fn on_decision, void *user, lql_query_result *out_result,
    lql_error *error) {
  return self->query_source_decisions_with_options(self, selector, read,
                                                   read_user, NULL, on_decision,
                                                   user, out_result, error);
}

static lql_status query_source_decisions_with_options_method(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error) {
  if (read == NULL || on_decision == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "read and on_decision are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return lql_eval_query_source_decisions(self, selector, read, read_user,
                                         options, on_decision, user, out_result,
                                         error);
}

static lql_status query_source_spooled_matches_method(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_query_match_fn on_match, void *user, lql_query_result *out_result,
    lql_error *error) {
  return self->query_source_spooled_matches_with_options(
      self, selector, read, read_user, NULL, on_match, user, out_result, error);
}

static lql_status
query_source_spooled_matches_with_options_method(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error) {
  if (read == NULL || on_match == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "read and on_match are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return lql_eval_query_source_spooled_matches(self, selector, read, read_user,
                                               options, on_match, user,
                                               out_result, error);
}

static lql_status
query_file_matches_method(lql *self, const lql_selector *selector, FILE *file,
                            lql_query_match_fn on_match, void *user,
                            lql_query_result *out_result, lql_error *error) {
  return self->query_file_matches_with_options(
      self, selector, file, NULL, on_match, user, out_result, error);
}

static lql_status query_file_matches_with_options_method(
    lql *self, const lql_selector *selector, FILE *file,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error) {
  lql_match_adapter adapter;
  lql_status st;
  if (file == NULL || on_match == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "file and on_match are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  adapter.file = file;
  adapter.on_match = on_match;
  adapter.user = user;
  st = self->query_file_decisions_with_options(self, selector, file, options,
                                               on_match_decision, &adapter,
                                               out_result, error);
  if (st != LQL_STATUS_OK && error != NULL &&
      strcmp(error->message, "query decision callback failed") == 0) {
    lql_set_error(error, st, "query match callback failed");
  }
  return st;
}

static lql_status payload_write_json_method(
    lql *self, const lql_payload *payload, FILE *out, lql_error *error) {
  if (payload == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "payload and output file are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return self->payload_write_json_sink(self, payload, payload_file_write, out,
                                       error);
}

static lql_status payload_write_json_sink_method(
    lql *self, const lql_payload *payload, lql_write_fn write, void *write_user,
    lql_error *error) {
  lql_payload_sink_adapter adapter;
  off_t current;
  lql_status copy_status;
  (void)self;
  if (payload == NULL || write == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "payload and write callback are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (payload->kind != LQL_PAYLOAD_SEEKABLE_RANGE || payload->source == NULL) {
    if (payload->kind == LQL_PAYLOAD_SPOOLED && payload->spooled != NULL) {
      lonejson_error lj_error;
      memset(&adapter, 0, sizeof(adapter));
      adapter.write = write;
      adapter.user = write_user;
      if (lonejson_spooled_write_to_sink(
              (const lonejson_spooled *)payload->spooled, payload_lql_sink,
              &adapter, &lj_error) == LONEJSON_STATUS_OK) {
        return LQL_STATUS_OK;
      }
      if (adapter.status != LQL_STATUS_OK) {
        lql_set_error(error, adapter.status, "payload sink write failed");
        return adapter.status;
      }
      lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
      return LQL_STATUS_JSON_ERROR;
    }
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "payload is not a seekable source range");
    return LQL_STATUS_UNSUPPORTED;
  }
  current = ftello(payload->source);
  if (current < (off_t)0) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "failed to record source position");
    return LQL_STATUS_UNSUPPORTED;
  }
  if (!payload_seek_u64(payload->source, payload->offset)) {
    if (fseeko(payload->source, current, SEEK_SET) != 0) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "failed to write seekable payload range");
      return LQL_STATUS_JSON_ERROR;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to write seekable payload range");
    return LQL_STATUS_JSON_ERROR;
  }
  copy_status =
      copy_range_to_sink(payload->source, payload->size, write, write_user);
  if (fseeko(payload->source, current, SEEK_SET) != 0) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to write seekable payload range");
    return LQL_STATUS_JSON_ERROR;
  }
  if (copy_status != LQL_STATUS_OK) {
    if (copy_status == LQL_STATUS_JSON_ERROR) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "failed to write seekable payload range");
    } else {
      lql_set_error(error, copy_status, "payload sink write failed");
    }
    return copy_status;
  }
  return LQL_STATUS_OK;
}

static lql_status payload_project_json_method(
    lql *self, const lql_payload *payload, const lql_projection *projection,
    FILE *out, int *out_found, lql_error *error) {
  if (out_found != NULL) {
    *out_found = 0;
  }
  if (payload == NULL || projection == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "payload, projection, and output file are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (payload->kind == LQL_PAYLOAD_SEEKABLE_RANGE && payload->source != NULL) {
    return self->project_file_range(self, projection, payload->source,
                                    payload->offset, payload->size, out,
                                    out_found, error);
  }
  if (payload->kind == LQL_PAYLOAD_SPOOLED && payload->spooled != NULL) {
    return lql_project_spooled(self, projection,
                               (const lonejson_spooled *)payload->spooled, out,
                               out_found, error);
  }
  lql_set_error(error, LQL_STATUS_UNSUPPORTED, "payload cannot be projected");
  return LQL_STATUS_UNSUPPORTED;
}

LQL_INTERNAL_SYMBOL lql_status lql_eval_query_file_decisions(
    lql *self, const lql_selector *selector, FILE *file,
    const lql_query_options *query_options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error) {
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
  if (query_options != NULL) {
    state.options = *query_options;
  }
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
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
  query_finish_file_bytes(&state.result, file);
  if (st != LONEJSON_STATUS_OK) {
    destroy_doc(&state.doc);
    lonejson_free(runtime);
    if (out_result != NULL) {
      *out_result = state.result;
    }
    if (state.callback_status != LQL_STATUS_OK) {
      lql_set_error(error, state.callback_status,
                    "query decision callback failed");
      return state.callback_status;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  destroy_doc(&state.doc);
  lonejson_free(runtime);
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL lql_status lql_eval_query_source_decisions(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *query_options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  query_stream_state state;
  source_reader_adapter adapter;

  memset(&state, 0, sizeof(state));
  state.selector = selector;
  state.on_decision = on_decision;
  state.user = user;
  state.callback_status = LQL_STATUS_OK;
  if (query_options != NULL) {
    state.options = *query_options;
  }
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  adapter.read = read;
  adapter.user = read_user;
  adapter.error_code = 0;
  adapter.total_read = 0u;
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_NONE;
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_candidate_begin;
  options.candidate_end = on_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_reader(runtime, source_reader_read, &adapter,
                                        &options, &lj_error);
  query_finish_source_bytes(&state.result, &adapter);
  if (st != LONEJSON_STATUS_OK) {
    destroy_doc(&state.doc);
    lonejson_free(runtime);
    if (out_result != NULL) {
      *out_result = state.result;
    }
    if (state.callback_status != LQL_STATUS_OK) {
      lql_set_error(error, state.callback_status,
                    "query decision callback failed");
      return state.callback_status;
    }
    if (adapter.error_code != 0) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR, "query source reader failed");
      return LQL_STATUS_JSON_ERROR;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  destroy_doc(&state.doc);
  lonejson_free(runtime);
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL lql_status lql_eval_query_source_spooled_matches(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *query_options, lql_query_match_fn on_match,
    void *user, lql_query_result *out_result, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  source_spooled_match_state state;
  source_reader_adapter adapter;

  memset(&state, 0, sizeof(state));
  state.selector = selector;
  state.on_match = on_match;
  state.user = user;
  state.callback_status = LQL_STATUS_OK;
  if (query_options != NULL) {
    state.options = *query_options;
  }
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  adapter.read = read;
  adapter.user = read_user;
  adapter.error_code = 0;
  adapter.total_read = 0u;
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_SPOOLED;
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_source_spooled_candidate_begin;
  options.candidate_end = on_source_spooled_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_reader(runtime, source_reader_read, &adapter,
                                        &options, &lj_error);
  query_finish_source_bytes(&state.result, &adapter);
  if (st != LONEJSON_STATUS_OK) {
    destroy_doc(&state.doc);
    lonejson_free(runtime);
    if (out_result != NULL) {
      *out_result = state.result;
    }
    if (state.callback_status != LQL_STATUS_OK) {
      lql_set_error(error, state.callback_status,
                    "query match callback failed");
      return state.callback_status;
    }
    if (adapter.error_code != 0) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR, "query source reader failed");
      return LQL_STATUS_JSON_ERROR;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  destroy_doc(&state.doc);
  lonejson_free(runtime);
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL lql_status lql_eval_query_file_range_spooled_matches(
    lql *self, const lql_selector *selector, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, int compact, const lql_projection *projection,
    const lql_mutation_plan *mutation_plan, int matches_only,
    lql_query_result *out_result, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  spooled_match_state state;
  eval_limited_file_reader reader;

  if (file == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "input and output files are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (!eval_seek_u64(file, offset)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, "failed to seek input range");
    return LQL_STATUS_JSON_ERROR;
  }
  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.selector = selector;
  state.out = out;
  state.compact = compact;
  state.projection = projection;
  state.mutation_plan = mutation_plan;
  state.matches_only = matches_only;
  state.expand_arrays = 1;
  lql_error_init(&state.projection_error);
  lql_error_init(&state.mutation_error);
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  if (compact) {
    state.compact_runtime = lql_lonejson_new(self, &lj_error);
    if (state.compact_runtime == NULL) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
      lonejson_free(runtime);
      destroy_doc(&state.doc);
      return LQL_STATUS_JSON_ERROR;
    }
  }
  reader.file = file;
  reader.remaining = size;
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_SPOOLED;
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_spooled_candidate_begin;
  options.candidate_end = on_spooled_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_reader(runtime, eval_limited_file_read,
                                        &reader, &options, &lj_error);
  if (state.compact_runtime != NULL) {
    lonejson_free(state.compact_runtime);
  }
  destroy_doc(&state.doc);
  lonejson_free(runtime);
  if (st != LONEJSON_STATUS_OK) {
    if (out_result != NULL) {
      *out_result = state.result;
    }
    if (state.mutation_error.code != LQL_STATUS_OK) {
      lql_set_error(error, state.mutation_error.code,
                    state.mutation_error.message);
      return state.mutation_error.code;
    }
    if (state.projection_error.code != LQL_STATUS_OK) {
      lql_set_error(error, state.projection_error.code,
                    state.projection_error.message);
      return state.projection_error.code;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL lql_status lql_eval_query_file_spooled_matches(
    lql *self, const lql_selector *selector, FILE *file, FILE *out, int compact,
    const lql_projection *projection, const lql_mutation_plan *mutation_plan,
    int matches_only, lql_query_result *out_result, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  spooled_match_state state;

  if (file == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "input and output files are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.selector = selector;
  state.out = out;
  state.compact = compact;
  state.projection = projection;
  state.mutation_plan = mutation_plan;
  state.matches_only = matches_only;
  state.expand_arrays = 1;
  lql_error_init(&state.projection_error);
  lql_error_init(&state.mutation_error);
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  if (compact) {
    state.compact_runtime = lql_lonejson_new(self, &lj_error);
    if (state.compact_runtime == NULL) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
      lonejson_free(runtime);
      destroy_doc(&state.doc);
      return LQL_STATUS_JSON_ERROR;
    }
  }
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_SPOOLED;
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_spooled_candidate_begin;
  options.candidate_end = on_spooled_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_filep(runtime, file, &options, &lj_error);
  if (state.compact_runtime != NULL) {
    lonejson_free(state.compact_runtime);
  }
  destroy_doc(&state.doc);
  lonejson_free(runtime);
  if (st != LONEJSON_STATUS_OK) {
    if (out_result != NULL) {
      *out_result = state.result;
    }
    if (state.mutation_error.code != LQL_STATUS_OK) {
      lql_set_error(error, state.mutation_error.code,
                    state.mutation_error.message);
      return state.mutation_error.code;
    }
    if (state.projection_error.code != LQL_STATUS_OK) {
      lql_set_error(error, state.projection_error.code,
                    state.projection_error.message);
      return state.projection_error.code;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL lql_status lql_eval_query_source_spooled_rewrite(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    FILE *out, int compact, const lql_projection *projection,
    const lql_mutation_plan *mutation_plan, int matches_only,
    lql_query_result *out_result, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  spooled_match_state state;
  source_reader_adapter adapter;

  if (read == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "read and output file are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.selector = selector;
  state.out = out;
  state.compact = compact;
  state.projection = projection;
  state.mutation_plan = mutation_plan;
  state.matches_only = matches_only;
  state.expand_arrays = 1;
  lql_error_init(&state.projection_error);
  lql_error_init(&state.mutation_error);
  memset(&adapter, 0, sizeof(adapter));
  adapter.read = read;
  adapter.user = read_user;
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  if (compact) {
    state.compact_runtime = lql_lonejson_new(self, &lj_error);
    if (state.compact_runtime == NULL) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
      lonejson_free(runtime);
      destroy_doc(&state.doc);
      return LQL_STATUS_JSON_ERROR;
    }
  }
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_SPOOLED;
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_spooled_candidate_begin;
  options.candidate_end = on_spooled_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_reader(runtime, source_reader_read, &adapter,
                                        &options, &lj_error);
  if (state.compact_runtime != NULL) {
    lonejson_free(state.compact_runtime);
  }
  if (out_result != NULL) {
    if (!state.result.stopped_early) {
      state.result.bytes_read = adapter.total_read;
    }
    *out_result = state.result;
  }
  destroy_doc(&state.doc);
  lonejson_free(runtime);
  if (st != LONEJSON_STATUS_OK) {
    if (state.mutation_error.code != LQL_STATUS_OK) {
      lql_set_error(error, state.mutation_error.code,
                    state.mutation_error.message);
      return state.mutation_error.code;
    }
    if (state.projection_error.code != LQL_STATUS_OK) {
      lql_set_error(error, state.projection_error.code,
                    state.projection_error.message);
      return state.projection_error.code;
    }
    if (adapter.error_code != 0) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR, "source read failed");
      return LQL_STATUS_JSON_ERROR;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL void lql_eval_methods_install(lql *ctx) {
  ctx->query_file_decisions = query_file_decisions_method;
  ctx->query_file_decisions_with_options =
      query_file_decisions_with_options_method;
  ctx->query_source_decisions = query_source_decisions_method;
  ctx->query_source_decisions_with_options =
      query_source_decisions_with_options_method;
  ctx->query_file_matches = query_file_matches_method;
  ctx->query_file_matches_with_options = query_file_matches_with_options_method;
  ctx->query_source_spooled_matches = query_source_spooled_matches_method;
  ctx->query_source_spooled_matches_with_options =
      query_source_spooled_matches_with_options_method;
  ctx->payload_write_json = payload_write_json_method;
  ctx->payload_write_json_sink = payload_write_json_sink_method;
  ctx->payload_project_json = payload_project_json_method;
}
