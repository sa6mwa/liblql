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

static unsigned char ascii_lower_byte(unsigned char c) {
  if (c >= (unsigned char)'A' && c <= (unsigned char)'Z') {
    return (unsigned char)(c + ((unsigned char)'a' - (unsigned char)'A'));
  }
  return c;
}

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

static int selector_any_push(lql_selector_parser *ctx, lql_selector *selector,
                             char *item, lql_selector_literal_kind kind,
                             int from_json) {
  char **next;
  size_t *next_lens;
  lql_selector_literal_kind *next_kinds;
  int *next_from_json;
  size_t len;
  len = strlen(item);
  next = (char **)ctx->allocator->realloc(ctx->allocator, selector->any,
                                          sizeof(char *) *
                                              (selector->any_count + 1u));
  if (next == NULL) {
    return 0;
  }
  selector->any = next;
  next_lens = (size_t *)ctx->allocator->realloc(
      ctx->allocator, selector->any_lens,
      sizeof(size_t) * (selector->any_count + 1u));
  if (next_lens == NULL) {
    return 0;
  }
  selector->any_lens = next_lens;
  next_kinds = (lql_selector_literal_kind *)ctx->allocator->realloc(
      ctx->allocator, selector->any_kinds,
      sizeof(lql_selector_literal_kind) * (selector->any_count + 1u));
  if (next_kinds == NULL) {
    return 0;
  }
  selector->any_kinds = next_kinds;
  next_from_json =
      (int *)ctx->allocator->realloc(ctx->allocator, selector->any_from_json,
                                     sizeof(int) * (selector->any_count + 1u));
  if (next_from_json == NULL) {
    return 0;
  }
  selector->any_from_json = next_from_json;
  selector->any[selector->any_count] = item;
  selector->any_lens[selector->any_count] = len;
  selector->any_kinds[selector->any_count] = kind;
  selector->any_from_json[selector->any_count] = from_json ? 1 : 0;
  ++selector->any_count;
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

static int key_allowed_for_kind(lql_selector_kind kind, const char *key) {
  if (key_is_field(key)) {
    return 1;
  }
  switch (kind) {
  case LQL_SELECTOR_KIND_EQ:
    return key_is_value(key);
  case LQL_SELECTOR_KIND_CONTAINS:
  case LQL_SELECTOR_KIND_ICONTAINS:
    return key_is_value(key) || key_is_any(key) || key_is_ignore_case(key);
  case LQL_SELECTOR_KIND_PREFIX:
  case LQL_SELECTOR_KIND_IPREFIX:
    return key_is_value(key) || key_is_ignore_case(key);
  case LQL_SELECTOR_KIND_RANGE:
    return key_is_range_bound(key);
  case LQL_SELECTOR_KIND_DATE:
    return key_is_value(key) || key_is_after(key) || key_is_before(key) ||
           key_is_range_bound(key) || key_is_since(key);
  case LQL_SELECTOR_KIND_IN:
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

static lql_selector_kind kind_from_name(const char *name) {
  if (strcmp(name, "and") == 0) {
    return LQL_SELECTOR_KIND_AND;
  }
  if (strcmp(name, "or") == 0) {
    return LQL_SELECTOR_KIND_OR;
  }
  if (strcmp(name, "not") == 0) {
    return LQL_SELECTOR_KIND_NOT;
  }
  if (strcmp(name, "eq") == 0) {
    return LQL_SELECTOR_KIND_EQ;
  }
  if (strcmp(name, "contains") == 0) {
    return LQL_SELECTOR_KIND_CONTAINS;
  }
  if (strcmp(name, "icontains") == 0) {
    return LQL_SELECTOR_KIND_ICONTAINS;
  }
  if (strcmp(name, "prefix") == 0) {
    return LQL_SELECTOR_KIND_PREFIX;
  }
  if (strcmp(name, "iprefix") == 0) {
    return LQL_SELECTOR_KIND_IPREFIX;
  }
  if (strcmp(name, "range") == 0) {
    return LQL_SELECTOR_KIND_RANGE;
  }
  if (strcmp(name, "date") == 0) {
    return LQL_SELECTOR_KIND_DATE;
  }
  if (strcmp(name, "in") == 0) {
    return LQL_SELECTOR_KIND_IN;
  }
  if (strcmp(name, "exists") == 0) {
    return LQL_SELECTOR_KIND_EXISTS;
  }
  return LQL_SELECTOR_KIND_ALL;
}

static int parse_number_literal(const char *decoded, double *out);

static lql_selector_literal_kind selector_literal_kind(const char *raw) {
  double number;
  if (raw == NULL || raw[0] == '"') {
    return LQL_SELECTOR_LITERAL_STRING;
  }
  if (strcmp(raw, "true") == 0 || strcmp(raw, "false") == 0) {
    return LQL_SELECTOR_LITERAL_BOOL;
  }
  if (strcmp(raw, "null") == 0) {
    return LQL_SELECTOR_LITERAL_NULL;
  }
  return parse_number_literal(raw, &number) ? LQL_SELECTOR_LITERAL_NUMBER
                                            : LQL_SELECTOR_LITERAL_STRING;
}

static int parse_any_values(lql_selector_parser *ctx, char *decoded,
                            lql_selector *selector,
                            int reject_surrounding_whitespace,
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
      if (!selector_any_push(ctx, selector, item, selector_literal_kind(item),
                             0)) {
        ctx->allocator->destroy(ctx->allocator, item);
        return 0;
      }
    } else {
      ctx->allocator->destroy(ctx->allocator, item);
    }
    cursor = bar == NULL ? NULL : bar + 1;
  }
  if (selector->any_count == 0u) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "selector any requires values");
    return 0;
  }
  return 1;
}

static int parse_number_literal(const char *decoded, double *out) {
  const char *begin;
  const char *end;
  begin = decoded;
  if (begin == NULL) {
    return 0;
  }
  while (isspace((unsigned char)*begin)) {
    ++begin;
  }
  end = begin + strlen(begin);
  while (end > begin && isspace((unsigned char)end[-1])) {
    --end;
  }
  return lql_number_parse_json(begin, (size_t)(end - begin), out);
}

static int ascii_equal_ignore_case(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (ascii_lower_byte((unsigned char)*a) !=
        ascii_lower_byte((unsigned char)*b)) {
      return 0;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

static int replace_selector_text(lql_selector_parser *ctx, char **slot,
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

static int set_range_bound(lql_selector_parser *ctx, lql_selector *selector,
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
    selector->range_is_temporal = 1;
    if (strcmp(key, "gt") == 0) {
      if (!replace_selector_text(ctx, &selector->range_gt_text, decoded)) {
        return 0;
      }
      selector->temporal_gt = temporal;
      selector->has_temporal_gt = 1;
    } else if (strcmp(key, "gte") == 0) {
      if (!replace_selector_text(ctx, &selector->range_gte_text, decoded)) {
        return 0;
      }
      selector->temporal_gte = temporal;
      selector->has_temporal_gte = 1;
    } else if (strcmp(key, "lt") == 0) {
      if (!replace_selector_text(ctx, &selector->range_lt_text, decoded)) {
        return 0;
      }
      selector->temporal_lt = temporal;
      selector->has_temporal_lt = 1;
    } else {
      if (!replace_selector_text(ctx, &selector->range_lte_text, decoded)) {
        return 0;
      }
      selector->temporal_lte = temporal;
      selector->has_temporal_lte = 1;
    }
    return 1;
  }
  if (strcmp(key, "gt") == 0) {
    selector->range_gt = number;
    selector->has_range_gt = 1;
  } else if (strcmp(key, "gte") == 0) {
    selector->range_gte = number;
    selector->has_range_gte = 1;
  } else if (strcmp(key, "lt") == 0) {
    selector->range_lt = number;
    selector->has_range_lt = 1;
  } else {
    selector->range_lte = number;
    selector->has_range_lte = 1;
  }
  return 1;
}

static int set_date_bound(lql_selector_parser *ctx, lql_selector *selector,
                          const char *slot, const char *decoded,
                          lql_error *error) {
  lql_temporal temporal;
  if (strcmp(slot, "since") == 0) {
    if (!replace_selector_text(ctx, &selector->date_since_text, decoded)) {
      return 0;
    }
    if (ascii_equal_ignore_case(decoded, "now")) {
      selector->since_macro = LQL_SINCE_NOW;
      return 1;
    }
    if (ascii_equal_ignore_case(decoded, "today")) {
      selector->since_macro = LQL_SINCE_TODAY;
      return 1;
    }
    if (ascii_equal_ignore_case(decoded, "yesterday")) {
      selector->since_macro = LQL_SINCE_YESTERDAY;
      return 1;
    }
  }
  if (!lql_parse_temporal_literal(decoded, &temporal)) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "date selector bound invalid");
    return 0;
  }
  if (strcmp(slot, "value") == 0) {
    if (!replace_selector_text(ctx, &selector->date_value_text, decoded)) {
      return 0;
    }
    selector->temporal_eq = temporal;
    selector->has_temporal_eq = 1;
  } else if (strcmp(slot, "since") == 0) {
    selector->temporal_gte = temporal;
    selector->has_temporal_gte = 1;
  } else if (strcmp(slot, "after") == 0) {
    if (!replace_selector_text(ctx, &selector->date_after_text, decoded)) {
      return 0;
    }
    selector->temporal_gt = temporal;
    selector->has_temporal_gt = 1;
  } else if (strcmp(slot, "before") == 0) {
    if (!replace_selector_text(ctx, &selector->date_before_text, decoded)) {
      return 0;
    }
    selector->temporal_lt = temporal;
    selector->has_temporal_lt = 1;
  } else if (strcmp(slot, "gt") == 0) {
    if (!replace_selector_text(ctx, &selector->date_gt_text, decoded)) {
      return 0;
    }
    selector->temporal_gt = temporal;
    selector->has_temporal_gt = 1;
  } else if (strcmp(slot, "gte") == 0) {
    if (!replace_selector_text(ctx, &selector->date_gte_text, decoded)) {
      return 0;
    }
    selector->temporal_gte = temporal;
    selector->has_temporal_gte = 1;
  } else if (strcmp(slot, "lt") == 0) {
    if (!replace_selector_text(ctx, &selector->date_lt_text, decoded)) {
      return 0;
    }
    selector->temporal_lt = temporal;
    selector->has_temporal_lt = 1;
  } else if (strcmp(slot, "lte") == 0) {
    if (!replace_selector_text(ctx, &selector->date_lte_text, decoded)) {
      return 0;
    }
    selector->temporal_lte = temporal;
    selector->has_temporal_lte = 1;
  }
  return 1;
}

static void prepare_selector_value_temporal(lql_selector *selector);

static lql_status parse_key_values(lql_selector_parser *ctx, char *body,
                                   lql_selector_kind kind,
                                   lql_selector *selector, lql_error *error) {
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

  memset(selector, 0, sizeof(*selector));
  selector->kind = kind;
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
                    "selector predicate requires key=value");
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
    if (kind == LQL_SELECTOR_KIND_IN && key_is_any(key) &&
        value_had_leading_space && raw_value[0] != '\0') {
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
    } else if (kind == LQL_SELECTOR_KIND_DATE && key_is_after(key)) {
      seen_slot = &seen_after;
    } else if (kind == LQL_SELECTOR_KIND_DATE && key_is_before(key)) {
      seen_slot = &seen_before;
    } else if (kind == LQL_SELECTOR_KIND_DATE && key_is_since(key)) {
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
      ctx->allocator->destroy(ctx->allocator, selector->field);
      selector->field = normalized;
    } else if (kind == LQL_SELECTOR_KIND_DATE && key_is_value(key)) {
      if (!set_date_bound(ctx, selector, "value", decoded, error)) {
        ctx->allocator->destroy(ctx->allocator, decoded);
        st = LQL_STATUS_PARSE_ERROR;
        goto fail;
      }
      ctx->allocator->destroy(ctx->allocator, decoded);
    } else if (key_is_value(key)) {
      ctx->allocator->destroy(ctx->allocator, selector->value);
      selector->value = decoded;
      selector->value_set = 1;
      selector->value_kind = selector_literal_kind(*seen_slot);
    } else if (kind == LQL_SELECTOR_KIND_DATE &&
               (key_is_after(key) || key_is_before(key) || key_is_since(key))) {
      date_slot = key_is_after(key)    ? "after"
                  : key_is_before(key) ? "before"
                                       : "since";
      if (!set_date_bound(ctx, selector, date_slot, decoded, error)) {
        ctx->allocator->destroy(ctx->allocator, decoded);
        st = LQL_STATUS_PARSE_ERROR;
        goto fail;
      }
      ctx->allocator->destroy(ctx->allocator, decoded);
    } else if (key_is_any(key)) {
      if (!parse_any_values(ctx, decoded, selector,
                            kind == LQL_SELECTOR_KIND_IN, error)) {
        ctx->allocator->destroy(ctx->allocator, decoded);
        st = error != NULL && error->code != LQL_STATUS_OK
                 ? error->code
                 : LQL_STATUS_PARSE_ERROR;
        goto fail;
      }
      ctx->allocator->destroy(ctx->allocator, decoded);
    } else if (key_is_ignore_case(key)) {
      if (!parse_bool_value(decoded, &selector->ignore_case)) {
        ctx->allocator->destroy(ctx->allocator, decoded);
        st = LQL_STATUS_PARSE_ERROR;
        lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                      "selector ignoreCase must be true/false/t/f");
        goto fail;
      }
      ctx->allocator->destroy(ctx->allocator, decoded);
    } else if (key_is_range_bound(key)) {
      if (kind == LQL_SELECTOR_KIND_DATE) {
        if (!set_date_bound(ctx, selector, key, decoded, error)) {
          ctx->allocator->destroy(ctx->allocator, decoded);
          st = LQL_STATUS_PARSE_ERROR;
          goto fail;
        }
      } else if (!set_range_bound(ctx, selector, key, decoded, error)) {
        ctx->allocator->destroy(ctx->allocator, decoded);
        st = LQL_STATUS_PARSE_ERROR;
        goto fail;
      }
      ctx->allocator->destroy(ctx->allocator, decoded);
    }
  }
  token_list_cleanup(ctx, &parts);
  if (selector->field == NULL) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "selector field required");
    goto fail_after_tokens;
  }
  if (kind == LQL_SELECTOR_KIND_DATE && seen_since != NULL &&
      (seen_value != NULL || seen_after != NULL || seen_before != NULL ||
       seen_gt != NULL || seen_gte != NULL || seen_lt != NULL ||
       seen_lte != NULL)) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector since cannot be combined with other bounds");
    goto fail_after_tokens;
  }
  if (kind == LQL_SELECTOR_KIND_DATE && seen_after != NULL && seen_gt != NULL) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector cannot combine after and gt");
    goto fail_after_tokens;
  }
  if (kind == LQL_SELECTOR_KIND_DATE && seen_before != NULL &&
      seen_lt != NULL) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector cannot combine before and lt");
    goto fail_after_tokens;
  }
  if (selector->any_count != 0u) {
    if (kind != LQL_SELECTOR_KIND_CONTAINS &&
        kind != LQL_SELECTOR_KIND_ICONTAINS && kind != LQL_SELECTOR_KIND_IN) {
      st = LQL_STATUS_PARSE_ERROR;
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector operator does not support any");
      goto fail_after_tokens;
    }
    if (kind != LQL_SELECTOR_KIND_IN &&
        (selector->value_set || selector->value != NULL)) {
      st = LQL_STATUS_PARSE_ERROR;
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector cannot set both value and any");
      goto fail_after_tokens;
    }
  }
  if (kind == LQL_SELECTOR_KIND_IN && selector->any_count == 0u) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "in selector requires any values");
    goto fail_after_tokens;
  }
  if (kind == LQL_SELECTOR_KIND_RANGE && !selector->has_range_gt &&
      !selector->has_range_gte && !selector->has_range_lt &&
      !selector->has_range_lte && !selector->has_temporal_gt &&
      !selector->has_temporal_gte && !selector->has_temporal_lt &&
      !selector->has_temporal_lte) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "range selector requires at least one bound");
    goto fail_after_tokens;
  }
  if (kind == LQL_SELECTOR_KIND_RANGE &&
      (selector->has_range_gt || selector->has_range_gte ||
       selector->has_range_lt || selector->has_range_lte) &&
      (selector->has_temporal_gt || selector->has_temporal_gte ||
       selector->has_temporal_lt || selector->has_temporal_lte)) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "range selector cannot mix numeric and datetime bounds");
    goto fail_after_tokens;
  }
  if (kind == LQL_SELECTOR_KIND_DATE && !selector->has_temporal_eq &&
      !selector->has_temporal_gt && !selector->has_temporal_gte &&
      !selector->has_temporal_lt && !selector->has_temporal_lte &&
      selector->since_macro == LQL_SINCE_NONE) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector requires at least one bound");
    goto fail_after_tokens;
  }
  prepare_selector_value_temporal(selector);
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

static int string_predicate_is_match_all_alias(lql_selector_kind kind,
                                               const lql_selector *selector) {
  int empty_value;
  if (kind != LQL_SELECTOR_KIND_CONTAINS &&
      kind != LQL_SELECTOR_KIND_ICONTAINS && kind != LQL_SELECTOR_KIND_PREFIX &&
      kind != LQL_SELECTOR_KIND_IPREFIX) {
    return 0;
  }
  if (selector->field == NULL || selector->any_count != 0u) {
    return 0;
  }
  empty_value = (!selector->value_set && selector->value == NULL) ||
                (selector->value_set && selector->value != NULL &&
                 selector->value[0] == '\0');
  if (!empty_value) {
    return 0;
  }
  if (strcmp(selector->field, "/") == 0) {
    return 1;
  }
  if ((kind == LQL_SELECTOR_KIND_CONTAINS ||
       kind == LQL_SELECTOR_KIND_ICONTAINS) &&
      (strcmp(selector->field, "/*") == 0 ||
       strcmp(selector->field, "/...") == 0)) {
    return 1;
  }
  return 0;
}

static void prepare_selector_value_temporal(lql_selector *selector) {
  selector->value_is_temporal = 0;
  if ((selector->kind == LQL_SELECTOR_KIND_EQ ||
       selector->kind == LQL_SELECTOR_KIND_NE) &&
      selector->value != NULL &&
      lql_parse_temporal_literal(selector->value, &selector->temporal_eq)) {
    selector->value_is_temporal = 1;
  }
}

static lql_status parse_exists_body(lql_selector_parser *ctx, const char *body,
                                    lql_selector *selector, lql_error *error) {
  lql_token_list parts;
  char *decoded;
  char *normalized;
  lql_status st;

  memset(selector, 0, sizeof(*selector));
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
  selector->field = normalized;
  return LQL_STATUS_OK;
}

static lql_status parse_one(lql_selector_parser *ctx, const char *expr,
                            lql_selector *out, lql_error *error) {
  char *copy;
  char *body;
  char *close;
  char *dot;
  char *op;
  char *value;
  char *name;
  char *raw_field;
  char *raw_value;
  lql_selector child;
  lql_status st;

  memset(out, 0, sizeof(*out));
  copy = ctx->allocator->strdup(ctx->allocator, expr);
  if (copy == NULL) {
    return LQL_STATUS_NO_MEMORY;
  }
  if (strcmp(copy, "/") == 0 || strcmp(copy, ".") == 0 ||
      strcmp(copy, "{}") == 0) {
    out->kind = LQL_SELECTOR_KIND_ALL;
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
    out->kind = strcmp(name, "and") == 0  ? LQL_SELECTOR_KIND_AND
                : strcmp(name, "or") == 0 ? LQL_SELECTOR_KIND_OR
                                          : LQL_SELECTOR_KIND_NOT;
    out->children = (lql_selector *)ctx->allocator->calloc(
        ctx->allocator, 1u, sizeof(lql_selector));
    if (out->children == NULL) {
      lql_selector_cleanup(ctx->receiver, &child);
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
    lql_selector child;
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
    out->field = normalize_field_path(ctx, raw_field);
    out->value = unquote(ctx, raw_value);
    out->value_is_string = raw_value[0] == '"';
    out->value_kind = selector_literal_kind(raw_value);
    ctx->allocator->destroy(ctx->allocator, raw_field);
    ctx->allocator->destroy(ctx->allocator, raw_value);
    out->value_set = 1;
    if (out->field == NULL || out->value == NULL) {
      ctx->allocator->destroy(ctx->allocator, copy);
      return LQL_STATUS_NO_MEMORY;
    }
    if (op0 == '!') {
      memset(&child, 0, sizeof(child));
      child = *out;
      child.kind = LQL_SELECTOR_KIND_EQ;
      prepare_selector_value_temporal(&child);
      memset(out, 0, sizeof(*out));
      out->children = (lql_selector *)ctx->allocator->calloc(
          ctx->allocator, 1u, sizeof(lql_selector));
      if (out->children == NULL) {
        lql_selector_cleanup(ctx->receiver, &child);
        ctx->allocator->destroy(ctx->allocator, copy);
        return LQL_STATUS_NO_MEMORY;
      }
      out->kind = LQL_SELECTOR_KIND_NOT;
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
        if (!set_range_bound(ctx, out, bound_key, out->value, error)) {
          lql_selector_cleanup(ctx->receiver, out);
          ctx->allocator->destroy(ctx->allocator, copy);
          return error != NULL && error->code != LQL_STATUS_OK
                     ? error->code
                     : LQL_STATUS_PARSE_ERROR;
        }
        out->kind = LQL_SELECTOR_KIND_RANGE;
      } else {
        out->kind = LQL_SELECTOR_KIND_EQ;
      }
    } else {
      out->kind = LQL_SELECTOR_KIND_EQ;
    }
    if (out->kind == LQL_SELECTOR_KIND_EQ) {
      prepare_selector_value_temporal(out);
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
    if (out->kind == LQL_SELECTOR_KIND_ALL) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR, "unknown selector operator");
      ctx->allocator->destroy(ctx->allocator, copy);
      return LQL_STATUS_PARSE_ERROR;
    }
    if (out->kind == LQL_SELECTOR_KIND_EXISTS) {
      st = parse_exists_body(ctx, body + 1, out, error);
      if (st == LQL_STATUS_OK) {
        out->kind = LQL_SELECTOR_KIND_EXISTS;
      }
      ctx->allocator->destroy(ctx->allocator, copy);
      return st;
    }
    st = parse_key_values(ctx, body + 1, out->kind, out, error);
    if (st != LQL_STATUS_OK) {
      ctx->allocator->destroy(ctx->allocator, copy);
      return st;
    }
    if (string_predicate_is_match_all_alias(out->kind, out)) {
      lql_selector_cleanup(ctx->receiver, out);
      out->kind = LQL_SELECTOR_KIND_ALL;
    }
    ctx->allocator->destroy(ctx->allocator, copy);
    return LQL_STATUS_OK;
  }

  lql_set_error(error, LQL_STATUS_PARSE_ERROR, "invalid selector expression");
  ctx->allocator->destroy(ctx->allocator, copy);
  return LQL_STATUS_PARSE_ERROR;
}

static int selector_is_predicate(const lql_selector *selector) {
  switch (selector->kind) {
  case LQL_SELECTOR_KIND_EQ:
  case LQL_SELECTOR_KIND_NE:
  case LQL_SELECTOR_KIND_CONTAINS:
  case LQL_SELECTOR_KIND_ICONTAINS:
  case LQL_SELECTOR_KIND_PREFIX:
  case LQL_SELECTOR_KIND_IPREFIX:
  case LQL_SELECTOR_KIND_RANGE:
  case LQL_SELECTOR_KIND_DATE:
  case LQL_SELECTOR_KIND_IN:
  case LQL_SELECTOR_KIND_EXISTS:
    return 1;
  default:
    return 0;
  }
}

static int finalize_selector(lql_selector_parser *ctx, lql_selector *selector) {
  (void)ctx;
  (void)selector;
  return 1;
}

static int append_selector(lql_selector_parser *ctx, lql_selector *parent,
                           lql_selector *child) {
  lql_selector *next;
  next = (lql_selector *)ctx->allocator->realloc(
      ctx->allocator, parent->children,
      sizeof(lql_selector) * (parent->child_count + 1u));
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
  SELECTOR_JSON_FRAME_SELECTOR = 0,
  SELECTOR_JSON_FRAME_CHILD_ARRAY = 1,
  SELECTOR_JSON_FRAME_PREDICATE = 2,
  SELECTOR_JSON_FRAME_ANY_ARRAY = 3
} selector_json_frame_kind;

typedef struct selector_json_frame {
  selector_json_frame_kind kind;
  lql_selector *selector;
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

static lql_status selector_json_fail(selector_json_state *state,
                                     lql_error *error, lql_status status,
                                     const char *message) {
  if (state->status == LQL_STATUS_OK) {
    state->status = status;
    lql_set_error(state->error, status, message);
  }
  lql_set_error(error, status, message);
  return status;
}

static selector_json_frame *selector_json_top(selector_json_state *state) {
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
                                    lql_selector *selector) {
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
  state->frames[state->frame_count].selector = selector;
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
  state->text =
      (char *)state->parser.allocator->alloc(state->parser.allocator, 1u);
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

static lql_selector_kind selector_json_kind_from_key(const char *key) {
  return kind_from_name(key);
}

static int selector_json_kind_is_valid_operator(lql_selector_kind kind) {
  switch (kind) {
  case LQL_SELECTOR_KIND_AND:
  case LQL_SELECTOR_KIND_OR:
  case LQL_SELECTOR_KIND_NOT:
  case LQL_SELECTOR_KIND_EQ:
  case LQL_SELECTOR_KIND_CONTAINS:
  case LQL_SELECTOR_KIND_ICONTAINS:
  case LQL_SELECTOR_KIND_PREFIX:
  case LQL_SELECTOR_KIND_IPREFIX:
  case LQL_SELECTOR_KIND_RANGE:
  case LQL_SELECTOR_KIND_DATE:
  case LQL_SELECTOR_KIND_IN:
  case LQL_SELECTOR_KIND_EXISTS:
    return 1;
  case LQL_SELECTOR_KIND_ALL:
  case LQL_SELECTOR_KIND_NE:
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
  } else if (key_is_after(key) && frame->selector != NULL &&
             frame->selector->kind == LQL_SELECTOR_KIND_DATE) {
    slot = &frame->seen_after;
  } else if (key_is_before(key) && frame->selector != NULL &&
             frame->selector->kind == LQL_SELECTOR_KIND_DATE) {
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
                                     lql_selector *selector,
                                     const char *value) {
  char *normalized;
  normalized = normalize_field_path(&state->parser, value);
  if (normalized == NULL) {
    return 0;
  }
  state->parser.allocator->destroy(state->parser.allocator, selector->field);
  selector->field = normalized;
  return 1;
}

static int selector_json_store_value(selector_json_state *state,
                                     lql_selector *selector, const char *value,
                                     lql_selector_literal_kind kind) {
  char *copy;
  copy = state->parser.allocator->strdup(state->parser.allocator, value);
  if (copy == NULL) {
    return 0;
  }
  state->parser.allocator->destroy(state->parser.allocator, selector->value);
  selector->value = copy;
  selector->value_set = 1;
  selector->value_is_string = kind == LQL_SELECTOR_LITERAL_STRING;
  selector->value_from_json = 1;
  selector->value_kind = kind;
  return 1;
}

static int selector_json_push_any(selector_json_state *state,
                                  lql_selector *selector, const char *value,
                                  lql_selector_literal_kind kind) {
  char *copy;
  copy = state->parser.allocator->strdup(state->parser.allocator, value);
  if (copy == NULL) {
    return 0;
  }
  if (!selector_any_push(&state->parser, selector, copy, kind, 1)) {
    state->parser.allocator->destroy(state->parser.allocator, copy);
    return 0;
  }
  return 1;
}

static int selector_json_store_scalar(selector_json_state *state,
                                      selector_json_frame *frame,
                                      const char *value,
                                      lql_selector_literal_kind kind) {
  double number;
  if (frame == NULL) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "unexpected selector JSON scalar");
    return 0;
  }
  if (kind == LQL_SELECTOR_LITERAL_NUMBER &&
      !lql_number_parse_json(value, strlen(value), &number)) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "selector JSON number must be finite");
    return 0;
  }
  if (frame->kind == SELECTOR_JSON_FRAME_PREDICATE &&
      frame->pending_key != NULL && key_is_value(frame->pending_key) &&
      frame->selector != NULL &&
      frame->selector->kind != LQL_SELECTOR_KIND_DATE) {
    return selector_json_store_value(state, frame->selector, value, kind);
  }
  if (frame->kind == SELECTOR_JSON_FRAME_ANY_ARRAY && frame->selector != NULL) {
    return selector_json_push_any(state, frame->selector, value, kind);
  }
  lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                "unexpected selector JSON scalar");
  return 0;
}

static lql_status selector_json_validate_predicate(selector_json_state *state,
                                                   selector_json_frame *frame) {
  lql_selector *selector;
  selector = frame->selector;
  if (selector == NULL) {
    return LQL_STATUS_PARSE_ERROR;
  }
  if (selector->field == NULL) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "selector field required");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (selector->kind == LQL_SELECTOR_KIND_IN && selector->any_count == 0u) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "in selector requires any values");
    return LQL_STATUS_PARSE_ERROR;
  }
  if ((selector->kind == LQL_SELECTOR_KIND_CONTAINS ||
       selector->kind == LQL_SELECTOR_KIND_ICONTAINS) &&
      selector->any_count != 0u &&
      (selector->value_set || selector->value != NULL)) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "selector cannot set both value and any");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (selector->kind == LQL_SELECTOR_KIND_RANGE && !selector->has_range_gt &&
      !selector->has_range_gte && !selector->has_range_lt &&
      !selector->has_range_lte && !selector->has_temporal_gt &&
      !selector->has_temporal_gte && !selector->has_temporal_lt &&
      !selector->has_temporal_lte) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "range selector requires at least one bound");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (selector->kind == LQL_SELECTOR_KIND_RANGE &&
      (selector->has_range_gt || selector->has_range_gte ||
       selector->has_range_lt || selector->has_range_lte) &&
      (selector->has_temporal_gt || selector->has_temporal_gte ||
       selector->has_temporal_lt || selector->has_temporal_lte)) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "range selector cannot mix numeric and datetime bounds");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (selector->kind == LQL_SELECTOR_KIND_DATE && frame->seen_since &&
      (frame->seen_value || frame->seen_after || frame->seen_before ||
       frame->seen_gt || frame->seen_gte || frame->seen_lt ||
       frame->seen_lte)) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "date selector since cannot be combined with other bounds");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (selector->kind == LQL_SELECTOR_KIND_DATE && frame->seen_after &&
      frame->seen_gt) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "date selector cannot combine after and gt");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (selector->kind == LQL_SELECTOR_KIND_DATE && frame->seen_before &&
      frame->seen_lt) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "date selector cannot combine before and lt");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (selector->kind == LQL_SELECTOR_KIND_DATE && !selector->has_temporal_eq &&
      !selector->has_temporal_gt && !selector->has_temporal_gte &&
      !selector->has_temporal_lt && !selector->has_temporal_lte &&
      selector->since_macro == LQL_SINCE_NONE) {
    lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                  "date selector requires at least one bound");
    return LQL_STATUS_PARSE_ERROR;
  }
  prepare_selector_value_temporal(selector);
  return LQL_STATUS_OK;
}

static lql_status selector_json_consume_key(selector_json_state *state,
                                            selector_json_frame *frame,
                                            lql_error *error) {
  state->parser.allocator->destroy(state->parser.allocator, frame->pending_key);
  frame->pending_key =
      state->parser.allocator->strdup(state->parser.allocator, state->text);
  if (frame->pending_key == NULL) {
    return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                              "out of memory");
  }
  return LQL_STATUS_OK;
}

static lql_status selector_json_begin_key(void *user, lql_error *error) {
  selector_json_state *state;
  state = (selector_json_state *)user;
  if (!selector_json_reset_text(state)) {
    return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                              "out of memory");
  }
  return LQL_STATUS_OK;
}

static lql_status selector_json_key_chunk(void *user, const char *data,
                                          size_t len, lql_error *error) {
  selector_json_state *state;
  state = (selector_json_state *)user;
  if (!selector_json_append_text(state, data, len)) {
    return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                              "out of memory");
  }
  return LQL_STATUS_OK;
}

static lql_status selector_json_end_key(void *user, lql_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  lql_selector_kind kind;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL || frame->kind == SELECTOR_JSON_FRAME_CHILD_ARRAY ||
      frame->kind == SELECTOR_JSON_FRAME_ANY_ARRAY) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON object key");
  }
  if (frame->kind == SELECTOR_JSON_FRAME_SELECTOR) {
    kind = selector_json_kind_from_key(state->text);
    if (!selector_json_kind_is_valid_operator(kind)) {
      return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                                "unknown selector JSON operator");
    }
    if (frame->key_count != 0) {
      return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                                "selector JSON object has multiple operators");
    }
    if (frame->selector != NULL) {
      frame->selector->kind = kind;
    }
    frame->key_count = 1;
  } else if (frame->kind == SELECTOR_JSON_FRAME_PREDICATE) {
    if (frame->selector == NULL ||
        !key_allowed_for_kind(frame->selector->kind, state->text)) {
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

static lql_selector *selector_json_append_child(selector_json_state *state,
                                                lql_selector *parent) {
  lql_selector child;
  memset(&child, 0, sizeof(child));
  if (!append_selector(&state->parser, parent, &child)) {
    return NULL;
  }
  return &parent->children[parent->child_count - 1u];
}

static lql_status selector_json_object_begin(void *user, lql_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  lql_selector *child;
  lql_selector_kind kind;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL) {
    if (state->selector == NULL) {
      return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                                "selector JSON root required");
    }
    if (!selector_json_push_frame(state, SELECTOR_JSON_FRAME_SELECTOR,
                                  state->selector)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LQL_STATUS_OK;
  }
  if (frame->kind == SELECTOR_JSON_FRAME_CHILD_ARRAY) {
    child = selector_json_append_child(state, frame->selector);
    if (child == NULL) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    if (!selector_json_push_frame(state, SELECTOR_JSON_FRAME_SELECTOR, child)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LQL_STATUS_OK;
  }
  if (frame->kind != SELECTOR_JSON_FRAME_SELECTOR ||
      frame->pending_key == NULL || frame->selector == NULL) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON object");
  }
  kind = frame->selector->kind;
  if (kind == LQL_SELECTOR_KIND_NOT) {
    child = selector_json_append_child(state, frame->selector);
    if (child == NULL) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    state->parser.allocator->destroy(state->parser.allocator,
                                     frame->pending_key);
    frame->pending_key = NULL;
    if (!selector_json_push_frame(state, SELECTOR_JSON_FRAME_SELECTOR, child)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LQL_STATUS_OK;
  }
  if (selector_is_predicate(frame->selector) &&
      kind != LQL_SELECTOR_KIND_EXISTS) {
    state->parser.allocator->destroy(state->parser.allocator,
                                     frame->pending_key);
    frame->pending_key = NULL;
    if (!selector_json_push_frame(state, SELECTOR_JSON_FRAME_PREDICATE,
                                  frame->selector)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LQL_STATUS_OK;
  }
  return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                            "unexpected selector JSON object");
}

static lql_status selector_json_object_end(void *user, lql_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  lql_status st;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL || (frame->kind != SELECTOR_JSON_FRAME_SELECTOR &&
                        frame->kind != SELECTOR_JSON_FRAME_PREDICATE)) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON object end");
  }
  if (frame->pending_key != NULL) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "selector JSON key has no value");
  }
  if (frame->kind == SELECTOR_JSON_FRAME_SELECTOR) {
    if (frame->key_count == 0 && frame->selector != NULL) {
      frame->selector->kind = LQL_SELECTOR_KIND_ALL;
    }
    if (frame->selector != NULL &&
        frame->selector->kind == LQL_SELECTOR_KIND_NOT &&
        frame->selector->child_count != 1u) {
      return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                                "not selector requires one child");
    }
  } else {
    st = selector_json_validate_predicate(state, frame);
    if (st != LQL_STATUS_OK) {
      return selector_json_fail(state, error, st,
                                state->error != NULL &&
                                        state->error->message[0] != '\0'
                                    ? state->error->message
                                    : "invalid selector JSON predicate");
    }
    if (string_predicate_is_match_all_alias(frame->selector->kind,
                                            frame->selector)) {
      lql_selector_cleanup(state->parser.receiver, frame->selector);
      frame->selector->kind = LQL_SELECTOR_KIND_ALL;
    }
  }
  selector_json_pop_frame(state);
  return LQL_STATUS_OK;
}

static lql_status selector_json_array_begin(void *user, lql_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL || frame->pending_key == NULL || frame->selector == NULL) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON array");
  }
  if (frame->kind == SELECTOR_JSON_FRAME_SELECTOR &&
      (frame->selector->kind == LQL_SELECTOR_KIND_AND ||
       frame->selector->kind == LQL_SELECTOR_KIND_OR)) {
    state->parser.allocator->destroy(state->parser.allocator,
                                     frame->pending_key);
    frame->pending_key = NULL;
    if (!selector_json_push_frame(state, SELECTOR_JSON_FRAME_CHILD_ARRAY,
                                  frame->selector)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LQL_STATUS_OK;
  }
  if (frame->kind == SELECTOR_JSON_FRAME_PREDICATE &&
      key_is_any(frame->pending_key)) {
    state->parser.allocator->destroy(state->parser.allocator,
                                     frame->pending_key);
    frame->pending_key = NULL;
    if (!selector_json_push_frame(state, SELECTOR_JSON_FRAME_ANY_ARRAY,
                                  frame->selector)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LQL_STATUS_OK;
  }
  return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                            "unexpected selector JSON array");
}

static lql_status selector_json_array_end(void *user, lql_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL || (frame->kind != SELECTOR_JSON_FRAME_CHILD_ARRAY &&
                        frame->kind != SELECTOR_JSON_FRAME_ANY_ARRAY)) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON array end");
  }
  selector_json_pop_frame(state);
  return LQL_STATUS_OK;
}

static lql_status selector_json_string_begin(void *user, lql_error *error) {
  selector_json_state *state;
  state = (selector_json_state *)user;
  if (!selector_json_reset_text(state)) {
    return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                              "out of memory");
  }
  return LQL_STATUS_OK;
}

static lql_status selector_json_string_chunk(void *user, const char *data,
                                             size_t len, lql_error *error) {
  selector_json_state *state;
  state = (selector_json_state *)user;
  if (!selector_json_append_text(state, data, len)) {
    return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                              "out of memory");
  }
  return LQL_STATUS_OK;
}

static lql_status selector_json_store_predicate_string(
    selector_json_state *state, selector_json_frame *frame, lql_error *error) {
  const char *key;
  lql_selector *selector;
  int ok;
  key = frame->pending_key;
  selector = frame->selector;
  ok = 1;
  if (key_is_field(key)) {
    ok = selector_json_store_field(state, selector, state->text);
  } else if (selector->kind == LQL_SELECTOR_KIND_DATE && key_is_value(key)) {
    ok = set_date_bound(&state->parser, selector, "value", state->text,
                        state->error);
  } else if (key_is_value(key)) {
    ok = selector_json_store_value(state, selector, state->text,
                                   LQL_SELECTOR_LITERAL_STRING);
  } else if (selector->kind == LQL_SELECTOR_KIND_DATE &&
             (key_is_after(key) || key_is_before(key) || key_is_since(key))) {
    ok = set_date_bound(&state->parser, selector,
                        key_is_after(key)    ? "after"
                        : key_is_before(key) ? "before"
                                             : "since",
                        state->text, state->error);
  } else if (key_is_any(key)) {
    ok = parse_any_values(&state->parser, state->text, selector,
                          selector->kind == LQL_SELECTOR_KIND_IN, state->error);
  } else if (key_is_ignore_case(key)) {
    ok = parse_bool_value(state->text, &selector->ignore_case);
    if (!ok) {
      lql_set_error(state->error, LQL_STATUS_PARSE_ERROR,
                    "selector ignoreCase must be true/false/t/f");
    }
  } else if (key_is_range_bound(key)) {
    if (selector->kind == LQL_SELECTOR_KIND_DATE) {
      ok = set_date_bound(&state->parser, selector, key, state->text,
                          state->error);
    } else {
      ok = set_range_bound(&state->parser, selector, key, state->text,
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
            : "invalid selector JSON predicate");
  }
  state->parser.allocator->destroy(state->parser.allocator, frame->pending_key);
  frame->pending_key = NULL;
  return LQL_STATUS_OK;
}

static lql_status selector_json_string_end(void *user, lql_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame == NULL) {
    return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                              "unexpected selector JSON string");
  }
  if (frame->kind == SELECTOR_JSON_FRAME_SELECTOR &&
      frame->pending_key != NULL && frame->selector != NULL &&
      frame->selector->kind == LQL_SELECTOR_KIND_EXISTS) {
    if (state->text[0] == '\0') {
      return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                                "exists selector requires exactly one path");
    }
    if (!selector_json_store_field(state, frame->selector, state->text)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    state->parser.allocator->destroy(state->parser.allocator,
                                     frame->pending_key);
    frame->pending_key = NULL;
    return LQL_STATUS_OK;
  }
  if (frame->kind == SELECTOR_JSON_FRAME_PREDICATE &&
      frame->pending_key != NULL) {
    return selector_json_store_predicate_string(state, frame, error);
  }
  if (frame->kind == SELECTOR_JSON_FRAME_ANY_ARRAY && frame->selector != NULL) {
    if (!selector_json_push_any(state, frame->selector, state->text,
                                LQL_SELECTOR_LITERAL_STRING)) {
      return selector_json_fail(state, error, LQL_STATUS_NO_MEMORY,
                                "out of memory");
    }
    return LQL_STATUS_OK;
  }
  return selector_json_fail(state, error, LQL_STATUS_PARSE_ERROR,
                            "unexpected selector JSON string");
}

static lql_status selector_json_number_begin(void *user, lql_error *error) {
  return selector_json_string_begin(user, error);
}

static lql_status selector_json_number_chunk(void *user, const char *data,
                                             size_t len, lql_error *error) {
  return selector_json_string_chunk(user, data, len, error);
}

static lql_status selector_json_number_end(void *user, lql_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  int ok;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame != NULL && frame->kind == SELECTOR_JSON_FRAME_PREDICATE &&
      frame->pending_key != NULL && frame->selector != NULL &&
      key_is_range_bound(frame->pending_key) &&
      frame->selector->kind == LQL_SELECTOR_KIND_RANGE) {
    ok = set_range_bound(&state->parser, frame->selector, frame->pending_key,
                         state->text, state->error);
  } else {
    ok = selector_json_store_scalar(state, frame, state->text,
                                    LQL_SELECTOR_LITERAL_NUMBER);
  }
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
  state->parser.allocator->destroy(state->parser.allocator, frame->pending_key);
  frame->pending_key = NULL;
  return LQL_STATUS_OK;
}

static lql_status selector_json_boolean(void *user, int value,
                                        lql_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  int ok;
  const char *literal;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  if (frame != NULL && frame->kind == SELECTOR_JSON_FRAME_PREDICATE &&
      frame->pending_key != NULL && frame->selector != NULL &&
      key_is_ignore_case(frame->pending_key)) {
    frame->selector->ignore_case = value ? 1 : 0;
  } else {
    literal = value ? "true" : "false";
    ok = selector_json_store_scalar(state, frame, literal,
                                    LQL_SELECTOR_LITERAL_BOOL);
    if (!ok) {
      return selector_json_fail(
          state, error,
          state->error != NULL && state->error->code != LQL_STATUS_OK
              ? state->error->code
              : LQL_STATUS_PARSE_ERROR,
          state->error != NULL && state->error->message[0] != '\0'
              ? state->error->message
              : "unexpected selector JSON boolean");
    }
  }
  state->parser.allocator->destroy(state->parser.allocator, frame->pending_key);
  frame->pending_key = NULL;
  return LQL_STATUS_OK;
}

static lql_status selector_json_null(void *user, lql_error *error) {
  selector_json_state *state;
  selector_json_frame *frame;
  int ok;
  state = (selector_json_state *)user;
  frame = selector_json_top(state);
  ok = selector_json_store_scalar(state, frame, "null",
                                  LQL_SELECTOR_LITERAL_NULL);
  if (!ok) {
    return selector_json_fail(
        state, error,
        state->error != NULL && state->error->code != LQL_STATUS_OK
            ? state->error->code
            : LQL_STATUS_PARSE_ERROR,
        state->error != NULL && state->error->message[0] != '\0'
            ? state->error->message
            : "unexpected selector JSON null");
  }
  if (frame != NULL && frame->pending_key != NULL) {
    state->parser.allocator->destroy(state->parser.allocator,
                                     frame->pending_key);
    frame->pending_key = NULL;
  }
  return LQL_STATUS_OK;
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

typedef struct selector_json_scanner {
  const unsigned char *data;
  size_t len;
  size_t pos;
  selector_json_state *state;
  lql_error *error;
} selector_json_scanner;

static lql_status selector_json_scan_value(selector_json_scanner *scanner);

static lql_status selector_json_scan_fail(selector_json_scanner *scanner,
                                          const char *message) {
  return selector_json_fail(scanner->state, scanner->error,
                            LQL_STATUS_JSON_ERROR, message);
}

static void selector_json_scan_ws(selector_json_scanner *scanner) {
  while (scanner->pos < scanner->len) {
    switch (scanner->data[scanner->pos]) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
      ++scanner->pos;
      break;
    default:
      return;
    }
  }
}

static int selector_json_hex_value(unsigned char c) {
  if (c >= (unsigned char)'0' && c <= (unsigned char)'9') {
    return (int)(c - (unsigned char)'0');
  }
  if (c >= (unsigned char)'a' && c <= (unsigned char)'f') {
    return (int)(c - (unsigned char)'a') + 10;
  }
  if (c >= (unsigned char)'A' && c <= (unsigned char)'F') {
    return (int)(c - (unsigned char)'A') + 10;
  }
  return -1;
}

static int selector_json_scan_hex4(selector_json_scanner *scanner,
                                   unsigned long *out) {
  unsigned long value;
  int digit;
  size_t i;
  if (scanner->pos + 4u > scanner->len) {
    return 0;
  }
  value = 0u;
  for (i = 0u; i < 4u; ++i) {
    digit = selector_json_hex_value(scanner->data[scanner->pos + i]);
    if (digit < 0) {
      return 0;
    }
    value = (value << 4) | (unsigned long)digit;
  }
  scanner->pos += 4u;
  *out = value;
  return 1;
}

static size_t selector_json_utf8_encode(unsigned long cp, char out[4]) {
  if (cp <= 0x7ful) {
    out[0] = (char)cp;
    return 1u;
  }
  if (cp <= 0x7fful) {
    out[0] = (char)(0xc0u | (unsigned int)(cp >> 6));
    out[1] = (char)(0x80u | (unsigned int)(cp & 0x3fu));
    return 2u;
  }
  if (cp <= 0xfffful) {
    out[0] = (char)(0xe0u | (unsigned int)(cp >> 12));
    out[1] = (char)(0x80u | (unsigned int)((cp >> 6) & 0x3fu));
    out[2] = (char)(0x80u | (unsigned int)(cp & 0x3fu));
    return 3u;
  }
  out[0] = (char)(0xf0u | (unsigned int)(cp >> 18));
  out[1] = (char)(0x80u | (unsigned int)((cp >> 12) & 0x3fu));
  out[2] = (char)(0x80u | (unsigned int)((cp >> 6) & 0x3fu));
  out[3] = (char)(0x80u | (unsigned int)(cp & 0x3fu));
  return 4u;
}

static lql_status
selector_json_emit_string_chunk(selector_json_scanner *scanner, int key,
                                const char *data, size_t len) {
  if (len == 0u) {
    return LQL_STATUS_OK;
  }
  if (key) {
    return selector_json_key_chunk(scanner->state, data, len, scanner->error);
  }
  return selector_json_string_chunk(scanner->state, data, len, scanner->error);
}

static lql_status selector_json_scan_string(selector_json_scanner *scanner,
                                            int key) {
  unsigned char c;
  unsigned long cp;
  unsigned long low;
  const unsigned char *chunk;
  char out[4];
  size_t chunk_start;
  size_t out_len;
  lql_status st;
  if (scanner->pos >= scanner->len ||
      scanner->data[scanner->pos] != (unsigned char)'"') {
    return selector_json_scan_fail(scanner, "JSON string expected");
  }
  ++scanner->pos;
  if (key) {
    st = selector_json_begin_key(scanner->state, scanner->error);
  } else {
    st = selector_json_string_begin(scanner->state, scanner->error);
  }
  if (st != LQL_STATUS_OK) {
    return st;
  }
  chunk_start = scanner->pos;
  while (scanner->pos < scanner->len) {
    c = scanner->data[scanner->pos];
    if (c == (unsigned char)'"') {
      st = selector_json_emit_string_chunk(
          scanner, key, (const char *)&scanner->data[chunk_start],
          scanner->pos - chunk_start);
      if (st != LQL_STATUS_OK) {
        return st;
      }
      ++scanner->pos;
      return key ? selector_json_end_key(scanner->state, scanner->error)
                 : selector_json_string_end(scanner->state, scanner->error);
    }
    if (c < 0x20u) {
      return selector_json_scan_fail(scanner,
                                     "JSON string contains control byte");
    }
    if (c != (unsigned char)'\\') {
      ++scanner->pos;
      continue;
    }
    st = selector_json_emit_string_chunk(
        scanner, key, (const char *)&scanner->data[chunk_start],
        scanner->pos - chunk_start);
    if (st != LQL_STATUS_OK) {
      return st;
    }
    ++scanner->pos;
    if (scanner->pos >= scanner->len) {
      return selector_json_scan_fail(scanner,
                                     "JSON string escape is truncated");
    }
    c = scanner->data[scanner->pos++];
    switch (c) {
    case '"':
    case '\\':
    case '/':
      out[0] = (char)c;
      out_len = 1u;
      break;
    case 'b':
      out[0] = '\b';
      out_len = 1u;
      break;
    case 'f':
      out[0] = '\f';
      out_len = 1u;
      break;
    case 'n':
      out[0] = '\n';
      out_len = 1u;
      break;
    case 'r':
      out[0] = '\r';
      out_len = 1u;
      break;
    case 't':
      out[0] = '\t';
      out_len = 1u;
      break;
    case 'u':
      if (!selector_json_scan_hex4(scanner, &cp)) {
        return selector_json_scan_fail(scanner, "invalid JSON unicode escape");
      }
      if (cp >= 0xd800ul && cp <= 0xdbfful) {
        if (scanner->pos + 6u > scanner->len ||
            scanner->data[scanner->pos] != (unsigned char)'\\' ||
            scanner->data[scanner->pos + 1u] != (unsigned char)'u') {
          return selector_json_scan_fail(scanner,
                                         "JSON high surrogate is unpaired");
        }
        scanner->pos += 2u;
        if (!selector_json_scan_hex4(scanner, &low) || low < 0xdc00ul ||
            low > 0xdffful) {
          return selector_json_scan_fail(scanner,
                                         "JSON high surrogate is unpaired");
        }
        cp = 0x10000ul + (((cp - 0xd800ul) << 10) | (low - 0xdc00ul));
      } else if (cp >= 0xdc00ul && cp <= 0xdffful) {
        return selector_json_scan_fail(scanner,
                                       "JSON low surrogate is unpaired");
      }
      out_len = selector_json_utf8_encode(cp, out);
      break;
    default:
      return selector_json_scan_fail(scanner, "invalid JSON string escape");
    }
    st = selector_json_emit_string_chunk(scanner, key, out, out_len);
    if (st != LQL_STATUS_OK) {
      return st;
    }
    chunk = &scanner->data[scanner->pos];
    chunk_start = (size_t)(chunk - scanner->data);
  }
  return selector_json_scan_fail(scanner, "unterminated JSON string");
}

static lql_status selector_json_scan_number(selector_json_scanner *scanner) {
  size_t start;
  lql_status st;
  start = scanner->pos;
  if (scanner->data[scanner->pos] == (unsigned char)'-') {
    ++scanner->pos;
    if (scanner->pos >= scanner->len) {
      return selector_json_scan_fail(scanner, "invalid JSON number");
    }
  }
  if (scanner->data[scanner->pos] == (unsigned char)'0') {
    ++scanner->pos;
  } else if (scanner->data[scanner->pos] >= (unsigned char)'1' &&
             scanner->data[scanner->pos] <= (unsigned char)'9') {
    do {
      ++scanner->pos;
    } while (scanner->pos < scanner->len &&
             scanner->data[scanner->pos] >= (unsigned char)'0' &&
             scanner->data[scanner->pos] <= (unsigned char)'9');
  } else {
    return selector_json_scan_fail(scanner, "invalid JSON number");
  }
  if (scanner->pos < scanner->len &&
      scanner->data[scanner->pos] == (unsigned char)'.') {
    ++scanner->pos;
    if (scanner->pos >= scanner->len ||
        scanner->data[scanner->pos] < (unsigned char)'0' ||
        scanner->data[scanner->pos] > (unsigned char)'9') {
      return selector_json_scan_fail(scanner, "invalid JSON number");
    }
    do {
      ++scanner->pos;
    } while (scanner->pos < scanner->len &&
             scanner->data[scanner->pos] >= (unsigned char)'0' &&
             scanner->data[scanner->pos] <= (unsigned char)'9');
  }
  if (scanner->pos < scanner->len &&
      (scanner->data[scanner->pos] == (unsigned char)'e' ||
       scanner->data[scanner->pos] == (unsigned char)'E')) {
    ++scanner->pos;
    if (scanner->pos < scanner->len &&
        (scanner->data[scanner->pos] == (unsigned char)'+' ||
         scanner->data[scanner->pos] == (unsigned char)'-')) {
      ++scanner->pos;
    }
    if (scanner->pos >= scanner->len ||
        scanner->data[scanner->pos] < (unsigned char)'0' ||
        scanner->data[scanner->pos] > (unsigned char)'9') {
      return selector_json_scan_fail(scanner, "invalid JSON number");
    }
    do {
      ++scanner->pos;
    } while (scanner->pos < scanner->len &&
             scanner->data[scanner->pos] >= (unsigned char)'0' &&
             scanner->data[scanner->pos] <= (unsigned char)'9');
  }
  st = selector_json_number_begin(scanner->state, scanner->error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  st = selector_json_number_chunk(scanner->state,
                                  (const char *)&scanner->data[start],
                                  scanner->pos - start, scanner->error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  return selector_json_number_end(scanner->state, scanner->error);
}

static int selector_json_match_literal(selector_json_scanner *scanner,
                                       const char *literal) {
  size_t len;
  len = strlen(literal);
  if (scanner->pos + len > scanner->len ||
      memcmp(&scanner->data[scanner->pos], literal, len) != 0) {
    return 0;
  }
  scanner->pos += len;
  return 1;
}

static lql_status selector_json_scan_array(selector_json_scanner *scanner) {
  lql_status st;
  ++scanner->pos;
  st = selector_json_array_begin(scanner->state, scanner->error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  selector_json_scan_ws(scanner);
  if (scanner->pos < scanner->len &&
      scanner->data[scanner->pos] == (unsigned char)']') {
    ++scanner->pos;
    return selector_json_array_end(scanner->state, scanner->error);
  }
  for (;;) {
    st = selector_json_scan_value(scanner);
    if (st != LQL_STATUS_OK) {
      return st;
    }
    selector_json_scan_ws(scanner);
    if (scanner->pos >= scanner->len) {
      return selector_json_scan_fail(scanner, "unterminated JSON array");
    }
    if (scanner->data[scanner->pos] == (unsigned char)']') {
      ++scanner->pos;
      return selector_json_array_end(scanner->state, scanner->error);
    }
    if (scanner->data[scanner->pos] != (unsigned char)',') {
      return selector_json_scan_fail(scanner, "JSON array comma expected");
    }
    ++scanner->pos;
    selector_json_scan_ws(scanner);
  }
}

static lql_status selector_json_scan_object(selector_json_scanner *scanner) {
  lql_status st;
  ++scanner->pos;
  st = selector_json_object_begin(scanner->state, scanner->error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  selector_json_scan_ws(scanner);
  if (scanner->pos < scanner->len &&
      scanner->data[scanner->pos] == (unsigned char)'}') {
    ++scanner->pos;
    return selector_json_object_end(scanner->state, scanner->error);
  }
  for (;;) {
    st = selector_json_scan_string(scanner, 1);
    if (st != LQL_STATUS_OK) {
      return st;
    }
    selector_json_scan_ws(scanner);
    if (scanner->pos >= scanner->len ||
        scanner->data[scanner->pos] != (unsigned char)':') {
      return selector_json_scan_fail(scanner, "JSON object colon expected");
    }
    ++scanner->pos;
    selector_json_scan_ws(scanner);
    st = selector_json_scan_value(scanner);
    if (st != LQL_STATUS_OK) {
      return st;
    }
    selector_json_scan_ws(scanner);
    if (scanner->pos >= scanner->len) {
      return selector_json_scan_fail(scanner, "unterminated JSON object");
    }
    if (scanner->data[scanner->pos] == (unsigned char)'}') {
      ++scanner->pos;
      return selector_json_object_end(scanner->state, scanner->error);
    }
    if (scanner->data[scanner->pos] != (unsigned char)',') {
      return selector_json_scan_fail(scanner, "JSON object comma expected");
    }
    ++scanner->pos;
    selector_json_scan_ws(scanner);
  }
}

static lql_status selector_json_scan_value(selector_json_scanner *scanner) {
  selector_json_scan_ws(scanner);
  if (scanner->pos >= scanner->len) {
    return selector_json_scan_fail(scanner, "JSON value expected");
  }
  switch (scanner->data[scanner->pos]) {
  case '{':
    return selector_json_scan_object(scanner);
  case '[':
    return selector_json_scan_array(scanner);
  case '"':
    return selector_json_scan_string(scanner, 0);
  case 't':
    if (selector_json_match_literal(scanner, "true")) {
      return selector_json_boolean(scanner->state, 1, scanner->error);
    }
    break;
  case 'f':
    if (selector_json_match_literal(scanner, "false")) {
      return selector_json_boolean(scanner->state, 0, scanner->error);
    }
    break;
  case 'n':
    if (selector_json_match_literal(scanner, "null")) {
      return selector_json_null(scanner->state, scanner->error);
    }
    break;
  default:
    if (scanner->data[scanner->pos] == (unsigned char)'-' ||
        (scanner->data[scanner->pos] >= (unsigned char)'0' &&
         scanner->data[scanner->pos] <= (unsigned char)'9')) {
      return selector_json_scan_number(scanner);
    }
    break;
  }
  return selector_json_scan_fail(scanner, "invalid JSON value");
}

static lql_status selector_json_scan_document(selector_json_state *state,
                                              const void *json, size_t json_len,
                                              lql_error *error) {
  selector_json_scanner scanner;
  memset(&scanner, 0, sizeof(scanner));
  scanner.data = (const unsigned char *)json;
  scanner.len = json_len;
  scanner.state = state;
  scanner.error = error;
  selector_json_scan_ws(&scanner);
  if (scanner.pos >= scanner.len) {
    return selector_json_scan_fail(&scanner, "JSON value expected");
  }
  if (selector_json_scan_value(&scanner) != LQL_STATUS_OK) {
    return state->status == LQL_STATUS_OK ? LQL_STATUS_JSON_ERROR
                                          : state->status;
  }
  selector_json_scan_ws(&scanner);
  if (scanner.pos != scanner.len) {
    return selector_json_scan_fail(&scanner, "trailing data after JSON value");
  }
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL lql_status
lql_parse_selector_json_internal(lql *self, const void *json, size_t json_len,
                                 lql_selector **out, lql_error *error) {
  lql_allocator *allocator;
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
  st = selector_json_scan_document(&state, json, json_len, error);
  if (st != LQL_STATUS_OK) {
    if (error != NULL && error->message[0] == '\0') {
      lql_set_error(error, st, "selector JSON parse failed");
    }
  } else if (state.frame_count != 0u) {
    st = LQL_STATUS_PARSE_ERROR;
    lql_set_error(error, st, "selector JSON ended inside a value");
  }
  selector_json_state_cleanup(&state);
  if (st != LQL_STATUS_OK) {
    lql_selector_cleanup(self, state.selector);
    allocator->destroy(allocator, state.selector);
    return st;
  }
  if (!finalize_selector(&state.parser, state.selector)) {
    lql_selector_cleanup(self, state.selector);
    allocator->destroy(allocator, state.selector);
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  *out = state.selector;
  return LQL_STATUS_OK;
}

static lql_status builder_init(lql *self, lql_selector_parser *ctx,
                               lql_selector **out, lql_error *error) {
  lql_allocator *allocator;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "out selector required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = NULL;
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector builder receiver required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  ctx->receiver = self;
  ctx->allocator = allocator;
  return LQL_STATUS_OK;
}

static lql_status selector_from_built_root(lql_selector_parser *ctx,
                                           lql_selector *root,
                                           lql_selector **out,
                                           lql_error *error) {
  lql_selector *selector;
  selector = (lql_selector *)ctx->allocator->calloc(ctx->allocator, 1u,
                                                    sizeof(*selector));
  if (selector == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  *selector = *root;
  memset(root, 0, sizeof(*root));
  if (!finalize_selector(ctx, selector)) {
    lql_selector_cleanup(ctx->receiver, selector);
    ctx->allocator->destroy(ctx->allocator, selector);
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  *out = selector;
  return LQL_STATUS_OK;
}

static int clone_string(lql_selector_parser *ctx, const char *src, char **out) {
  if (src == NULL) {
    *out = NULL;
    return 1;
  }
  *out = ctx->allocator->strdup(ctx->allocator, src);
  return *out != NULL;
}

static int clone_selector_payload(lql_selector_parser *ctx, lql_selector *dst,
                                  const lql_selector *src) {
  size_t i;
  memset(dst, 0, sizeof(*dst));
  *dst = *src;
  dst->field = NULL;
  dst->value = NULL;
  dst->any = NULL;
  dst->any_lens = NULL;
  dst->any_kinds = NULL;
  dst->any_from_json = NULL;
  dst->any_count = 0u;
  dst->range_gt_text = NULL;
  dst->range_gte_text = NULL;
  dst->range_lt_text = NULL;
  dst->range_lte_text = NULL;
  dst->date_value_text = NULL;
  dst->date_since_text = NULL;
  dst->date_after_text = NULL;
  dst->date_before_text = NULL;
  dst->date_gt_text = NULL;
  dst->date_gte_text = NULL;
  dst->date_lt_text = NULL;
  dst->date_lte_text = NULL;
  if (!clone_string(ctx, src->field, &dst->field) ||
      !clone_string(ctx, src->value, &dst->value) ||
      !clone_string(ctx, src->range_gt_text, &dst->range_gt_text) ||
      !clone_string(ctx, src->range_gte_text, &dst->range_gte_text) ||
      !clone_string(ctx, src->range_lt_text, &dst->range_lt_text) ||
      !clone_string(ctx, src->range_lte_text, &dst->range_lte_text) ||
      !clone_string(ctx, src->date_value_text, &dst->date_value_text) ||
      !clone_string(ctx, src->date_since_text, &dst->date_since_text) ||
      !clone_string(ctx, src->date_after_text, &dst->date_after_text) ||
      !clone_string(ctx, src->date_before_text, &dst->date_before_text) ||
      !clone_string(ctx, src->date_gt_text, &dst->date_gt_text) ||
      !clone_string(ctx, src->date_gte_text, &dst->date_gte_text) ||
      !clone_string(ctx, src->date_lt_text, &dst->date_lt_text) ||
      !clone_string(ctx, src->date_lte_text, &dst->date_lte_text)) {
    return 0;
  }
  for (i = 0u; i < src->any_count; ++i) {
    char *copy;
    copy = lql_strndup_local(ctx, src->any[i], src->any_lens[i]);
    if (copy == NULL ||
        !selector_any_push(ctx, dst, copy, src->any_kinds[i],
                           src->any_from_json != NULL ? src->any_from_json[i]
                                                      : 0)) {
      ctx->allocator->destroy(ctx->allocator, copy);
      return 0;
    }
  }
  return 1;
}

static int clone_selector(lql_selector_parser *ctx, lql_selector *dst,
                          const lql_selector *src) {
  size_t i;
  memset(dst, 0, sizeof(*dst));
  dst->kind = src->kind;
  if (!clone_selector_payload(ctx, dst, src)) {
    return 0;
  }
  if (src->child_count == 0u) {
    return 1;
  }
  dst->children = (lql_selector *)ctx->allocator->calloc(
      ctx->allocator, src->child_count, sizeof(lql_selector));
  if (dst->children == NULL) {
    return 0;
  }
  dst->child_count = src->child_count;
  for (i = 0u; i < src->child_count; ++i) {
    if (!clone_selector(ctx, &dst->children[i], &src->children[i])) {
      return 0;
    }
  }
  return 1;
}

static lql_selector_kind node_kind_from_public(lql_selector_node_kind kind) {
  switch (kind) {
  case LQL_SELECTOR_NODE_AND:
    return LQL_SELECTOR_KIND_AND;
  case LQL_SELECTOR_NODE_OR:
    return LQL_SELECTOR_KIND_OR;
  case LQL_SELECTOR_NODE_NOT:
    return LQL_SELECTOR_KIND_NOT;
  case LQL_SELECTOR_NODE_EQ:
    return LQL_SELECTOR_KIND_EQ;
  case LQL_SELECTOR_NODE_CONTAINS:
    return LQL_SELECTOR_KIND_CONTAINS;
  case LQL_SELECTOR_NODE_ICONTAINS:
    return LQL_SELECTOR_KIND_ICONTAINS;
  case LQL_SELECTOR_NODE_PREFIX:
    return LQL_SELECTOR_KIND_PREFIX;
  case LQL_SELECTOR_NODE_IPREFIX:
    return LQL_SELECTOR_KIND_IPREFIX;
  case LQL_SELECTOR_NODE_RANGE:
    return LQL_SELECTOR_KIND_RANGE;
  case LQL_SELECTOR_NODE_DATE:
    return LQL_SELECTOR_KIND_DATE;
  case LQL_SELECTOR_NODE_IN:
    return LQL_SELECTOR_KIND_IN;
  case LQL_SELECTOR_NODE_EXISTS:
    return LQL_SELECTOR_KIND_EXISTS;
  case LQL_SELECTOR_NODE_ALL:
    return LQL_SELECTOR_KIND_ALL;
  }
  return LQL_SELECTOR_KIND_ALL;
}

static int view_has_value(lql_string_view view) {
  return view.data != NULL || view.len != 0u;
}

static int view_to_cstr(lql_selector_parser *ctx, lql_string_view view,
                        int allow_empty, char **out) {
  if (view.data == NULL && view.len != 0u) {
    return 0;
  }
  if (view.data == NULL) {
    if (!allow_empty) {
      return 0;
    }
    *out = ctx->allocator->strdup(ctx->allocator, "");
    return *out != NULL;
  }
  if (!allow_empty && view.len == 0u) {
    return 0;
  }
  *out = lql_strndup_local(ctx, view.data, view.len);
  return *out != NULL;
}

static int view_to_trimmed_cstr(lql_selector_parser *ctx, lql_string_view view,
                                int allow_empty, char **out) {
  if (view.data == NULL && view.len != 0u) {
    return 0;
  }
  if (view.data == NULL) {
    if (!allow_empty) {
      return 0;
    }
    *out = ctx->allocator->strdup(ctx->allocator, "");
    return *out != NULL;
  }
  *out = trim_dup(ctx, view.data, view.len);
  if (*out == NULL) {
    return 0;
  }
  if (!allow_empty && (*out)[0] == '\0') {
    ctx->allocator->destroy(ctx->allocator, *out);
    *out = NULL;
    return 0;
  }
  return 1;
}

static int build_field(lql_selector_parser *ctx, lql_string_view view,
                       char **out) {
  char *raw;
  char *normalized;
  raw = NULL;
  if (!view_to_cstr(ctx, view, 0, &raw)) {
    return 0;
  }
  normalized = normalize_field_path(ctx, raw);
  ctx->allocator->destroy(ctx->allocator, raw);
  if (normalized == NULL) {
    return 0;
  }
  *out = normalized;
  return 1;
}

static int build_any_values(lql_selector_parser *ctx, lql_selector *selector,
                            const lql_string_view *values, size_t count) {
  size_t i;
  char *copy;
  for (i = 0u; i < count; ++i) {
    copy = NULL;
    if (!view_to_cstr(ctx, values[i], 0, &copy)) {
      return 0;
    }
    if (!selector_any_push(ctx, selector, copy, LQL_SELECTOR_LITERAL_STRING,
                           0)) {
      ctx->allocator->destroy(ctx->allocator, copy);
      return 0;
    }
  }
  return 1;
}

static int build_range_bound(lql_selector_parser *ctx, lql_selector *selector,
                             const char *key, lql_selector_range_bound bound,
                             lql_error *error) {
  char number_buf[64];
  char *text;
  if (bound.kind == LQL_SELECTOR_BOUND_ABSENT) {
    return 1;
  }
  if (bound.kind == LQL_SELECTOR_BOUND_NUMBER) {
    if (!lql_number_format_json(bound.number, number_buf, sizeof(number_buf))) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "range selector numeric bound must be finite");
      return 0;
    }
    return set_range_bound(ctx, selector, key, number_buf, error);
  }
  if (bound.kind == LQL_SELECTOR_BOUND_DATETIME) {
    text = NULL;
    if (!view_to_trimmed_cstr(ctx, bound.datetime, 0, &text)) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "range selector datetime bound required");
      return 0;
    }
    if (!set_range_bound(ctx, selector, key, text, error)) {
      ctx->allocator->destroy(ctx->allocator, text);
      return 0;
    }
    ctx->allocator->destroy(ctx->allocator, text);
    return 1;
  }
  lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                "range selector bound kind invalid");
  return 0;
}

static int build_date_string(lql_selector_parser *ctx, lql_selector *selector,
                             const char *slot, lql_string_view value,
                             lql_error *error) {
  char *text;
  if (!view_has_value(value)) {
    return 1;
  }
  text = NULL;
  if (!view_to_cstr(ctx, value, 0, &text)) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector bound required");
    return 0;
  }
  if (!set_date_bound(ctx, selector, slot, text, error)) {
    ctx->allocator->destroy(ctx->allocator, text);
    return 0;
  }
  ctx->allocator->destroy(ctx->allocator, text);
  return 1;
}

static lql_status selector_build_result(lql_selector_parser *ctx,
                                        lql_selector *selector,
                                        lql_selector **out, lql_error *error) {
  lql_status st;
  st = selector_from_built_root(ctx, selector, out, error);
  if (st != LQL_STATUS_OK) {
    lql_selector_cleanup(ctx->receiver, selector);
  }
  return st;
}

LQL_INTERNAL_SYMBOL lql_status lql_selector_build_all_internal(
    lql *self, lql_selector **out, lql_error *error) {
  lql_selector_parser ctx;
  lql_selector root;
  lql_status st;
  st = builder_init(self, &ctx, out, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  memset(&root, 0, sizeof(root));
  root.kind = LQL_SELECTOR_KIND_ALL;
  return selector_from_built_root(&ctx, &root, out, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_selector_build_compound_internal(
    lql *self, lql_selector_node_kind kind, const lql_selector *const *children,
    size_t child_count, lql_selector **out, lql_error *error) {
  lql_selector_parser ctx;
  lql_selector root;
  lql_selector_kind internal_kind;
  size_t i;
  lql_status st;
  st = builder_init(self, &ctx, out, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  internal_kind = node_kind_from_public(kind);
  if (internal_kind != LQL_SELECTOR_KIND_AND &&
      internal_kind != LQL_SELECTOR_KIND_OR) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "compound selector kind must be AND or OR");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (child_count != 0u && children == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "compound selector children required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&root, 0, sizeof(root));
  root.kind = child_count == 0u ? LQL_SELECTOR_KIND_ALL : internal_kind;
  if (child_count != 0u) {
    root.children = (lql_selector *)ctx.allocator->calloc(
        ctx.allocator, child_count, sizeof(lql_selector));
    if (root.children == NULL) {
      lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
      return LQL_STATUS_NO_MEMORY;
    }
    root.child_count = child_count;
    for (i = 0u; i < child_count; ++i) {
      if (children[i] == NULL ||
          !clone_selector(&ctx, &root.children[i], children[i])) {
        lql_selector_cleanup(ctx.receiver, &root);
        lql_set_error(error,
                      children[i] == NULL ? LQL_STATUS_INVALID_ARGUMENT
                                          : LQL_STATUS_NO_MEMORY,
                      children[i] == NULL ? "compound selector child required"
                                          : "out of memory");
        return children[i] == NULL ? LQL_STATUS_INVALID_ARGUMENT
                                   : LQL_STATUS_NO_MEMORY;
      }
    }
  }
  return selector_build_result(&ctx, &root, out, error);
}

LQL_INTERNAL_SYMBOL lql_status
lql_selector_build_not_internal(lql *self, const lql_selector *child,
                                lql_selector **out, lql_error *error) {
  const lql_selector *children[1];
  lql_selector_parser ctx;
  lql_selector root;
  lql_status st;
  if (child == NULL) {
    if (out != NULL) {
      *out = NULL;
    }
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "not selector child required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  st = builder_init(self, &ctx, out, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  children[0] = child;
  memset(&root, 0, sizeof(root));
  root.kind = LQL_SELECTOR_KIND_NOT;
  root.children = (lql_selector *)ctx.allocator->calloc(ctx.allocator, 1u,
                                                        sizeof(lql_selector));
  if (root.children == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  root.child_count = 1u;
  if (!clone_selector(&ctx, &root.children[0], children[0])) {
    lql_selector_cleanup(ctx.receiver, &root);
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  return selector_build_result(&ctx, &root, out, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_selector_build_string_internal(
    lql *self, lql_selector_node_kind kind,
    const lql_selector_string_term *term, const lql_string_view *any_values,
    lql_selector **out, lql_error *error) {
  lql_selector_parser ctx;
  lql_selector selector;
  lql_selector_kind internal_kind;
  int has_value;
  lql_status st;
  st = builder_init(self, &ctx, out, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  if (term == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "string selector term required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  internal_kind = node_kind_from_public(kind);
  if (internal_kind != LQL_SELECTOR_KIND_EQ &&
      internal_kind != LQL_SELECTOR_KIND_CONTAINS &&
      internal_kind != LQL_SELECTOR_KIND_ICONTAINS &&
      internal_kind != LQL_SELECTOR_KIND_PREFIX &&
      internal_kind != LQL_SELECTOR_KIND_IPREFIX) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "string selector kind invalid");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&selector, 0, sizeof(selector));
  selector.kind = internal_kind;
  if (!build_field(&ctx, term->field, &selector.field)) {
    lql_selector_cleanup(ctx.receiver, &selector);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "selector field required");
    return LQL_STATUS_PARSE_ERROR;
  }
  has_value = term->value_present || term->value.len != 0u;
  if (term->any_count != 0u) {
    if (any_values == NULL) {
      lql_selector_cleanup(ctx.receiver, &selector);
      lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                    "selector any values required");
      return LQL_STATUS_INVALID_ARGUMENT;
    }
    if (internal_kind != LQL_SELECTOR_KIND_CONTAINS &&
        internal_kind != LQL_SELECTOR_KIND_ICONTAINS) {
      lql_selector_cleanup(ctx.receiver, &selector);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector operator does not support any");
      return LQL_STATUS_PARSE_ERROR;
    }
    if (has_value) {
      lql_selector_cleanup(ctx.receiver, &selector);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector cannot set both value and any");
      return LQL_STATUS_PARSE_ERROR;
    }
    if (!build_any_values(&ctx, &selector, any_values, term->any_count)) {
      lql_selector_cleanup(ctx.receiver, &selector);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector any requires values");
      return LQL_STATUS_PARSE_ERROR;
    }
  }
  if (has_value) {
    if (!view_to_cstr(&ctx, term->value, 1, &selector.value)) {
      lql_selector_cleanup(ctx.receiver, &selector);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR, "selector value invalid");
      return LQL_STATUS_PARSE_ERROR;
    }
    selector.value_set = term->value_present ? 1 : 0;
    selector.value_is_string = 1;
    selector.value_kind = LQL_SELECTOR_LITERAL_STRING;
  }
  selector.ignore_case = term->ignore_case ? 1 : 0;
  prepare_selector_value_temporal(&selector);
  return selector_build_result(&ctx, &selector, out, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_selector_build_range_internal(
    lql *self, const lql_selector_range_term *term, lql_selector **out,
    lql_error *error) {
  lql_selector_parser ctx;
  lql_selector selector;
  lql_status st;
  st = builder_init(self, &ctx, out, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  if (term == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "range selector term required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&selector, 0, sizeof(selector));
  selector.kind = LQL_SELECTOR_KIND_RANGE;
  if (!build_field(&ctx, term->field, &selector.field)) {
    lql_selector_cleanup(ctx.receiver, &selector);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "selector field required");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (!build_range_bound(&ctx, &selector, "gt", term->gt, error) ||
      !build_range_bound(&ctx, &selector, "gte", term->gte, error) ||
      !build_range_bound(&ctx, &selector, "lt", term->lt, error) ||
      !build_range_bound(&ctx, &selector, "lte", term->lte, error)) {
    lql_selector_cleanup(ctx.receiver, &selector);
    return error != NULL && error->code != LQL_STATUS_OK
               ? error->code
               : LQL_STATUS_PARSE_ERROR;
  }
  if (!selector.has_range_gt && !selector.has_range_gte &&
      !selector.has_range_lt && !selector.has_range_lte &&
      !selector.has_temporal_gt && !selector.has_temporal_gte &&
      !selector.has_temporal_lt && !selector.has_temporal_lte) {
    lql_selector_cleanup(ctx.receiver, &selector);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "range selector requires at least one bound");
    return LQL_STATUS_PARSE_ERROR;
  }
  if ((selector.has_range_gt || selector.has_range_gte ||
       selector.has_range_lt || selector.has_range_lte) &&
      (selector.has_temporal_gt || selector.has_temporal_gte ||
       selector.has_temporal_lt || selector.has_temporal_lte)) {
    lql_selector_cleanup(ctx.receiver, &selector);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "range selector cannot mix numeric and datetime bounds");
    return LQL_STATUS_PARSE_ERROR;
  }
  return selector_build_result(&ctx, &selector, out, error);
}

LQL_INTERNAL_SYMBOL lql_status
lql_selector_build_date_internal(lql *self, const lql_selector_date_term *term,
                                 lql_selector **out, lql_error *error) {
  lql_selector_parser ctx;
  lql_selector selector;
  lql_status st;
  const char *macro;
  lql_string_view macro_view;
  st = builder_init(self, &ctx, out, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  if (term == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "date selector term required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&selector, 0, sizeof(selector));
  selector.kind = LQL_SELECTOR_KIND_DATE;
  if (!build_field(&ctx, term->field, &selector.field)) {
    lql_selector_cleanup(ctx.receiver, &selector);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "selector field required");
    return LQL_STATUS_PARSE_ERROR;
  }
  macro_view = term->since;
  macro = NULL;
  if (!view_has_value(macro_view)) {
    if (term->since_kind == LQL_SELECTOR_SINCE_NOW) {
      macro = "now";
    } else if (term->since_kind == LQL_SELECTOR_SINCE_TODAY) {
      macro = "today";
    } else if (term->since_kind == LQL_SELECTOR_SINCE_YESTERDAY) {
      macro = "yesterday";
    } else if (term->since_kind == LQL_SELECTOR_SINCE_LITERAL) {
      lql_selector_cleanup(ctx.receiver, &selector);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "date selector since literal required");
      return LQL_STATUS_PARSE_ERROR;
    }
    if (macro != NULL) {
      macro_view.data = macro;
      macro_view.len = strlen(macro);
    }
  }
  if (!build_date_string(&ctx, &selector, "value", term->value, error) ||
      !build_date_string(&ctx, &selector, "since", macro_view, error) ||
      !build_date_string(&ctx, &selector, "after", term->after, error) ||
      !build_date_string(&ctx, &selector, "before", term->before, error) ||
      !build_date_string(&ctx, &selector, "gt", term->gt, error) ||
      !build_date_string(&ctx, &selector, "gte", term->gte, error) ||
      !build_date_string(&ctx, &selector, "lt", term->lt, error) ||
      !build_date_string(&ctx, &selector, "lte", term->lte, error)) {
    lql_selector_cleanup(ctx.receiver, &selector);
    return error != NULL && error->code != LQL_STATUS_OK
               ? error->code
               : LQL_STATUS_PARSE_ERROR;
  }
  if (view_has_value(macro_view) &&
      (view_has_value(term->value) || view_has_value(term->after) ||
       view_has_value(term->before) || view_has_value(term->gt) ||
       view_has_value(term->gte) || view_has_value(term->lt) ||
       view_has_value(term->lte))) {
    lql_selector_cleanup(ctx.receiver, &selector);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector since cannot be combined with other bounds");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (view_has_value(term->after) && view_has_value(term->gt)) {
    lql_selector_cleanup(ctx.receiver, &selector);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector cannot combine after and gt");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (view_has_value(term->before) && view_has_value(term->lt)) {
    lql_selector_cleanup(ctx.receiver, &selector);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector cannot combine before and lt");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (!selector.has_temporal_eq && !selector.has_temporal_gt &&
      !selector.has_temporal_gte && !selector.has_temporal_lt &&
      !selector.has_temporal_lte && selector.since_macro == LQL_SINCE_NONE) {
    lql_selector_cleanup(ctx.receiver, &selector);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector requires at least one bound");
    return LQL_STATUS_PARSE_ERROR;
  }
  return selector_build_result(&ctx, &selector, out, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_selector_build_in_internal(
    lql *self, const lql_selector_in_term *term,
    const lql_string_view *any_values, lql_selector **out, lql_error *error) {
  lql_selector_parser ctx;
  lql_selector selector;
  lql_status st;
  st = builder_init(self, &ctx, out, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  if (term == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "in selector term required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&selector, 0, sizeof(selector));
  selector.kind = LQL_SELECTOR_KIND_IN;
  if (!build_field(&ctx, term->field, &selector.field)) {
    lql_selector_cleanup(ctx.receiver, &selector);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "selector field required");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (term->any_count == 0u || any_values == NULL) {
    lql_selector_cleanup(ctx.receiver, &selector);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "in selector requires any values");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (!build_any_values(&ctx, &selector, any_values, term->any_count)) {
    lql_selector_cleanup(ctx.receiver, &selector);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "in selector requires non-empty any values");
    return LQL_STATUS_PARSE_ERROR;
  }
  return selector_build_result(&ctx, &selector, out, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_selector_build_exists_internal(
    lql *self, lql_string_view path, lql_selector **out, lql_error *error) {
  lql_selector_parser ctx;
  lql_selector selector;
  lql_status st;
  st = builder_init(self, &ctx, out, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  memset(&selector, 0, sizeof(selector));
  selector.kind = LQL_SELECTOR_KIND_EXISTS;
  if (!build_field(&ctx, path, &selector.field)) {
    lql_selector_cleanup(ctx.receiver, &selector);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "exists selector requires exactly one path");
    return LQL_STATUS_PARSE_ERROR;
  }
  return selector_build_result(&ctx, &selector, out, error);
}

typedef struct indexed_group {
  int indexed;
  lql_selector_kind wrapper;
  char *index;
  lql_selector selector;
  lql_selector or_group;
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
    lql_selector_cleanup(ctx->receiver, &groups[i].selector);
    lql_selector_cleanup(ctx->receiver, &groups[i].or_group);
    indexed_groups_cleanup(ctx, groups[i].groups, groups[i].group_count);
  }
  ctx->allocator->destroy(ctx->allocator, groups);
}

static lql_status finalize_indexed_group(lql_selector_parser *ctx,
                                         indexed_group *group,
                                         int root_or_mode);

static int parse_indexed_wrapper(lql_selector_parser *ctx, const char *token,
                                 lql_selector_kind *wrapper, char **out_index,
                                 const char **out_rest) {
  const char *p;
  const char *idx;
  size_t len;
  char *copy;
  if (strncmp(token, "and.", 4u) == 0) {
    *wrapper = LQL_SELECTOR_KIND_AND;
    p = token + 4;
  } else if (strncmp(token, "or.", 3u) == 0) {
    *wrapper = LQL_SELECTOR_KIND_OR;
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
                                         lql_selector_kind wrapper,
                                         const char *index) {
  size_t i;
  for (i = 0u; i < count; ++i) {
    if (groups[i].wrapper == wrapper && strcmp(groups[i].index, index) == 0) {
      return &groups[i];
    }
  }
  return NULL;
}

static int parse_simple_wrapper(const char *token, lql_selector_kind *wrapper,
                                const char **out_rest) {
  if (strncmp(token, "and.", 4u) == 0) {
    *wrapper = LQL_SELECTOR_KIND_AND;
    *out_rest = token + 4;
    return 1;
  }
  if (strncmp(token, "or.", 3u) == 0) {
    *wrapper = LQL_SELECTOR_KIND_OR;
    *out_rest = token + 3;
    return 1;
  }
  if (strncmp(token, "not.", 4u) == 0) {
    *wrapper = LQL_SELECTOR_KIND_NOT;
    *out_rest = token + 4;
    return 1;
  }
  return 0;
}

static int token_contains_indexed_wrapper(lql_selector_parser *ctx,
                                          const char *token, int *oom) {
  lql_selector_kind wrapper;
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

static int selector_conflicts_with_child(const lql_selector *selector,
                                         const lql_selector *child) {
  size_t i;
  const lql_selector *current;
  if (selector == NULL || child == NULL) {
    return 0;
  }
  if (!selector_is_predicate(child) && child->kind != LQL_SELECTOR_KIND_NOT) {
    return 0;
  }
  for (i = 0u; i < selector->child_count; ++i) {
    current = &selector->children[i];
    if (current->kind == child->kind) {
      return 1;
    }
  }
  return 0;
}

static indexed_group *
ensure_indexed_group(lql_selector_parser *ctx, indexed_group **groups,
                     size_t *count, lql_selector_kind wrapper, char **index) {
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
    group->selector.kind = LQL_SELECTOR_KIND_AND;
    ++*count;
  }
  return group;
}

static lql_status append_plain_group_node(lql_selector_parser *ctx,
                                          indexed_group *group,
                                          lql_selector *child,
                                          lql_error *error) {
  if (child->kind == LQL_SELECTOR_KIND_OR && child->child_count == 1u) {
    if (group->or_group.kind == LQL_SELECTOR_KIND_ALL) {
      group->or_group.kind = LQL_SELECTOR_KIND_OR;
    }
    if (group->indexed &&
        selector_conflicts_with_child(&group->or_group, &child->children[0])) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector expression has conflicting indexed clauses");
      return LQL_STATUS_PARSE_ERROR;
    }
    if (!append_selector(ctx, &group->or_group, &child->children[0])) {
      return LQL_STATUS_NO_MEMORY;
    }
    ctx->allocator->destroy(ctx->allocator, child->children);
    child->children = NULL;
    child->child_count = 0u;
    return LQL_STATUS_OK;
  }
  if (group->indexed &&
      selector_conflicts_with_child(&group->selector, child)) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "selector expression has conflicting indexed clauses");
    return LQL_STATUS_PARSE_ERROR;
  }
  if (!append_selector(ctx, &group->selector, child)) {
    return LQL_STATUS_NO_MEMORY;
  }
  return LQL_STATUS_OK;
}

static lql_status append_token_to_group(lql_selector_parser *ctx,
                                        indexed_group *group, const char *token,
                                        lql_error *error) {
  lql_selector_kind wrapper;
  indexed_group *child_group;
  const char *rest;
  char *index;
  lql_selector child;
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
    wrapped.selector.kind = wrapper;
    st = append_token_to_group(ctx, &wrapped, rest, error);
    if (st == LQL_STATUS_OK) {
      st = finalize_indexed_group(ctx, &wrapped,
                                  wrapper == LQL_SELECTOR_KIND_OR);
    }
    if (st == LQL_STATUS_OK) {
      st = append_plain_group_node(ctx, group, &wrapped.selector, error);
    }
    lql_selector_cleanup(ctx->receiver, &wrapped.selector);
    lql_selector_cleanup(ctx->receiver, &wrapped.or_group);
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
    lql_selector_cleanup(ctx->receiver, &child);
  }
  return st;
}

static lql_status finalize_indexed_group(lql_selector_parser *ctx,
                                         indexed_group *group,
                                         int root_or_mode) {
  size_t i;
  lql_status st;

  if (group->or_group.kind == LQL_SELECTOR_KIND_ALL) {
    group->or_group.kind = LQL_SELECTOR_KIND_OR;
  }
  for (i = 0u; i < group->group_count; ++i) {
    st = finalize_indexed_group(ctx, &group->groups[i], 0);
    if (st != LQL_STATUS_OK) {
      return st;
    }
    if (!root_or_mode && group->groups[i].wrapper == LQL_SELECTOR_KIND_OR) {
      if (!append_selector(ctx, &group->or_group, &group->groups[i].selector)) {
        return LQL_STATUS_NO_MEMORY;
      }
    } else if (!append_selector(ctx, &group->selector,
                                &group->groups[i].selector)) {
      return LQL_STATUS_NO_MEMORY;
    }
  }
  if (group->or_group.child_count != 0u) {
    if (!append_selector(ctx, &group->selector, &group->or_group)) {
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
  lql_selector_kind wrapper;
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
    selector->kind = LQL_SELECTOR_KIND_ALL;
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
    selector->kind = LQL_SELECTOR_KIND_ALL;
  } else if (tokens.count == 1u) {
    index = NULL;
    rest = NULL;
    wrapper_status =
        parse_indexed_wrapper(ctx, tokens.items[0], &wrapper, &index, &rest);
    if (wrapper_status < 0) {
      st = LQL_STATUS_NO_MEMORY;
    } else if (wrapper_status == 0) {
      wrapper_probe_oom = 0;
      needs_group = token_contains_indexed_wrapper(ctx, tokens.items[0],
                                                   &wrapper_probe_oom);
      if (wrapper_probe_oom) {
        st = LQL_STATUS_NO_MEMORY;
      } else if (!needs_group) {
        st = parse_one(ctx, tokens.items[0], selector, error);
      } else {
        memset(&root_group, 0, sizeof(root_group));
        root_group.selector.kind =
            or_mode ? LQL_SELECTOR_KIND_OR : LQL_SELECTOR_KIND_AND;
        st = append_token_to_group(ctx, &root_group, tokens.items[0], error);
        if (st == LQL_STATUS_OK) {
          st = finalize_indexed_group(ctx, &root_group, or_mode);
        }
        if (st == LQL_STATUS_OK) {
          *selector = root_group.selector;
          memset(&root_group.selector, 0, sizeof(root_group.selector));
        }
        lql_selector_cleanup(ctx->receiver, &root_group.selector);
        indexed_groups_cleanup(ctx, root_group.groups, root_group.group_count);
      }
    } else {
      memset(&root_group, 0, sizeof(root_group));
      root_group.selector.kind =
          or_mode ? LQL_SELECTOR_KIND_OR : LQL_SELECTOR_KIND_AND;
      st = append_token_to_group(ctx, &root_group, tokens.items[0], error);
      if (st == LQL_STATUS_OK) {
        st = finalize_indexed_group(ctx, &root_group, or_mode);
      }
      if (st == LQL_STATUS_OK) {
        *selector = root_group.selector;
        memset(&root_group.selector, 0, sizeof(root_group.selector));
      }
      lql_selector_cleanup(ctx->receiver, &root_group.selector);
      indexed_groups_cleanup(ctx, root_group.groups, root_group.group_count);
    }
    ctx->allocator->destroy(ctx->allocator, index);
  } else {
    memset(&root_group, 0, sizeof(root_group));
    root_group.selector.kind =
        or_mode ? LQL_SELECTOR_KIND_OR : LQL_SELECTOR_KIND_AND;
    for (i = 0u; i < tokens.count && st == LQL_STATUS_OK; ++i) {
      st = append_token_to_group(ctx, &root_group, tokens.items[i], error);
    }
    if (st == LQL_STATUS_OK) {
      st = finalize_indexed_group(ctx, &root_group, or_mode);
    }
    if (st == LQL_STATUS_OK) {
      *selector = root_group.selector;
      memset(&root_group.selector, 0, sizeof(root_group.selector));
    }
    lql_selector_cleanup(ctx->receiver, &root_group.selector);
    indexed_groups_cleanup(ctx, root_group.groups, root_group.group_count);
  }
  token_list_cleanup(ctx, &tokens);
  if (st != LQL_STATUS_OK) {
    lql_selector_cleanup(ctx->receiver, selector);
    ctx->allocator->destroy(ctx->allocator, selector);
    return st;
  }
  if (!finalize_selector(ctx, selector)) {
    lql_selector_cleanup(ctx->receiver, selector);
    ctx->allocator->destroy(ctx->allocator, selector);
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  *out = selector;
  return LQL_STATUS_OK;
}
