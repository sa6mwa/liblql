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

typedef enum mutation_kind {
  MUTATION_SET = 0,
  MUTATION_INCREMENT,
  MUTATION_REMOVE
} mutation_kind;

typedef enum mutation_file_mode {
  MUTATION_FILE_NONE = 0,
  MUTATION_FILE_AUTO,
  MUTATION_FILE_TEXT,
  MUTATION_FILE_BASE64
} mutation_file_mode;

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
  mutation_file_mode file_mode;
  char *file_path;
} mutation_item;

struct lql_mutation_plan {
  mutation_item *items;
  size_t count;
};

typedef struct limited_file_reader {
  FILE *file;
  lql_uint64 remaining;
} limited_file_reader;

typedef struct mutation_path_frame {
  unsigned char *array_segments;
  size_t segment_count;
  char container;
} mutation_path_frame;

typedef struct mutation_stream_state {
  const lql_mutation_plan *plan;
  lonejson_writer writer;
  lonejson_error *error;
  char *key_buf;
  size_t key_len;
  char *num_buf;
  size_t num_len;
  int *applied;
  size_t *prefix_seen_depth;
  size_t source_depth;
  int root_seen;
  int root_is_object;
  int skipping;
  size_t skip_depth;
  int active_increment;
  int active_keyed;
  size_t active_index;
  mutation_path_frame *path_frames;
  size_t path_frame_count;
} mutation_stream_state;

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
  free(item->file_path);
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

static char *mutation_unquote(char *value) {
  size_t len;
  char *out;
  char *r;
  char *w;
  len = strlen(value);
  if (len >= 2u && ((value[0] == '"' && value[len - 1u] == '"') ||
                    (value[0] == '\'' && value[len - 1u] == '\''))) {
    out = (char *)malloc(len - 1u);
    if (out == NULL) {
      return NULL;
    }
    r = value + 1;
    w = out;
    while (r < value + len - 1u) {
      if (*r == '\\' && r + 1 < value + len - 1u) {
        ++r;
      }
      *w++ = *r++;
    }
    *w = '\0';
    free(value);
    return out;
  }
  return value;
}

static int path_is_absolute(const char *path) {
  return path != NULL && path[0] == '/';
}

static char *join_paths(const char *base, const char *path) {
  size_t base_len;
  size_t path_len;
  char *out;
  int need_sep;
  base_len = strlen(base);
  path_len = strlen(path);
  need_sep = base_len != 0u && base[base_len - 1u] != '/';
  out = (char *)malloc(base_len + (need_sep ? 1u : 0u) + path_len + 1u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, base, base_len);
  if (need_sep) {
    out[base_len++] = '/';
  }
  memcpy(out + base_len, path, path_len + 1u);
  return out;
}

static char *resolve_file_value_path(const char *raw,
                                     const lql_mutation_parse_options *options,
                                     lql_error *error) {
  char *path;
  const char *home;
  char *expanded;
  path = trimmed_dup_range(raw, strlen(raw));
  if (path == NULL) {
    return NULL;
  }
  path = mutation_unquote(path);
  if (path == NULL) {
    return NULL;
  }
  if (path[0] == '\0') {
    free(path);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "file-backed mutation missing file path");
    return NULL;
  }
  if (strcmp(path, "~") == 0 || has_prefix(path, "~/")) {
    home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
      free(path);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "failed to resolve home for file-backed mutation");
      return NULL;
    }
    if (strcmp(path, "~") == 0) {
      expanded = lql_strdup(home);
    } else {
      expanded = join_paths(home, path + 2u);
    }
    free(path);
    return expanded;
  }
  if (path_is_absolute(path)) {
    return path;
  }
  if (options == NULL || options->file_value_base_dir == NULL ||
      options->file_value_base_dir[0] == '\0') {
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "relative file-backed mutation path requires file value base "
                  "dir");
    free(path);
    return NULL;
  }
  expanded = join_paths(options->file_value_base_dir, path);
  free(path);
  return expanded;
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

static int ascii_equal_ignore_case_n(const char *a, size_t a_len,
                                     const char *b) {
  size_t i;
  for (i = 0u; i < a_len && b[i] != '\0'; ++i) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
      return 0;
    }
  }
  return i == a_len && b[i] == '\0';
}

static int time_literal_has_zone(const char *value) {
  const char *p;
  int colon_count;
  p = value;
  while (*p != '\0' && *p != 'T') {
    ++p;
  }
  if (*p != 'T') {
    return 0;
  }
  ++p;
  colon_count = 0;
  while (*p != '\0') {
    if (*p == ':') {
      ++colon_count;
    } else if (colon_count >= 2 && (*p == 'Z' || *p == '+' || *p == '-')) {
      return 1;
    }
    ++p;
  }
  return 0;
}

static int seek_u64(FILE *file, lql_uint64 offset) {
  off_t seek_offset;
  seek_offset = (off_t)offset;
  if (seek_offset < (off_t)0 || (lql_uint64)seek_offset != offset) {
    return 0;
  }
  return fseeko(file, seek_offset, SEEK_SET) == 0;
}

static lonejson_status file_sink(void *user, const void *data, size_t len,
                                 lonejson_error *error) {
  FILE *out;
  (void)error;
  out = (FILE *)user;
  return fwrite(data, 1u, len, out) == len ? LONEJSON_STATUS_OK
                                           : LONEJSON_STATUS_CALLBACK_FAILED;
}

static lonejson_read_result limited_read(void *user, unsigned char *buffer,
                                         size_t capacity) {
  limited_file_reader *reader;
  lonejson_read_result result;
  size_t want;
  result = lonejson_default_read_result();
  reader = (limited_file_reader *)user;
  if (reader->remaining == 0u) {
    result.eof = 1;
    return result;
  }
  want = reader->remaining > (lql_uint64)capacity ? capacity
                                                  : (size_t)reader->remaining;
  result.bytes_read = fread(buffer, 1u, want, reader->file);
  reader->remaining -= (lql_uint64)result.bytes_read;
  if (result.bytes_read != want && ferror(reader->file)) {
    result.error_code = 1;
  }
  if (reader->remaining == 0u) {
    result.eof = 1;
  }
  return result;
}

static int append_buf(char **buf, size_t *len, const char *data, size_t n) {
  char *next;
  next = (char *)realloc(*buf, *len + n + 1u);
  if (next == NULL) {
    return 0;
  }
  memcpy(next + *len, data, n);
  *len += n;
  next[*len] = '\0';
  *buf = next;
  return 1;
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

static const char *unquoted_value(const char *value, size_t *out_len);

static int parse_mutation_expr(const char *expr, lql_mutation_plan *plan,
                               const lql_mutation_parse_options *options,
                               lql_error *error);

static int parse_brace_mutation(const char *expr, lql_mutation_plan *plan,
                                const lql_mutation_parse_options *options,
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
    if (!parse_mutation_expr(parts.items[i], &nested, options, error)) {
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
  const char *text;
  size_t len;
  char normalized[40];
  char *copy;
  if (time_mode) {
    text = unquoted_value(value, &len);
    copy = trimmed_dup_range(text, len);
    if (copy == NULL) {
      return 0;
    }
    if (ascii_equal_ignore_case(copy, "NOW")) {
      if (!lql_temporal_now(&temporal)) {
        free(copy);
        lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                      "failed to resolve current time");
        return 0;
      }
    } else if (!time_literal_has_zone(copy) ||
               !lql_parse_temporal_literal(copy, &temporal) ||
               temporal.date_only) {
      free(copy);
      lql_set_error(error, LQL_STATUS_PARSE_ERROR, "invalid time literal");
      return 0;
    }
    free(copy);
    if (!lql_temporal_format_rfc3339_nano(&temporal, normalized,
                                          sizeof(normalized))) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR, "invalid time literal");
      return 0;
    }
    item->value = lql_strdup(normalized);
    if (item->value == NULL) {
      return 0;
    }
    free(value);
    item->time_value = 1;
    return 1;
  }
  item->value = value;
  return 1;
}

static int parse_mutation_expr(const char *raw, lql_mutation_plan *plan,
                               const lql_mutation_parse_options *options,
                               lql_error *error) {
  char *expr;
  char *eq;
  char *path_text;
  char *value;
  mutation_item item;
  int remove_mode;
  mutation_file_mode file_mode;
  int time_mode;
  int brace_result;
  double delta;
  size_t len;

  expr = trimmed_dup_range(raw, strlen(raw));
  if (expr == NULL) {
    return 0;
  }
  remove_mode = 0;
  file_mode = MUTATION_FILE_NONE;
  time_mode = 0;
  if (has_prefix(expr, "file:")) {
    file_mode = MUTATION_FILE_AUTO;
    memmove(expr, expr + 5, strlen(expr + 5) + 1u);
  } else if (has_prefix(expr, "textfile:")) {
    file_mode = MUTATION_FILE_TEXT;
    memmove(expr, expr + 9, strlen(expr + 9) + 1u);
  } else if (has_prefix(expr, "base64file:")) {
    file_mode = MUTATION_FILE_BASE64;
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
    if (file_mode != MUTATION_FILE_NONE || remove_mode) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "time-prefixed mutation has invalid prefix combination");
      free(expr);
      return 0;
    }
    time_mode = 1;
    memmove(expr, expr + 5, strlen(expr + 5) + 1u);
  }
  memset(&item, 0, sizeof(item));
  if (remove_mode) {
    if (file_mode != MUTATION_FILE_NONE) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "file-backed mutation has invalid prefix combination");
      free(expr);
      return 0;
    }
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
    if (file_mode != MUTATION_FILE_NONE) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "file-backed mutation does not support increment");
      free(expr);
      return 0;
    }
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
  brace_result = parse_brace_mutation(expr, plan, options, error);
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
  if (file_mode != MUTATION_FILE_NONE) {
    if (options == NULL || !options->enable_file_values) {
      lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                    "file-backed mutations are disabled");
      free(value);
      free(expr);
      return 0;
    }
    item.file_mode = file_mode;
    item.file_path = resolve_file_value_path(value, options, error);
    free(value);
    if (item.file_path == NULL) {
      free(expr);
      return 0;
    }
    if (!split_path(path_text, &item.path, error) ||
        !append_item(plan, &item)) {
      mutation_item_cleanup(&item);
      free(expr);
      return 0;
    }
    free(expr);
    return 1;
  }
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

lql_status lql_mutation_plan_parse_with_options(
    const char *const *exprs, size_t expr_count,
    const lql_mutation_parse_options *options, lql_mutation_plan **out,
    lql_error *error) {
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
      if (!parse_mutation_expr(parts.items[j], plan, options, error)) {
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

lql_status lql_mutation_plan_parse(const char *const *exprs, size_t expr_count,
                                   lql_mutation_plan **out, lql_error *error) {
  return lql_mutation_plan_parse_with_options(exprs, expr_count, NULL, out,
                                              error);
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

static int mutation_is_root_field_supported(const mutation_item *item) {
  return item->path.segment_count == 1u && item->path.segments[0][0] != '\0' &&
         strcmp(item->path.segments[0], "*") != 0 &&
         strcmp(item->path.segments[0], "[]") != 0 &&
         strcmp(item->path.segments[0], "**") != 0 &&
         strcmp(item->path.segments[0], "...") != 0;
}

static int mutation_plan_supports_root_fields(const lql_mutation_plan *plan) {
  size_t i;
  size_t j;
  if (plan == NULL || plan->count == 0u) {
    return 0;
  }
  for (i = 0u; i < plan->count; ++i) {
    if (!mutation_is_root_field_supported(&plan->items[i])) {
      return 0;
    }
    for (j = i + 1u; j < plan->count; ++j) {
      if (plan->items[j].path.segment_count == 1u &&
          strcmp(plan->items[i].path.segments[0],
                 plan->items[j].path.segments[0]) == 0) {
        return 0;
      }
    }
  }
  return 1;
}

static int mutation_is_stream_path_supported(const mutation_item *item) {
  size_t i;
  if (item->path.segment_count == 0u) {
    return 0;
  }
  for (i = 0u; i < item->path.segment_count; ++i) {
    if (item->path.segments[i][0] == '\0') {
      return 0;
    }
  }
  return 1;
}

static int mutation_path_has_wildcard(const mutation_path *path) {
  size_t i;
  if (path == NULL) {
    return 0;
  }
  for (i = 0u; i < path->segment_count; ++i) {
    if (strcmp(path->segments[i], "*") == 0 ||
        strcmp(path->segments[i], "[]") == 0 ||
        strcmp(path->segments[i], "**") == 0 ||
        strcmp(path->segments[i], "...") == 0) {
      return 1;
    }
  }
  return 0;
}

static int mutation_paths_equal(const mutation_path *a,
                                const mutation_path *b) {
  size_t i;
  if (a->segment_count != b->segment_count) {
    return 0;
  }
  for (i = 0u; i < a->segment_count; ++i) {
    if (strcmp(a->segments[i], b->segments[i]) != 0) {
      return 0;
    }
  }
  return 1;
}

static int mutation_path_is_prefix(const mutation_path *prefix,
                                   const mutation_path *path) {
  size_t i;
  if (prefix->segment_count >= path->segment_count) {
    return 0;
  }
  for (i = 0u; i < prefix->segment_count; ++i) {
    if (strcmp(prefix->segments[i], path->segments[i]) != 0) {
      return 0;
    }
  }
  return 1;
}

static int mutation_plan_supports_stream_paths(const lql_mutation_plan *plan) {
  size_t i;
  size_t j;
  if (plan == NULL || plan->count == 0u) {
    return 0;
  }
  for (i = 0u; i < plan->count; ++i) {
    if (!mutation_is_stream_path_supported(&plan->items[i])) {
      return 0;
    }
    for (j = i + 1u; j < plan->count; ++j) {
      if (mutation_paths_equal(&plan->items[i].path, &plan->items[j].path)) {
        return 0;
      }
      if (!mutation_path_has_wildcard(&plan->items[i].path) &&
          !mutation_path_has_wildcard(&plan->items[j].path) &&
          (mutation_path_is_prefix(&plan->items[i].path,
                                   &plan->items[j].path) ||
           mutation_path_is_prefix(&plan->items[j].path,
                                   &plan->items[i].path))) {
        return 0;
      }
    }
  }
  return 1;
}

static int stream_path_segment_matches(const char *expected,
                                       const lonejson_path_segment *actual,
                                       int actual_is_array) {
  if (strcmp(expected, "*") == 0) {
    return !actual_is_array;
  }
  if (strcmp(expected, "[]") == 0) {
    return actual_is_array;
  }
  if (strcmp(expected, "**") == 0) {
    return 1;
  }
  return strlen(expected) == actual->len &&
         memcmp(expected, actual->data, actual->len) == 0;
}

static int virtual_path_segment_matches(const mutation_path *item_path,
                                        size_t item_index,
                                        const lonejson_value_path *parent,
                                        const mutation_path_frame *frame,
                                        const char *key, size_t key_len,
                                        size_t actual_index) {
  lonejson_path_segment segment;
  if (actual_index < parent->segment_count) {
    return stream_path_segment_matches(
        item_path->segments[item_index], &parent->segments[actual_index],
        frame->array_segments[actual_index] ? 1 : 0);
  }
  segment.data = key;
  segment.len = key_len;
  return stream_path_segment_matches(item_path->segments[item_index], &segment,
                                     0);
}

static int virtual_path_matches_from(const mutation_path *item_path,
                                     size_t item_index,
                                     const lonejson_value_path *parent,
                                     const mutation_path_frame *frame,
                                     const char *key, size_t key_len,
                                     size_t actual_index) {
  size_t actual_count;
  size_t i;
  actual_count = parent->segment_count + 1u;
  if (item_index == item_path->segment_count) {
    return actual_index == actual_count;
  }
  if (strcmp(item_path->segments[item_index], "...") == 0) {
    if (item_index + 1u == item_path->segment_count) {
      return 1;
    }
    for (i = actual_index; i <= actual_count; ++i) {
      if (virtual_path_matches_from(item_path, item_index + 1u, parent, frame,
                                    key, key_len, i)) {
        return 1;
      }
    }
    return 0;
  }
  if (actual_index >= actual_count ||
      !virtual_path_segment_matches(item_path, item_index, parent, frame, key,
                                    key_len, actual_index)) {
    return 0;
  }
  return virtual_path_matches_from(item_path, item_index + 1u, parent, frame,
                                   key, key_len, actual_index + 1u);
}

static int mutation_item_matches_virtual_key(const mutation_item *item,
                                             const lonejson_value_path *parent,
                                             const mutation_path_frame *frame,
                                             const char *key, size_t key_len) {
  return parent != NULL && frame != NULL &&
         frame->segment_count == parent->segment_count &&
         virtual_path_matches_from(&item->path, 0u, parent, frame, key, key_len,
                                   0u);
}

static int value_path_segment_is_array(const mutation_stream_state *state,
                                       const lonejson_value_path *path,
                                       size_t index) {
  const mutation_path_frame *frame;
  if (state == NULL || path == NULL || index >= path->segment_count ||
      state->path_frame_count == 0u) {
    return 0;
  }
  frame = &state->path_frames[state->path_frame_count - 1u];
  if (frame->segment_count == path->segment_count) {
    return frame->array_segments[index] ? 1 : 0;
  }
  if (frame->segment_count + 1u == path->segment_count) {
    if (index < frame->segment_count) {
      return frame->array_segments[index] ? 1 : 0;
    }
    return frame->container == 'a';
  }
  return 0;
}

static int value_path_is_array_element(const mutation_stream_state *state,
                                       const lonejson_value_path *path) {
  if (path == NULL || path->segment_count == 0u) {
    return 0;
  }
  return value_path_segment_is_array(state, path, path->segment_count - 1u);
}

static int value_path_item_matches_from(const mutation_stream_state *state,
                                        const mutation_path *item_path,
                                        size_t item_index,
                                        const lonejson_value_path *path,
                                        size_t path_index) {
  size_t i;
  if (item_index == item_path->segment_count) {
    return path_index == path->segment_count;
  }
  if (strcmp(item_path->segments[item_index], "...") == 0) {
    if (item_index + 1u == item_path->segment_count) {
      return 1;
    }
    for (i = path_index; i <= path->segment_count; ++i) {
      if (value_path_item_matches_from(state, item_path, item_index + 1u, path,
                                       i)) {
        return 1;
      }
    }
    return 0;
  }
  if (path_index >= path->segment_count ||
      !stream_path_segment_matches(
          item_path->segments[item_index], &path->segments[path_index],
          value_path_segment_is_array(state, path, path_index))) {
    return 0;
  }
  return value_path_item_matches_from(state, item_path, item_index + 1u, path,
                                      path_index + 1u);
}

static int mutation_value_index(const mutation_stream_state *state,
                                const lonejson_value_path *path, size_t *out) {
  size_t i;
  if (!value_path_is_array_element(state, path)) {
    return 0;
  }
  for (i = 0u; i < state->plan->count; ++i) {
    if (mutation_path_has_wildcard(&state->plan->items[i].path) &&
        value_path_item_matches_from(state, &state->plan->items[i].path, 0u,
                                     path, 0u)) {
      *out = i;
      return 1;
    }
  }
  return 0;
}

static const mutation_path_frame *
current_path_frame(const mutation_stream_state *state,
                   const lonejson_value_path *path) {
  const mutation_path_frame *frame;
  if (state == NULL || path == NULL || state->path_frame_count == 0u) {
    return NULL;
  }
  frame = &state->path_frames[state->path_frame_count - 1u];
  return frame->segment_count == path->segment_count ? frame : NULL;
}

static int stream_path_prefix_matches(const mutation_path *item_path,
                                      const lonejson_value_path *path,
                                      const mutation_path_frame *frame) {
  size_t i;
  if (path == NULL || frame == NULL ||
      path->segment_count > item_path->segment_count ||
      frame->segment_count != path->segment_count) {
    return 0;
  }
  for (i = 0u; i < path->segment_count; ++i) {
    if (!stream_path_segment_matches(item_path->segments[i], &path->segments[i],
                                     frame->array_segments[i] ? 1 : 0)) {
      return 0;
    }
  }
  return 1;
}

static int mutation_key_matches_path(const mutation_item *item,
                                     const lonejson_value_path *parent,
                                     const mutation_path_frame *frame,
                                     const char *key, size_t key_len) {
  return mutation_item_matches_virtual_key(item, parent, frame, key, key_len);
}

static int mutation_descends_from_object(const mutation_item *item,
                                         const lonejson_value_path *path,
                                         const mutation_path_frame *frame) {
  return path != NULL && item->path.segment_count > path->segment_count &&
         stream_path_prefix_matches(&item->path, path, frame);
}

static int mutation_key_index(const lql_mutation_plan *plan,
                              const lonejson_value_path *parent,
                              const mutation_path_frame *frame, const char *key,
                              size_t key_len, size_t *out) {
  size_t i;
  for (i = 0u; i < plan->count; ++i) {
    if (mutation_key_matches_path(&plan->items[i], parent, frame, key,
                                  key_len)) {
      *out = i;
      return 1;
    }
  }
  return 0;
}

static const char *unquoted_value(const char *value, size_t *out_len) {
  size_t len;
  len = strlen(value);
  if (len >= 2u && ((value[0] == '"' && value[len - 1u] == '"') ||
                    (value[0] == '\'' && value[len - 1u] == '\''))) {
    *out_len = len - 2u;
    return value + 1;
  }
  *out_len = len;
  return value;
}

static void set_lonejson_error(lonejson_error *error, lonejson_status status,
                               const char *message) {
  size_t len;
  if (error == NULL) {
    return;
  }
  error->code = status;
  error->line = 0u;
  error->column = 0u;
  error->offset = 0u;
  error->system_errno = 0;
  error->truncated = 0;
  len = strlen(message);
  if (len >= sizeof(error->message)) {
    len = sizeof(error->message) - 1u;
  }
  memcpy(error->message, message, len);
  error->message[len] = '\0';
}

static int inspect_file_textlike(const char *path, lonejson_error *error) {
  FILE *file;
  unsigned char buf[8192];
  size_t got;
  size_t i;
  unsigned int expected;
  unsigned int min_next;
  unsigned int max_next;
  unsigned int b;
  file = fopen(path, "rb");
  if (file == NULL) {
    set_lonejson_error(error, LONEJSON_STATUS_IO_ERROR,
                       "failed to open file-backed mutation value");
    return -1;
  }
  expected = 0u;
  min_next = 0x80u;
  max_next = 0xBFu;
  while ((got = fread(buf, 1u, sizeof(buf), file)) != 0u) {
    for (i = 0u; i < got; ++i) {
      b = (unsigned int)buf[i];
      if (b == 0u) {
        fclose(file);
        return 0;
      }
      if (expected == 0u) {
        if (b < 0x80u) {
          continue;
        } else if (b >= 0xC2u && b <= 0xDFu) {
          expected = 1u;
          min_next = 0x80u;
          max_next = 0xBFu;
        } else if (b == 0xE0u) {
          expected = 2u;
          min_next = 0xA0u;
          max_next = 0xBFu;
        } else if ((b >= 0xE1u && b <= 0xECu) || (b >= 0xEEu && b <= 0xEFu)) {
          expected = 2u;
          min_next = 0x80u;
          max_next = 0xBFu;
        } else if (b == 0xEDu) {
          expected = 2u;
          min_next = 0x80u;
          max_next = 0x9Fu;
        } else if (b == 0xF0u) {
          expected = 3u;
          min_next = 0x90u;
          max_next = 0xBFu;
        } else if (b >= 0xF1u && b <= 0xF3u) {
          expected = 3u;
          min_next = 0x80u;
          max_next = 0xBFu;
        } else if (b == 0xF4u) {
          expected = 3u;
          min_next = 0x80u;
          max_next = 0x8Fu;
        } else {
          fclose(file);
          return 0;
        }
      } else {
        if (b < min_next || b > max_next) {
          fclose(file);
          return 0;
        }
        --expected;
        min_next = 0x80u;
        max_next = 0xBFu;
      }
    }
  }
  if (ferror(file)) {
    fclose(file);
    set_lonejson_error(error, LONEJSON_STATUS_IO_ERROR,
                       "failed to read file-backed mutation value");
    return -1;
  }
  fclose(file);
  return expected == 0u ? 1 : 0;
}

static lonejson_status write_mutation_set_value(lonejson_writer *writer,
                                                const mutation_item *item,
                                                lonejson_error *error) {
  const char *value;
  const char *text;
  size_t len;
  double number;
  char number_buf[64];
  lonejson_source source;
  lonejson_status st;
  mutation_file_mode file_mode;
  int textlike;
  file_mode = item->file_mode;
  if (file_mode == MUTATION_FILE_AUTO) {
    textlike = inspect_file_textlike(item->file_path, error);
    if (textlike < 0) {
      return LONEJSON_STATUS_IO_ERROR;
    }
    file_mode = textlike ? MUTATION_FILE_TEXT : MUTATION_FILE_BASE64;
  }
  if (file_mode == MUTATION_FILE_TEXT || file_mode == MUTATION_FILE_BASE64) {
    lonejson_source_init(&source);
    st = lonejson_source_set_path(&source, item->file_path, error);
    if (st == LONEJSON_STATUS_OK) {
      if (file_mode == MUTATION_FILE_TEXT) {
        st = lonejson_writer_source_text(writer, &source, error);
      } else {
        st = lonejson_writer_source_base64(writer, &source, error);
      }
    }
    lonejson_source_cleanup(&source);
    return st;
  }
  value = item->value == NULL ? "" : item->value;
  text = unquoted_value(value, &len);
  if (ascii_equal_ignore_case_n(text, len, "true")) {
    return lonejson_writer_bool(writer, 1, error);
  }
  if (ascii_equal_ignore_case_n(text, len, "false")) {
    return lonejson_writer_bool(writer, 0, error);
  }
  if (ascii_equal_ignore_case_n(text, len, "null")) {
    return lonejson_writer_null(writer, error);
  }
  if (text == value && parse_number(value, &number)) {
    (void)number;
    return lonejson_writer_number_text(writer, value, strlen(value), error);
  }
  if (item->kind == MUTATION_INCREMENT) {
    sprintf(number_buf, "%.17g", item->delta);
    return lonejson_writer_number_text(writer, number_buf, strlen(number_buf),
                                       error);
  }
  return lonejson_writer_string(writer, text, len, error);
}

static lonejson_status finish_skip_value(mutation_stream_state *state) {
  if (state->skip_depth == 0u) {
    state->skipping = 0;
  }
  return LONEJSON_STATUS_OK;
}

static const mutation_path_frame *
mutation_parent_frame(const mutation_stream_state *state) {
  if (state == NULL || state->path_frame_count == 0u) {
    return NULL;
  }
  return &state->path_frames[state->path_frame_count - 1u];
}

static lonejson_status mutation_push_path_frame(mutation_stream_state *state,
                                                const lonejson_value_path *path,
                                                char container,
                                                lonejson_error *error) {
  const mutation_path_frame *parent;
  mutation_path_frame frame;
  mutation_path_frame *next;
  size_t i;
  (void)error;
  memset(&frame, 0, sizeof(frame));
  frame.container = container;
  if (path != NULL && path->segment_count != 0u) {
    frame.array_segments = (unsigned char *)calloc(
        path->segment_count, sizeof(frame.array_segments[0]));
    if (frame.array_segments == NULL) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    parent = mutation_parent_frame(state);
    if (parent != NULL) {
      for (i = 0u; i < parent->segment_count && i < path->segment_count; ++i) {
        frame.array_segments[i] = parent->array_segments[i];
      }
      if (path->segment_count > parent->segment_count &&
          parent->container == 'a') {
        frame.array_segments[path->segment_count - 1u] = 1u;
      }
    }
  }
  frame.segment_count = path == NULL ? 0u : path->segment_count;
  next = (mutation_path_frame *)realloc(state->path_frames,
                                        sizeof(state->path_frames[0]) *
                                            (state->path_frame_count + 1u));
  if (next == NULL) {
    free(frame.array_segments);
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  state->path_frames = next;
  state->path_frames[state->path_frame_count++] = frame;
  return LONEJSON_STATUS_OK;
}

static void mutation_pop_path_frame(mutation_stream_state *state) {
  if (state == NULL || state->path_frame_count == 0u) {
    return;
  }
  --state->path_frame_count;
  free(state->path_frames[state->path_frame_count].array_segments);
  if (state->path_frame_count == 0u) {
    free(state->path_frames);
    state->path_frames = NULL;
  }
}

static void mutation_cleanup_path_frames(mutation_stream_state *state) {
  while (state != NULL && state->path_frame_count != 0u) {
    mutation_pop_path_frame(state);
  }
}

static lonejson_status
begin_array_value_mutation(mutation_stream_state *state,
                           const lonejson_value_path *path,
                           lonejson_error *error, int *matched);

static lonejson_status mutation_object_begin(void *user,
                                             const lonejson_value_path *path,
                                             lonejson_error *error) {
  mutation_stream_state *state;
  lonejson_status frame_status;
  int matched_value;
  state = (mutation_stream_state *)user;
  if (state->active_increment) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  frame_status = mutation_push_path_frame(state, path, 'o', error);
  if (frame_status != LONEJSON_STATUS_OK) {
    return frame_status;
  }
  if (state->source_depth == 0u) {
    state->root_seen = 1;
    state->root_is_object = 1;
    ++state->source_depth;
    return lonejson_writer_begin_object(&state->writer, error);
  }
  if (begin_array_value_mutation(state, path, error, &matched_value) !=
      LONEJSON_STATUS_OK) {
    mutation_pop_path_frame(state);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (matched_value) {
    if (state->active_increment) {
      mutation_pop_path_frame(state);
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    if (state->skipping) {
      state->skip_depth = 1u;
    }
    ++state->source_depth;
    return LONEJSON_STATUS_OK;
  }
  if (state->skipping) {
    ++state->skip_depth;
    ++state->source_depth;
    return LONEJSON_STATUS_OK;
  }
  ++state->source_depth;
  return lonejson_writer_begin_object(&state->writer, error);
}

static int mutation_items_share_prefix(const mutation_item *a,
                                       const mutation_item *b, size_t depth) {
  size_t i;
  if (a->path.segment_count < depth || b->path.segment_count < depth) {
    return 0;
  }
  for (i = 0u; i < depth; ++i) {
    if (strcmp(a->path.segments[i], b->path.segments[i]) != 0) {
      return 0;
    }
  }
  return 1;
}

static lonejson_status write_synthetic_leaf_value(mutation_stream_state *state,
                                                  const mutation_item *item,
                                                  size_t index,
                                                  lonejson_error *error) {
  char number_buf[64];
  if (item->kind == MUTATION_INCREMENT) {
    sprintf(number_buf, "%.17g", item->delta);
    if (lonejson_writer_number_text(&state->writer, number_buf,
                                    strlen(number_buf),
                                    error) != LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
  } else if (write_mutation_set_value(&state->writer, item, error) !=
             LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  state->applied[index] = 1;
  return LONEJSON_STATUS_OK;
}

static lonejson_status
begin_array_value_mutation(mutation_stream_state *state,
                           const lonejson_value_path *path,
                           lonejson_error *error, int *matched) {
  const mutation_item *item;
  size_t index;
  *matched = 0;
  if (!mutation_value_index(state, path, &index)) {
    return LONEJSON_STATUS_OK;
  }
  *matched = 1;
  item = &state->plan->items[index];
  if (item->kind == MUTATION_REMOVE) {
    if (lonejson_writer_null(&state->writer, error) != LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->applied[index] = 1;
    state->skipping = 1;
    state->skip_depth = 0u;
    return LONEJSON_STATUS_OK;
  }
  if (item->kind == MUTATION_SET) {
    if (write_mutation_set_value(&state->writer, item, error) !=
        LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->applied[index] = 1;
    state->skipping = 1;
    state->skip_depth = 0u;
    return LONEJSON_STATUS_OK;
  }
  state->active_increment = 1;
  state->active_keyed = 0;
  state->active_index = index;
  free(state->num_buf);
  state->num_buf = NULL;
  state->num_len = 0u;
  return LONEJSON_STATUS_OK;
}

static lonejson_status write_synthetic_subtree(mutation_stream_state *state,
                                               const mutation_item *anchor,
                                               size_t depth,
                                               lonejson_error *error) {
  size_t i;
  const mutation_item *item;
  for (i = 0u; i < state->plan->count; ++i) {
    item = &state->plan->items[i];
    if (state->applied[i] || item->kind == MUTATION_REMOVE ||
        !mutation_items_share_prefix(anchor, item, depth) ||
        item->path.segment_count <= depth) {
      continue;
    }
    if (lonejson_writer_key(&state->writer, item->path.segments[depth],
                            strlen(item->path.segments[depth]),
                            error) != LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    if (item->path.segment_count == depth + 1u) {
      if (write_synthetic_leaf_value(state, item, i, error) !=
          LONEJSON_STATUS_OK) {
        return LONEJSON_STATUS_CALLBACK_FAILED;
      }
    } else {
      if (lonejson_writer_begin_object(&state->writer, error) !=
          LONEJSON_STATUS_OK) {
        return LONEJSON_STATUS_CALLBACK_FAILED;
      }
      if (write_synthetic_subtree(state, item, depth + 1u, error) !=
          LONEJSON_STATUS_OK) {
        return LONEJSON_STATUS_CALLBACK_FAILED;
      }
      if (lonejson_writer_end_object(&state->writer, error) !=
          LONEJSON_STATUS_OK) {
        return LONEJSON_STATUS_CALLBACK_FAILED;
      }
    }
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
write_missing_object_mutations(mutation_stream_state *state,
                               const lonejson_value_path *path,
                               lonejson_error *error) {
  size_t i;
  const mutation_item *item;
  const mutation_path_frame *frame;
  frame = current_path_frame(state, path);
  for (i = 0u; i < state->plan->count; ++i) {
    item = &state->plan->items[i];
    if (state->applied[i] || item->kind == MUTATION_REMOVE ||
        mutation_path_has_wildcard(&item->path) ||
        !mutation_descends_from_object(item, path, frame) ||
        state->prefix_seen_depth[i] > path->segment_count) {
      continue;
    }
    if (write_synthetic_subtree(state, item, path->segment_count, error) !=
        LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
  }
  return LONEJSON_STATUS_OK;
}

static int
immediate_array_object_mutation_index(const mutation_stream_state *state,
                                      const lonejson_value_path *path,
                                      size_t *out_index) {
  size_t i;
  const mutation_item *item;
  const mutation_path_frame *frame;
  if (path == NULL) {
    return 0;
  }
  frame = current_path_frame(state, path);
  for (i = 0u; i < state->plan->count; ++i) {
    item = &state->plan->items[i];
    if (state->applied[i] || item->kind == MUTATION_REMOVE ||
        mutation_path_has_wildcard(&item->path) ||
        item->path.segment_count != path->segment_count + 1u ||
        !stream_path_prefix_matches(&item->path, path, frame)) {
      continue;
    }
    *out_index = i;
    return 1;
  }
  return 0;
}

static lonejson_status
write_array_replacement_object(mutation_stream_state *state,
                               const lonejson_value_path *path,
                               lonejson_error *error) {
  size_t index;
  if (!immediate_array_object_mutation_index(state, path, &index)) {
    return LONEJSON_STATUS_OK;
  }
  if (lonejson_writer_begin_object(&state->writer, error) !=
          LONEJSON_STATUS_OK ||
      write_synthetic_subtree(state, &state->plan->items[index],
                              path->segment_count,
                              error) != LONEJSON_STATUS_OK ||
      lonejson_writer_end_object(&state->writer, error) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  state->skipping = 1;
  state->skip_depth = 0u;
  return LONEJSON_STATUS_OK;
}

static lonejson_status mutation_object_end(void *user,
                                           const lonejson_value_path *path,
                                           lonejson_error *error) {
  mutation_stream_state *state;
  lonejson_status status;
  state = (mutation_stream_state *)user;
  if (state->source_depth == 0u) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  --state->source_depth;
  if (state->skipping) {
    if (state->skip_depth != 0u) {
      --state->skip_depth;
    }
    status = finish_skip_value(state);
    mutation_pop_path_frame(state);
    return status;
  }
  if (write_missing_object_mutations(state, path, error) !=
      LONEJSON_STATUS_OK) {
    mutation_pop_path_frame(state);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  status = lonejson_writer_end_object(&state->writer, error);
  mutation_pop_path_frame(state);
  return status;
}

static lonejson_status mutation_array_begin(void *user,
                                            const lonejson_value_path *path,
                                            lonejson_error *error) {
  mutation_stream_state *state;
  lonejson_status frame_status;
  int matched_value;
  state = (mutation_stream_state *)user;
  if (state->active_increment) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  frame_status = mutation_push_path_frame(state, path, 'a', error);
  if (frame_status != LONEJSON_STATUS_OK) {
    return frame_status;
  }
  if (state->source_depth == 0u) {
    state->root_seen = 1;
    state->root_is_object = 0;
  }
  if (state->source_depth != 0u &&
      begin_array_value_mutation(state, path, error, &matched_value) !=
          LONEJSON_STATUS_OK) {
    mutation_pop_path_frame(state);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (state->source_depth != 0u && matched_value) {
    if (state->active_increment) {
      mutation_pop_path_frame(state);
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    if (state->skipping) {
      state->skip_depth = 1u;
    }
    ++state->source_depth;
    return LONEJSON_STATUS_OK;
  }
  if (state->skipping) {
    ++state->skip_depth;
    ++state->source_depth;
    return LONEJSON_STATUS_OK;
  }
  if (write_array_replacement_object(state, path, error) !=
      LONEJSON_STATUS_OK) {
    mutation_pop_path_frame(state);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (state->skipping) {
    ++state->skip_depth;
    ++state->source_depth;
    return LONEJSON_STATUS_OK;
  }
  ++state->source_depth;
  return lonejson_writer_begin_array(&state->writer, error);
}

static lonejson_status mutation_array_end(void *user,
                                          const lonejson_value_path *path,
                                          lonejson_error *error) {
  mutation_stream_state *state;
  lonejson_status status;
  (void)path;
  state = (mutation_stream_state *)user;
  if (state->source_depth == 0u) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  --state->source_depth;
  if (state->skipping) {
    if (state->skip_depth != 0u) {
      --state->skip_depth;
    }
    status = finish_skip_value(state);
    mutation_pop_path_frame(state);
    return status;
  }
  status = lonejson_writer_end_array(&state->writer, error);
  mutation_pop_path_frame(state);
  return status;
}

static lonejson_status mutation_key_begin(void *user,
                                          const lonejson_value_path *path,
                                          lonejson_error *error) {
  mutation_stream_state *state;
  (void)path;
  (void)error;
  state = (mutation_stream_state *)user;
  free(state->key_buf);
  state->key_buf = NULL;
  state->key_len = 0u;
  return LONEJSON_STATUS_OK;
}

static lonejson_status mutation_key_chunk(void *user,
                                          const lonejson_value_path *path,
                                          const char *data, size_t len,
                                          lonejson_error *error) {
  mutation_stream_state *state;
  (void)path;
  (void)error;
  state = (mutation_stream_state *)user;
  return append_buf(&state->key_buf, &state->key_len, data, len)
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_ALLOCATION_FAILED;
}

static lonejson_status mutation_key_end(void *user,
                                        const lonejson_value_path *path,
                                        lonejson_error *error) {
  mutation_stream_state *state;
  const mutation_item *item;
  const mutation_path_frame *frame;
  size_t index;
  size_t i;
  state = (mutation_stream_state *)user;
  if (state->skipping) {
    return LONEJSON_STATUS_OK;
  }
  frame = current_path_frame(state, path);
  for (i = 0u; i < state->plan->count; ++i) {
    if (path != NULL &&
        state->plan->items[i].path.segment_count > path->segment_count &&
        stream_path_prefix_matches(&state->plan->items[i].path, path, frame) &&
        (strcmp(state->plan->items[i].path.segments[path->segment_count],
                "*") == 0 ||
         (strlen(state->plan->items[i].path.segments[path->segment_count]) ==
              state->key_len &&
          memcmp(state->plan->items[i].path.segments[path->segment_count],
                 state->key_buf, state->key_len) == 0)) &&
        state->prefix_seen_depth[i] < path->segment_count + 1u) {
      state->prefix_seen_depth[i] = path->segment_count + 1u;
    }
  }
  if (mutation_key_index(state->plan, path, frame, state->key_buf,
                         state->key_len, &index)) {
    item = &state->plan->items[index];
    if (item->kind == MUTATION_REMOVE) {
      state->applied[index] = 1;
      state->skipping = 1;
      state->skip_depth = 0u;
      return LONEJSON_STATUS_OK;
    }
    if (item->kind == MUTATION_SET) {
      if (lonejson_writer_key(&state->writer, state->key_buf, state->key_len,
                              error) != LONEJSON_STATUS_OK ||
          write_mutation_set_value(&state->writer, item, error) !=
              LONEJSON_STATUS_OK) {
        return LONEJSON_STATUS_CALLBACK_FAILED;
      }
      state->applied[index] = 1;
      state->skipping = 1;
      state->skip_depth = 0u;
      return LONEJSON_STATUS_OK;
    }
    state->active_increment = 1;
    state->active_keyed = 1;
    state->active_index = index;
    free(state->num_buf);
    state->num_buf = NULL;
    state->num_len = 0u;
    return LONEJSON_STATUS_OK;
  }
  return lonejson_writer_key(&state->writer, state->key_buf, state->key_len,
                             error);
}

static lonejson_status mutation_string_begin(void *user,
                                             const lonejson_value_path *path,
                                             lonejson_error *error) {
  mutation_stream_state *state;
  int matched_value;
  state = (mutation_stream_state *)user;
  if (state->active_increment) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (state->source_depth == 0u) {
    state->root_seen = 1;
    state->root_is_object = 0;
  }
  if (state->skipping) {
    return LONEJSON_STATUS_OK;
  }
  if (begin_array_value_mutation(state, path, error, &matched_value) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (matched_value) {
    if (state->active_increment) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    return LONEJSON_STATUS_OK;
  }
  return lonejson_writer_string_begin(&state->writer, error);
}

static lonejson_status mutation_string_chunk(void *user,
                                             const lonejson_value_path *path,
                                             const char *data, size_t len,
                                             lonejson_error *error) {
  mutation_stream_state *state;
  (void)path;
  state = (mutation_stream_state *)user;
  if (state->skipping) {
    return LONEJSON_STATUS_OK;
  }
  return lonejson_writer_string_chunk(&state->writer, data, len, error);
}

static lonejson_status mutation_string_end(void *user,
                                           const lonejson_value_path *path,
                                           lonejson_error *error) {
  mutation_stream_state *state;
  (void)path;
  state = (mutation_stream_state *)user;
  if (state->skipping) {
    return finish_skip_value(state);
  }
  return lonejson_writer_string_end(&state->writer, error);
}

static lonejson_status mutation_number_begin(void *user,
                                             const lonejson_value_path *path,
                                             lonejson_error *error) {
  mutation_stream_state *state;
  int matched_value;
  state = (mutation_stream_state *)user;
  if (state->source_depth == 0u) {
    state->root_seen = 1;
    state->root_is_object = 0;
  }
  if (state->skipping) {
    free(state->num_buf);
    state->num_buf = NULL;
    state->num_len = 0u;
    return LONEJSON_STATUS_OK;
  }
  if (begin_array_value_mutation(state, path, error, &matched_value) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (matched_value) {
    return LONEJSON_STATUS_OK;
  }
  free(state->num_buf);
  state->num_buf = NULL;
  state->num_len = 0u;
  return LONEJSON_STATUS_OK;
}

static lonejson_status mutation_number_chunk(void *user,
                                             const lonejson_value_path *path,
                                             const char *data, size_t len,
                                             lonejson_error *error) {
  mutation_stream_state *state;
  (void)path;
  (void)error;
  state = (mutation_stream_state *)user;
  if (state->skipping) {
    return LONEJSON_STATUS_OK;
  }
  return append_buf(&state->num_buf, &state->num_len, data, len)
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_ALLOCATION_FAILED;
}

static lonejson_status mutation_number_end(void *user,
                                           const lonejson_value_path *path,
                                           lonejson_error *error) {
  mutation_stream_state *state;
  double existing;
  double next_value;
  char number_buf[64];
  const mutation_item *item;
  (void)path;
  state = (mutation_stream_state *)user;
  if (state->skipping) {
    return finish_skip_value(state);
  }
  if (!state->active_increment) {
    return lonejson_writer_number_text(&state->writer, state->num_buf,
                                       state->num_len, error);
  }
  if (!parse_number(state->num_buf, &existing)) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  item = &state->plan->items[state->active_index];
  next_value = existing + item->delta;
  sprintf(number_buf, "%.17g", next_value);
  if (state->active_keyed) {
    if (lonejson_writer_key(&state->writer, state->key_buf, state->key_len,
                            error) != LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
  }
  if (lonejson_writer_number_text(&state->writer, number_buf,
                                  strlen(number_buf),
                                  error) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  state->applied[state->active_index] = 1;
  state->active_increment = 0;
  state->active_keyed = 0;
  return LONEJSON_STATUS_OK;
}

static lonejson_status mutation_boolean(void *user,
                                        const lonejson_value_path *path,
                                        int value, lonejson_error *error) {
  mutation_stream_state *state;
  int matched_value;
  state = (mutation_stream_state *)user;
  if (state->source_depth == 0u) {
    state->root_seen = 1;
    state->root_is_object = 0;
  }
  if (state->active_increment) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (state->skipping) {
    return finish_skip_value(state);
  }
  if (begin_array_value_mutation(state, path, error, &matched_value) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (matched_value) {
    if (state->active_increment) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    return finish_skip_value(state);
  }
  return lonejson_writer_bool(&state->writer, value, error);
}

static lonejson_status mutation_null(void *user,
                                     const lonejson_value_path *path,
                                     lonejson_error *error) {
  mutation_stream_state *state;
  int matched_value;
  state = (mutation_stream_state *)user;
  if (state->source_depth == 0u) {
    state->root_seen = 1;
    state->root_is_object = 0;
  }
  if (state->active_increment) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (state->skipping) {
    return finish_skip_value(state);
  }
  if (begin_array_value_mutation(state, path, error, &matched_value) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (matched_value) {
    if (state->active_increment) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    return finish_skip_value(state);
  }
  return lonejson_writer_null(&state->writer, error);
}

static void init_mutation_visitor(lonejson_path_value_visitor *visitor) {
  *visitor = lonejson_default_path_value_visitor();
  visitor->object_begin = mutation_object_begin;
  visitor->object_end = mutation_object_end;
  visitor->object_key_begin = mutation_key_begin;
  visitor->object_key_chunk = mutation_key_chunk;
  visitor->object_key_end = mutation_key_end;
  visitor->array_begin = mutation_array_begin;
  visitor->array_end = mutation_array_end;
  visitor->string_begin = mutation_string_begin;
  visitor->string_chunk = mutation_string_chunk;
  visitor->string_end = mutation_string_end;
  visitor->number_begin = mutation_number_begin;
  visitor->number_chunk = mutation_number_chunk;
  visitor->number_end = mutation_number_end;
  visitor->boolean_value = mutation_boolean;
  visitor->null_value = mutation_null;
}

static lql_status
mutate_file_range_with_supported_plan(const lql_mutation_plan *plan, FILE *file,
                                      lql_uint64 offset, lql_uint64 size,
                                      FILE *out, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_status st;
  limited_file_reader reader;
  mutation_stream_state state;
  size_t i;

  if (plan == NULL || file == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "plan, file, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (!seek_u64(file, offset)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to seek mutation source range");
    return LQL_STATUS_JSON_ERROR;
  }
  runtime = lonejson_new(NULL, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  memset(&state, 0, sizeof(state));
  state.plan = plan;
  state.error = &lj_error;
  state.applied = (int *)calloc(plan->count, sizeof(state.applied[0]));
  state.prefix_seen_depth =
      (size_t *)calloc(plan->count, sizeof(state.prefix_seen_depth[0]));
  if (state.applied == NULL || state.prefix_seen_depth == NULL) {
    free(state.applied);
    free(state.prefix_seen_depth);
    lonejson_free(runtime);
    return LQL_STATUS_NO_MEMORY;
  }
  if (lonejson_writer_init_sink(runtime, &state.writer, file_sink, out,
                                &lj_error) != LONEJSON_STATUS_OK) {
    free(state.applied);
    free(state.prefix_seen_depth);
    lonejson_free(runtime);
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  init_mutation_visitor(&visitor);
  reader.file = file;
  reader.remaining = size;
  st = lonejson_visit_path_value_reader(runtime, limited_read, &reader,
                                        &visitor, &state, &lj_error);
  if (st == LONEJSON_STATUS_OK && (!state.root_seen || !state.root_is_object)) {
    st = LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (st == LONEJSON_STATUS_OK) {
    for (i = 0u; i < plan->count; ++i) {
      if (!state.applied[i] && plan->items[i].kind != MUTATION_REMOVE &&
          !mutation_path_has_wildcard(&plan->items[i].path)) {
        st = LONEJSON_STATUS_CALLBACK_FAILED;
        lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                      "mutation path could not be applied without "
                      "unsupported materialization");
        break;
      }
    }
  }
  if (st == LONEJSON_STATUS_OK) {
    st = lonejson_writer_finish(&state.writer, &lj_error);
  }
  lonejson_writer_cleanup(&state.writer);
  mutation_cleanup_path_frames(&state);
  free(state.key_buf);
  free(state.num_buf);
  free(state.applied);
  free(state.prefix_seen_depth);
  lonejson_free(runtime);
  if (st != LONEJSON_STATUS_OK) {
    if (error != NULL && error->code == LQL_STATUS_UNSUPPORTED) {
      return LQL_STATUS_UNSUPPORTED;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  return LQL_STATUS_OK;
}

lql_status lql_mutate_file_range_root_fields(const lql_mutation_plan *plan,
                                             FILE *file, lql_uint64 offset,
                                             lql_uint64 size, FILE *out,
                                             lql_error *error) {
  if (plan == NULL || file == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "plan, file, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (!mutation_plan_supports_root_fields(plan)) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "mutation plan requires unsupported non-root behavior");
    return LQL_STATUS_UNSUPPORTED;
  }
  return mutate_file_range_with_supported_plan(plan, file, offset, size, out,
                                               error);
}

lql_status lql_mutate_file_range_paths(const lql_mutation_plan *plan,
                                       FILE *file, lql_uint64 offset,
                                       lql_uint64 size, FILE *out,
                                       lql_error *error) {
  if (plan == NULL || file == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "plan, file, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (!mutation_plan_supports_stream_paths(plan)) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "mutation plan requires unsupported path behavior");
    return LQL_STATUS_UNSUPPORTED;
  }
  return mutate_file_range_with_supported_plan(plan, file, offset, size, out,
                                               error);
}
