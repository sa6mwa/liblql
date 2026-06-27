#include "lql_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct lql_token_list {
  char **items;
  size_t count;
} lql_token_list;

typedef struct lql_selector_parser {
  lql *receiver;
  lql_allocator *allocator;
} lql_selector_parser;

static char *lql_strndup_local(lql_selector_parser *ctx, const char *src,
                               size_t len) {
  char *out;
  out = (char *)ctx->allocator->alloc(ctx->allocator, len + 1u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, src, len);
  out[len] = '\0';
  return out;
}

static char *trim_dup(lql_selector_parser *ctx, const char *src, size_t len) {
  while (len > 0u && isspace((unsigned char)*src)) {
    ++src;
    --len;
  }
  while (len > 0u && isspace((unsigned char)src[len - 1u])) {
    --len;
  }
  return lql_strndup_local(ctx, src, len);
}

static void token_list_cleanup(lql_selector_parser *ctx, lql_token_list *list) {
  size_t i;
  for (i = 0u; i < list->count; ++i) {
    ctx->allocator->destroy(ctx->allocator, list->items[i]);
  }
  ctx->allocator->destroy(ctx->allocator, list->items);
  list->items = NULL;
  list->count = 0u;
}

static int token_list_push(lql_selector_parser *ctx, lql_token_list *list,
                           char *item) {
  char **next;
  next = (char **)ctx->allocator->realloc(ctx->allocator, list->items,
                                          sizeof(char *) * (list->count + 1u));
  if (next == NULL) {
    return 0;
  }
  list->items = next;
  list->items[list->count++] = item;
  return 1;
}

static int term_any_push(lql_selector_parser *ctx, lql_term *term, char *item) {
  char **next;
  size_t *next_lens;
  size_t len;
  len = strlen(item);
  next = (char **)ctx->allocator->realloc(
      ctx->allocator, term->any, sizeof(char *) * (term->any_count + 1u));
  if (next == NULL) {
    return 0;
  }
  term->any = next;
  next_lens = (size_t *)ctx->allocator->realloc(
      ctx->allocator, term->any_lens, sizeof(size_t) * (term->any_count + 1u));
  if (next_lens == NULL) {
    return 0;
  }
  term->any_lens = next_lens;
  term->any[term->any_count] = item;
  term->any_lens[term->any_count] = len;
  ++term->any_count;
  return 1;
}

static lql_status split_top(lql_selector_parser *ctx, const char *expr,
                            lql_token_list *out, lql_error *error) {
  const char *start;
  size_t depth;
  int quote;
  const char *p;
  char *item;

  memset(out, 0, sizeof(*out));
  start = expr;
  depth = 0u;
  quote = 0;
  for (p = expr; *p != '\0'; ++p) {
    if (quote != 0) {
      if (*p == quote) {
        quote = 0;
      } else if (*p == '\\' && p[1] != '\0') {
        ++p;
      }
      continue;
    }
    if (*p == '"' || *p == '\'') {
      quote = *p;
    } else if (*p == '{') {
      ++depth;
    } else if (*p == '}') {
      if (depth == 0u) {
        lql_set_error(error, LQL_STATUS_PARSE_ERROR, "unexpected selector }");
        return LQL_STATUS_PARSE_ERROR;
      }
      --depth;
    } else if ((*p == ',' || *p == '\n') && depth == 0u) {
      item = trim_dup(ctx, start, (size_t)(p - start));
      if (item == NULL) {
        return LQL_STATUS_NO_MEMORY;
      }
      if (item[0] != '\0' && !token_list_push(ctx, out, item)) {
        ctx->allocator->destroy(ctx->allocator, item);
        return LQL_STATUS_NO_MEMORY;
      }
      if (item[0] == '\0') {
        ctx->allocator->destroy(ctx->allocator, item);
      }
      start = p + 1;
    }
  }
  if (quote != 0 || depth != 0u) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "unterminated selector expression");
    return LQL_STATUS_PARSE_ERROR;
  }
  item = trim_dup(ctx, start, (size_t)(p - start));
  if (item == NULL) {
    return LQL_STATUS_NO_MEMORY;
  }
  if (item[0] != '\0' && !token_list_push(ctx, out, item)) {
    ctx->allocator->destroy(ctx->allocator, item);
    return LQL_STATUS_NO_MEMORY;
  }
  if (item[0] == '\0') {
    ctx->allocator->destroy(ctx->allocator, item);
  }
  return LQL_STATUS_OK;
}

static int selector_assignment_separator(const char *p) {
  const char *q;
  const char *key;

  if (*p == ',' || *p == '\n') {
    return 1;
  }
  if (!isspace((unsigned char)*p)) {
    return 0;
  }
  q = p;
  while (isspace((unsigned char)*q)) {
    ++q;
  }
  key = q;
  if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || *q == '_')) {
    return 0;
  }
  ++q;
  while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
         (*q >= '0' && *q <= '9') || *q == '_' || *q == '.') {
    ++q;
  }
  while (isspace((unsigned char)*q)) {
    ++q;
  }
  return q > key && *q == '=';
}

static lql_status split_assignments(lql_selector_parser *ctx, const char *expr,
                                    lql_token_list *out, lql_error *error) {
  const char *start;
  size_t depth;
  int quote;
  const char *p;
  const char *next;
  char *item;

  memset(out, 0, sizeof(*out));
  start = expr;
  depth = 0u;
  quote = 0;
  for (p = expr; *p != '\0'; ++p) {
    if (quote != 0) {
      if (*p == quote) {
        quote = 0;
      } else if (*p == '\\' && p[1] != '\0') {
        ++p;
      }
      continue;
    }
    if (*p == '"' || *p == '\'') {
      quote = *p;
    } else if (*p == '{') {
      ++depth;
    } else if (*p == '}') {
      if (depth == 0u) {
        lql_set_error(error, LQL_STATUS_PARSE_ERROR, "unexpected selector }");
        return LQL_STATUS_PARSE_ERROR;
      }
      --depth;
    } else if (depth == 0u && selector_assignment_separator(p)) {
      item = trim_dup(ctx, start, (size_t)(p - start));
      if (item == NULL) {
        return LQL_STATUS_NO_MEMORY;
      }
      if (item[0] != '\0' && !token_list_push(ctx, out, item)) {
        ctx->allocator->destroy(ctx->allocator, item);
        return LQL_STATUS_NO_MEMORY;
      }
      if (item[0] == '\0') {
        ctx->allocator->destroy(ctx->allocator, item);
      }
      next = p;
      while (*next == ',' || isspace((unsigned char)*next)) {
        ++next;
      }
      start = next;
      p = next - 1;
    }
  }
  if (quote != 0 || depth != 0u) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "unterminated selector expression");
    return LQL_STATUS_PARSE_ERROR;
  }
  item = trim_dup(ctx, start, (size_t)(p - start));
  if (item == NULL) {
    return LQL_STATUS_NO_MEMORY;
  }
  if (item[0] != '\0' && !token_list_push(ctx, out, item)) {
    ctx->allocator->destroy(ctx->allocator, item);
    return LQL_STATUS_NO_MEMORY;
  }
  if (item[0] == '\0') {
    ctx->allocator->destroy(ctx->allocator, item);
  }
  return LQL_STATUS_OK;
}

static char *unquote(lql_selector_parser *ctx, char *value) {
  size_t len;
  char *out;
  char *w;
  char *r;
  len = strlen(value);
  if (len >= 2u && ((value[0] == '"' && value[len - 1u] == '"') ||
                    (value[0] == '\'' && value[len - 1u] == '\''))) {
    out = lql_strndup_local(ctx, value + 1, len - 2u);
    if (out == NULL) {
      return NULL;
    }
    w = out;
    for (r = out; *r != '\0'; ++r) {
      if (*r == '\\' && r[1] != '\0') {
        ++r;
      }
      *w++ = *r;
    }
    *w = '\0';
    return out;
  }
  return ctx->allocator->strdup(ctx->allocator, value);
}

static int append_text(lql_selector_parser *ctx, char **buf, size_t *len,
                       size_t *cap, const char *text, size_t n) {
  char *next;
  size_t next_cap;
  if (*len + n + 1u > *cap) {
    next_cap = *cap == 0u ? 32u : *cap;
    while (*len + n + 1u > next_cap) {
      next_cap *= 2u;
    }
    next = (char *)ctx->allocator->realloc(ctx->allocator, *buf, next_cap);
    if (next == NULL) {
      return 0;
    }
    *buf = next;
    *cap = next_cap;
  }
  memcpy(*buf + *len, text, n);
  *len += n;
  (*buf)[*len] = '\0';
  return 1;
}

static int raw_segment_is(const char *seg, size_t len, const char *lit) {
  return strlen(lit) == len && memcmp(seg, lit, len) == 0;
}

static char *normalize_field_path(lql_selector_parser *ctx, const char *field) {
  char *out;
  size_t out_len;
  size_t out_cap;
  const char *seg;
  const char *slash;
  size_t len;
  size_t base_len;
  size_t count;
  size_t i;

  if (field == NULL) {
    return NULL;
  }
  if (field[0] != '/') {
    return ctx->allocator->strdup(ctx->allocator, field);
  }
  out = NULL;
  out_len = 0u;
  out_cap = 0u;
  seg = field + 1;
  if (!append_text(ctx, &out, &out_len, &out_cap, "/", 1u)) {
    return NULL;
  }
  while (*seg != '\0') {
    slash = strchr(seg, '/');
    len = slash == NULL ? strlen(seg) : (size_t)(slash - seg);
    base_len = len;
    count = 0u;
    if (!raw_segment_is(seg, len, "") && !raw_segment_is(seg, len, "[]") &&
        !raw_segment_is(seg, len, "*") && !raw_segment_is(seg, len, "**") &&
        !raw_segment_is(seg, len, "...")) {
      while (base_len > 2u && seg[base_len - 2u] == '[' &&
             seg[base_len - 1u] == ']') {
        base_len -= 2u;
        ++count;
      }
    }
    if (out_len > 1u) {
      if (!append_text(ctx, &out, &out_len, &out_cap, "/", 1u)) {
        ctx->allocator->destroy(ctx->allocator, out);
        return NULL;
      }
    }
    if (!append_text(ctx, &out, &out_len, &out_cap, seg,
                     count == 0u ? len : base_len)) {
      ctx->allocator->destroy(ctx->allocator, out);
      return NULL;
    }
    for (i = 0u; i < count; ++i) {
      if (!append_text(ctx, &out, &out_len, &out_cap, "/[]", 3u)) {
        ctx->allocator->destroy(ctx->allocator, out);
        return NULL;
      }
    }
    if (slash == NULL) {
      break;
    }
    seg = slash + 1;
  }
  return out;
}

static int parse_bool_value(const char *value, int *out) {
  if (strcmp(value, "true") == 0 || strcmp(value, "t") == 0) {
    *out = 1;
    return 1;
  }
  if (strcmp(value, "false") == 0 || strcmp(value, "f") == 0) {
    *out = 0;
    return 1;
  }
  return 0;
}

static int key_is_field(const char *key) {
  return strcmp(key, "field") == 0 || strcmp(key, "f") == 0;
}

static int key_is_value(const char *key) {
  return strcmp(key, "value") == 0 || strcmp(key, "v") == 0;
}

static int key_is_any(const char *key) {
  return strcmp(key, "any") == 0 || strcmp(key, "a") == 0;
}

static int key_is_ignore_case(const char *key) {
  return strcmp(key, "ignoreCase") == 0 || strcmp(key, "ignorecase") == 0 ||
         strcmp(key, "ic") == 0;
}

static int key_is_range_bound(const char *key) {
  return strcmp(key, "gt") == 0 || strcmp(key, "gte") == 0 ||
         strcmp(key, "lt") == 0 || strcmp(key, "lte") == 0;
}

static int key_is_after(const char *key) {
  return strcmp(key, "after") == 0 || strcmp(key, "a") == 0;
}

static int key_is_before(const char *key) {
  return strcmp(key, "before") == 0 || strcmp(key, "b") == 0;
}

static int key_is_since(const char *key) { return strcmp(key, "since") == 0; }

static int key_allowed_for_kind(lql_node_kind kind, const char *key) {
  if (key_is_field(key)) {
    return 1;
  }
  switch (kind) {
  case LQL_NODE_EQ:
    return key_is_value(key);
  case LQL_NODE_CONTAINS:
  case LQL_NODE_ICONTAINS:
    return key_is_value(key) || key_is_any(key) || key_is_ignore_case(key);
  case LQL_NODE_PREFIX:
  case LQL_NODE_IPREFIX:
    return key_is_value(key) || key_is_ignore_case(key);
  case LQL_NODE_RANGE:
    return key_is_range_bound(key);
  case LQL_NODE_DATE:
    return key_is_value(key) || key_is_after(key) || key_is_before(key) ||
           key_is_range_bound(key) || key_is_since(key);
  case LQL_NODE_IN:
    return key_is_any(key);
  default:
    return 0;
  }
}

static void dispose_seen_key_values(lql_selector_parser *ctx, char *field,
                                    char *value, char *any, char *ignore_case,
                                    char *gt, char *gte, char *lt, char *lte,
                                    char *after, char *before, char *since) {
  ctx->allocator->destroy(ctx->allocator, field);
  ctx->allocator->destroy(ctx->allocator, value);
  ctx->allocator->destroy(ctx->allocator, any);
  ctx->allocator->destroy(ctx->allocator, ignore_case);
  ctx->allocator->destroy(ctx->allocator, gt);
  ctx->allocator->destroy(ctx->allocator, gte);
  ctx->allocator->destroy(ctx->allocator, lt);
  ctx->allocator->destroy(ctx->allocator, lte);
  ctx->allocator->destroy(ctx->allocator, after);
  ctx->allocator->destroy(ctx->allocator, before);
  ctx->allocator->destroy(ctx->allocator, since);
}

static int remember_key_value(lql_selector_parser *ctx, char **slot,
                              char **raw_value, int *skip, lql_error *error) {
  *skip = 0;
  if (*slot == NULL) {
    *slot = *raw_value;
    *raw_value = NULL;
    return 1;
  }
  if (strcmp(*slot, *raw_value) == 0) {
    ctx->allocator->destroy(ctx->allocator, *raw_value);
    *raw_value = NULL;
    *skip = 1;
    return 1;
  }
  ctx->allocator->destroy(ctx->allocator, *raw_value);
  *raw_value = NULL;
  lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                "selector expression has duplicate key");
  return 0;
}

static lql_node_kind kind_from_name(const char *name) {
  if (strcmp(name, "and") == 0) {
    return LQL_NODE_AND;
  }
  if (strcmp(name, "or") == 0) {
    return LQL_NODE_OR;
  }
  if (strcmp(name, "not") == 0) {
    return LQL_NODE_NOT;
  }
  if (strcmp(name, "eq") == 0) {
    return LQL_NODE_EQ;
  }
  if (strcmp(name, "contains") == 0) {
    return LQL_NODE_CONTAINS;
  }
  if (strcmp(name, "icontains") == 0) {
    return LQL_NODE_ICONTAINS;
  }
  if (strcmp(name, "prefix") == 0) {
    return LQL_NODE_PREFIX;
  }
  if (strcmp(name, "iprefix") == 0) {
    return LQL_NODE_IPREFIX;
  }
  if (strcmp(name, "range") == 0) {
    return LQL_NODE_RANGE;
  }
  if (strcmp(name, "date") == 0) {
    return LQL_NODE_DATE;
  }
  if (strcmp(name, "in") == 0) {
    return LQL_NODE_IN;
  }
  if (strcmp(name, "exists") == 0) {
    return LQL_NODE_EXISTS;
  }
  return LQL_NODE_ALL;
}

static int parse_any_values(lql_selector_parser *ctx, char *decoded,
                            lql_term *term, int reject_surrounding_whitespace,
                            lql_error *error) {
  char *cursor;
  char *bar;
  char *item;
  size_t raw_len;
  const char *raw_start;
  const char *raw_end;

  cursor = decoded;
  while (cursor != NULL) {
    bar = strchr(cursor, '|');
    if (bar != NULL) {
      *bar = '\0';
    }
    raw_len = strlen(cursor);
    raw_start = cursor;
    while (*raw_start != '\0' && isspace((unsigned char)*raw_start)) {
      ++raw_start;
    }
    raw_end = cursor + raw_len;
    while (raw_end > raw_start && isspace((unsigned char)raw_end[-1])) {
      --raw_end;
    }
    if (reject_surrounding_whitespace && raw_end > raw_start &&
        (raw_start != cursor || raw_end != cursor + raw_len)) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector any values must not have surrounding whitespace");
      return 0;
    }
    item = trim_dup(ctx, cursor, strlen(cursor));
    if (item == NULL) {
      return 0;
    }
    if (item[0] != '\0') {
      if (!term_any_push(ctx, term, item)) {
        ctx->allocator->destroy(ctx->allocator, item);
        return 0;
      }
    } else {
      ctx->allocator->destroy(ctx->allocator, item);
    }
    cursor = bar == NULL ? NULL : bar + 1;
  }
  if (term->any_count == 0u) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "selector any requires values");
    return 0;
  }
  return 1;
}

static int parse_number_literal(const char *decoded, double *out) {
  char *end;
  double value;
  value = strtod(decoded, &end);
  if (end == decoded) {
    return 0;
  }
  while (isspace((unsigned char)*end)) {
    ++end;
  }
  if (*end != '\0') {
    return 0;
  }
  *out = value;
  return 1;
}

static int ascii_equal_ignore_case(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
      return 0;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

static int replace_term_text(lql_selector_parser *ctx, char **slot,
                             const char *decoded) {
  char *copy;
  copy = ctx->allocator->strdup(ctx->allocator, decoded);
  if (copy == NULL) {
    return 0;
  }
  ctx->allocator->destroy(ctx->allocator, *slot);
  *slot = copy;
  return 1;
}

static int set_range_bound(lql_selector_parser *ctx, lql_term *term,
                           const char *key, const char *decoded,
                           lql_error *error) {
  lql_temporal temporal;
  double number;
  int is_temporal;

  is_temporal = lql_parse_temporal_literal(decoded, &temporal);
  if (!is_temporal && !parse_number_literal(decoded, &number)) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "range selector bound invalid");
    return 0;
  }
  if (is_temporal) {
    term->range_is_temporal = 1;
    if (strcmp(key, "gt") == 0) {
      if (!replace_term_text(ctx, &term->range_gt_text, decoded)) {
        return 0;
      }
      term->temporal_gt = temporal;
      term->has_temporal_gt = 1;
    } else if (strcmp(key, "gte") == 0) {
      if (!replace_term_text(ctx, &term->range_gte_text, decoded)) {
        return 0;
      }
      term->temporal_gte = temporal;
      term->has_temporal_gte = 1;
    } else if (strcmp(key, "lt") == 0) {
      if (!replace_term_text(ctx, &term->range_lt_text, decoded)) {
        return 0;
      }
      term->temporal_lt = temporal;
      term->has_temporal_lt = 1;
    } else {
      if (!replace_term_text(ctx, &term->range_lte_text, decoded)) {
        return 0;
      }
      term->temporal_lte = temporal;
      term->has_temporal_lte = 1;
    }
    return 1;
  }
  if (strcmp(key, "gt") == 0) {
    term->range_gt = number;
    term->has_range_gt = 1;
  } else if (strcmp(key, "gte") == 0) {
    term->range_gte = number;
    term->has_range_gte = 1;
  } else if (strcmp(key, "lt") == 0) {
    term->range_lt = number;
    term->has_range_lt = 1;
  } else {
    term->range_lte = number;
    term->has_range_lte = 1;
  }
  return 1;
}

static int set_date_bound(lql_selector_parser *ctx, lql_term *term,
                          const char *slot, const char *decoded,
                          lql_error *error) {
  lql_temporal temporal;
  if (strcmp(slot, "since") == 0) {
    if (!replace_term_text(ctx, &term->date_since_text, decoded)) {
      return 0;
    }
    if (ascii_equal_ignore_case(decoded, "now")) {
      term->since_macro = LQL_SINCE_NOW;
      return 1;
    }
    if (ascii_equal_ignore_case(decoded, "today")) {
      term->since_macro = LQL_SINCE_TODAY;
      return 1;
    }
    if (ascii_equal_ignore_case(decoded, "yesterday")) {
      term->since_macro = LQL_SINCE_YESTERDAY;
      return 1;
    }
  }
  if (!lql_parse_temporal_literal(decoded, &temporal)) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "date selector bound invalid");
    return 0;
  }
  if (strcmp(slot, "value") == 0) {
    if (!replace_term_text(ctx, &term->date_value_text, decoded)) {
      return 0;
    }
    term->temporal_eq = temporal;
    term->has_temporal_eq = 1;
  } else if (strcmp(slot, "since") == 0) {
    term->temporal_gte = temporal;
    term->has_temporal_gte = 1;
  } else if (strcmp(slot, "after") == 0) {
    if (!replace_term_text(ctx, &term->date_after_text, decoded)) {
      return 0;
    }
    term->temporal_gt = temporal;
    term->has_temporal_gt = 1;
  } else if (strcmp(slot, "before") == 0) {
    if (!replace_term_text(ctx, &term->date_before_text, decoded)) {
      return 0;
    }
    term->temporal_lt = temporal;
    term->has_temporal_lt = 1;
  } else if (strcmp(slot, "gt") == 0) {
    if (!replace_term_text(ctx, &term->date_gt_text, decoded)) {
      return 0;
    }
    term->temporal_gt = temporal;
    term->has_temporal_gt = 1;
  } else if (strcmp(slot, "gte") == 0) {
    if (!replace_term_text(ctx, &term->date_gte_text, decoded)) {
      return 0;
    }
    term->temporal_gte = temporal;
    term->has_temporal_gte = 1;
  } else if (strcmp(slot, "lt") == 0) {
    if (!replace_term_text(ctx, &term->date_lt_text, decoded)) {
      return 0;
    }
    term->temporal_lt = temporal;
    term->has_temporal_lt = 1;
  } else if (strcmp(slot, "lte") == 0) {
    if (!replace_term_text(ctx, &term->date_lte_text, decoded)) {
      return 0;
    }
    term->temporal_lte = temporal;
    term->has_temporal_lte = 1;
  }
  return 1;
}

static lql_status parse_key_values(lql_selector_parser *ctx, char *body,
                                   lql_node_kind kind, lql_term *term,
                                   lql_error *error) {
  lql_token_list parts;
  size_t i;
  lql_status st;
  char *eq;
  char *key;
  char *val;
  char *decoded;
  char *normalized;
  char *raw_value;
  char *key_end;
  char *seen_field;
  char *seen_value;
  char *seen_any;
  char *seen_ignore_case;
  char *seen_gt;
  char *seen_gte;
  char *seen_lt;
  char *seen_lte;
  char *seen_after;
  char *seen_before;
  char *seen_since;
  char **seen_slot;
  const char *date_slot;
  int skip_duplicate;
  int value_had_leading_space;

  memset(term, 0, sizeof(*term));
  seen_field = NULL;
  seen_value = NULL;
  seen_any = NULL;
  seen_ignore_case = NULL;
  seen_gt = NULL;
  seen_gte = NULL;
  seen_lt = NULL;
  seen_lte = NULL;
  seen_after = NULL;
  seen_before = NULL;
  seen_since = NULL;
  st = LQL_STATUS_NO_MEMORY;
  st = split_assignments(ctx, body, &parts, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  for (i = 0u; i < parts.count; ++i) {
    eq = strchr(parts.items[i], '=');
    if (eq == NULL) {
      token_list_cleanup(ctx, &parts);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector term requires key=value");
      return LQL_STATUS_PARSE_ERROR;
    }
    *eq = '\0';
    key = parts.items[i];
    val = eq + 1;
    while (isspace((unsigned char)*key)) {
      ++key;
    }
    key_end = key + strlen(key);
    while (key_end > key && isspace((unsigned char)key_end[-1])) {
      --key_end;
      *key_end = '\0';
    }
    value_had_leading_space = isspace((unsigned char)*val) ? 1 : 0;
    while (isspace((unsigned char)*val)) {
      ++val;
    }
    if (!key_allowed_for_kind(kind, key)) {
      st = LQL_STATUS_PARSE_ERROR;
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector operator does not support key");
      goto fail;
    }
    raw_value = trim_dup(ctx, val, strlen(val));
    if (raw_value == NULL) {
      goto fail;
    }
    if (kind == LQL_NODE_IN && key_is_any(key) && value_had_leading_space &&
        raw_value[0] != '\0') {
      ctx->allocator->destroy(ctx->allocator, raw_value);
      st = LQL_STATUS_PARSE_ERROR;
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector any values must not have surrounding whitespace");
      goto fail;
    }
    seen_slot = NULL;
    if (key_is_field(key)) {
      seen_slot = &seen_field;
    } else if (key_is_value(key)) {
      seen_slot = &seen_value;
    } else if (kind == LQL_NODE_DATE && key_is_after(key)) {
      seen_slot = &seen_after;
    } else if (kind == LQL_NODE_DATE && key_is_before(key)) {
      seen_slot = &seen_before;
    } else if (kind == LQL_NODE_DATE && key_is_since(key)) {
      seen_slot = &seen_since;
    } else if (key_is_any(key)) {
      seen_slot = &seen_any;
    } else if (key_is_ignore_case(key)) {
      seen_slot = &seen_ignore_case;
    } else if (strcmp(key, "gt") == 0) {
      seen_slot = &seen_gt;
    } else if (strcmp(key, "gte") == 0) {
      seen_slot = &seen_gte;
    } else if (strcmp(key, "lt") == 0) {
      seen_slot = &seen_lt;
    } else if (strcmp(key, "lte") == 0) {
      seen_slot = &seen_lte;
    } else if (key_is_after(key)) {
      seen_slot = &seen_after;
    } else if (key_is_before(key)) {
      seen_slot = &seen_before;
    } else if (key_is_since(key)) {
      seen_slot = &seen_since;
    }
    if (seen_slot == NULL) {
      ctx->allocator->destroy(ctx->allocator, raw_value);
      st = LQL_STATUS_PARSE_ERROR;
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector operator does not support key");
      goto fail;
    }
    if (!remember_key_value(ctx, seen_slot, &raw_value, &skip_duplicate,
                            error)) {
      st = LQL_STATUS_PARSE_ERROR;
      goto fail;
    }
    if (skip_duplicate) {
      continue;
    }
    decoded = unquote(ctx, *seen_slot);
    if (decoded == NULL) {
      goto fail;
    }
    if (key_is_field(key)) {
      normalized = normalize_field_path(ctx, decoded);
      ctx->allocator->destroy(ctx->allocator, decoded);
      if (normalized == NULL) {
        goto fail;
      }
      ctx->allocator->destroy(ctx->allocator, term->field);
      term->field = normalized;
    } else if (kind == LQL_NODE_DATE && key_is_value(key)) {
      if (!set_date_bound(ctx, term, "value", decoded, error)) {
        ctx->allocator->destroy(ctx->allocator, decoded);
        st = LQL_STATUS_PARSE_ERROR;
        goto fail;
      }
      ctx->allocator->destroy(ctx->allocator, decoded);
    } else if (key_is_value(key)) {
      ctx->allocator->destroy(ctx->allocator, term->value);
      term->value = decoded;
      term->value_set = 1;
    } else if (kind == LQL_NODE_DATE &&
               (key_is_after(key) || key_is_before(key) || key_is_since(key))) {
      date_slot = key_is_after(key)    ? "after"
                  : key_is_before(key) ? "before"
                                       : "since";
      if (!set_date_bound(ctx, term, date_slot, decoded, error)) {
        ctx->allocator->destroy(ctx->allocator, decoded);
        st = LQL_STATUS_PARSE_ERROR;
        goto fail;
      }
      ctx->allocator->destroy(ctx->allocator, decoded);
    } else if (key_is_any(key)) {
      if (!parse_any_values(ctx, decoded, term, kind == LQL_NODE_IN, error)) {
        ctx->allocator->destroy(ctx->allocator, decoded);
        st = error != NULL && error->code != LQL_STATUS_OK
                 ? error->code
                 : LQL_STATUS_PARSE_ERROR;
        goto fail;
      }
      ctx->allocator->destroy(ctx->allocator, decoded);
    } else if (key_is_ignore_case(key)) {
      if (!parse_bool_value(decoded, &term->ignore_case)) {
        ctx->allocator->destroy(ctx->allocator, decoded);
        st = LQL_STATUS_PARSE_ERROR;
        lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                      "selector ignoreCase must be true/false/t/f");
        goto fail;
      }
      ctx->allocator->destroy(ctx->allocator, decoded);
    } else if (key_is_range_bound(key)) {
      if (kind == LQL_NODE_DATE) {
        if (!set_date_bound(ctx, term, key, decoded, error)) {
          ctx->allocator->destroy(ctx->allocator, decoded);
          st = LQL_STATUS_PARSE_ERROR;
          goto fail;
        }
      } else if (!set_range_bound(ctx, term, key, decoded, error)) {
        ctx->allocator->destroy(ctx->allocator, decoded);
        st = LQL_STATUS_PARSE_ERROR;
        goto fail;
      }
      ctx->allocator->destroy(ctx->allocator, decoded);
    }
  }
  token_list_cleanup(ctx, &parts);
  if (term->field == NULL) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "selector field required");
    goto fail_after_tokens;
  }
  if (kind == LQL_NODE_DATE && seen_since != NULL &&
      (seen_value != NULL || seen_after != NULL || seen_before != NULL ||
       seen_gt != NULL || seen_gte != NULL || seen_lt != NULL ||
       seen_lte != NULL)) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector since cannot be combined with other bounds");
    goto fail_after_tokens;
  }
  if (kind == LQL_NODE_DATE && seen_after != NULL && seen_gt != NULL) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector cannot combine after and gt");
    goto fail_after_tokens;
  }
  if (kind == LQL_NODE_DATE && seen_before != NULL && seen_lt != NULL) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector cannot combine before and lt");
    goto fail_after_tokens;
  }
  if (term->any_count != 0u) {
    if (kind != LQL_NODE_CONTAINS && kind != LQL_NODE_ICONTAINS &&
        kind != LQL_NODE_IN) {
      st = LQL_STATUS_PARSE_ERROR;
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector operator does not support any");
      goto fail_after_tokens;
    }
    if (kind != LQL_NODE_IN && (term->value_set || term->value != NULL)) {
      st = LQL_STATUS_PARSE_ERROR;
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector cannot set both value and any");
      goto fail_after_tokens;
    }
  }
  if (kind == LQL_NODE_IN && term->any_count == 0u) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "in selector requires any values");
    goto fail_after_tokens;
  }
  if (kind == LQL_NODE_RANGE && !term->has_range_gt && !term->has_range_gte &&
      !term->has_range_lt && !term->has_range_lte && !term->has_temporal_gt &&
      !term->has_temporal_gte && !term->has_temporal_lt &&
      !term->has_temporal_lte) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "range selector requires at least one bound");
    goto fail_after_tokens;
  }
  if (kind == LQL_NODE_RANGE &&
      (term->has_range_gt || term->has_range_gte || term->has_range_lt ||
       term->has_range_lte) &&
      (term->has_temporal_gt || term->has_temporal_gte ||
       term->has_temporal_lt || term->has_temporal_lte)) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "range selector cannot mix numeric and datetime bounds");
    goto fail_after_tokens;
  }
  if (kind == LQL_NODE_DATE && !term->has_temporal_eq &&
      !term->has_temporal_gt && !term->has_temporal_gte &&
      !term->has_temporal_lt && !term->has_temporal_lte &&
      term->since_macro == LQL_SINCE_NONE) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector requires at least one bound");
    goto fail_after_tokens;
  }
  dispose_seen_key_values(ctx, seen_field, seen_value, seen_any,
                          seen_ignore_case, seen_gt, seen_gte, seen_lt,
                          seen_lte, seen_after, seen_before, seen_since);
  return LQL_STATUS_OK;

fail:
  token_list_cleanup(ctx, &parts);
fail_after_tokens:
  dispose_seen_key_values(ctx, seen_field, seen_value, seen_any,
                          seen_ignore_case, seen_gt, seen_gte, seen_lt,
                          seen_lte, seen_after, seen_before, seen_since);
  return st;
}

static int string_term_is_match_all_alias(lql_node_kind kind,
                                          const lql_term *term) {
  int empty_value;
  if (kind != LQL_NODE_CONTAINS && kind != LQL_NODE_ICONTAINS &&
      kind != LQL_NODE_PREFIX && kind != LQL_NODE_IPREFIX) {
    return 0;
  }
  if (term->field == NULL || term->any_count != 0u) {
    return 0;
  }
  empty_value =
      (!term->value_set && term->value == NULL) ||
      (term->value_set && term->value != NULL && term->value[0] == '\0');
  if (!empty_value) {
    return 0;
  }
  if (strcmp(term->field, "/") == 0) {
    return 1;
  }
  if ((kind == LQL_NODE_CONTAINS || kind == LQL_NODE_ICONTAINS) &&
      (strcmp(term->field, "/*") == 0 || strcmp(term->field, "/...") == 0)) {
    return 1;
  }
  return 0;
}

static lql_status parse_exists_body(lql_selector_parser *ctx, const char *body,
                                    lql_term *term, lql_error *error) {
  lql_token_list parts;
  char *decoded;
  char *normalized;
  lql_status st;

  memset(term, 0, sizeof(*term));
  st = split_top(ctx, body, &parts, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  if (parts.count != 1u || strchr(parts.items[0], '=') != NULL) {
    token_list_cleanup(ctx, &parts);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "exists selector requires exactly one path");
    return LQL_STATUS_PARSE_ERROR;
  }
  decoded = unquote(ctx, parts.items[0]);
  if (decoded == NULL) {
    token_list_cleanup(ctx, &parts);
    return LQL_STATUS_NO_MEMORY;
  }
  if (decoded[0] == '\0') {
    ctx->allocator->destroy(ctx->allocator, decoded);
    token_list_cleanup(ctx, &parts);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "exists selector requires exactly one path");
    return LQL_STATUS_PARSE_ERROR;
  }
  normalized = normalize_field_path(ctx, decoded);
  ctx->allocator->destroy(ctx->allocator, decoded);
  token_list_cleanup(ctx, &parts);
  if (normalized == NULL) {
    return LQL_STATUS_NO_MEMORY;
  }
  term->field = normalized;
  return LQL_STATUS_OK;
}

static lql_status parse_one(lql_selector_parser *ctx, const char *expr,
                            lql_node *out, lql_error *error) {
  char *copy;
  char *body;
  char *close;
  char *dot;
  char *op;
  char *value;
  char *name;
  char *raw_field;
  char *raw_value;
  lql_node child;
  lql_status st;

  memset(out, 0, sizeof(*out));
  copy = ctx->allocator->strdup(ctx->allocator, expr);
  if (copy == NULL) {
    return LQL_STATUS_NO_MEMORY;
  }
  if (strcmp(copy, "/") == 0 || strcmp(copy, ".") == 0 ||
      strcmp(copy, "{}") == 0) {
    out->kind = LQL_NODE_ALL;
    ctx->allocator->destroy(ctx->allocator, copy);
    return LQL_STATUS_OK;
  }
  if (strncmp(copy, "and.", 4u) == 0 || strncmp(copy, "or.", 3u) == 0 ||
      strncmp(copy, "not.", 4u) == 0) {
    dot = strchr(copy, '.');
    name = copy;
    *dot = '\0';
    st = parse_one(ctx, dot + 1, &child, error);
    if (st != LQL_STATUS_OK) {
      ctx->allocator->destroy(ctx->allocator, copy);
      return st;
    }
    out->kind = strcmp(name, "and") == 0  ? LQL_NODE_AND
                : strcmp(name, "or") == 0 ? LQL_NODE_OR
                                          : LQL_NODE_NOT;
    out->children = (lql_node *)ctx->allocator->calloc(ctx->allocator, 1u,
                                                       sizeof(lql_node));
    if (out->children == NULL) {
      lql_node_cleanup(ctx->receiver, &child);
      ctx->allocator->destroy(ctx->allocator, copy);
      return LQL_STATUS_NO_MEMORY;
    }
    out->children[0] = child;
    out->child_count = 1u;
    ctx->allocator->destroy(ctx->allocator, copy);
    return LQL_STATUS_OK;
  }

  op = strstr(copy, "!=");
  if (op == NULL) {
    op = strstr(copy, ">=");
  }
  if (op == NULL) {
    op = strstr(copy, "<=");
  }
  if (op == NULL) {
    op = strchr(copy, '=');
  }
  if (op == NULL) {
    op = strchr(copy, '>');
  }
  if (op == NULL) {
    op = strchr(copy, '<');
  }
  if (copy[0] == '/' && op != NULL) {
    int op2;
    char op0;
    const char *bound_key;
    lql_node child;
    op2 = (op[1] == '=' || op[0] == '!') ? 1 : 0;
    op0 = op[0];
    value = op + 1 + (size_t)op2;
    *op = '\0';
    raw_field = trim_dup(ctx, copy, strlen(copy));
    raw_value = trim_dup(ctx, value, strlen(value));
    if (raw_field == NULL || raw_value == NULL) {
      ctx->allocator->destroy(ctx->allocator, raw_field);
      ctx->allocator->destroy(ctx->allocator, raw_value);
      ctx->allocator->destroy(ctx->allocator, copy);
      return LQL_STATUS_NO_MEMORY;
    }
    out->term.field = normalize_field_path(ctx, raw_field);
    out->term.value = unquote(ctx, raw_value);
    ctx->allocator->destroy(ctx->allocator, raw_field);
    ctx->allocator->destroy(ctx->allocator, raw_value);
    out->term.value_set = 1;
    if (out->term.field == NULL || out->term.value == NULL) {
      ctx->allocator->destroy(ctx->allocator, copy);
      return LQL_STATUS_NO_MEMORY;
    }
    if (op0 == '!') {
      memset(&child, 0, sizeof(child));
      child.kind = LQL_NODE_EQ;
      child.term = out->term;
      memset(&out->term, 0, sizeof(out->term));
      out->children = (lql_node *)ctx->allocator->calloc(ctx->allocator, 1u,
                                                         sizeof(lql_node));
      if (out->children == NULL) {
        lql_node_cleanup(ctx->receiver, &child);
        ctx->allocator->destroy(ctx->allocator, copy);
        return LQL_STATUS_NO_MEMORY;
      }
      out->kind = LQL_NODE_NOT;
      out->children[0] = child;
      out->child_count = 1u;
    } else if (strchr("><", op0) != NULL || op2) {
      if (op0 == '>') {
        bound_key = op2 ? "gte" : "gt";
      } else if (op0 == '<') {
        bound_key = op2 ? "lte" : "lt";
      } else {
        bound_key = NULL;
      }
      if (bound_key != NULL) {
        if (!set_range_bound(ctx, &out->term, bound_key, out->term.value,
                             error)) {
          lql_node_cleanup(ctx->receiver, out);
          ctx->allocator->destroy(ctx->allocator, copy);
          return error != NULL && error->code != LQL_STATUS_OK
                     ? error->code
                     : LQL_STATUS_PARSE_ERROR;
        }
        out->kind = LQL_NODE_RANGE;
      } else {
        out->kind = LQL_NODE_EQ;
      }
    } else {
      out->kind = LQL_NODE_EQ;
    }
    ctx->allocator->destroy(ctx->allocator, copy);
    return LQL_STATUS_OK;
  }

  body = strchr(copy, '{');
  close = body == NULL ? NULL : strrchr(body, '}');
  if (body != NULL && close != NULL && close[1] == '\0') {
    *body = '\0';
    *close = '\0';
    out->kind = kind_from_name(copy);
    if (out->kind == LQL_NODE_ALL) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR, "unknown selector operator");
      ctx->allocator->destroy(ctx->allocator, copy);
      return LQL_STATUS_PARSE_ERROR;
    }
    if (out->kind == LQL_NODE_EXISTS) {
      st = parse_exists_body(ctx, body + 1, &out->term, error);
      ctx->allocator->destroy(ctx->allocator, copy);
      return st;
    }
    st = parse_key_values(ctx, body + 1, out->kind, &out->term, error);
    if (st != LQL_STATUS_OK) {
      ctx->allocator->destroy(ctx->allocator, copy);
      return st;
    }
    if (string_term_is_match_all_alias(out->kind, &out->term)) {
      lql_node_cleanup(ctx->receiver, out);
      out->kind = LQL_NODE_ALL;
    }
    ctx->allocator->destroy(ctx->allocator, copy);
    return LQL_STATUS_OK;
  }

  lql_set_error(error, LQL_STATUS_PARSE_ERROR, "invalid selector expression");
  ctx->allocator->destroy(ctx->allocator, copy);
  return LQL_STATUS_PARSE_ERROR;
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

static void assign_hit_indexes(lql_node *node, size_t *next) {
  size_t i;
  if (node == NULL) {
    return;
  }
  if (node_is_term(node)) {
    node->hit_index = *next;
    ++*next;
    return;
  }
  for (i = 0u; i < node->child_count; ++i) {
    assign_hit_indexes(&node->children[i], next);
  }
}

static int append_node(lql_selector_parser *ctx, lql_node *parent,
                       lql_node *child) {
  lql_node *next;
  next = (lql_node *)ctx->allocator->realloc(ctx->allocator, parent->children,
                                             sizeof(lql_node) *
                                                 (parent->child_count + 1u));
  if (next == NULL) {
    return 0;
  }
  parent->children = next;
  parent->children[parent->child_count] = *child;
  ++parent->child_count;
  memset(child, 0, sizeof(*child));
  return 1;
}

typedef enum selector_json_frame_kind {
  SELECTOR_JSON_FRAME_NODE = 0,
  SELECTOR_JSON_FRAME_CHILD_ARRAY = 1,
  SELECTOR_JSON_FRAME_TERM = 2,
  SELECTOR_JSON_FRAME_ANY_ARRAY = 3
} selector_json_frame_kind;

typedef struct selector_json_frame {
  selector_json_frame_kind kind;
  lql_node *node;
  char *pending_key;
  int key_count;
  int seen_field;
  int seen_value;
  int seen_any;
  int seen_ignore_case;
  int seen_gt;
  int seen_gte;
  int seen_lt;
  int seen_lte;
  int seen_after;
  int seen_before;
  int seen_since;
} selector_json_frame;

typedef struct selector_json_state {
  lql_selector_parser parser;
  lql_error *error;
  lql_status status;
  lql_selector *selector;
  selector_json_frame *frames;
  size_t frame_count;
  char *text;
  size_t text_len;
  size_t text_cap;
} selector_json_state;

static void selector_json_set_lj_error(lonejson_error *error,
                                       lonejson_status status,
                                       const char *message) {
  size_t len;
  if (error == NULL) {
    return;
  }
  error->code = status;
  error->message[0] = '\0';
  if (message == NULL) {
    return;
  }
  len = strlen(message);
  if (len >= sizeof(error->message)) {
    len = sizeof(error->message) - 1u;
  }
  memcpy(error->message, message, len);
  error->message[len] = '\0';
}

static lonejson_status selector_json_fail(selector_json_state *state,
                                          lonejson_error *error,
                                          lql_status status,
                                          const char *message) {
  lonejson_status lj_status;
  if (state->status == LQL_STATUS_OK) {
    state->status = status;
    lql_set_error(state->error, status, message);
  }
  lj_status = status == LQL_STATUS_NO_MEMORY
                  ? LONEJSON_STATUS_ALLOCATION_FAILED
                  : LONEJSON_STATUS_CALLBACK_FAILED;
  selector_json_set_lj_error(error, lj_status, message);
  return lj_status;
}

static selector_json_frame *
selector_json_top(selector_json_state *state) {
  if (state->frame_count == 0u) {
    return NULL;
  }
  return &state->frames[state->frame_count - 1u];
}

static void selector_json_frame_cleanup(selector_json_state *state,
                                        selector_json_frame *frame) {
  if (frame != NULL) {
    state->parser.allocator->destroy(state->parser.allocator,
                                     frame->pending_key);
    frame->pending_key = NULL;
  }
}

static int selector_json_push_frame(selector_json_state *state,
                                    selector_json_frame_kind kind,
                                    lql_node *node) {
  selector_json_frame *next;
  next = (selector_json_frame *)state->parser.allocator->realloc(
      state->parser.allocator, state->frames,
      sizeof(selector_json_frame) * (state->frame_count + 1u));
  if (next == NULL) {
    return 0;
  }
  state->frames = next;
  memset(&state->frames[state->frame_count], 0,
         sizeof(state->frames[state->frame_count]));
  state->frames[state->frame_count].kind = kind;
  state->frames[state->frame_count].node = node;
  ++state->frame_count;
  return 1;
}

static void selector_json_pop_frame(selector_json_state *state) {
  if (state->frame_count == 0u) {
    return;
  }
  selector_json_frame_cleanup(state, &state->frames[state->frame_count - 1u]);
  --state->frame_count;
}

static int selector_json_reset_text(selector_json_state *state) {
  state->text_len = 0u;
  if (state->text != NULL) {
    state->text[0] = '\0';
    return 1;
  }
  state->text = (char *)state->parser.allocator->alloc(state->parser.allocator,
                                                       1u);
  if (state->text == NULL) {
    return 0;
  }
  state->text_cap = 1u;
  state->text[0] = '\0';
  return 1;
}

static int selector_json_append_text(selector_json_state *state,
                                     const char *data, size_t len) {
  return append_text(&state->parser, &state->text, &state->text_len,
                     &state->text_cap, data, len);
}

static lql_node_kind selector_json_kind_from_key(const char *key) {
  return kind_from_name(key);
}

static int selector_json_kind_is_valid_operator(lql_node_kind kind) {
  switch (kind) {
  case LQL_NODE_AND:
  case LQL_NODE_OR:
  case LQL_NODE_NOT:
  case LQL_NODE_EQ:
  case LQL_NODE_CONTAINS:
  case LQL_NODE_ICONTAINS:
  case LQL_NODE_PREFIX:
  case LQL_NODE_IPREFIX:
  case LQL_NODE_RANGE:
  case LQL_NODE_DATE:
  case LQL_NODE_IN:
  case LQL_NODE_EXISTS:
    return 1;
  case LQL_NODE_ALL:
  case LQL_NODE_NE:
    return 0;
  }
  return 0;
}

static int selector_json_mark_seen(selector_json_frame *frame,
                                   const char *key) {
  int *slot;
  slot = NULL;
  if (key_is_field(key)) {
    slot = &frame->seen_field;
  } else if (key_is_value(key)) {
    slot = &frame->seen_value;
  } else if (key_is_ignore_case(key)) {
    slot = &frame->seen_ignore_case;
  } else if (strcmp(key, "gt") == 0) {
    slot = &frame->seen_gt;
  } else if (strcmp(key, "gte") == 0) {
    slot = &frame->seen_gte;
  } else if (strcmp(key, "lt") == 0) {
    slot = &frame->seen_lt;
  } else if (strcmp(key, "lte") == 0) {
    slot = &frame->seen_lte;
  } else if (key_is_after(key) && frame->node != NULL &&
             frame->node->kind == LQL_NODE_DATE) {
    slot = &frame->seen_after;
  } else if (key_is_before(key) && frame->node != NULL &&
             frame->node->kind == LQL_NODE_DATE) {
    slot = &frame->seen_before;
  } else if (key_is_since(key)) {
    slot = &frame->seen_since;
  } else if (key_is_any(key)) {
    slot = &frame->seen_any;
  }
  if (slot == NULL) {
    return 1;
  }
  if (*slot) {
    return 0;
  }
  *slot = 1;
  return 1;
}

static int selector_json_store_field(selector_json_state *state,
                                     lql_node *node, const char *value) {
  char *normalized;
  normalized = normalize_field_path(&state->parser, value);
  if (normalized == NULL) {
    return 0;
  }
  state->parser.allocator->destroy(state->parser.allocator, node->term.field);
  node->term.field = normalized;
  return 1;
}

static int selector_json_store_value(selector_json_state *state,
                                     lql_node *node, const char *value) {
  char *copy;
  copy = state->parser.allocator->strdup(state->parser.allocator, value);
  if (copy == NULL) {
    return 0;
  }
  state->parser.allocator->destroy(state->parser.allocator, node->term.value);
  node->term.value = copy;
  node->term.value_set = 1;
  return 1;
}

static int selector_json_push_any(selector_json_state *state, lql_node *node,
                                  const char *value) {
  char *copy;
  copy = state->parser.allocator->strdup(state->parser.allocator, value);
  if (copy == NULL) {
    return 0;
  }
  if (!term_any_push(&state->parser, &node->term, copy)) {
    state->parser.allocator->destroy(state->parser.allocator, copy);
    return 0;
  }
  return 1;
}

static lql_status selector_json_validate_term(selector_json_state *state,
                                              selector_json_frame *frame) {
  lql_node *node;
  node = frame->node;
  if (node == NULL) {
    return LQL_STATUS_PARSE_ERROR;
  }
  if (node->term.field == NULL) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "selector field required");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (node->kind == LQL_NODE_IN && node->term.any_count == 0u) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "in selector requires any values");
    return LQL_STATUS_PARSE_ERROR;
  }
  if ((node->kind == LQL_NODE_CONTAINS ||
       node->kind == LQL_NODE_ICONTAINS) &&
      node->term.any_count != 0u &&
      (node->term.value_set || node->term.value != NULL)) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "selector cannot set both value and any");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (node->kind == LQL_NODE_RANGE &&
      !node->term.has_range_gt && !node->term.has_range_gte &&
      !node->term.has_range_lt && !node->term.has_range_lte &&
      !node->term.has_temporal_gt && !node->term.has_temporal_gte &&
      !node->term.has_temporal_lt && !node->term.has_temporal_lte) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "range selector requires at least one bound");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (node->kind == LQL_NODE_RANGE &&
      (node->term.has_range_gt || node->term.has_range_gte ||
       node->term.has_range_lt || node->term.has_range_lte) &&
      (node->term.has_temporal_gt || node->term.has_temporal_gte ||
       node->term.has_temporal_lt || node->term.has_temporal_lte)) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "range selector cannot mix numeric and datetime bounds");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (node->kind == LQL_NODE_DATE && frame->seen_since &&
      (frame->seen_value || frame->seen_after || frame->seen_before ||
       frame->seen_gt || frame->seen_gte || frame->seen_lt ||
       frame->seen_lte)) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "date selector since cannot be combined with other bounds");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (node->kind == LQL_NODE_DATE && frame->seen_after && frame->seen_gt) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "date selector cannot combine after and gt");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (node->kind == LQL_NODE_DATE && frame->seen_before && frame->seen_lt) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "date selector cannot combine before and lt");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (node->kind == LQL_NODE_DATE && !node->term.has_temporal_eq &&
      !node->term.has_temporal_gt && !node->term.has_temporal_gte &&
      !node->term.has_temporal_lt && !node->term.has_temporal_lte &&
      node->term.since_macro == LQL_SINCE_NONE) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "date selector requires at least one bound");
    return LQL_STATUS_PARSE_ERROR;
  }
  return LQL_STATUS_OK;
}

static lonejson_status selector_json_consume_key(selector_json_state *state,
                                                 selector_json_frame *frame,
                                                 lonejson_error *error) {
  state->parser.allocator->destroy(state->parser.allocator,
                                   frame->pending_key);
  frame->pending_key =
      state->parser.allocator->strdup(state->parser.allocator, state->text);
  if (frame->pending_key == NULL) {
    return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                              "out of memory");
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status selector_json_begin_key(void *user,
                                               lonejson_error *error) {
  selector_json_state *state;
  state = (selector_json_state *)user;
  if (!selector_json_reset_text(state)) {
    return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                              "out of memory");
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status selector_json_key_chunk(void *user, const char *data,
                                               size_t len,
                                               lonejson_error *error) {
  selector_json_state *state;
  state = (selector_json_state *)user;
  if (!selector_json_append_text(state, data, len)) {
    return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                              "out of memory");
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status selector_json_end_key(void *user,
                                             lonejson_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  lql_node_kind kind;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL || frame->kind == SELECTOR_JSON_FRAME_CHILD_ARRAY ||
      frame->kind == SELECTOR_JSON_FRAME_ANY_ARRAY) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON object key");
  }
  if (frame->kind == SELECTOR_JSON_FRAME_NODE) {
    kind = selector_json_kind_from_key(state->text);
    if (!selector_json_kind_is_valid_operator(kind)) {
      return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                                "unknown selector JSON operator");
    }
    if (frame->key_count != 0) {
      return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                                "selector JSON node has multiple operators");
    }
    if (frame->node != NULL) {
      frame->node->kind = kind;
    }
    frame->key_count = 1;
  } else if (frame->kind == SELECTOR_JSON_FRAME_TERM) {
    if (frame->node == NULL ||
        !key_allowed_for_kind(frame->node->kind, state->text)) {
      return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                                "selector operator does not support key");
    }
    if (!selector_json_mark_seen(frame, state->text)) {
      return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                                "selector expression has duplicate key");
    }
  }
  return selector_json_consume_key(state, frame, error);
}

static lql_node *selector_json_append_child(selector_json_state *state,
                                            lql_node *parent) {
  lql_node child;
  memset(&child, 0, sizeof(child));
  if (!append_node(&state->parser, parent, &child)) {
    return NULL;
  }
  return &parent->children[parent->child_count - 1u];
}

static lonejson_status selector_json_object_begin(void *user,
                                                  lonejson_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  lql_node *child;
  lql_node_kind kind;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL) {
    if (state->selector == NULL) {
      return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                                "selector JSON root required");
    }
    if (!selector_json_push_frame(state, SELECTOR_JSON_FRAME_NODE,
                                  &state->selector->root)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LONEJSON_STATUS_OK;
  }
  if (frame->kind == SELECTOR_JSON_FRAME_CHILD_ARRAY) {
    child = selector_json_append_child(state, frame->node);
    if (child == NULL) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    if (!selector_json_push_frame(state, SELECTOR_JSON_FRAME_NODE, child)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LONEJSON_STATUS_OK;
  }
  if (frame->kind != SELECTOR_JSON_FRAME_NODE ||
      frame->pending_key == NULL || frame->node == NULL) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON object");
  }
  kind = frame->node->kind;
  if (kind == LQL_NODE_NOT) {
    child = selector_json_append_child(state, frame->node);
    if (child == NULL) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    state->parser.allocator->destroy(state->parser.allocator,
                                     frame->pending_key);
    frame->pending_key = NULL;
    if (!selector_json_push_frame(state, SELECTOR_JSON_FRAME_NODE, child)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LONEJSON_STATUS_OK;
  }
  if (node_is_term(frame->node) && kind != LQL_NODE_EXISTS) {
    state->parser.allocator->destroy(state->parser.allocator,
                                     frame->pending_key);
    frame->pending_key = NULL;
    if (!selector_json_push_frame(state, SELECTOR_JSON_FRAME_TERM,
                                  frame->node)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LONEJSON_STATUS_OK;
  }
  return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                            "unexpected selector JSON object");
}

static lonejson_status selector_json_object_end(void *user,
                                                lonejson_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  lql_status st;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL ||
      (frame->kind != SELECTOR_JSON_FRAME_NODE &&
       frame->kind != SELECTOR_JSON_FRAME_TERM)) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON object end");
  }
  if (frame->pending_key != NULL) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "selector JSON key has no value");
  }
  if (frame->kind == SELECTOR_JSON_FRAME_NODE) {
    if (frame->key_count == 0 && frame->node != NULL) {
      frame->node->kind = LQL_NODE_ALL;
    }
    if (frame->node != NULL && frame->node->kind == LQL_NODE_NOT &&
        frame->node->child_count != 1u) {
      return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                                "not selector requires one child");
    }
  } else {
    st = selector_json_validate_term(state, frame);
    if (st != LQL_STATUS_OK) {
      return selector_json_fail(
          state, error, st,
          state->error != NULL && state->error->message[0] != '\0'
              ? state->error->message
              : "invalid selector JSON term");
    }
    if (string_term_is_match_all_alias(frame->node->kind,
                                       &frame->node->term)) {
      lql_node_cleanup(state->parser.receiver, frame->node);
      frame->node->kind = LQL_NODE_ALL;
    }
  }
  selector_json_pop_frame(state);
  return LONEJSON_STATUS_OK;
}

static lonejson_status selector_json_array_begin(void *user,
                                                 lonejson_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL || frame->pending_key == NULL || frame->node == NULL) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON array");
  }
  if (frame->kind == SELECTOR_JSON_FRAME_NODE &&
      (frame->node->kind == LQL_NODE_AND || frame->node->kind == LQL_NODE_OR)) {
    state->parser.allocator->destroy(state->parser.allocator,
                                     frame->pending_key);
    frame->pending_key = NULL;
    if (!selector_json_push_frame(state, SELECTOR_JSON_FRAME_CHILD_ARRAY,
                                  frame->node)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LONEJSON_STATUS_OK;
  }
  if (frame->kind == SELECTOR_JSON_FRAME_TERM && key_is_any(frame->pending_key)) {
    state->parser.allocator->destroy(state->parser.allocator,
                                     frame->pending_key);
    frame->pending_key = NULL;
    if (!selector_json_push_frame(state, SELECTOR_JSON_FRAME_ANY_ARRAY,
                                  frame->node)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LONEJSON_STATUS_OK;
  }
  return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                            "unexpected selector JSON array");
}

static lonejson_status selector_json_array_end(void *user,
                                               lonejson_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL ||
      (frame->kind != SELECTOR_JSON_FRAME_CHILD_ARRAY &&
       frame->kind != SELECTOR_JSON_FRAME_ANY_ARRAY)) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON array end");
  }
  selector_json_pop_frame(state);
  return LONEJSON_STATUS_OK;
}

static lonejson_status selector_json_string_begin(void *user,
                                                  lonejson_error *error) {
  selector_json_state *state;
  state = (selector_json_state *)user;
  if (!selector_json_reset_text(state)) {
    return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                              "out of memory");
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status selector_json_string_chunk(void *user, const char *data,
                                                  size_t len,
                                                  lonejson_error *error) {
  selector_json_state *state;
  state = (selector_json_state *)user;
  if (!selector_json_append_text(state, data, len)) {
    return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                              "out of memory");
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status selector_json_store_term_string(
    selector_json_state *state, selector_json_frame *frame,
    lonejson_error *error) {
  const char *key;
  lql_node *node;
  int ok;
  key = frame->pending_key;
  node = frame->node;
  ok = 1;
  if (key_is_field(key)) {
    ok = selector_json_store_field(state, node, state->text);
  } else if (node->kind == LQL_NODE_DATE && key_is_value(key)) {
    ok = set_date_bound(&state->parser, &node->term, "value", state->text,
                        state->error);
  } else if (key_is_value(key)) {
    ok = selector_json_store_value(state, node, state->text);
  } else if (node->kind == LQL_NODE_DATE &&
             (key_is_after(key) || key_is_before(key) || key_is_since(key))) {
    ok = set_date_bound(&state->parser, &node->term,
                        key_is_after(key)    ? "after"
                        : key_is_before(key) ? "before"
                                             : "since",
                        state->text, state->error);
  } else if (key_is_any(key)) {
    ok = parse_any_values(&state->parser, state->text, &node->term,
                          node->kind == LQL_NODE_IN, state->error);
  } else if (key_is_ignore_case(key)) {
    ok = parse_bool_value(state->text, &node->term.ignore_case);
    if (!ok) {
      lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                    "selector ignoreCase must be true/false/t/f");
    }
  } else if (key_is_range_bound(key)) {
    if (node->kind == LQL_NODE_DATE) {
      ok = set_date_bound(&state->parser, &node->term, key, state->text,
                          state->error);
    } else {
      ok = set_range_bound(&state->parser, &node->term, key, state->text,
                           state->error);
    }
  }
  if (!ok) {
    return selector_json_fail(
        state, error,
        state->error != NULL && state->error->code != LQL_STATUS_OK
            ? state->error->code
            : LQL_STATUS_PARSE_ERROR,
        state->error != NULL && state->error->message[0] != '\0'
            ? state->error->message
            : "invalid selector JSON term");
  }
  state->parser.allocator->destroy(state->parser.allocator,
                                   frame->pending_key);
  frame->pending_key = NULL;
  return LONEJSON_STATUS_OK;
}

static lonejson_status selector_json_string_end(void *user,
                                                lonejson_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON string");
  }
  if (frame->kind == SELECTOR_JSON_FRAME_NODE &&
      frame->pending_key != NULL && frame->node != NULL &&
      frame->node->kind == LQL_NODE_EXISTS) {
    if (state->text[0] == '\0') {
      return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                                "exists selector requires exactly one path");
    }
    if (!selector_json_store_field(state, frame->node, state->text)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    state->parser.allocator->destroy(state->parser.allocator,
                                     frame->pending_key);
    frame->pending_key = NULL;
    return LONEJSON_STATUS_OK;
  }
  if (frame->kind == SELECTOR_JSON_FRAME_TERM && frame->pending_key != NULL) {
    return selector_json_store_term_string(state, frame, error);
  }
  if (frame->kind == SELECTOR_JSON_FRAME_ANY_ARRAY && frame->node != NULL) {
    if (!selector_json_push_any(state, frame->node, state->text)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LONEJSON_STATUS_OK;
  }
  return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                            "unexpected selector JSON string");
}

static lonejson_status selector_json_number_begin(void *user,
                                                  lonejson_error *error) {
  return selector_json_string_begin(user, error);
}

static lonejson_status selector_json_number_chunk(void *user, const char *data,
                                                  size_t len,
                                                  lonejson_error *error) {
  return selector_json_string_chunk(user, data, len, error);
}

static lonejson_status selector_json_number_end(void *user,
                                                lonejson_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  int ok;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL || frame->kind != SELECTOR_JSON_FRAME_TERM ||
      frame->pending_key == NULL || frame->node == NULL ||
      !key_is_range_bound(frame->pending_key) ||
      frame->node->kind != LQL_NODE_RANGE) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON number");
  }
  ok = set_range_bound(&state->parser, &frame->node->term,
                       frame->pending_key, state->text, state->error);
  if (!ok) {
    return selector_json_fail(
        state, error,
        state->error != NULL && state->error->code != LQL_STATUS_OK
            ? state->error->code
            : LQL_STATUS_PARSE_ERROR,
        state->error != NULL && state->error->message[0] != '\0'
            ? state->error->message
            : "range selector bound invalid");
  }
  state->parser.allocator->destroy(state->parser.allocator,
                                   frame->pending_key);
  frame->pending_key = NULL;
  return LONEJSON_STATUS_OK;
}

static lonejson_status selector_json_boolean(void *user, int value,
                                             lonejson_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL || frame->kind != SELECTOR_JSON_FRAME_TERM ||
      frame->pending_key == NULL || frame->node == NULL ||
      !key_is_ignore_case(frame->pending_key)) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON boolean");
  }
  frame->node->term.ignore_case = value ? 1 : 0;
  state->parser.allocator->destroy(state->parser.allocator,
                                   frame->pending_key);
  frame->pending_key = NULL;
  return LONEJSON_STATUS_OK;
}

static lonejson_status selector_json_null(void *user, lonejson_error *error) {
  selector_json_state *state;
  state = (selector_json_state *)user;
  return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                            "selector JSON null is unsupported");
}

static void selector_json_state_cleanup(selector_json_state *state) {
  size_t i;
  if (state == NULL) {
    return;
  }
  for (i = 0u; i < state->frame_count; ++i) {
    selector_json_frame_cleanup(state, &state->frames[i]);
  }
  state->parser.allocator->destroy(state->parser.allocator, state->frames);
  state->parser.allocator->destroy(state->parser.allocator, state->text);
  state->frames = NULL;
  state->frame_count = 0u;
  state->text = NULL;
  state->text_len = 0u;
  state->text_cap = 0u;
}

static void selector_json_visitor_init(lonejson_value_visitor *visitor) {
  *visitor = lonejson_default_value_visitor();
  visitor->object_begin = selector_json_object_begin;
  visitor->object_end = selector_json_object_end;
  visitor->object_key_begin = selector_json_begin_key;
  visitor->object_key_chunk = selector_json_key_chunk;
  visitor->object_key_end = selector_json_end_key;
  visitor->array_begin = selector_json_array_begin;
  visitor->array_end = selector_json_array_end;
  visitor->string_begin = selector_json_string_begin;
  visitor->string_chunk = selector_json_string_chunk;
  visitor->string_end = selector_json_string_end;
  visitor->number_begin = selector_json_number_begin;
  visitor->number_chunk = selector_json_number_chunk;
  visitor->number_end = selector_json_number_end;
  visitor->boolean_value = selector_json_boolean;
  visitor->null_value = selector_json_null;
}

LQL_INTERNAL_SYMBOL lql_status lql_parse_selector_json_internal(
    lql *self, const void *json, size_t json_len, lql_selector **out,
    lql_error *error) {
  lql_allocator *allocator;
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_status lj_status;
  lonejson_value_visitor visitor;
  selector_json_state state;
  lql_status st;

  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "out selector required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = NULL;
  if (json == NULL && json_len != 0u) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "selector JSON required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector parser receiver required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&state, 0, sizeof(state));
  state.parser.receiver = self;
  state.parser.allocator = allocator;
  state.error = error;
  state.status = LQL_STATUS_OK;
  state.selector =
      (lql_selector *)allocator->calloc(allocator, 1u, sizeof(*state.selector));
  if (state.selector == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    allocator->destroy(allocator, state.selector);
    lql_set_error(error, LQL_STATUS_NO_MEMORY, lj_error.message);
    return LQL_STATUS_NO_MEMORY;
  }
  selector_json_visitor_init(&visitor);
  lj_status = lonejson_visit_value_buffer(runtime, json, json_len, &visitor,
                                          &state, &lj_error);
  lonejson_free(runtime);
  st = LQL_STATUS_OK;
  if (lj_status != LONEJSON_STATUS_OK) {
    st = state.status == LQL_STATUS_OK ? LQL_STATUS_JSON_ERROR : state.status;
    if (error != NULL && error->message[0] == '\0') {
      lql_set_error(error, st,
                    lj_error.message[0] == '\0'
                        ? "selector JSON parse failed"
                        : lj_error.message);
    }
  } else if (state.frame_count != 0u) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, st, "selector JSON ended inside a value");
  }
  selector_json_state_cleanup(&state);
  if (st != LQL_STATUS_OK) {
    lql_node_cleanup(self, &state.selector->root);
    allocator->destroy(allocator, state.selector);
    return st;
  }
  state.selector->hit_count = 0u;
  assign_hit_indexes(&state.selector->root, &state.selector->hit_count);
  *out = state.selector;
  return LQL_STATUS_OK;
}

typedef struct indexed_group {
  int indexed;
  lql_node_kind wrapper;
  char *index;
  lql_node node;
  lql_node or_group;
  struct indexed_group *groups;
  size_t group_count;
} indexed_group;

static void indexed_groups_cleanup(lql_selector_parser *ctx,
                                   indexed_group *groups, size_t count) {
  size_t i;
  if (groups == NULL) {
    return;
  }
  for (i = 0u; i < count; ++i) {
    ctx->allocator->destroy(ctx->allocator, groups[i].index);
    lql_node_cleanup(ctx->receiver, &groups[i].node);
    lql_node_cleanup(ctx->receiver, &groups[i].or_group);
    indexed_groups_cleanup(ctx, groups[i].groups, groups[i].group_count);
  }
  ctx->allocator->destroy(ctx->allocator, groups);
}

static lql_status finalize_indexed_group(lql_selector_parser *ctx,
                                         indexed_group *group,
                                         int root_or_mode);

static int parse_indexed_wrapper(lql_selector_parser *ctx, const char *token,
                                 lql_node_kind *wrapper, char **out_index,
                                 const char **out_rest) {
  const char *p;
  const char *idx;
  size_t len;
  char *copy;
  if (strncmp(token, "and.", 4u) == 0) {
    *wrapper = LQL_NODE_AND;
    p = token + 4;
  } else if (strncmp(token, "or.", 3u) == 0) {
    *wrapper = LQL_NODE_OR;
    p = token + 3;
  } else {
    return 0;
  }
  idx = p;
  while (*p >= '0' && *p <= '9') {
    ++p;
  }
  if (p == idx || *p != '.') {
    return 0;
  }
  len = (size_t)(p - idx);
  copy = (char *)ctx->allocator->alloc(ctx->allocator, len + 1u);
  if (copy == NULL) {
    return -1;
  }
  memcpy(copy, idx, len);
  copy[len] = '\0';
  *out_index = copy;
  *out_rest = p + 1;
  return 1;
}

static indexed_group *find_indexed_group(indexed_group *groups, size_t count,
                                         lql_node_kind wrapper,
                                         const char *index) {
  size_t i;
  for (i = 0u; i < count; ++i) {
    if (groups[i].wrapper == wrapper && strcmp(groups[i].index, index) == 0) {
      return &groups[i];
    }
  }
  return NULL;
}

static int parse_simple_wrapper(const char *token, lql_node_kind *wrapper,
                                const char **out_rest) {
  if (strncmp(token, "and.", 4u) == 0) {
    *wrapper = LQL_NODE_AND;
    *out_rest = token + 4;
    return 1;
  }
  if (strncmp(token, "or.", 3u) == 0) {
    *wrapper = LQL_NODE_OR;
    *out_rest = token + 3;
    return 1;
  }
  if (strncmp(token, "not.", 4u) == 0) {
    *wrapper = LQL_NODE_NOT;
    *out_rest = token + 4;
    return 1;
  }
  return 0;
}

static int token_contains_indexed_wrapper(lql_selector_parser *ctx,
                                          const char *token, int *oom) {
  lql_node_kind wrapper;
  char *index;
  const char *rest;
  int st;
  index = NULL;
  rest = NULL;
  st = parse_indexed_wrapper(ctx, token, &wrapper, &index, &rest);
  if (st < 0) {
    *oom = 1;
    return 0;
  }
  ctx->allocator->destroy(ctx->allocator, index);
  if (st > 0) {
    return 1;
  }
  if (parse_simple_wrapper(token, &wrapper, &rest)) {
    return token_contains_indexed_wrapper(ctx, rest, oom);
  }
  return 0;
}

static int node_conflicts_with_child(const lql_node *node,
                                     const lql_node *child) {
  size_t i;
  const lql_node *current;
  if (node == NULL || child == NULL) {
    return 0;
  }
  if (!node_is_term(child) && child->kind != LQL_NODE_NOT) {
    return 0;
  }
  for (i = 0u; i < node->child_count; ++i) {
    current = &node->children[i];
    if (current->kind == child->kind) {
      return 1;
    }
  }
  return 0;
}

static indexed_group *ensure_indexed_group(lql_selector_parser *ctx,
                                           indexed_group **groups,
                                           size_t *count, lql_node_kind wrapper,
                                           char **index) {
  indexed_group *group;
  indexed_group *next;
  group = find_indexed_group(*groups, *count, wrapper, *index);
  if (group == NULL) {
    next = (indexed_group *)ctx->allocator->realloc(
        ctx->allocator, *groups, sizeof(indexed_group) * (*count + 1u));
    if (next == NULL) {
      return 0;
    }
    *groups = next;
    group = &(*groups)[*count];
    memset(group, 0, sizeof(*group));
    group->wrapper = wrapper;
    group->index = *index;
    *index = NULL;
    group->indexed = 1;
    group->node.kind = LQL_NODE_AND;
    ++*count;
  }
  return group;
}

static lql_status append_plain_group_node(lql_selector_parser *ctx,
                                          indexed_group *group, lql_node *child,
                                          lql_error *error) {
  if (child->kind == LQL_NODE_OR && child->child_count == 1u) {
    if (group->or_group.kind == LQL_NODE_ALL) {
      group->or_group.kind = LQL_NODE_OR;
    }
    if (group->indexed &&
        node_conflicts_with_child(&group->or_group, &child->children[0])) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector expression has conflicting indexed clauses");
      return LQL_STATUS_PARSE_ERROR;
    }
    if (!append_node(ctx, &group->or_group, &child->children[0])) {
      return LQL_STATUS_NO_MEMORY;
    }
    ctx->allocator->destroy(ctx->allocator, child->children);
    child->children = NULL;
    child->child_count = 0u;
    return LQL_STATUS_OK;
  }
  if (group->indexed && node_conflicts_with_child(&group->node, child)) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "selector expression has conflicting indexed clauses");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (!append_node(ctx, &group->node, child)) {
    return LQL_STATUS_NO_MEMORY;
  }
  return LQL_STATUS_OK;
}

static lql_status append_token_to_group(lql_selector_parser *ctx,
                                        indexed_group *group, const char *token,
                                        lql_error *error) {
  lql_node_kind wrapper;
  indexed_group *child_group;
  const char *rest;
  char *index;
  lql_node child;
  lql_status st;
  int wrapper_status;

  index = NULL;
  rest = NULL;
  wrapper_status = parse_indexed_wrapper(ctx, token, &wrapper, &index, &rest);
  if (wrapper_status < 0) {
    return LQL_STATUS_NO_MEMORY;
  }
  if (wrapper_status > 0) {
    child_group = ensure_indexed_group(ctx, &group->groups, &group->group_count,
                                       wrapper, &index);
    ctx->allocator->destroy(ctx->allocator, index);
    if (child_group == NULL) {
      return LQL_STATUS_NO_MEMORY;
    }
    return append_token_to_group(ctx, child_group, rest, error);
  }
  if (parse_simple_wrapper(token, &wrapper, &rest)) {
    indexed_group wrapped;
    memset(&wrapped, 0, sizeof(wrapped));
    wrapped.node.kind = wrapper;
    st = append_token_to_group(ctx, &wrapped, rest, error);
    if (st == LQL_STATUS_OK) {
      st = finalize_indexed_group(ctx, &wrapped, wrapper == LQL_NODE_OR);
    }
    if (st == LQL_STATUS_OK) {
      st = append_plain_group_node(ctx, group, &wrapped.node, error);
    }
    lql_node_cleanup(ctx->receiver, &wrapped.node);
    lql_node_cleanup(ctx->receiver, &wrapped.or_group);
    indexed_groups_cleanup(ctx, wrapped.groups, wrapped.group_count);
    return st;
  }

  memset(&child, 0, sizeof(child));
  st = parse_one(ctx, token, &child, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  st = append_plain_group_node(ctx, group, &child, error);
  if (st != LQL_STATUS_OK) {
    lql_node_cleanup(ctx->receiver, &child);
  }
  return st;
}

static lql_status finalize_indexed_group(lql_selector_parser *ctx,
                                         indexed_group *group,
                                         int root_or_mode) {
  size_t i;
  lql_status st;

  if (group->or_group.kind == LQL_NODE_ALL) {
    group->or_group.kind = LQL_NODE_OR;
  }
  for (i = 0u; i < group->group_count; ++i) {
    st = finalize_indexed_group(ctx, &group->groups[i], 0);
    if (st != LQL_STATUS_OK) {
      return st;
    }
    if (!root_or_mode && group->groups[i].wrapper == LQL_NODE_OR) {
      if (!append_node(ctx, &group->or_group, &group->groups[i].node)) {
        return LQL_STATUS_NO_MEMORY;
      }
    } else if (!append_node(ctx, &group->node, &group->groups[i].node)) {
      return LQL_STATUS_NO_MEMORY;
    }
  }
  if (group->or_group.child_count != 0u) {
    if (!append_node(ctx, &group->node, &group->or_group)) {
      return LQL_STATUS_NO_MEMORY;
    }
  }
  indexed_groups_cleanup(ctx, group->groups, group->group_count);
  group->groups = NULL;
  group->group_count = 0u;
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL lql_status lql_parse_selector_internal(lql *self,
                                                           const char *expr,
                                                           int or_mode,
                                                           lql_selector **out,
                                                           lql_error *error) {
  lql_selector_parser ctx_storage;
  lql_selector_parser *ctx;
  lql_allocator *allocator;
  lql_token_list tokens;
  lql_selector *selector;
  indexed_group root_group;
  lql_node_kind wrapper;
  char *index;
  const char *rest;
  int wrapper_status;
  int needs_group;
  int wrapper_probe_oom;
  size_t i;
  lql_status st;

  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "out selector required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector parser receiver required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  ctx_storage.receiver = self;
  ctx_storage.allocator = allocator;
  ctx = &ctx_storage;
  *out = NULL;
  if (expr == NULL || *expr == '\0') {
    selector = (lql_selector *)ctx->allocator->calloc(ctx->allocator, 1u,
                                                      sizeof(*selector));
    if (selector == NULL) {
      return LQL_STATUS_NO_MEMORY;
    }
    selector->root.kind = LQL_NODE_ALL;
    *out = selector;
    return LQL_STATUS_OK;
  }
  st = split_top(ctx, expr, &tokens, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  selector = (lql_selector *)ctx->allocator->calloc(ctx->allocator, 1u,
                                                    sizeof(*selector));
  if (selector == NULL) {
    token_list_cleanup(ctx, &tokens);
    return LQL_STATUS_NO_MEMORY;
  }
  if (tokens.count == 0u) {
    selector->root.kind = LQL_NODE_ALL;
  } else if (tokens.count == 1u) {
    index = NULL;
    rest = NULL;
    wrapper_status =
        parse_indexed_wrapper(ctx, tokens.items[0], &wrapper, &index, &rest);
    if (wrapper_status < 0) {
      st = LQL_STATUS_NO_MEMORY;
    } else if (wrapper_status == 0) {
      wrapper_probe_oom = 0;
      needs_group =
          token_contains_indexed_wrapper(ctx, tokens.items[0],
                                         &wrapper_probe_oom);
      if (wrapper_probe_oom) {
        st = LQL_STATUS_NO_MEMORY;
      } else if (!needs_group) {
        st = parse_one(ctx, tokens.items[0], &selector->root, error);
      } else {
        memset(&root_group, 0, sizeof(root_group));
        root_group.node.kind = or_mode ? LQL_NODE_OR : LQL_NODE_AND;
        st = append_token_to_group(ctx, &root_group, tokens.items[0], error);
        if (st == LQL_STATUS_OK) {
          st = finalize_indexed_group(ctx, &root_group, or_mode);
        }
        if (st == LQL_STATUS_OK) {
          selector->root = root_group.node;
          memset(&root_group.node, 0, sizeof(root_group.node));
        }
        lql_node_cleanup(ctx->receiver, &root_group.node);
        indexed_groups_cleanup(ctx, root_group.groups, root_group.group_count);
      }
    } else {
      memset(&root_group, 0, sizeof(root_group));
      root_group.node.kind = or_mode ? LQL_NODE_OR : LQL_NODE_AND;
      st = append_token_to_group(ctx, &root_group, tokens.items[0], error);
      if (st == LQL_STATUS_OK) {
        st = finalize_indexed_group(ctx, &root_group, or_mode);
      }
      if (st == LQL_STATUS_OK) {
        selector->root = root_group.node;
        memset(&root_group.node, 0, sizeof(root_group.node));
      }
      lql_node_cleanup(ctx->receiver, &root_group.node);
      indexed_groups_cleanup(ctx, root_group.groups, root_group.group_count);
    }
    ctx->allocator->destroy(ctx->allocator, index);
  } else {
    memset(&root_group, 0, sizeof(root_group));
    root_group.node.kind = or_mode ? LQL_NODE_OR : LQL_NODE_AND;
    for (i = 0u; i < tokens.count && st == LQL_STATUS_OK; ++i) {
      st = append_token_to_group(ctx, &root_group, tokens.items[i], error);
    }
    if (st == LQL_STATUS_OK) {
      st = finalize_indexed_group(ctx, &root_group, or_mode);
    }
    if (st == LQL_STATUS_OK) {
      selector->root = root_group.node;
      memset(&root_group.node, 0, sizeof(root_group.node));
    }
    lql_node_cleanup(ctx->receiver, &root_group.node);
    indexed_groups_cleanup(ctx, root_group.groups, root_group.group_count);
  }
  token_list_cleanup(ctx, &tokens);
  if (st != LQL_STATUS_OK) {
    lql_node_cleanup(ctx->receiver, &selector->root);
    ctx->allocator->destroy(ctx->allocator, selector);
    return st;
  }
  selector->hit_count = 0u;
  assign_hit_indexes(&selector->root, &selector->hit_count);
  *out = selector;
  return LQL_STATUS_OK;
}
