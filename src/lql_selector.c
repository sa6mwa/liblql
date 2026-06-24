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
  if (strcmp(name, "exists") == 0) {
    return LQL_NODE_EXISTS;
  }
  return LQL_NODE_ALL;
}

static int parse_key_values(char *body, lql_term *term, lql_error *error) {
  lql_token_list parts;
  size_t i;
  lql_status st;
  char *eq;
  char *key;
  char *val;
  char *decoded;

  memset(term, 0, sizeof(*term));
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
    while (isspace((unsigned char)*val)) {
      ++val;
    }
    decoded = unquote(val);
    if (decoded == NULL) {
      token_list_cleanup(&parts);
      return 0;
    }
    if (strcmp(key, "field") == 0 || strcmp(key, "f") == 0) {
      free(term->field);
      term->field = decoded;
    } else if (strcmp(key, "value") == 0 || strcmp(key, "v") == 0) {
      free(term->value);
      term->value = decoded;
    } else if (strcmp(key, "gt") == 0) {
      term->range_op = '>';
      term->number = strtod(decoded, NULL);
      term->has_number = 1;
      free(decoded);
    } else if (strcmp(key, "gte") == 0) {
      term->range_op = 'G';
      term->number = strtod(decoded, NULL);
      term->has_number = 1;
      free(decoded);
    } else if (strcmp(key, "lt") == 0) {
      term->range_op = '<';
      term->number = strtod(decoded, NULL);
      term->has_number = 1;
      free(decoded);
    } else if (strcmp(key, "lte") == 0) {
      term->range_op = 'L';
      term->number = strtod(decoded, NULL);
      term->has_number = 1;
      free(decoded);
    } else {
      free(decoded);
    }
  }
  token_list_cleanup(&parts);
  if (term->field == NULL) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "selector field required");
    return 0;
  }
  return 1;
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
    op2 = (op[1] == '=' || op[0] == '!') ? 1 : 0;
    op0 = op[0];
    value = op + 1 + (size_t)op2;
    *op = '\0';
    out->term.field = lql_strdup(copy);
    out->term.value = unquote(value);
    if (out->term.field == NULL || out->term.value == NULL) {
      free(copy);
      return LQL_STATUS_NO_MEMORY;
    }
    if (op0 == '!') {
      out->kind = LQL_NODE_NE;
    } else if (strchr("><", op0) != NULL || op2) {
      if (op0 == '>') {
        out->term.range_op = op2 ? 'G' : '>';
      } else if (op0 == '<') {
        out->term.range_op = op2 ? 'L' : '<';
      } else {
        out->term.range_op = 0;
      }
      if (out->term.range_op != 0) {
        out->kind = LQL_NODE_RANGE;
        out->term.number = strtod(out->term.value, NULL);
        out->term.has_number = 1;
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
      out->term.field = unquote(body + 1);
      free(copy);
      return out->term.field == NULL ? LQL_STATUS_NO_MEMORY : LQL_STATUS_OK;
    }
    if (!parse_key_values(body + 1, &out->term, error)) {
      free(copy);
      return error != NULL ? error->code : LQL_STATUS_PARSE_ERROR;
    }
    free(copy);
    return LQL_STATUS_OK;
  }

  lql_set_error(error, LQL_STATUS_PARSE_ERROR, "invalid selector expression");
  free(copy);
  return LQL_STATUS_PARSE_ERROR;
}

lql_status lql_parse_selector_internal(const char *expr, int or_mode,
                                       lql_selector **out, lql_error *error) {
  lql_token_list tokens;
  lql_selector *selector;
  size_t i;
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
    st = parse_one(tokens.items[0], &selector->root, error);
  } else {
    selector->root.kind = or_mode ? LQL_NODE_OR : LQL_NODE_AND;
    selector->root.children =
        (lql_node *)calloc(tokens.count, sizeof(lql_node));
    if (selector->root.children == NULL) {
      st = LQL_STATUS_NO_MEMORY;
    } else {
      selector->root.child_count = tokens.count;
      for (i = 0u; i < tokens.count && st == LQL_STATUS_OK; ++i) {
        st = parse_one(tokens.items[i], &selector->root.children[i], error);
      }
    }
  }
  token_list_cleanup(&tokens);
  if (st != LQL_STATUS_OK) {
    lql_selector_free(selector);
    return st;
  }
  *out = selector;
  return LQL_STATUS_OK;
}
