#include "lql_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef enum mutation_kind {
  MUTATION_SET = 0,
  MUTATION_INCREMENT,
  MUTATION_REMOVE
} mutation_kind;

typedef struct mutation_path {
  char **segments;
  size_t segment_count;
} mutation_path;

typedef struct mutation_item {
  mutation_kind kind;
  mutation_path path;
  char *value;
  double delta;
  int time_value;
} mutation_item;

struct lql_mutation_plan {
  mutation_item *items;
  size_t count;
};

typedef struct string_list {
  char **items;
  size_t count;
} string_list;

static void mutation_path_cleanup(mutation_path *path) {
  size_t i;
  if (path == NULL) {
    return;
  }
  for (i = 0u; i < path->segment_count; ++i) {
    free(path->segments[i]);
  }
  free(path->segments);
  path->segments = NULL;
  path->segment_count = 0u;
}

static void mutation_item_cleanup(mutation_item *item) {
  if (item == NULL) {
    return;
  }
  mutation_path_cleanup(&item->path);
  free(item->value);
  memset(item, 0, sizeof(*item));
}

static void mutation_plan_cleanup_items(lql_mutation_plan *plan) {
  size_t i;
  if (plan == NULL) {
    return;
  }
  for (i = 0u; i < plan->count; ++i) {
    mutation_item_cleanup(&plan->items[i]);
  }
  free(plan->items);
  plan->items = NULL;
  plan->count = 0u;
}

static void string_list_cleanup(string_list *list) {
  size_t i;
  if (list == NULL) {
    return;
  }
  for (i = 0u; i < list->count; ++i) {
    free(list->items[i]);
  }
  free(list->items);
  list->items = NULL;
  list->count = 0u;
}

static const char *skip_space(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    ++p;
  }
  return p;
}

static size_t trimmed_len(const char *start) {
  const char *end;
  end = start + strlen(start);
  while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                         end[-1] == '\n')) {
    --end;
  }
  return (size_t)(end - start);
}

static char *trimmed_dup_range(const char *start, size_t len) {
  const char *end;
  char *out;
  start = skip_space(start);
  end = start + len;
  while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                         end[-1] == '\n')) {
    --end;
  }
  len = (size_t)(end - start);
  out = (char *)malloc(len + 1u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, start, len);
  out[len] = '\0';
  return out;
}

static int has_prefix(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int has_suffix(const char *s, const char *suffix) {
  size_t slen;
  size_t plen;
  slen = strlen(s);
  plen = strlen(suffix);
  return slen >= plen && strcmp(s + slen - plen, suffix) == 0;
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

static int add_string(string_list *list, char *value) {
  char **next;
  next = (char **)realloc(list->items,
                          sizeof(list->items[0]) * (list->count + 1u));
  if (next == NULL) {
    return 0;
  }
  list->items = next;
  list->items[list->count++] = value;
  return 1;
}

static int split_expressions(const char *input, string_list *out,
                             lql_error *error) {
  const char *chunk;
  const char *p;
  int depth;
  int in_quote;
  char quote;
  int escape;
  char *item;
  size_t len;

  memset(out, 0, sizeof(*out));
  chunk = input;
  p = input;
  depth = 0;
  in_quote = 0;
  quote = '\0';
  escape = 0;
  while (*p != '\0') {
    if (escape) {
      escape = 0;
    } else if (*p == '\\') {
      escape = 1;
    } else if (*p == '"' || *p == '\'') {
      if (!in_quote) {
        in_quote = 1;
        quote = *p;
      } else if (quote == *p) {
        in_quote = 0;
        quote = '\0';
      }
    } else if (*p == '{' && !in_quote) {
      ++depth;
    } else if (*p == '}' && !in_quote) {
      if (depth == 0) {
        lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                      "unexpected closing brace");
        string_list_cleanup(out);
        return 0;
      }
      --depth;
    } else if ((*p == ',' || *p == '\n') && !in_quote && depth == 0) {
      item = trimmed_dup_range(chunk, (size_t)(p - chunk));
      if (item == NULL) {
        string_list_cleanup(out);
        return 0;
      }
      if (item[0] != '\0' && !add_string(out, item)) {
        free(item);
        string_list_cleanup(out);
        return 0;
      }
      if (item[0] == '\0') {
        free(item);
      }
      chunk = p + 1;
    }
    ++p;
  }
  if (in_quote) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "unterminated quote in mutation expression");
    string_list_cleanup(out);
    return 0;
  }
  if (depth != 0) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "unterminated brace in mutation expression");
    string_list_cleanup(out);
    return 0;
  }
  len = trimmed_len(chunk);
  if (len != 0u) {
    item = trimmed_dup_range(chunk, strlen(chunk));
    if (item == NULL || !add_string(out, item)) {
      free(item);
      string_list_cleanup(out);
      return 0;
    }
  }
  return 1;
}

static char *decode_path_segment(const char *src, size_t len) {
  char *out;
  size_t i;
  size_t j;
  out = (char *)malloc(len + 1u);
  if (out == NULL) {
    return NULL;
  }
  i = 0u;
  j = 0u;
  while (i < len) {
    if (src[i] == '~' && i + 1u < len) {
      if (src[i + 1u] == '0') {
        out[j++] = '~';
        i += 2u;
        continue;
      }
      if (src[i + 1u] == '1') {
        out[j++] = '/';
        i += 2u;
        continue;
      }
    }
    out[j++] = src[i++];
  }
  out[j] = '\0';
  return out;
}

static int path_add_segment(mutation_path *path, char *segment) {
  char **next;
  next = (char **)realloc(path->segments, sizeof(path->segments[0]) *
                                              (path->segment_count + 1u));
  if (next == NULL) {
    return 0;
  }
  path->segments = next;
  path->segments[path->segment_count++] = segment;
  return 1;
}

static int expand_and_add_segment(mutation_path *path, char *segment) {
  size_t len;
  char *base;
  char *wild;
  len = strlen(segment);
  if (len > 2u && strcmp(segment, "[]") != 0 &&
      strcmp(segment + len - 2u, "[]") == 0) {
    segment[len - 2u] = '\0';
    base = lql_strdup(segment);
    segment[len - 2u] = '[';
    if (base == NULL) {
      free(segment);
      return 0;
    }
    wild = lql_strdup("[]");
    if (wild == NULL) {
      free(base);
      free(segment);
      return 0;
    }
    free(segment);
    return path_add_segment(path, base) && path_add_segment(path, wild);
  }
  return path_add_segment(path, segment);
}

static int split_path(const char *raw, mutation_path *out, lql_error *error) {
  const char *path;
  const char *seg;
  const char *slash;
  char *decoded;
  size_t len;

  memset(out, 0, sizeof(*out));
  path = skip_space(raw);
  len = trimmed_len(path);
  if (len == 0u) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "path empty");
    return 0;
  }
  if (path[0] != '/') {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "mutation path must start with '/'");
    return 0;
  }
  seg = path + 1;
  while ((size_t)(seg - path) <= len) {
    slash = memchr(seg, '/', (size_t)(path + len - seg));
    if (slash == NULL) {
      slash = path + len;
    }
    decoded = decode_path_segment(seg, (size_t)(slash - seg));
    if (decoded == NULL || !expand_and_add_segment(out, decoded)) {
      free(decoded);
      mutation_path_cleanup(out);
      return 0;
    }
    if (slash == path + len) {
      break;
    }
    seg = slash + 1;
  }
  if (out->segment_count == 0u ||
      (out->segment_count == 1u && out->segments[0][0] == '\0')) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "mutation path refers to document root");
    mutation_path_cleanup(out);
    return 0;
  }
  return 1;
}

static int append_item(lql_mutation_plan *plan, mutation_item *item) {
  mutation_item *next;
  next = (mutation_item *)realloc(plan->items,
                                  sizeof(plan->items[0]) * (plan->count + 1u));
  if (next == NULL) {
    return 0;
  }
  plan->items = next;
  plan->items[plan->count++] = *item;
  memset(item, 0, sizeof(*item));
  return 1;
}

static int prepend_path(mutation_item *item, const mutation_path *prefix) {
  char **segments;
  size_t i;
  size_t count;
  count = prefix->segment_count + item->path.segment_count;
  segments = (char **)calloc(count, sizeof(segments[0]));
  if (segments == NULL) {
    return 0;
  }
  for (i = 0u; i < prefix->segment_count; ++i) {
    segments[i] = lql_strdup(prefix->segments[i]);
    if (segments[i] == NULL) {
      goto fail;
    }
  }
  for (i = 0u; i < item->path.segment_count; ++i) {
    segments[prefix->segment_count + i] = item->path.segments[i];
    item->path.segments[i] = NULL;
  }
  free(item->path.segments);
  item->path.segments = segments;
  item->path.segment_count = count;
  return 1;
fail:
  for (i = 0u; i < count; ++i) {
    free(segments[i]);
  }
  free(segments);
  return 0;
}

static int parse_number(const char *s, double *out) {
  char *end;
  double value;
  value = strtod(s, &end);
  if (end == s) {
    return 0;
  }
  end = (char *)skip_space(end);
  if (*end != '\0') {
    return 0;
  }
  *out = value;
  return 1;
}

static int parse_mutation_expr(const char *expr, lql_mutation_plan *plan,
                               lql_error *error);

static int parse_brace_mutation(const char *expr, lql_mutation_plan *plan,
                                lql_error *error) {
  const char *open;
  size_t len;
  char *prefix_text;
  char *body;
  mutation_path prefix;
  string_list parts;
  lql_mutation_plan nested;
  size_t i;

  len = strlen(expr);
  if (len == 0u || expr[len - 1u] != '}') {
    return 0;
  }
  open = strchr(expr, '{');
  if (open == NULL || open == expr) {
    return 0;
  }
  prefix_text = trimmed_dup_range(expr, (size_t)(open - expr));
  if (prefix_text == NULL) {
    return -1;
  }
  if (strchr(prefix_text, '=') != NULL) {
    free(prefix_text);
    return 0;
  }
  if (!split_path(prefix_text, &prefix, error)) {
    free(prefix_text);
    return -1;
  }
  free(prefix_text);
  body = trimmed_dup_range(open + 1, len - (size_t)(open - expr) - 2u);
  if (body == NULL) {
    mutation_path_cleanup(&prefix);
    return -1;
  }
  if (!split_expressions(body, &parts, error)) {
    free(body);
    mutation_path_cleanup(&prefix);
    return -1;
  }
  free(body);
  if (parts.count == 0u) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "brace mutation empty");
    string_list_cleanup(&parts);
    mutation_path_cleanup(&prefix);
    return -1;
  }
  memset(&nested, 0, sizeof(nested));
  for (i = 0u; i < parts.count; ++i) {
    if (!parse_mutation_expr(parts.items[i], &nested, error)) {
      mutation_plan_cleanup_items(&nested);
      string_list_cleanup(&parts);
      mutation_path_cleanup(&prefix);
      return -1;
    }
  }
  for (i = 0u; i < nested.count; ++i) {
    if (!prepend_path(&nested.items[i], &prefix) ||
        !append_item(plan, &nested.items[i])) {
      mutation_plan_cleanup_items(&nested);
      string_list_cleanup(&parts);
      mutation_path_cleanup(&prefix);
      return -1;
    }
  }
  free(nested.items);
  string_list_cleanup(&parts);
  mutation_path_cleanup(&prefix);
  return 1;
}

static int parse_set_value(char *value, int time_mode, mutation_item *item,
                           lql_error *error) {
  lql_temporal temporal;
  if (time_mode) {
    if (!ascii_equal_ignore_case(value, "NOW") &&
        !lql_parse_temporal_literal(value, &temporal)) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR, "invalid time literal");
      return 0;
    }
    item->time_value = 1;
  }
  item->value = value;
  return 1;
}

static int parse_mutation_expr(const char *raw, lql_mutation_plan *plan,
                               lql_error *error) {
  char *expr;
  char *eq;
  char *path_text;
  char *value;
  mutation_item item;
  int remove_mode;
  int file_mode;
  int time_mode;
  int brace_result;
  double delta;
  size_t len;

  expr = trimmed_dup_range(raw, strlen(raw));
  if (expr == NULL) {
    return 0;
  }
  remove_mode = 0;
  file_mode = 0;
  time_mode = 0;
  if (has_prefix(expr, "file:")) {
    file_mode = 1;
    memmove(expr, expr + 5, strlen(expr + 5) + 1u);
  } else if (has_prefix(expr, "textfile:")) {
    file_mode = 1;
    memmove(expr, expr + 9, strlen(expr + 9) + 1u);
  } else if (has_prefix(expr, "base64file:")) {
    file_mode = 1;
    memmove(expr, expr + 11, strlen(expr + 11) + 1u);
  }
  if (has_prefix(expr, "rm:")) {
    remove_mode = 1;
    memmove(expr, expr + 3, strlen(expr + 3) + 1u);
  } else if (has_prefix(expr, "remove:")) {
    remove_mode = 1;
    memmove(expr, expr + 7, strlen(expr + 7) + 1u);
  } else if (has_prefix(expr, "delete:")) {
    remove_mode = 1;
    memmove(expr, expr + 7, strlen(expr + 7) + 1u);
  } else if (has_prefix(expr, "del:")) {
    remove_mode = 1;
    memmove(expr, expr + 4, strlen(expr + 4) + 1u);
  }
  if (has_prefix(expr, "time:")) {
    if (file_mode || remove_mode) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "time-prefixed mutation has invalid prefix combination");
      free(expr);
      return 0;
    }
    time_mode = 1;
    memmove(expr, expr + 5, strlen(expr + 5) + 1u);
  }
  if (file_mode) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "file-backed mutations are disabled");
    free(expr);
    return 0;
  }
  memset(&item, 0, sizeof(item));
  if (remove_mode) {
    item.kind = MUTATION_REMOVE;
    if (!split_path(expr, &item.path, error) || !append_item(plan, &item)) {
      mutation_item_cleanup(&item);
      free(expr);
      return 0;
    }
    free(expr);
    return 1;
  }
  if (has_suffix(expr, "++") || has_suffix(expr, "--")) {
    if (time_mode) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "time-prefixed mutation does not support increment");
      free(expr);
      return 0;
    }
    len = strlen(expr);
    delta = expr[len - 2u] == '+' ? 1.0 : -1.0;
    expr[len - 2u] = '\0';
    item.kind = MUTATION_INCREMENT;
    item.delta = delta;
    if (!split_path(expr, &item.path, error) || !append_item(plan, &item)) {
      mutation_item_cleanup(&item);
      free(expr);
      return 0;
    }
    free(expr);
    return 1;
  }
  brace_result = parse_brace_mutation(expr, plan, error);
  if (brace_result != 0) {
    free(expr);
    return brace_result > 0;
  }
  eq = strchr(expr, '=');
  if (eq == NULL) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "invalid mutation, expected key=value");
    free(expr);
    return 0;
  }
  *eq = '\0';
  path_text = expr;
  value = trimmed_dup_range(eq + 1, strlen(eq + 1));
  if (value == NULL) {
    free(expr);
    return 0;
  }
  item.kind = MUTATION_SET;
  if (!time_mode && (value[0] == '+' || value[0] == '-') &&
      parse_number(value, &delta)) {
    if (delta == 0.0) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "increment mutation requires non-zero delta");
      free(value);
      free(expr);
      return 0;
    }
    item.kind = MUTATION_INCREMENT;
    item.delta = delta;
    free(value);
  } else if (!parse_set_value(value, time_mode, &item, error)) {
    free(value);
    free(expr);
    return 0;
  }
  if (!split_path(path_text, &item.path, error) || !append_item(plan, &item)) {
    mutation_item_cleanup(&item);
    free(expr);
    return 0;
  }
  free(expr);
  return 1;
}

lql_status lql_mutation_plan_parse(const char *const *exprs, size_t expr_count,
                                   lql_mutation_plan **out, lql_error *error) {
  lql_mutation_plan *plan;
  string_list parts;
  size_t i;
  size_t j;

  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "out is required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = NULL;
  if (exprs == NULL || expr_count == 0u) {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR, "no field mutations provided");
    return LQL_STATUS_PARSE_ERROR;
  }
  plan = (lql_mutation_plan *)calloc(1u, sizeof(*plan));
  if (plan == NULL) {
    return LQL_STATUS_NO_MEMORY;
  }
  for (i = 0u; i < expr_count; ++i) {
    if (exprs[i] == NULL) {
      continue;
    }
    if (!split_expressions(exprs[i], &parts, error)) {
      lql_mutation_plan_free(plan);
      return error != NULL && error->code != LQL_STATUS_OK
                 ? error->code
                 : LQL_STATUS_NO_MEMORY;
    }
    for (j = 0u; j < parts.count; ++j) {
      if (!parse_mutation_expr(parts.items[j], plan, error)) {
        string_list_cleanup(&parts);
        lql_mutation_plan_free(plan);
        return error != NULL && error->code != LQL_STATUS_OK
                   ? error->code
                   : LQL_STATUS_NO_MEMORY;
      }
    }
    string_list_cleanup(&parts);
  }
  if (plan->count == 0u) {
    lql_mutation_plan_free(plan);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "no valid field mutations parsed");
    return LQL_STATUS_PARSE_ERROR;
  }
  *out = plan;
  return LQL_STATUS_OK;
}

size_t lql_mutation_plan_count(const lql_mutation_plan *plan) {
  return plan == NULL ? 0u : plan->count;
}

void lql_mutation_plan_free(lql_mutation_plan *plan) {
  size_t i;
  if (plan == NULL) {
    return;
  }
  for (i = 0u; i < plan->count; ++i) {
    mutation_item_cleanup(&plan->items[i]);
  }
  free(plan->items);
  free(plan);
}
