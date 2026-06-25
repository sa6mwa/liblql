#include "lql_internal.h"

#include <ctype.h>
#include <lonejson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct scalar {
  char *path;
  char *value;
  int is_number;
  double number;
} scalar;

typedef struct eval_doc {
  scalar *items;
  size_t count;
  char **path;
  size_t depth;
  char *key_buf;
  size_t key_len;
  char *val_buf;
  size_t val_len;
  int collecting_key;
  int collecting_string;
  int collecting_number;
} eval_doc;

static void free_doc(eval_doc *doc) {
  size_t i;
  for (i = 0u; i < doc->count; ++i) {
    free(doc->items[i].path);
    free(doc->items[i].value);
  }
  free(doc->items);
  for (i = 0u; i < doc->depth; ++i) {
    free(doc->path[i]);
  }
  free(doc->path);
  free(doc->key_buf);
  free(doc->val_buf);
  memset(doc, 0, sizeof(*doc));
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

static int push_path(eval_doc *doc, const char *seg) {
  char **next;
  next = (char **)realloc(doc->path, sizeof(char *) * (doc->depth + 1u));
  if (next == NULL) {
    return 0;
  }
  doc->path = next;
  doc->path[doc->depth] = lql_strdup(seg);
  if (doc->path[doc->depth] == NULL) {
    return 0;
  }
  ++doc->depth;
  return 1;
}

static void pop_path(eval_doc *doc) {
  if (doc->depth > 0u) {
    --doc->depth;
    free(doc->path[doc->depth]);
    doc->path[doc->depth] = NULL;
  }
}

static char *current_pointer(eval_doc *doc) {
  size_t len;
  size_t i;
  char *out;
  char *p;
  len = 1u;
  for (i = 0u; i < doc->depth; ++i) {
    len += strlen(doc->path[i]) + 1u;
  }
  out = (char *)malloc(len + 1u);
  if (out == NULL) {
    return NULL;
  }
  p = out;
  *p++ = '/';
  if (doc->depth == 0u) {
    *p = '\0';
    return out;
  }
  for (i = 0u; i < doc->depth; ++i) {
    size_t n;
    if (i != 0u) {
      *p++ = '/';
    }
    n = strlen(doc->path[i]);
    memcpy(p, doc->path[i], n);
    p += n;
  }
  *p = '\0';
  return out;
}

static lonejson_status add_scalar(eval_doc *doc, const char *value,
                                  int is_number, lonejson_error *error) {
  scalar *next;
  char *path;
  char *copy;
  (void)error;
  path = current_pointer(doc);
  copy = lql_strdup(value);
  if (path == NULL || copy == NULL) {
    free(path);
    free(copy);
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  next = (scalar *)realloc(doc->items, sizeof(scalar) * (doc->count + 1u));
  if (next == NULL) {
    free(path);
    free(copy);
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  doc->items = next;
  doc->items[doc->count].path = path;
  doc->items[doc->count].value = copy;
  doc->items[doc->count].is_number = is_number;
  doc->items[doc->count].number = is_number ? strtod(value, NULL) : 0.0;
  ++doc->count;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_key_begin(void *user, lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  free(doc->key_buf);
  doc->key_buf = NULL;
  doc->key_len = 0u;
  doc->collecting_key = 1;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_key_chunk(void *user, const char *data, size_t len,
                                    lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  return append_buf(&doc->key_buf, &doc->key_len, data, len)
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_ALLOCATION_FAILED;
}

static lonejson_status on_key_end(void *user, lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  doc->collecting_key = 0;
  return push_path(doc, doc->key_buf == NULL ? "" : doc->key_buf)
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_ALLOCATION_FAILED;
}

static lonejson_status on_object_end(void *user, lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  pop_path(doc);
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_string_begin(void *user, lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  free(doc->val_buf);
  doc->val_buf = NULL;
  doc->val_len = 0u;
  doc->collecting_string = 1;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_string_chunk(void *user, const char *data, size_t len,
                                       lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  return append_buf(&doc->val_buf, &doc->val_len, data, len)
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_ALLOCATION_FAILED;
}

static lonejson_status on_string_end(void *user, lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  lonejson_status st;
  doc->collecting_string = 0;
  st = add_scalar(doc, doc->val_buf == NULL ? "" : doc->val_buf, 0, error);
  pop_path(doc);
  return st;
}

static lonejson_status on_number_begin(void *user, lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  free(doc->val_buf);
  doc->val_buf = NULL;
  doc->val_len = 0u;
  doc->collecting_number = 1;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_number_chunk(void *user, const char *data, size_t len,
                                       lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  return append_buf(&doc->val_buf, &doc->val_len, data, len)
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_ALLOCATION_FAILED;
}

static lonejson_status on_number_end(void *user, lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  lonejson_status st;
  doc->collecting_number = 0;
  st = add_scalar(doc, doc->val_buf == NULL ? "" : doc->val_buf, 1, error);
  pop_path(doc);
  return st;
}

static lonejson_status on_boolean(void *user, int value,
                                  lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  lonejson_status st;
  st = add_scalar(doc, value ? "true" : "false", 0, error);
  pop_path(doc);
  return st;
}

static lonejson_status on_null(void *user, lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  lonejson_status st;
  st = add_scalar(doc, "", 0, error);
  pop_path(doc);
  return st;
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

static int path_matches(const char *pattern, const char *path) {
  return pattern != NULL && strcmp(pattern, path) == 0;
}

static int eval_node(const lql_node *node, const eval_doc *doc) {
  size_t i;
  int found;
  const scalar *s;
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
    break;
  }
  found = 0;
  for (i = 0u; i < doc->count; ++i) {
    s = &doc->items[i];
    if (!path_matches(node->term.field, s->path)) {
      continue;
    }
    found = 1;
    switch (node->kind) {
    case LQL_NODE_EQ:
      if (strcmp(s->value, node->term.value == NULL ? "" : node->term.value) ==
          0) {
        return 1;
      }
      break;
    case LQL_NODE_NE:
      if (strcmp(s->value, node->term.value == NULL ? "" : node->term.value) !=
          0) {
        return 1;
      }
      break;
    case LQL_NODE_CONTAINS:
    case LQL_NODE_ICONTAINS:
      if (contains_case(s->value,
                        node->term.value == NULL ? "" : node->term.value,
                        node->kind == LQL_NODE_ICONTAINS)) {
        return 1;
      }
      break;
    case LQL_NODE_PREFIX:
    case LQL_NODE_IPREFIX: {
      size_t n = strlen(node->term.value == NULL ? "" : node->term.value);
      if (strlen(s->value) >= n &&
          (node->kind == LQL_NODE_IPREFIX
               ? ascii_case_equal_prefix(s->value, node->term.value, n)
               : memcmp(s->value, node->term.value, n) == 0)) {
        return 1;
      }
      break;
    }
    case LQL_NODE_RANGE:
      if (s->is_number && node->term.has_number) {
        if ((node->term.range_op == '>' && s->number > node->term.number) ||
            (node->term.range_op == 'G' && s->number >= node->term.number) ||
            (node->term.range_op == '<' && s->number < node->term.number) ||
            (node->term.range_op == 'L' && s->number <= node->term.number)) {
          return 1;
        }
      }
      break;
    case LQL_NODE_EXISTS:
      return 1;
    default:
      break;
    }
  }
  return node->kind == LQL_NODE_EXISTS ? found : 0;
}

static void init_eval_visitor(lonejson_value_visitor *visitor) {
  *visitor = lonejson_default_value_visitor();
  visitor->object_key_begin = on_key_begin;
  visitor->object_key_chunk = on_key_chunk;
  visitor->object_key_end = on_key_end;
  visitor->object_end = on_object_end;
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
  free_doc(&state->doc);
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
  free_doc(&state->doc);
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
  lonejson_value_visitor visitor;
  lonejson_status st;
  eval_doc doc;

  memset(&doc, 0, sizeof(doc));
  runtime = lonejson_new(NULL, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  init_eval_visitor(&visitor);
  st = lonejson_visit_value_buffer(runtime, json, json_len, &visitor, &doc,
                                   &lj_error);
  if (st != LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    free_doc(&doc);
    lonejson_free(runtime);
    return LQL_STATUS_JSON_ERROR;
  }
  *out_matched = eval_node(&selector->root, &doc);
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
  lonejson_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  query_stream_state state;

  memset(&state, 0, sizeof(state));
  state.selector = selector;
  state.on_decision = on_decision;
  state.user = user;
  state.callback_status = LQL_STATUS_OK;
  runtime = lonejson_new(NULL, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_NONE;
  options.visitor = &visitor;
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
