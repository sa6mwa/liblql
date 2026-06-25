#include "lql_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct lql_token_list {
  char **items;
  size_t count;
} lql_token_list;

static char *lql_strndup_local(const char *src, size_t len) {
  char *out;
  out = (char *)malloc(len + 1u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, src, len);
  out[len] = '\0';
  return out;
}

static char *trim_dup(const char *src, size_t len) {
  while (len > 0u && isspace((unsigned char)*src)) {
    ++src;
    --len;
  }
  while (len > 0u && isspace((unsigned char)src[len - 1u])) {
    --len;
  }
  return lql_strndup_local(src, len);
}

static void token_list_cleanup(lql_token_list *list) {
  size_t i;
  for (i = 0u; i < list->count; ++i) {
    free(list->items[i]);
  }
  free(list->items);
  list->items = NULL;
  list->count = 0u;
}

static int token_list_push(lql_token_list *list, char *item) {
  char **next;
  next = (char **)realloc(list->items, sizeof(char *) * (list->count + 1u));
  if (next == NULL) {
    return 0;
  }
  list->items = next;
  list->items[list->count++] = item;
  return 1;
}

static int term_any_push(lql_term *term, char *item) {
  char **next;
  next = (char **)realloc(term->any, sizeof(char *) * (term->any_count + 1u));
  if (next == NULL) {
    return 0;
  }
  term->any = next;
  term->any[term->any_count++] = item;
  return 1;
}

static lql_status split_top(const char *expr, lql_token_list *out,
                            lql_error *error) {
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
      item = trim_dup(start, (size_t)(p - start));
      if (item == NULL) {
        return LQL_STATUS_NO_MEMORY;
      }
      if (item[0] != '\0' && !token_list_push(out, item)) {
        free(item);
        return LQL_STATUS_NO_MEMORY;
      }
      if (item[0] == '\0') {
        free(item);
      }
      start = p + 1;
    }
  }
  if (quote != 0 || depth != 0u) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "unterminated selector expression");
    return LQL_STATUS_PARSE_ERROR;
  }
  item = trim_dup(start, (size_t)(p - start));
  if (item == NULL) {
    return LQL_STATUS_NO_MEMORY;
  }
  if (item[0] != '\0' && !token_list_push(out, item)) {
    free(item);
    return LQL_STATUS_NO_MEMORY;
  }
  if (item[0] == '\0') {
    free(item);
  }
  return LQL_STATUS_OK;
}

static char *unquote(char *value) {
  size_t len;
  char *out;
  char *w;
  char *r;
  len = strlen(value);
  if (len >= 2u && ((value[0] == '"' && value[len - 1u] == '"') ||
                    (value[0] == '\'' && value[len - 1u] == '\''))) {
    out = lql_strndup_local(value + 1, len - 2u);
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
  return lql_strdup(value);
}

static int append_text(char **buf, size_t *len, size_t *cap, const char *text,
                       size_t n) {
  char *next;
  size_t next_cap;
  if (*len + n + 1u > *cap) {
    next_cap = *cap == 0u ? 32u : *cap;
    while (*len + n + 1u > next_cap) {
      next_cap *= 2u;
    }
    next = (char *)realloc(*buf, next_cap);
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

static char *normalize_field_path(const char *field) {
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
    return lql_strdup(field);
  }
  out = NULL;
  out_len = 0u;
  out_cap = 0u;
  seg = field + 1;
  if (!append_text(&out, &out_len, &out_cap, "/", 1u)) {
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
      if (!append_text(&out, &out_len, &out_cap, "/", 1u)) {
        free(out);
        return NULL;
      }
    }
    if (!append_text(&out, &out_len, &out_cap, seg,
                     count == 0u ? len : base_len)) {
      free(out);
      return NULL;
    }
    for (i = 0u; i < count; ++i) {
      if (!append_text(&out, &out_len, &out_cap, "/[]", 3u)) {
        free(out);
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

static void free_seen_key_values(char *field, char *value, char *any,
                                 char *ignore_case, char *gt, char *gte,
                                 char *lt, char *lte, char *after, char *before,
                                 char *since) {
  free(field);
  free(value);
  free(any);
  free(ignore_case);
  free(gt);
  free(gte);
  free(lt);
  free(lte);
  free(after);
  free(before);
  free(since);
}

static int remember_key_value(char **slot, char **raw_value, int *skip,
                              lql_error *error) {
  *skip = 0;
  if (*slot == NULL) {
    *slot = *raw_value;
    *raw_value = NULL;
    return 1;
  }
  if (strcmp(*slot, *raw_value) == 0) {
    free(*raw_value);
    *raw_value = NULL;
    *skip = 1;
    return 1;
  }
  free(*raw_value);
  *raw_value = NULL;
  lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                "selector expression has duplicate key");
  return 0;
}

static lql_node_kind kind_from_name(const char *name) {
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

static int parse_any_values(char *decoded, lql_term *term, lql_error *error) {
  char *cursor;
  char *bar;
  char *item;

  cursor = decoded;
  while (cursor != NULL) {
    bar = strchr(cursor, '|');
    if (bar != NULL) {
      *bar = '\0';
    }
    item = trim_dup(cursor, strlen(cursor));
    if (item == NULL) {
      return 0;
    }
    if (item[0] != '\0') {
      if (!term_any_push(term, item)) {
        free(item);
        return 0;
      }
    } else {
      free(item);
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

static int set_range_bound(lql_term *term, const char *key, const char *decoded,
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
      term->temporal_gt = temporal;
      term->has_temporal_gt = 1;
    } else if (strcmp(key, "gte") == 0) {
      term->temporal_gte = temporal;
      term->has_temporal_gte = 1;
    } else if (strcmp(key, "lt") == 0) {
      term->temporal_lt = temporal;
      term->has_temporal_lt = 1;
    } else {
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

static int set_date_bound(lql_term *term, const char *slot, const char *decoded,
                          lql_error *error) {
  lql_temporal temporal;
  if (strcmp(slot, "since") == 0) {
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
    term->temporal_eq = temporal;
    term->has_temporal_eq = 1;
  } else if (strcmp(slot, "since") == 0) {
    term->temporal_gte = temporal;
    term->has_temporal_gte = 1;
  } else if (strcmp(slot, "gt") == 0) {
    term->temporal_gt = temporal;
    term->has_temporal_gt = 1;
  } else if (strcmp(slot, "gte") == 0) {
    term->temporal_gte = temporal;
    term->has_temporal_gte = 1;
  } else if (strcmp(slot, "lt") == 0) {
    term->temporal_lt = temporal;
    term->has_temporal_lt = 1;
  } else if (strcmp(slot, "lte") == 0) {
    term->temporal_lte = temporal;
    term->has_temporal_lte = 1;
  }
  return 1;
}

static int parse_key_values(char *body, lql_node_kind kind, lql_term *term,
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
  st = split_top(body, &parts, error);
  if (st != LQL_STATUS_OK) {
    return 0;
  }
  for (i = 0u; i < parts.count; ++i) {
    eq = strchr(parts.items[i], '=');
    if (eq == NULL) {
      token_list_cleanup(&parts);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector term requires key=value");
      return 0;
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
    while (isspace((unsigned char)*val)) {
      ++val;
    }
    if (!key_allowed_for_kind(kind, key)) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector operator does not support key");
      goto fail;
    }
    raw_value = trim_dup(val, strlen(val));
    if (raw_value == NULL) {
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
      free(raw_value);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector operator does not support key");
      goto fail;
    }
    if (!remember_key_value(seen_slot, &raw_value, &skip_duplicate, error)) {
      goto fail;
    }
    if (skip_duplicate) {
      continue;
    }
    decoded = unquote(*seen_slot);
    if (decoded == NULL) {
      goto fail;
    }
    if (key_is_field(key)) {
      normalized = normalize_field_path(decoded);
      free(decoded);
      if (normalized == NULL) {
        goto fail;
      }
      free(term->field);
      term->field = normalized;
    } else if (kind == LQL_NODE_DATE && key_is_value(key)) {
      if (!set_date_bound(term, "value", decoded, error)) {
        free(decoded);
        goto fail;
      }
      free(decoded);
    } else if (key_is_value(key)) {
      free(term->value);
      term->value = decoded;
      term->value_set = 1;
    } else if (kind == LQL_NODE_DATE &&
               (key_is_after(key) || key_is_before(key) || key_is_since(key))) {
      date_slot = key_is_after(key)    ? "gt"
                  : key_is_before(key) ? "lt"
                                       : "since";
      if (!set_date_bound(term, date_slot, decoded, error)) {
        free(decoded);
        goto fail;
      }
      free(decoded);
    } else if (key_is_any(key)) {
      if (!parse_any_values(decoded, term, error)) {
        free(decoded);
        goto fail;
      }
      free(decoded);
    } else if (key_is_ignore_case(key)) {
      if (!parse_bool_value(decoded, &term->ignore_case)) {
        free(decoded);
        lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                      "selector ignoreCase must be true/false/t/f");
        goto fail;
      }
      free(decoded);
    } else if (key_is_range_bound(key)) {
      if (kind == LQL_NODE_DATE) {
        if (!set_date_bound(term, key, decoded, error)) {
          free(decoded);
          goto fail;
        }
      } else if (!set_range_bound(term, key, decoded, error)) {
        free(decoded);
        goto fail;
      }
      free(decoded);
    }
  }
  token_list_cleanup(&parts);
  if (term->field == NULL) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "selector field required");
    goto fail_after_tokens;
  }
  if (kind == LQL_NODE_DATE && seen_since != NULL &&
      (seen_value != NULL || seen_after != NULL || seen_before != NULL ||
       seen_gt != NULL || seen_gte != NULL || seen_lt != NULL ||
       seen_lte != NULL)) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector since cannot be combined with other bounds");
    goto fail_after_tokens;
  }
  if (kind == LQL_NODE_DATE && seen_after != NULL && seen_gt != NULL) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector cannot combine after and gt");
    goto fail_after_tokens;
  }
  if (kind == LQL_NODE_DATE && seen_before != NULL && seen_lt != NULL) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector cannot combine before and lt");
    goto fail_after_tokens;
  }
  if (term->any_count != 0u) {
    if (kind != LQL_NODE_CONTAINS && kind != LQL_NODE_ICONTAINS &&
        kind != LQL_NODE_IN) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector operator does not support any");
      goto fail_after_tokens;
    }
    if (kind != LQL_NODE_IN && (term->value_set || term->value != NULL)) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "selector cannot set both value and any");
      goto fail_after_tokens;
    }
  }
  if (kind == LQL_NODE_IN && term->any_count == 0u) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "in selector requires any values");
    goto fail_after_tokens;
  }
  if (kind == LQL_NODE_RANGE && !term->has_range_gt && !term->has_range_gte &&
      !term->has_range_lt && !term->has_range_lte && !term->has_temporal_gt &&
      !term->has_temporal_gte && !term->has_temporal_lt &&
      !term->has_temporal_lte) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "range selector requires at least one bound");
    goto fail_after_tokens;
  }
  if (kind == LQL_NODE_RANGE &&
      (term->has_range_gt || term->has_range_gte || term->has_range_lt ||
       term->has_range_lte) &&
      (term->has_temporal_gt || term->has_temporal_gte ||
       term->has_temporal_lt || term->has_temporal_lte)) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "range selector cannot mix numeric and datetime bounds");
    goto fail_after_tokens;
  }
  if (kind == LQL_NODE_DATE && !term->has_temporal_eq &&
      !term->has_temporal_gt && !term->has_temporal_gte &&
      !term->has_temporal_lt && !term->has_temporal_lte &&
      term->since_macro == LQL_SINCE_NONE) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "date selector requires at least one bound");
    goto fail_after_tokens;
  }
  free_seen_key_values(seen_field, seen_value, seen_any, seen_ignore_case,
                       seen_gt, seen_gte, seen_lt, seen_lte, seen_after,
                       seen_before, seen_since);
  return 1;

fail:
  token_list_cleanup(&parts);
fail_after_tokens:
  free_seen_key_values(seen_field, seen_value, seen_any, seen_ignore_case,
                       seen_gt, seen_gte, seen_lt, seen_lte, seen_after,
                       seen_before, seen_since);
  return 0;
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

static lql_status parse_one(const char *expr, lql_node *out, lql_error *error) {
  char *copy;
  char *body;
  char *close;
  char *dot;
  char *op;
  char *value;
  char *name;
  lql_node child;
  lql_status st;

  memset(out, 0, sizeof(*out));
  copy = lql_strdup(expr);
  if (copy == NULL) {
    return LQL_STATUS_NO_MEMORY;
  }
  if (strcmp(copy, "/") == 0 || strcmp(copy, ".") == 0 ||
      strcmp(copy, "{}") == 0) {
    out->kind = LQL_NODE_ALL;
    free(copy);
    return LQL_STATUS_OK;
  }
  if (strncmp(copy, "and.", 4u) == 0 || strncmp(copy, "or.", 3u) == 0 ||
      strncmp(copy, "not.", 4u) == 0) {
    dot = strchr(copy, '.');
    name = copy;
    *dot = '\0';
    st = parse_one(dot + 1, &child, error);
    if (st != LQL_STATUS_OK) {
      free(copy);
      return st;
    }
    out->kind = strcmp(name, "and") == 0  ? LQL_NODE_AND
                : strcmp(name, "or") == 0 ? LQL_NODE_OR
                                          : LQL_NODE_NOT;
    out->children = (lql_node *)calloc(1u, sizeof(lql_node));
    if (out->children == NULL) {
      lql_node_cleanup(&child);
      free(copy);
      return LQL_STATUS_NO_MEMORY;
    }
    out->children[0] = child;
    out->child_count = 1u;
    free(copy);
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
    out->term.field = normalize_field_path(copy);
    out->term.value = unquote(value);
    out->term.value_set = 1;
    if (out->term.field == NULL || out->term.value == NULL) {
      free(copy);
      return LQL_STATUS_NO_MEMORY;
    }
    if (op0 == '!') {
      memset(&child, 0, sizeof(child));
      child.kind = LQL_NODE_EQ;
      child.term = out->term;
      memset(&out->term, 0, sizeof(out->term));
      out->children = (lql_node *)calloc(1u, sizeof(lql_node));
      if (out->children == NULL) {
        lql_node_cleanup(&child);
        free(copy);
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
        if (!set_range_bound(&out->term, bound_key, out->term.value, error)) {
          lql_node_cleanup(out);
          free(copy);
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
    free(copy);
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
      free(copy);
      return LQL_STATUS_PARSE_ERROR;
    }
    if (out->kind == LQL_NODE_EXISTS) {
      value = unquote(body + 1);
      out->term.field = value == NULL ? NULL : normalize_field_path(value);
      free(value);
      free(copy);
      return out->term.field == NULL ? LQL_STATUS_NO_MEMORY : LQL_STATUS_OK;
    }
    if (!parse_key_values(body + 1, out->kind, &out->term, error)) {
      free(copy);
      if (error != NULL && error->code != LQL_STATUS_OK) {
        return error->code;
      }
      return LQL_STATUS_NO_MEMORY;
    }
    if (string_term_is_match_all_alias(out->kind, &out->term)) {
      lql_node_cleanup(out);
      out->kind = LQL_NODE_ALL;
    }
    free(copy);
    return LQL_STATUS_OK;
  }

  lql_set_error(error, LQL_STATUS_PARSE_ERROR, "invalid selector expression");
  free(copy);
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

static int append_node(lql_node *parent, lql_node *child) {
  lql_node *next;
  next = (lql_node *)realloc(parent->children,
                             sizeof(lql_node) * (parent->child_count + 1u));
  if (next == NULL) {
    return 0;
  }
  parent->children = next;
  parent->children[parent->child_count] = *child;
  ++parent->child_count;
  memset(child, 0, sizeof(*child));
  return 1;
}

typedef struct indexed_group {
  lql_node_kind wrapper;
  char *index;
  lql_node node;
} indexed_group;

static void indexed_groups_cleanup(indexed_group *groups, size_t count) {
  size_t i;
  if (groups == NULL) {
    return;
  }
  for (i = 0u; i < count; ++i) {
    free(groups[i].index);
    lql_node_cleanup(&groups[i].node);
  }
  free(groups);
}

static int parse_indexed_wrapper(const char *token, lql_node_kind *wrapper,
                                 char **out_index, const char **out_rest) {
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
  copy = (char *)malloc(len + 1u);
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

static int indexed_group_conflicts(const indexed_group *group,
                                   const lql_node *child) {
  size_t i;
  const lql_node *current;
  if (group == NULL || child == NULL || child->kind != LQL_NODE_EQ ||
      child->term.field == NULL) {
    return 0;
  }
  for (i = 0u; i < group->node.child_count; ++i) {
    current = &group->node.children[i];
    if (current->kind == LQL_NODE_EQ && current->term.field != NULL &&
        strcmp(current->term.field, child->term.field) == 0 &&
        strcmp(current->term.value == NULL ? "" : current->term.value,
               child->term.value == NULL ? "" : child->term.value) != 0) {
      return 1;
    }
  }
  return 0;
}

static int append_indexed_group(indexed_group **groups, size_t *count,
                                lql_node_kind wrapper, char **index,
                                lql_node *child, lql_error *error) {
  indexed_group *group;
  indexed_group *next;
  group = find_indexed_group(*groups, *count, wrapper, *index);
  if (group == NULL) {
    next = (indexed_group *)realloc(*groups,
                                    sizeof(indexed_group) * (*count + 1u));
    if (next == NULL) {
      return 0;
    }
    *groups = next;
    group = &(*groups)[*count];
    memset(group, 0, sizeof(*group));
    group->wrapper = wrapper;
    group->index = *index;
    *index = NULL;
    group->node.kind = LQL_NODE_AND;
    ++*count;
  }
  if (indexed_group_conflicts(group, child)) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "selector expression has conflicting indexed clauses");
    return -1;
  }
  return append_node(&group->node, child);
}

lql_status lql_parse_selector_internal(const char *expr, int or_mode,
                                       lql_selector **out, lql_error *error) {
  lql_token_list tokens;
  lql_selector *selector;
  lql_node node;
  lql_node or_group;
  indexed_group *groups;
  lql_node_kind wrapper;
  char *index;
  const char *rest;
  size_t i;
  size_t group_count;
  int append_status;
  lql_status st;

  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "out selector required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = NULL;
  if (expr == NULL || *expr == '\0') {
    selector = (lql_selector *)calloc(1u, sizeof(*selector));
    if (selector == NULL) {
      return LQL_STATUS_NO_MEMORY;
    }
    selector->root.kind = LQL_NODE_ALL;
    *out = selector;
    return LQL_STATUS_OK;
  }
  st = split_top(expr, &tokens, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  selector = (lql_selector *)calloc(1u, sizeof(*selector));
  if (selector == NULL) {
    token_list_cleanup(&tokens);
    return LQL_STATUS_NO_MEMORY;
  }
  if (tokens.count == 0u) {
    selector->root.kind = LQL_NODE_ALL;
  } else if (tokens.count == 1u) {
    index = NULL;
    rest = NULL;
    switch (parse_indexed_wrapper(tokens.items[0], &wrapper, &index, &rest)) {
    case -1:
      st = LQL_STATUS_NO_MEMORY;
      break;
    case 1:
      st = parse_one(rest, &selector->root, error);
      free(index);
      break;
    default:
      st = parse_one(tokens.items[0], &selector->root, error);
      break;
    }
  } else {
    groups = NULL;
    group_count = 0u;
    memset(&or_group, 0, sizeof(or_group));
    or_group.kind = LQL_NODE_OR;
    selector->root.kind = or_mode ? LQL_NODE_OR : LQL_NODE_AND;
    for (i = 0u; i < tokens.count && st == LQL_STATUS_OK; ++i) {
      memset(&node, 0, sizeof(node));
      index = NULL;
      rest = NULL;
      switch (parse_indexed_wrapper(tokens.items[i], &wrapper, &index, &rest)) {
      case -1:
        st = LQL_STATUS_NO_MEMORY;
        break;
      case 1:
        st = parse_one(rest, &node, error);
        if (st == LQL_STATUS_OK) {
          append_status = append_indexed_group(&groups, &group_count, wrapper,
                                               &index, &node, error);
          if (append_status != 1) {
            if (append_status < 0) {
              st = LQL_STATUS_PARSE_ERROR;
            } else {
              st = LQL_STATUS_NO_MEMORY;
            }
            lql_node_cleanup(&node);
          }
        }
        if (st == LQL_STATUS_PARSE_ERROR) {
          if (error != NULL && error->code != LQL_STATUS_PARSE_ERROR) {
            st = LQL_STATUS_PARSE_ERROR;
          }
        }
        free(index);
        continue;
      default:
        st = parse_one(tokens.items[i], &node, error);
        break;
      }
      if (st != LQL_STATUS_OK) {
        break;
      }
      if (!or_mode && node.kind == LQL_NODE_OR && node.child_count == 1u) {
        if (!append_node(&or_group, &node.children[0])) {
          lql_node_cleanup(&node);
          st = LQL_STATUS_NO_MEMORY;
          break;
        }
        free(node.children);
        node.children = NULL;
        node.child_count = 0u;
      } else if (!append_node(&selector->root, &node)) {
        lql_node_cleanup(&node);
        st = LQL_STATUS_NO_MEMORY;
        break;
      }
    }
    for (i = 0u; i < group_count && st == LQL_STATUS_OK; ++i) {
      if (!or_mode && groups[i].wrapper == LQL_NODE_OR) {
        if (!append_node(&or_group, &groups[i].node)) {
          st = LQL_STATUS_NO_MEMORY;
        }
      } else if (!append_node(&selector->root, &groups[i].node)) {
        st = LQL_STATUS_NO_MEMORY;
      }
    }
    if (st == LQL_STATUS_OK && !or_mode && or_group.child_count != 0u &&
        !append_node(&selector->root, &or_group)) {
      st = LQL_STATUS_NO_MEMORY;
    }
    if (or_group.child_count != 0u) {
      lql_node_cleanup(&or_group);
    }
    indexed_groups_cleanup(groups, group_count);
  }
  token_list_cleanup(&tokens);
  if (st != LQL_STATUS_OK) {
    lql_selector_free(selector);
    return st;
  }
  selector->hit_count = 0u;
  assign_hit_indexes(&selector->root, &selector->hit_count);
  *out = selector;
  return LQL_STATUS_OK;
}
