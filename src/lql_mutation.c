#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "lql_internal.h"

#include <ctype.h>
#include <limits.h>
#include <lonejson.h>
#include <stddef.h>
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
  unsigned char *segment_kinds;
  size_t *segment_lens;
  size_t segment_count;
  int has_wildcard;
} mutation_path;

#define MUTATION_PATH_LITERAL 0u
#define MUTATION_PATH_OBJECT_WILDCARD 1u
#define MUTATION_PATH_ARRAY_WILDCARD 2u
#define MUTATION_PATH_RECURSIVE 3u
#define MUTATION_PATH_ELLIPSIS 4u

typedef struct mutation_item {
  mutation_kind kind;
  mutation_path path;
  char *value;
  double delta;
  int time_value;
  int can_create_missing_object;
  mutation_file_mode file_mode;
  char *file_path;
} mutation_item;

struct lql_mutation_plan {
  mutation_item *items;
  size_t count;
  int literal_only_paths;
};

static void mutation_plan_refresh_traits(lql_mutation_plan *plan) {
  size_t i;
  size_t j;
  int literal_only;
  if (plan == NULL) {
    return;
  }
  literal_only = plan->count != 0u;
  for (i = 0u; i < plan->count && literal_only; ++i) {
    for (j = 0u; j < plan->items[i].path.segment_count; ++j) {
      if (plan->items[i].path.segment_kinds[j] != MUTATION_PATH_LITERAL) {
        literal_only = 0;
        break;
      }
    }
  }
  plan->literal_only_paths = literal_only;
}

typedef struct limited_file_reader {
  FILE *file;
  lql_uint64 remaining;
} limited_file_reader;

typedef struct buffer_reader {
  const unsigned char *data;
  size_t len;
  size_t offset;
} buffer_reader;

typedef struct mutation_source_reader {
  lql_read_fn read;
  void *user;
  int error_code;
} mutation_source_reader;

typedef struct mutation_path_frame {
  unsigned char *array_segments;
  unsigned long array_segment_bits;
  size_t segment_count;
  char container;
} mutation_path_frame;

#define MUTATION_FRAME_INLINE_BITS (sizeof(unsigned long) * CHAR_BIT)
#define MUTATION_FRAME_INLINE_COUNT 32u
#define MUTATION_PLAN_INLINE_COUNT 16u
#define MUTATION_KEY_INLINE_CAP 128u
#define MUTATION_NUM_INLINE_CAP 64u

static int mutation_frame_array_segment(const mutation_path_frame *frame,
                                        size_t index) {
  if (frame == NULL || index >= frame->segment_count) {
    return 0;
  }
  if (frame->array_segments != NULL) {
    return frame->array_segments[index] ? 1 : 0;
  }
  if (index >= MUTATION_FRAME_INLINE_BITS) {
    return 0;
  }
  return (frame->array_segment_bits & (1UL << index)) != 0u;
}

static int
mutation_frame_array_segment_known(const mutation_path_frame *frame,
                                   size_t index) {
  if (frame->array_segments != NULL) {
    return frame->array_segments[index] ? 1 : 0;
  }
  return (frame->array_segment_bits & (1UL << index)) != 0u;
}

static void mutation_frame_set_array_segment(mutation_path_frame *frame,
                                             size_t index) {
  if (frame == NULL || index >= frame->segment_count) {
    return;
  }
  if (frame->array_segments != NULL) {
    frame->array_segments[index] = 1u;
    return;
  }
  if (index < MUTATION_FRAME_INLINE_BITS) {
    frame->array_segment_bits |= 1UL << index;
  }
}

typedef struct mutation_stream_state {
  lql_allocator *allocator;
  const lql_mutation_plan *plan;
  lonejson_writer writer;
  lonejson_error *error;
  char *key_buf;
  size_t key_len;
  char inline_key_buf[MUTATION_KEY_INLINE_CAP];
  char *num_buf;
  size_t num_len;
  char inline_num_buf[MUTATION_NUM_INLINE_CAP];
  int *applied;
  size_t *prefix_seen_depth;
  void *plan_scratch_alloc;
  size_t source_depth;
  int root_seen;
  int root_is_object;
  int skipping;
  size_t skip_depth;
  int skip_has_mask;
  size_t skip_mask_index;
  int active_increment;
  int active_keyed;
  size_t active_index;
  mutation_path_frame *path_frames;
  size_t path_frame_count;
  size_t path_frame_cap;
  mutation_path_frame inline_path_frames[MUTATION_FRAME_INLINE_COUNT];
  int inline_applied[MUTATION_PLAN_INLINE_COUNT];
  size_t inline_prefix_seen_depth[MUTATION_PLAN_INLINE_COUNT];
} mutation_stream_state;

typedef struct string_list {
  char **items;
  size_t count;
} string_list;

typedef struct mutation_parse_context {
  lql *self;
  lql_allocator *allocator;
  const lql_mutation_parse_options *options;
  lql_error *error;
} mutation_parse_context;

static void mutation_stream_state_init(mutation_stream_state *state) {
  memset(state, 0, offsetof(mutation_stream_state, inline_path_frames));
}

static void mutation_path_cleanup(lql *self, mutation_path *path) {
  lql_allocator *allocator;
  size_t i;
  if (path == NULL) {
    return;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    return;
  }
  for (i = 0u; i < path->segment_count; ++i) {
    allocator->destroy(allocator, path->segments[i]);
  }
  allocator->destroy(allocator, path->segments);
  allocator->destroy(allocator, path->segment_kinds);
  allocator->destroy(allocator, path->segment_lens);
  path->segments = NULL;
  path->segment_kinds = NULL;
  path->segment_lens = NULL;
  path->segment_count = 0u;
  path->has_wildcard = 0;
}

static void mutation_item_cleanup(lql *self, mutation_item *item) {
  lql_allocator *allocator;
  if (item == NULL) {
    return;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    return;
  }
  mutation_path_cleanup(self, &item->path);
  allocator->destroy(allocator, item->value);
  allocator->destroy(allocator, item->file_path);
  memset(item, 0, sizeof(*item));
}

static void mutation_plan_cleanup_items(lql *self, lql_mutation_plan *plan) {
  lql_allocator *allocator;
  size_t i;
  if (plan == NULL) {
    return;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    return;
  }
  for (i = 0u; i < plan->count; ++i) {
    mutation_item_cleanup(self, &plan->items[i]);
  }
  allocator->destroy(allocator, plan->items);
  plan->items = NULL;
  plan->count = 0u;
  plan->literal_only_paths = 0;
}

static void string_list_cleanup(mutation_parse_context *ctx,
                                string_list *list) {
  size_t i;
  if (list == NULL) {
    return;
  }
  for (i = 0u; i < list->count; ++i) {
    ctx->allocator->destroy(ctx->allocator, list->items[i]);
  }
  ctx->allocator->destroy(ctx->allocator, list->items);
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

static char *trimmed_dup_range(mutation_parse_context *ctx, const char *start,
                               size_t len) {
  const char *limit;
  const char *end;
  char *out;
  limit = start + len;
  while (start < limit && (*start == ' ' || *start == '\t' || *start == '\r' ||
                           *start == '\n')) {
    ++start;
  }
  end = limit;
  while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                         end[-1] == '\n')) {
    --end;
  }
  len = (size_t)(end - start);
  out = (char *)ctx->allocator->alloc(ctx->allocator, len + 1u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, start, len);
  out[len] = '\0';
  return out;
}

static char *trimmed_dup_runtime_range(mutation_stream_state *state,
                                       const char *start, size_t len) {
  const char *end;
  char *out;
  start = skip_space(start);
  end = start + len;
  while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                         end[-1] == '\n')) {
    --end;
  }
  len = (size_t)(end - start);
  out = (char *)state->allocator->alloc(state->allocator, len + 1u);
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

static char *mutation_unquote(mutation_parse_context *ctx, char *value) {
  size_t len;
  char *out;
  char *r;
  char *w;
  len = strlen(value);
  if (len >= 2u && ((value[0] == '"' && value[len - 1u] == '"') ||
                    (value[0] == '\'' && value[len - 1u] == '\''))) {
    out = (char *)ctx->allocator->alloc(ctx->allocator, len - 1u);
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
    ctx->allocator->destroy(ctx->allocator, value);
    return out;
  }
  return value;
}

static int path_is_absolute(const char *path) {
  return path != NULL && path[0] == '/';
}

static char *join_paths(mutation_parse_context *ctx, const char *base,
                        const char *path) {
  size_t base_len;
  size_t path_len;
  char *out;
  int need_sep;
  base_len = strlen(base);
  path_len = strlen(path);
  need_sep = base_len != 0u && base[base_len - 1u] != '/';
  out = (char *)ctx->allocator->alloc(
      ctx->allocator, base_len + (need_sep ? 1u : 0u) + path_len + 1u);
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

static char *resolve_file_value_path(mutation_parse_context *ctx,
                                     const char *raw) {
  char *path;
  const char *home;
  char *expanded;
  path = trimmed_dup_range(ctx, raw, strlen(raw));
  if (path == NULL) {
    return NULL;
  }
  path = mutation_unquote(ctx, path);
  if (path == NULL) {
    return NULL;
  }
  if (path[0] == '\0') {
    ctx->allocator->destroy(ctx->allocator, path);
    lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                  "file-backed mutation missing file path");
    return NULL;
  }
  if (strcmp(path, "~") == 0 || has_prefix(path, "~/")) {
    home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
      ctx->allocator->destroy(ctx->allocator, path);
      lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                    "failed to resolve home for file-backed mutation");
      return NULL;
    }
    if (strcmp(path, "~") == 0) {
      expanded = ctx->allocator->strdup(ctx->allocator, home);
    } else {
      expanded = join_paths(ctx, home, path + 2u);
    }
    ctx->allocator->destroy(ctx->allocator, path);
    return expanded;
  }
  if (path_is_absolute(path)) {
    return path;
  }
  if (ctx->options == NULL || ctx->options->file_value_base_dir == NULL ||
      ctx->options->file_value_base_dir[0] == '\0') {
    lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                  "relative file-backed mutation path requires file value base "
                  "dir");
    ctx->allocator->destroy(ctx->allocator, path);
    return NULL;
  }
  expanded = join_paths(ctx, ctx->options->file_value_base_dir, path);
  ctx->allocator->destroy(ctx->allocator, path);
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
#if defined(__linux__)
  return fwrite_unlocked(data, 1u, len, out) == len
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_CALLBACK_FAILED;
#else
  return fwrite(data, 1u, len, out) == len ? LONEJSON_STATUS_OK
                                           : LONEJSON_STATUS_CALLBACK_FAILED;
#endif
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
  if (result.bytes_read != want) {
    result.error_code = 1;
  }
  if (reader->remaining == 0u) {
    result.eof = 1;
  }
  return result;
}

static lonejson_read_result buffer_read(void *user, unsigned char *buffer,
                                        size_t capacity) {
  buffer_reader *reader;
  lonejson_read_result result;
  size_t remaining;
  size_t want;

  result = lonejson_default_read_result();
  reader = (buffer_reader *)user;
  if (reader->offset >= reader->len) {
    result.eof = 1;
    return result;
  }
  remaining = reader->len - reader->offset;
  want = remaining > capacity ? capacity : remaining;
  if (want != 0u) {
    memcpy(buffer, reader->data + reader->offset, want);
    reader->offset += want;
  }
  result.bytes_read = want;
  if (reader->offset >= reader->len) {
    result.eof = 1;
  }
  return result;
}

static lonejson_read_result
mutation_source_read(void *user, unsigned char *buffer, size_t capacity) {
  mutation_source_reader *reader;
  lql_read_result source_result;
  lonejson_read_result result;

  result = lonejson_default_read_result();
  reader = (mutation_source_reader *)user;
  source_result = reader->read(reader->user, buffer, capacity);
  if (source_result.bytes_read > capacity) {
    reader->error_code = 1;
    result.error_code = 1;
    return result;
  }
  if (source_result.error_code != 0) {
    reader->error_code = source_result.error_code;
    result.error_code = source_result.error_code;
    return result;
  }
  result.bytes_read = source_result.bytes_read;
  result.eof = source_result.eof;
  return result;
}

static void mutation_inline_buf_reset(mutation_stream_state *state, char **buf,
                                      size_t *len, char *inline_buf) {
  if (state == NULL || buf == NULL || len == NULL) {
    return;
  }
  if (*buf != NULL && *buf != inline_buf) {
    state->allocator->destroy(state->allocator, *buf);
  }
  *buf = NULL;
  *len = 0u;
}

static int mutation_inline_buf_append(mutation_stream_state *state, char **buf,
                                      size_t *len, char *inline_buf,
                                      size_t inline_cap, const char *data,
                                      size_t n) {
  char *next;
  size_t next_len;
  if (state == NULL || buf == NULL || len == NULL || inline_buf == NULL ||
      data == NULL) {
    return 0;
  }
  if (n > (size_t)-1 - *len - 1u) {
    return 0;
  }
  next_len = *len + n;
  if (*buf == NULL || *buf == inline_buf) {
    if (next_len + 1u <= inline_cap) {
      if (*buf == NULL) {
        *buf = inline_buf;
      }
      memcpy(inline_buf + *len, data, n);
      *len = next_len;
      inline_buf[*len] = '\0';
      return 1;
    }
    next = (char *)state->allocator->alloc(state->allocator, next_len + 1u);
    if (next == NULL) {
      return 0;
    }
    if (*len != 0u) {
      memcpy(next, inline_buf, *len);
    }
  } else {
    next = (char *)state->allocator->realloc(state->allocator, *buf,
                                             next_len + 1u);
    if (next == NULL) {
      return 0;
    }
  }
  memcpy(next + *len, data, n);
  *len = next_len;
  next[*len] = '\0';
  *buf = next;
  return 1;
}

static void mutation_key_reset(mutation_stream_state *state) {
  if (state == NULL) {
    return;
  }
  mutation_inline_buf_reset(state, &state->key_buf, &state->key_len,
                            state->inline_key_buf);
}

static int mutation_key_append(mutation_stream_state *state, const char *data,
                               size_t n) {
  if (state == NULL) {
    return 0;
  }
  return mutation_inline_buf_append(
      state, &state->key_buf, &state->key_len, state->inline_key_buf,
      MUTATION_KEY_INLINE_CAP, data, n);
}

static void mutation_num_reset(mutation_stream_state *state) {
  if (state == NULL) {
    return;
  }
  mutation_inline_buf_reset(state, &state->num_buf, &state->num_len,
                            state->inline_num_buf);
}

static int mutation_num_append(mutation_stream_state *state, const char *data,
                               size_t n) {
  if (state == NULL) {
    return 0;
  }
  return mutation_inline_buf_append(
      state, &state->num_buf, &state->num_len, state->inline_num_buf,
      MUTATION_NUM_INLINE_CAP, data, n);
}

static int add_string(mutation_parse_context *ctx, string_list *list,
                      char *value) {
  char **next;
  next = (char **)ctx->allocator->realloc(
      ctx->allocator, list->items, sizeof(list->items[0]) * (list->count + 1u));
  if (next == NULL) {
    return 0;
  }
  list->items = next;
  list->items[list->count++] = value;
  return 1;
}

static int split_expressions(mutation_parse_context *ctx, const char *input,
                             string_list *out) {
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
      if (depth != 0) {
        --depth;
      }
    } else if ((*p == ',' || *p == '\n') && !in_quote && depth == 0) {
      item = trimmed_dup_range(ctx, chunk, (size_t)(p - chunk));
      if (item == NULL) {
        string_list_cleanup(ctx, out);
        return 0;
      }
      if (item[0] != '\0' && !add_string(ctx, out, item)) {
        ctx->allocator->destroy(ctx->allocator, item);
        string_list_cleanup(ctx, out);
        return 0;
      }
      if (item[0] == '\0') {
        ctx->allocator->destroy(ctx->allocator, item);
      }
      chunk = p + 1;
    }
    ++p;
  }
  if (in_quote) {
    lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                  "unterminated quote in mutation expression");
    string_list_cleanup(ctx, out);
    return 0;
  }
  if (depth != 0) {
    lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                  "unterminated brace in mutation expression");
    string_list_cleanup(ctx, out);
    return 0;
  }
  len = trimmed_len(chunk);
  if (len != 0u) {
    item = trimmed_dup_range(ctx, chunk, strlen(chunk));
    if (item == NULL || !add_string(ctx, out, item)) {
      ctx->allocator->destroy(ctx->allocator, item);
      string_list_cleanup(ctx, out);
      return 0;
    }
  }
  return 1;
}

static char *decode_path_segment(mutation_parse_context *ctx, const char *src,
                                 size_t len) {
  char *out;
  size_t i;
  size_t j;
  out = (char *)ctx->allocator->alloc(ctx->allocator, len + 1u);
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

static int path_add_segment(mutation_parse_context *ctx, mutation_path *path,
                            char *segment) {
  char **next;
  unsigned char *next_kinds;
  size_t *next_lens;
  size_t len;
  unsigned char kind;

  len = strlen(segment);
  kind = MUTATION_PATH_LITERAL;
  if (len == 1u && segment[0] == '*') {
    kind = MUTATION_PATH_OBJECT_WILDCARD;
  } else if (len == 2u && segment[0] == '[' && segment[1] == ']') {
    kind = MUTATION_PATH_ARRAY_WILDCARD;
  } else if (len == 2u && segment[0] == '*' && segment[1] == '*') {
    kind = MUTATION_PATH_RECURSIVE;
  } else if (len == 3u && segment[0] == '.' && segment[1] == '.' &&
             segment[2] == '.') {
    kind = MUTATION_PATH_ELLIPSIS;
  }

  next = (char **)ctx->allocator->realloc(ctx->allocator, path->segments,
                                          sizeof(path->segments[0]) *
                                              (path->segment_count + 1u));
  if (next == NULL) {
    return 0;
  }
  path->segments = next;
  next_kinds = (unsigned char *)ctx->allocator->realloc(
      ctx->allocator, path->segment_kinds,
      sizeof(path->segment_kinds[0]) * (path->segment_count + 1u));
  if (next_kinds == NULL) {
    return 0;
  }
  path->segment_kinds = next_kinds;
  next_lens = (size_t *)ctx->allocator->realloc(
      ctx->allocator, path->segment_lens,
      sizeof(path->segment_lens[0]) * (path->segment_count + 1u));
  if (next_lens == NULL) {
    return 0;
  }
  path->segment_lens = next_lens;
  path->segments[path->segment_count] = segment;
  path->segment_kinds[path->segment_count] = kind;
  path->segment_lens[path->segment_count] = len;
  if (kind != MUTATION_PATH_LITERAL) {
    path->has_wildcard = 1;
  }
  ++path->segment_count;
  return 1;
}

static int expand_and_add_segment(mutation_parse_context *ctx,
                                  mutation_path *path, char *segment) {
  size_t len;
  char *base;
  char *wild;
  len = strlen(segment);
  if (len > 2u && strcmp(segment, "[]") != 0 &&
      strcmp(segment + len - 2u, "[]") == 0) {
    segment[len - 2u] = '\0';
    base = ctx->allocator->strdup(ctx->allocator, segment);
    segment[len - 2u] = '[';
    if (base == NULL) {
      ctx->allocator->destroy(ctx->allocator, segment);
      return 0;
    }
    wild = ctx->allocator->strdup(ctx->allocator, "[]");
    if (wild == NULL) {
      ctx->allocator->destroy(ctx->allocator, base);
      ctx->allocator->destroy(ctx->allocator, segment);
      return 0;
    }
    ctx->allocator->destroy(ctx->allocator, segment);
    return path_add_segment(ctx, path, base) &&
           path_add_segment(ctx, path, wild);
  }
  return path_add_segment(ctx, path, segment);
}

static int split_path(mutation_parse_context *ctx, const char *raw,
                      mutation_path *out) {
  const char *path;
  const char *seg;
  const char *slash;
  char *decoded;
  size_t len;

  memset(out, 0, sizeof(*out));
  path = skip_space(raw);
  len = trimmed_len(path);
  if (len == 0u) {
    lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR, "path empty");
    return 0;
  }
  if (path[0] != '/') {
    lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                  "mutation path must start with '/'");
    return 0;
  }
  seg = path + 1;
  while ((size_t)(seg - path) <= len) {
    slash = memchr(seg, '/', (size_t)(path + len - seg));
    if (slash == NULL) {
      slash = path + len;
    }
    decoded = decode_path_segment(ctx, seg, (size_t)(slash - seg));
    if (decoded == NULL || !expand_and_add_segment(ctx, out, decoded)) {
      ctx->allocator->destroy(ctx->allocator, decoded);
      mutation_path_cleanup(ctx->self, out);
      return 0;
    }
    if (slash == path + len) {
      break;
    }
    seg = slash + 1;
  }
  if (out->segment_count == 0u ||
      (out->segment_count == 1u && out->segments[0][0] == '\0')) {
    lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                  "mutation path refers to document root");
    mutation_path_cleanup(ctx->self, out);
    return 0;
  }
  return 1;
}

static int append_item(mutation_parse_context *ctx, lql_mutation_plan *plan,
                       mutation_item *item) {
  mutation_item *next;
  item->can_create_missing_object =
      item->kind != MUTATION_REMOVE && !item->path.has_wildcard;
  next = (mutation_item *)ctx->allocator->realloc(
      ctx->allocator, plan->items, sizeof(plan->items[0]) * (plan->count + 1u));
  if (next == NULL) {
    return 0;
  }
  plan->items = next;
  plan->items[plan->count++] = *item;
  memset(item, 0, sizeof(*item));
  return 1;
}

static int prepend_path(mutation_parse_context *ctx, mutation_item *item,
                        const mutation_path *prefix) {
  char **segments;
  unsigned char *segment_kinds;
  size_t *segment_lens;
  size_t i;
  size_t count;
  count = prefix->segment_count + item->path.segment_count;
  segments = (char **)ctx->allocator->calloc(ctx->allocator, count,
                                             sizeof(segments[0]));
  if (segments == NULL) {
    return 0;
  }
  segment_kinds = (unsigned char *)ctx->allocator->calloc(
      ctx->allocator, count, sizeof(segment_kinds[0]));
  if (segment_kinds == NULL) {
    ctx->allocator->destroy(ctx->allocator, segments);
    return 0;
  }
  segment_lens = (size_t *)ctx->allocator->calloc(ctx->allocator, count,
                                                  sizeof(segment_lens[0]));
  if (segment_lens == NULL) {
    ctx->allocator->destroy(ctx->allocator, segment_kinds);
    ctx->allocator->destroy(ctx->allocator, segments);
    return 0;
  }
  for (i = 0u; i < prefix->segment_count; ++i) {
    segments[i] = ctx->allocator->strdup(ctx->allocator, prefix->segments[i]);
    if (segments[i] == NULL) {
      goto fail;
    }
    segment_kinds[i] = prefix->segment_kinds[i];
    segment_lens[i] = prefix->segment_lens[i];
  }
  for (i = 0u; i < item->path.segment_count; ++i) {
    segments[prefix->segment_count + i] = item->path.segments[i];
    segment_kinds[prefix->segment_count + i] = item->path.segment_kinds[i];
    segment_lens[prefix->segment_count + i] = item->path.segment_lens[i];
    item->path.segments[i] = NULL;
  }
  ctx->allocator->destroy(ctx->allocator, item->path.segments);
  ctx->allocator->destroy(ctx->allocator, item->path.segment_kinds);
  ctx->allocator->destroy(ctx->allocator, item->path.segment_lens);
  item->path.segments = segments;
  item->path.segment_kinds = segment_kinds;
  item->path.segment_lens = segment_lens;
  item->path.segment_count = count;
  item->path.has_wildcard = prefix->has_wildcard || item->path.has_wildcard;
  return 1;
fail:
  for (i = 0u; i < count; ++i) {
    ctx->allocator->destroy(ctx->allocator, segments[i]);
  }
  ctx->allocator->destroy(ctx->allocator, segment_lens);
  ctx->allocator->destroy(ctx->allocator, segment_kinds);
  ctx->allocator->destroy(ctx->allocator, segments);
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

static int parse_number_slice(mutation_stream_state *state, const char *s,
                              size_t len, double *out) {
  char *copy;
  int ok;
  copy = trimmed_dup_runtime_range(state, s, len);
  if (copy == NULL) {
    return 0;
  }
  ok = parse_number(copy, out);
  state->allocator->destroy(state->allocator, copy);
  return ok;
}

static const char *unquoted_value(const char *value, size_t *out_len);

static int parse_mutation_expr(mutation_parse_context *ctx, const char *expr,
                               lql_mutation_plan *plan);

static int parse_brace_mutation(mutation_parse_context *ctx, const char *expr,
                                lql_mutation_plan *plan) {
  const char *open;
  const char *close;
  const char *p;
  size_t len;
  char *prefix_text;
  char *body;
  int depth;
  int in_quote;
  int escape;
  char quote;
  mutation_path prefix;
  string_list parts;
  lql_mutation_plan nested;
  size_t i;

  len = strlen(expr);
  if (len == 0u) {
    return 0;
  }
  open = strchr(expr, '{');
  if (open == NULL || open == expr) {
    return 0;
  }
  depth = 1;
  in_quote = 0;
  escape = 0;
  quote = '\0';
  close = NULL;
  for (p = open + 1; *p != '\0'; ++p) {
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
      --depth;
      if (depth == 0) {
        close = p;
        break;
      }
    }
  }
  if (close == NULL || in_quote) {
    lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                  "unterminated brace in mutation expression");
    return -1;
  }
  if (*skip_space(close + 1) != '\0') {
    lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                  "unexpected trailing text after brace mutation");
    return -1;
  }
  prefix_text = trimmed_dup_range(ctx, expr, (size_t)(open - expr));
  if (prefix_text == NULL) {
    return -1;
  }
  if (strchr(prefix_text, '=') != NULL) {
    ctx->allocator->destroy(ctx->allocator, prefix_text);
    return 0;
  }
  if (!split_path(ctx, prefix_text, &prefix)) {
    ctx->allocator->destroy(ctx->allocator, prefix_text);
    return -1;
  }
  ctx->allocator->destroy(ctx->allocator, prefix_text);
  body = trimmed_dup_range(ctx, open + 1, (size_t)(close - open - 1));
  if (body == NULL) {
    mutation_path_cleanup(ctx->self, &prefix);
    return -1;
  }
  if (!split_expressions(ctx, body, &parts)) {
    ctx->allocator->destroy(ctx->allocator, body);
    mutation_path_cleanup(ctx->self, &prefix);
    return -1;
  }
  ctx->allocator->destroy(ctx->allocator, body);
  if (parts.count == 0u) {
    lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR, "brace mutation empty");
    string_list_cleanup(ctx, &parts);
    mutation_path_cleanup(ctx->self, &prefix);
    return -1;
  }
  memset(&nested, 0, sizeof(nested));
  for (i = 0u; i < parts.count; ++i) {
    if (!parse_mutation_expr(ctx, parts.items[i], &nested)) {
      mutation_plan_cleanup_items(ctx->self, &nested);
      string_list_cleanup(ctx, &parts);
      mutation_path_cleanup(ctx->self, &prefix);
      return -1;
    }
  }
  for (i = 0u; i < nested.count; ++i) {
    if (!prepend_path(ctx, &nested.items[i], &prefix) ||
        !append_item(ctx, plan, &nested.items[i])) {
      mutation_plan_cleanup_items(ctx->self, &nested);
      string_list_cleanup(ctx, &parts);
      mutation_path_cleanup(ctx->self, &prefix);
      return -1;
    }
  }
  ctx->allocator->destroy(ctx->allocator, nested.items);
  string_list_cleanup(ctx, &parts);
  mutation_path_cleanup(ctx->self, &prefix);
  return 1;
}

static int parse_set_value(mutation_parse_context *ctx, char *value,
                           int time_mode, mutation_item *item) {
  lql_temporal temporal;
  const char *text;
  size_t len;
  char normalized[40];
  char *copy;
  if (time_mode) {
    text = unquoted_value(value, &len);
    copy = trimmed_dup_range(ctx, text, len);
    if (copy == NULL) {
      return 0;
    }
    if (ascii_equal_ignore_case(copy, "NOW")) {
      if (!lql_temporal_now(&temporal)) {
        ctx->allocator->destroy(ctx->allocator, copy);
        lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                      "failed to resolve current time");
        return 0;
      }
    } else if (!time_literal_has_zone(copy) ||
               !lql_parse_temporal_literal(copy, &temporal) ||
               temporal.date_only) {
      ctx->allocator->destroy(ctx->allocator, copy);
      lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR, "invalid time literal");
      return 0;
    }
    ctx->allocator->destroy(ctx->allocator, copy);
    if (!lql_temporal_format_rfc3339_nano(&temporal, normalized,
                                          sizeof(normalized))) {
      lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR, "invalid time literal");
      return 0;
    }
    item->value = ctx->allocator->strdup(ctx->allocator, normalized);
    if (item->value == NULL) {
      return 0;
    }
    ctx->allocator->destroy(ctx->allocator, value);
    item->time_value = 1;
    return 1;
  }
  item->value = value;
  return 1;
}

static int parse_mutation_expr(mutation_parse_context *ctx, const char *raw,
                               lql_mutation_plan *plan) {
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

  expr = trimmed_dup_range(ctx, raw, strlen(raw));
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
      lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                    "time-prefixed mutation has invalid prefix combination");
      ctx->allocator->destroy(ctx->allocator, expr);
      return 0;
    }
    time_mode = 1;
    memmove(expr, expr + 5, strlen(expr + 5) + 1u);
  }
  memset(&item, 0, sizeof(item));
  if (remove_mode) {
    if (file_mode != MUTATION_FILE_NONE) {
      lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                    "file-backed mutation has invalid prefix combination");
      ctx->allocator->destroy(ctx->allocator, expr);
      return 0;
    }
    item.kind = MUTATION_REMOVE;
    if (!split_path(ctx, expr, &item.path) || !append_item(ctx, plan, &item)) {
      mutation_item_cleanup(ctx->self, &item);
      ctx->allocator->destroy(ctx->allocator, expr);
      return 0;
    }
    ctx->allocator->destroy(ctx->allocator, expr);
    return 1;
  }
  if (has_suffix(expr, "++") || has_suffix(expr, "--")) {
    if (file_mode != MUTATION_FILE_NONE) {
      lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                    "file-backed mutation does not support increment");
      ctx->allocator->destroy(ctx->allocator, expr);
      return 0;
    }
    if (time_mode) {
      lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                    "time-prefixed mutation does not support increment");
      ctx->allocator->destroy(ctx->allocator, expr);
      return 0;
    }
    len = strlen(expr);
    delta = expr[len - 2u] == '+' ? 1.0 : -1.0;
    expr[len - 2u] = '\0';
    item.kind = MUTATION_INCREMENT;
    item.delta = delta;
    if (!split_path(ctx, expr, &item.path) || !append_item(ctx, plan, &item)) {
      mutation_item_cleanup(ctx->self, &item);
      ctx->allocator->destroy(ctx->allocator, expr);
      return 0;
    }
    ctx->allocator->destroy(ctx->allocator, expr);
    return 1;
  }
  brace_result = parse_brace_mutation(ctx, expr, plan);
  if (brace_result != 0) {
    ctx->allocator->destroy(ctx->allocator, expr);
    return brace_result > 0;
  }
  eq = strchr(expr, '=');
  if (eq == NULL) {
    lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                  "invalid mutation, expected key=value");
    ctx->allocator->destroy(ctx->allocator, expr);
    return 0;
  }
  *eq = '\0';
  path_text = expr;
  value = trimmed_dup_range(ctx, eq + 1, strlen(eq + 1));
  if (value == NULL) {
    ctx->allocator->destroy(ctx->allocator, expr);
    return 0;
  }
  item.kind = MUTATION_SET;
  if (file_mode != MUTATION_FILE_NONE) {
    if (ctx->options == NULL || !ctx->options->enable_file_values) {
      lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                    "file-backed mutations are disabled");
      ctx->allocator->destroy(ctx->allocator, value);
      ctx->allocator->destroy(ctx->allocator, expr);
      return 0;
    }
    item.file_mode = file_mode;
    item.file_path = resolve_file_value_path(ctx, value);
    ctx->allocator->destroy(ctx->allocator, value);
    if (item.file_path == NULL) {
      ctx->allocator->destroy(ctx->allocator, expr);
      return 0;
    }
    if (!split_path(ctx, path_text, &item.path) ||
        !append_item(ctx, plan, &item)) {
      mutation_item_cleanup(ctx->self, &item);
      ctx->allocator->destroy(ctx->allocator, expr);
      return 0;
    }
    ctx->allocator->destroy(ctx->allocator, expr);
    return 1;
  }
  if (!time_mode && (value[0] == '+' || value[0] == '-') &&
      parse_number(value, &delta)) {
    if (delta == 0.0) {
      lql_set_error(ctx->error, LQL_STATUS_PARSE_ERROR,
                    "increment mutation requires non-zero delta");
      ctx->allocator->destroy(ctx->allocator, value);
      ctx->allocator->destroy(ctx->allocator, expr);
      return 0;
    }
    item.kind = MUTATION_INCREMENT;
    item.delta = delta;
    ctx->allocator->destroy(ctx->allocator, value);
  } else if (!parse_set_value(ctx, value, time_mode, &item)) {
    ctx->allocator->destroy(ctx->allocator, value);
    ctx->allocator->destroy(ctx->allocator, expr);
    return 0;
  }
  if (!split_path(ctx, path_text, &item.path) ||
      !append_item(ctx, plan, &item)) {
    mutation_item_cleanup(ctx->self, &item);
    ctx->allocator->destroy(ctx->allocator, expr);
    return 0;
  }
  ctx->allocator->destroy(ctx->allocator, expr);
  return 1;
}

static lql_status mutation_plan_parse_with_options_method(
    lql *self, const char *const *exprs, size_t expr_count,
    const lql_mutation_parse_options *options, lql_mutation_plan **out,
    lql_error *error) {
  lql_allocator *allocator;
  lql_mutation_plan *plan;
  mutation_parse_context parse_ctx;
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
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "lql receiver allocator required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  parse_ctx.self = self;
  parse_ctx.allocator = allocator;
  parse_ctx.options = options;
  parse_ctx.error = error;
  plan = (lql_mutation_plan *)allocator->calloc(allocator, 1u, sizeof(*plan));
  if (plan == NULL) {
    return LQL_STATUS_NO_MEMORY;
  }
  for (i = 0u; i < expr_count; ++i) {
    if (exprs[i] == NULL) {
      continue;
    }
    if (!split_expressions(&parse_ctx, exprs[i], &parts)) {
      self->mutation_plan_destroy(self, plan);
      return error != NULL && error->code != LQL_STATUS_OK
                 ? error->code
                 : LQL_STATUS_NO_MEMORY;
    }
    for (j = 0u; j < parts.count; ++j) {
      if (!parse_mutation_expr(&parse_ctx, parts.items[j], plan)) {
        string_list_cleanup(&parse_ctx, &parts);
        self->mutation_plan_destroy(self, plan);
        return error != NULL && error->code != LQL_STATUS_OK
                   ? error->code
                   : LQL_STATUS_NO_MEMORY;
      }
    }
    string_list_cleanup(&parse_ctx, &parts);
  }
  if (plan->count == 0u) {
    self->mutation_plan_destroy(self, plan);
    lql_set_error(error, LQL_STATUS_PARSE_ERROR,
                  "no valid field mutations parsed");
    return LQL_STATUS_PARSE_ERROR;
  }
  mutation_plan_refresh_traits(plan);
  *out = plan;
  return LQL_STATUS_OK;
}

static lql_status mutation_plan_parse_method(lql *self,
                                             const char *const *exprs,
                                             size_t expr_count,
                                             lql_mutation_plan **out,
                                             lql_error *error) {
  return self->mutation_plan_parse_with_options(self, exprs, expr_count, NULL,
                                                out, error);
}

static size_t mutation_plan_count_method(const lql *self,
                                         const lql_mutation_plan *plan) {
  (void)self;
  return plan == NULL ? 0u : plan->count;
}

static void mutation_plan_destroy_method(lql *self, lql_mutation_plan *plan) {
  lql_allocator *allocator;
  if (plan == NULL) {
    return;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    return;
  }
  mutation_plan_cleanup_items(self, plan);
  allocator->destroy(allocator, plan);
}

static int mutation_is_root_field_supported(const mutation_item *item) {
  return item->path.segment_count == 1u && item->path.segments[0][0] != '\0' &&
         item->path.segment_kinds[0] == MUTATION_PATH_LITERAL;
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
          plan->items[i].path.segment_lens[0] ==
              plan->items[j].path.segment_lens[0] &&
          memcmp(plan->items[i].path.segments[0],
                 plan->items[j].path.segments[0],
                 plan->items[i].path.segment_lens[0]) == 0) {
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

static int mutation_paths_equal(const mutation_path *a,
                                const mutation_path *b) {
  size_t i;
  if (a->segment_count != b->segment_count) {
    return 0;
  }
  for (i = 0u; i < a->segment_count; ++i) {
    if (a->segment_kinds[i] != b->segment_kinds[i] ||
        a->segment_lens[i] != b->segment_lens[i] ||
        memcmp(a->segments[i], b->segments[i], a->segment_lens[i]) != 0) {
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
    if (prefix->segment_kinds[i] != path->segment_kinds[i] ||
        prefix->segment_lens[i] != path->segment_lens[i] ||
        memcmp(prefix->segments[i], path->segments[i],
               prefix->segment_lens[i]) != 0) {
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
      if (!plan->items[i].path.has_wildcard &&
          !plan->items[j].path.has_wildcard &&
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

static int stream_path_segment_matches(unsigned char expected_kind,
                                       const char *expected,
                                       size_t expected_len,
                                       const lonejson_path_segment *actual,
                                       int actual_is_array) {
  switch (expected_kind) {
  case MUTATION_PATH_OBJECT_WILDCARD:
    return !actual_is_array;
  case MUTATION_PATH_ARRAY_WILDCARD:
    return actual_is_array;
  case MUTATION_PATH_RECURSIVE:
    return 1;
  default:
    return expected_len == actual->len &&
           memcmp(expected, actual->data, actual->len) == 0;
  }
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
        item_path->segment_kinds[item_index], item_path->segments[item_index],
        item_path->segment_lens[item_index], &parent->segments[actual_index],
        mutation_frame_array_segment_known(frame, actual_index));
  }
  segment.data = key;
  segment.len = key_len;
  return stream_path_segment_matches(
      item_path->segment_kinds[item_index], item_path->segments[item_index],
      item_path->segment_lens[item_index], &segment, 0);
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
  while (item_index < item_path->segment_count) {
    if (item_path->segment_kinds[item_index] == MUTATION_PATH_ELLIPSIS) {
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
    ++item_index;
    ++actual_index;
  }
  return actual_index == actual_count;
}

static const mutation_path_frame *
current_value_path_frame(const mutation_stream_state *state) {
  if (state == NULL || state->path_frame_count == 0u) {
    return NULL;
  }
  return &state->path_frames[state->path_frame_count - 1u];
}

static int value_path_segment_is_array_from_frame(
    const mutation_path_frame *frame, const lonejson_value_path *path,
    size_t index) {
  if (frame == NULL || path == NULL || index >= path->segment_count) {
    return 0;
  }
  if (frame->segment_count == path->segment_count) {
    return mutation_frame_array_segment_known(frame, index);
  }
  if (frame->segment_count + 1u == path->segment_count) {
    if (index < frame->segment_count) {
      return mutation_frame_array_segment_known(frame, index);
    }
    return frame->container == 'a';
  }
  return 0;
}

static int
value_path_is_array_element_from_frame(const mutation_path_frame *frame,
                                       const lonejson_value_path *path) {
  if (path == NULL || path->segment_count == 0u) {
    return 0;
  }
  return value_path_segment_is_array_from_frame(frame, path,
                                                path->segment_count - 1u);
}

static int
value_path_item_matches_from_frame(const mutation_path_frame *frame,
                                   const mutation_path *item_path,
                                   size_t item_index,
                                   const lonejson_value_path *path,
                                   size_t path_index) {
  size_t i;
  while (item_index < item_path->segment_count) {
    if (item_path->segment_kinds[item_index] == MUTATION_PATH_ELLIPSIS) {
      if (item_index + 1u == item_path->segment_count) {
        return 1;
      }
      for (i = path_index; i <= path->segment_count; ++i) {
        if (value_path_item_matches_from_frame(frame, item_path,
                                               item_index + 1u, path, i)) {
          return 1;
        }
      }
      return 0;
    }
    if (path_index >= path->segment_count ||
        !stream_path_segment_matches(
            item_path->segment_kinds[item_index],
            item_path->segments[item_index],
            item_path->segment_lens[item_index], &path->segments[path_index],
            value_path_segment_is_array_from_frame(frame, path, path_index))) {
      return 0;
    }
    ++item_index;
    ++path_index;
  }
  return path_index == path->segment_count;
}

static int mutation_value_index(const mutation_stream_state *state,
                                const lonejson_value_path *path, size_t *out) {
  size_t i;
  const mutation_path_frame *frame;
  frame = current_value_path_frame(state);
  if (!value_path_is_array_element_from_frame(frame, path)) {
    return 0;
  }
  for (i = 0u; i < state->plan->count; ++i) {
    if (value_path_item_matches_from_frame(frame, &state->plan->items[i].path,
                                           0u, path, 0u)) {
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
    if (!stream_path_segment_matches(
            item_path->segment_kinds[i], item_path->segments[i],
            item_path->segment_lens[i], &path->segments[i],
            mutation_frame_array_segment_known(frame, i))) {
      return 0;
    }
  }
  return 1;
}

static int stream_path_prefix_matches_known(const mutation_path *item_path,
                                            const lonejson_value_path *path,
                                            const mutation_path_frame *frame,
                                            size_t depth) {
  size_t i;
  for (i = 0u; i < depth; ++i) {
    if (!stream_path_segment_matches(
            item_path->segment_kinds[i], item_path->segments[i],
            item_path->segment_lens[i], &path->segments[i],
            mutation_frame_array_segment_known(frame, i))) {
      return 0;
    }
  }
  return 1;
}

static int mutation_item_matches_virtual_key(const mutation_item *item,
                                             const lonejson_value_path *parent,
                                             const mutation_path_frame *frame,
                                             const char *key, size_t key_len) {
  return virtual_path_matches_from(&item->path, 0u, parent, frame, key, key_len,
                                   0u);
}

static int mutation_descends_from_object(const mutation_item *item,
                                         const lonejson_value_path *path,
                                         const mutation_path_frame *frame) {
  return path != NULL && item->path.segment_count > path->segment_count &&
         stream_path_prefix_matches(&item->path, path, frame);
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

static int inspect_file_textlike(const char *path, lonejson_error *error,
                                 int report_text_error) {
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
        if (report_text_error) {
          set_lonejson_error(error, LONEJSON_STATUS_CALLBACK_FAILED,
                             "file-backed text mutation contains NUL byte");
        }
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
          if (report_text_error) {
            set_lonejson_error(error, LONEJSON_STATUS_CALLBACK_FAILED,
                               "file-backed text mutation contains invalid "
                               "UTF-8");
          }
          fclose(file);
          return 0;
        }
      } else {
        if (b < min_next || b > max_next) {
          if (report_text_error) {
            set_lonejson_error(error, LONEJSON_STATUS_CALLBACK_FAILED,
                               "file-backed text mutation contains invalid "
                               "UTF-8");
          }
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
  if (expected != 0u && report_text_error) {
    set_lonejson_error(error, LONEJSON_STATUS_CALLBACK_FAILED,
                       "file-backed text mutation contains invalid UTF-8");
  }
  return expected == 0u ? 1 : 0;
}

static lonejson_status write_mutation_set_value(mutation_stream_state *state,
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
    textlike = inspect_file_textlike(item->file_path, error, 0);
    if (textlike < 0) {
      return LONEJSON_STATUS_IO_ERROR;
    }
    file_mode = textlike ? MUTATION_FILE_TEXT : MUTATION_FILE_BASE64;
  } else if (file_mode == MUTATION_FILE_TEXT) {
    textlike = inspect_file_textlike(item->file_path, error, 1);
    if (textlike < 0) {
      return LONEJSON_STATUS_IO_ERROR;
    }
    if (!textlike) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
  }
  if (file_mode == MUTATION_FILE_TEXT || file_mode == MUTATION_FILE_BASE64) {
    lonejson_source_init(&source);
    st = lonejson_source_set_path(&source, item->file_path, error);
    if (st == LONEJSON_STATUS_OK) {
      if (file_mode == MUTATION_FILE_TEXT) {
        st = lonejson_writer_source_text(&state->writer, &source, error);
      } else {
        st = lonejson_writer_source_base64(&state->writer, &source, error);
      }
    }
    lonejson_source_cleanup(&source);
    return st;
  }
  value = item->value == NULL ? "" : item->value;
  text = unquoted_value(value, &len);
  if (ascii_equal_ignore_case_n(text, len, "true")) {
    return lonejson_writer_bool(&state->writer, 1, error);
  }
  if (ascii_equal_ignore_case_n(text, len, "false")) {
    return lonejson_writer_bool(&state->writer, 0, error);
  }
  if (ascii_equal_ignore_case_n(text, len, "null")) {
    return lonejson_writer_null(&state->writer, error);
  }
  if (parse_number_slice(state, text, len, &number)) {
    (void)number;
    return lonejson_writer_number_text(&state->writer, text, len, error);
  }
  if (item->kind == MUTATION_INCREMENT) {
    sprintf(number_buf, "%.17g", item->delta);
    return lonejson_writer_number_text(&state->writer, number_buf,
                                       strlen(number_buf), error);
  }
  return lonejson_writer_string(&state->writer, text, len, error);
}

static lonejson_status finish_skip_value(mutation_stream_state *state) {
  if (state->skip_depth == 0u) {
    state->skipping = 0;
    state->skip_has_mask = 0;
    state->skip_mask_index = 0u;
  }
  return LONEJSON_STATUS_OK;
}

static void begin_masked_skip(mutation_stream_state *state, size_t index) {
  state->skipping = 1;
  state->skip_depth = 0u;
  state->skip_has_mask = 1;
  state->skip_mask_index = index;
}

static lonejson_status mutation_increment_not_numeric(lonejson_error *error) {
  set_lonejson_error(error, LONEJSON_STATUS_CALLBACK_FAILED,
                     "mutation increment target not numeric");
  return LONEJSON_STATUS_CALLBACK_FAILED;
}

static int skipped_earlier_increment_index(const mutation_stream_state *state,
                                           const lonejson_value_path *path,
                                           size_t *out) {
  size_t i;
  const mutation_item *item;
  const mutation_path_frame *frame;
  if (state == NULL || path == NULL || !state->skipping ||
      !state->skip_has_mask) {
    return 0;
  }
  frame = current_value_path_frame(state);
  for (i = 0u; i < state->skip_mask_index; ++i) {
    item = &state->plan->items[i];
    if (item->kind == MUTATION_INCREMENT &&
        value_path_item_matches_from_frame(frame, &item->path, 0u, path, 0u)) {
      *out = i;
      return 1;
    }
  }
  return 0;
}

static int skipped_earlier_increment_key_index(
    const mutation_stream_state *state, const lonejson_value_path *parent,
    const mutation_path_frame *frame, const char *key, size_t key_len,
    size_t *out) {
  size_t i;
  const mutation_item *item;
  if (state == NULL || !state->skipping || !state->skip_has_mask) {
    return 0;
  }
  if (parent == NULL || frame == NULL ||
      frame->segment_count != parent->segment_count) {
    return 0;
  }
  for (i = 0u; i < state->skip_mask_index; ++i) {
    item = &state->plan->items[i];
    if (item->kind == MUTATION_INCREMENT &&
        mutation_item_matches_virtual_key(item, parent, frame, key, key_len)) {
      *out = i;
      return 1;
    }
  }
  return 0;
}

static int mutation_scan_key(mutation_stream_state *state,
                             const lonejson_value_path *path,
                             const mutation_path_frame *frame, const char *key,
                             size_t key_len, size_t *out) {
  const mutation_item *item;
  size_t i;
  size_t depth;
  size_t next_depth;
  unsigned char next_kind;
  int found;

  if (path == NULL || frame == NULL ||
      frame->segment_count != path->segment_count) {
    return 0;
  }

  depth = path->segment_count;
  next_depth = depth + 1u;
  found = 0;
  for (i = 0u; i < state->plan->count; ++i) {
    item = &state->plan->items[i];
    if (state->prefix_seen_depth[i] < next_depth &&
        item->path.segment_count > depth &&
        stream_path_prefix_matches_known(&item->path, path, frame, depth)) {
      next_kind = item->path.segment_kinds[depth];
      if (next_kind == MUTATION_PATH_OBJECT_WILDCARD ||
          (next_kind == MUTATION_PATH_LITERAL &&
           item->path.segment_lens[depth] == key_len &&
           memcmp(item->path.segments[depth], key, key_len) == 0)) {
        state->prefix_seen_depth[i] = next_depth;
      }
    }
    if (!found &&
        mutation_item_matches_virtual_key(item, path, frame, key, key_len)) {
      *out = i;
      found = 1;
    }
  }
  return found;
}

static int mutation_scan_key_literal_plan(
    mutation_stream_state *state, const lonejson_value_path *path,
    const mutation_path_frame *frame, const char *key, size_t key_len,
    size_t *out) {
  const mutation_item *item;
  size_t i;
  size_t depth;
  size_t next_depth;
  int found;

  if (path == NULL || frame == NULL ||
      frame->segment_count != path->segment_count) {
    return 0;
  }

  depth = path->segment_count;
  next_depth = depth + 1u;
  found = 0;
  for (i = 0u; i < state->plan->count; ++i) {
    item = &state->plan->items[i];
    if (item->path.segment_count <= depth) {
      continue;
    }
    if (item->path.segment_lens[depth] != key_len ||
        memcmp(item->path.segments[depth], key, key_len) != 0) {
      continue;
    }
    if (!stream_path_prefix_matches_known(&item->path, path, frame, depth)) {
      continue;
    }
    if (state->prefix_seen_depth[i] < next_depth) {
      state->prefix_seen_depth[i] = next_depth;
    }
    if (!found && item->path.segment_count == next_depth) {
      *out = i;
      found = 1;
    }
  }
  return found;
}

static const mutation_path_frame *
mutation_parent_frame(const mutation_stream_state *state) {
  if (state == NULL || state->path_frame_count == 0u) {
    return NULL;
  }
  return &state->path_frames[state->path_frame_count - 1u];
}

static int mutation_ensure_path_frame_capacity(mutation_stream_state *state) {
  mutation_path_frame *next;
  size_t next_cap;

  if (state == NULL) {
    return 0;
  }
  if (state->path_frames == NULL) {
    state->path_frames = state->inline_path_frames;
    state->path_frame_cap = MUTATION_FRAME_INLINE_COUNT;
  }
  if (state->path_frame_count < state->path_frame_cap) {
    return 1;
  }
  next_cap = state->path_frame_cap * 2u;
  if (next_cap <= state->path_frame_cap) {
    return 0;
  }
  if (state->path_frames == state->inline_path_frames) {
    next = (mutation_path_frame *)state->allocator->alloc(
        state->allocator, sizeof(next[0]) * next_cap);
    if (next == NULL) {
      return 0;
    }
    memcpy(next, state->path_frames, sizeof(next[0]) * state->path_frame_count);
  } else {
    next = (mutation_path_frame *)state->allocator->realloc(
        state->allocator, state->path_frames, sizeof(next[0]) * next_cap);
    if (next == NULL) {
      return 0;
    }
  }
  state->path_frames = next;
  state->path_frame_cap = next_cap;
  return 1;
}

static lonejson_status mutation_push_path_frame(mutation_stream_state *state,
                                                const lonejson_value_path *path,
                                                char container,
                                                lonejson_error *error) {
  const mutation_path_frame *parent;
  mutation_path_frame frame;
  size_t i;
  (void)error;
  memset(&frame, 0, sizeof(frame));
  frame.container = container;
  frame.segment_count = path == NULL ? 0u : path->segment_count;
  if (frame.segment_count > MUTATION_FRAME_INLINE_BITS) {
    frame.array_segments = (unsigned char *)state->allocator->calloc(
        state->allocator, frame.segment_count, sizeof(frame.array_segments[0]));
    if (frame.array_segments == NULL) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
  }
  if (frame.segment_count != 0u) {
    parent = mutation_parent_frame(state);
    if (parent != NULL) {
      for (i = 0u; i < parent->segment_count && i < frame.segment_count; ++i) {
        if (mutation_frame_array_segment(parent, i)) {
          mutation_frame_set_array_segment(&frame, i);
        }
      }
      if (frame.segment_count > parent->segment_count &&
          parent->container == 'a') {
        mutation_frame_set_array_segment(&frame, frame.segment_count - 1u);
      }
    }
  }
  if (!mutation_ensure_path_frame_capacity(state)) {
    state->allocator->destroy(state->allocator, frame.array_segments);
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  state->path_frames[state->path_frame_count++] = frame;
  return LONEJSON_STATUS_OK;
}

static void mutation_pop_path_frame(mutation_stream_state *state) {
  if (state == NULL || state->path_frame_count == 0u) {
    return;
  }
  --state->path_frame_count;
  state->allocator->destroy(
      state->allocator,
      state->path_frames[state->path_frame_count].array_segments);
  if (state->path_frame_count == 0u) {
    if (state->path_frames != state->inline_path_frames) {
      state->allocator->destroy(state->allocator, state->path_frames);
    }
    state->path_frames = NULL;
    state->path_frame_cap = 0u;
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
static lonejson_status
write_missing_object_mutations(mutation_stream_state *state,
                               const lonejson_value_path *path,
                               lonejson_error *error);
static lonejson_status write_synthetic_subtree(mutation_stream_state *state,
                                               const mutation_item *anchor,
                                               size_t depth,
                                               lonejson_error *error);

typedef struct mutation_missing_object_match {
  size_t index;
  size_t depth;
  int virtual_object;
} mutation_missing_object_match;

static int mutation_descends_from_virtual_object(
    const mutation_item *item, const lonejson_value_path *parent,
    const mutation_path_frame *frame, const char *key, size_t key_len) {
  size_t i;
  size_t depth;
  lonejson_path_segment segment;
  if (item == NULL || parent == NULL || frame == NULL ||
      frame->segment_count != parent->segment_count) {
    return 0;
  }
  depth = parent->segment_count + 1u;
  if (item->path.segment_count <= depth) {
    return 0;
  }
  for (i = 0u; i < parent->segment_count; ++i) {
    if (!stream_path_segment_matches(
            item->path.segment_kinds[i], item->path.segments[i],
            item->path.segment_lens[i], &parent->segments[i],
            mutation_frame_array_segment_known(frame, i))) {
      return 0;
    }
  }
  segment.data = key;
  segment.len = key_len;
  return stream_path_segment_matches(
      item->path.segment_kinds[parent->segment_count],
      item->path.segments[parent->segment_count],
      item->path.segment_lens[parent->segment_count], &segment, 0);
}

static int
mutation_find_missing_object_key_value(mutation_stream_state *state,
                                       const lonejson_value_path *parent,
                                       mutation_missing_object_match *match) {
  size_t i;
  size_t depth;
  const mutation_item *item;
  const mutation_path_frame *frame;
  if (parent != NULL && parent->segment_count != 0u) {
    frame = current_path_frame(state, parent);
    for (i = 0u; i < state->plan->count; ++i) {
      item = &state->plan->items[i];
      if (state->applied[i] || !item->can_create_missing_object ||
          state->prefix_seen_depth[i] > parent->segment_count ||
          !mutation_descends_from_object(item, parent, frame)) {
        continue;
      }
      match->index = i;
      match->depth = parent->segment_count;
      match->virtual_object = 0;
      return 1;
    }
  }
  frame = current_path_frame(state, parent);
  depth = parent == NULL ? 0u : parent->segment_count + 1u;
  for (i = 0u; i < state->plan->count; ++i) {
    item = &state->plan->items[i];
    if (state->applied[i] || !item->can_create_missing_object ||
        state->prefix_seen_depth[i] > depth ||
        !mutation_descends_from_virtual_object(
            item, parent, frame, state->key_buf, state->key_len)) {
      continue;
    }
    match->index = i;
    match->depth = depth;
    match->virtual_object = 1;
    return 1;
  }
  return 0;
}

static lonejson_status
write_missing_object_key_value(mutation_stream_state *state,
                               const lonejson_value_path *parent,
                               const mutation_missing_object_match *match,
                               lonejson_error *error) {
  size_t i;
  size_t depth;
  const mutation_item *item;
  const mutation_path_frame *frame;
  if (match == NULL || match->index >= state->plan->count) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  frame = current_path_frame(state, parent);
  depth = match->depth;
  if (lonejson_writer_begin_object(&state->writer, error) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (write_synthetic_subtree(state, &state->plan->items[match->index], depth,
                              error) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  for (i = match->index + 1u; i < state->plan->count; ++i) {
    item = &state->plan->items[i];
    if (state->applied[i] || !item->can_create_missing_object ||
        state->prefix_seen_depth[i] > depth) {
      continue;
    }
    if (match->virtual_object) {
      if (!mutation_descends_from_virtual_object(
              item, parent, frame, state->key_buf, state->key_len)) {
        continue;
      }
    } else if (!mutation_descends_from_object(item, parent, frame)) {
      continue;
    }
    if (write_synthetic_subtree(state, item, depth, error) !=
        LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
  }
  if (lonejson_writer_end_object(&state->writer, error) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status mutation_object_begin(void *user,
                                             const lonejson_value_path *path,
                                             lonejson_error *error) {
  mutation_stream_state *state;
  lonejson_status frame_status;
  int matched_value;
  state = (mutation_stream_state *)user;
  if (state->active_increment) {
    return mutation_increment_not_numeric(error);
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
  if (state->skipping) {
    if (skipped_earlier_increment_index(state, path, &state->active_index)) {
      return mutation_increment_not_numeric(error);
    }
    ++state->skip_depth;
    ++state->source_depth;
    return LONEJSON_STATUS_OK;
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
    if (a->path.segment_kinds[i] != b->path.segment_kinds[i] ||
        a->path.segment_lens[i] != b->path.segment_lens[i] ||
        memcmp(a->path.segments[i], b->path.segments[i],
               a->path.segment_lens[i]) != 0) {
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
  } else if (write_mutation_set_value(state, item, error) !=
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
    begin_masked_skip(state, index);
    return LONEJSON_STATUS_OK;
  }
  if (item->kind == MUTATION_SET) {
    if (write_mutation_set_value(state, item, error) != LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->applied[index] = 1;
    begin_masked_skip(state, index);
    return LONEJSON_STATUS_OK;
  }
  state->active_increment = 1;
  state->active_keyed = 0;
  state->active_index = index;
  mutation_num_reset(state);
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
                            item->path.segment_lens[depth],
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
    if (state->applied[i] || !item->can_create_missing_object ||
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
  mutation_missing_object_match missing;
  int matched_value;
  state = (mutation_stream_state *)user;
  if (state->active_increment) {
    return mutation_increment_not_numeric(error);
  }
  frame_status = mutation_push_path_frame(state, path, 'a', error);
  if (frame_status != LONEJSON_STATUS_OK) {
    return frame_status;
  }
  if (state->source_depth == 0u) {
    state->root_seen = 1;
    state->root_is_object = 0;
  }
  if (state->source_depth != 0u && state->skipping) {
    if (skipped_earlier_increment_index(state, path, &state->active_index)) {
      return mutation_increment_not_numeric(error);
    }
    ++state->skip_depth;
    ++state->source_depth;
    return LONEJSON_STATUS_OK;
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
  if (state->source_depth != 0u && !state->skipping &&
      mutation_find_missing_object_key_value(state, path, &missing)) {
    if (write_missing_object_key_value(state, path, &missing, error) !=
        LONEJSON_STATUS_OK) {
      mutation_pop_path_frame(state);
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->skipping = 1;
    state->skip_depth = 1u;
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
  mutation_key_reset(state);
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
  return mutation_key_append(state, data, len)
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
  state = (mutation_stream_state *)user;
  if (state->skipping) {
    frame = current_path_frame(state, path);
    if (skipped_earlier_increment_key_index(state, path, frame, state->key_buf,
                                            state->key_len, &index)) {
      state->active_increment = 1;
      state->active_keyed = 1;
      state->active_index = index;
      mutation_num_reset(state);
    }
    return LONEJSON_STATUS_OK;
  }
  frame = current_path_frame(state, path);
  if ((state->plan->literal_only_paths
           ? mutation_scan_key_literal_plan(state, path, frame, state->key_buf,
                                            state->key_len, &index)
           : mutation_scan_key(state, path, frame, state->key_buf,
                               state->key_len, &index))) {
    item = &state->plan->items[index];
    if (item->kind == MUTATION_REMOVE) {
      state->applied[index] = 1;
      begin_masked_skip(state, index);
      return LONEJSON_STATUS_OK;
    }
    if (item->kind == MUTATION_SET) {
      if (lonejson_writer_key(&state->writer, state->key_buf, state->key_len,
                              error) != LONEJSON_STATUS_OK ||
          write_mutation_set_value(state, item, error) != LONEJSON_STATUS_OK) {
        return LONEJSON_STATUS_CALLBACK_FAILED;
      }
      state->applied[index] = 1;
      begin_masked_skip(state, index);
      return LONEJSON_STATUS_OK;
    }
    state->active_increment = 1;
    state->active_keyed = 1;
    state->active_index = index;
    mutation_num_reset(state);
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
    return mutation_increment_not_numeric(error);
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
    if (state->active_increment ||
        skipped_earlier_increment_index(state, path, &state->active_index)) {
      state->active_increment = 1;
      state->active_keyed = 0;
      mutation_num_reset(state);
      return LONEJSON_STATUS_OK;
    }
    mutation_num_reset(state);
    return LONEJSON_STATUS_OK;
  }
  if (begin_array_value_mutation(state, path, error, &matched_value) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (matched_value) {
    return LONEJSON_STATUS_OK;
  }
  mutation_num_reset(state);
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
    if (state->active_increment && !mutation_num_append(state, data, len)) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    return LONEJSON_STATUS_OK;
  }
  return mutation_num_append(state, data, len)
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
    if (state->active_increment) {
      if (!parse_number(state->num_buf, &existing)) {
        return mutation_increment_not_numeric(error);
      }
      state->applied[state->active_index] = 1;
      state->active_increment = 0;
      state->active_keyed = 0;
    }
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
    return mutation_increment_not_numeric(error);
  }
  if (state->skipping) {
    if (skipped_earlier_increment_index(state, path, &state->active_index)) {
      return mutation_increment_not_numeric(error);
    }
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
    return mutation_increment_not_numeric(error);
  }
  if (state->skipping) {
    if (skipped_earlier_increment_index(state, path, &state->active_index)) {
      return mutation_increment_not_numeric(error);
    }
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

static int mutation_state_init_plan_scratch(mutation_stream_state *state,
                                            const lql_mutation_plan *plan) {
  size_t max_size;
  size_t count;
  size_t applied_bytes;
  size_t prefix_offset;
  size_t prefix_bytes;
  size_t align;
  size_t rem;
  size_t total_bytes;
  char *scratch;
  if (state == NULL || plan == NULL) {
    return 0;
  }
  if (plan->count <= MUTATION_PLAN_INLINE_COUNT) {
    state->applied = state->inline_applied;
    state->prefix_seen_depth = state->inline_prefix_seen_depth;
    state->plan_scratch_alloc = NULL;
    memset(state->applied, 0, sizeof(state->applied[0]) * plan->count);
    memset(state->prefix_seen_depth, 0,
           sizeof(state->prefix_seen_depth[0]) * plan->count);
    return 1;
  }
  max_size = (size_t)-1;
  count = plan->count;
  if (count > max_size / sizeof(state->applied[0]) ||
      count > max_size / sizeof(state->prefix_seen_depth[0])) {
    return 0;
  }
  applied_bytes = sizeof(state->applied[0]) * count;
  prefix_bytes = sizeof(state->prefix_seen_depth[0]) * count;
  align = sizeof(state->prefix_seen_depth[0]);
  prefix_offset = applied_bytes;
  rem = prefix_offset % align;
  if (rem != 0u) {
    if (prefix_offset > max_size - (align - rem)) {
      return 0;
    }
    prefix_offset += align - rem;
  }
  if (prefix_offset > max_size - prefix_bytes) {
    return 0;
  }
  total_bytes = prefix_offset + prefix_bytes;
  scratch = (char *)state->allocator->calloc(state->allocator, 1u, total_bytes);
  if (scratch == NULL) {
    state->applied = NULL;
    state->prefix_seen_depth = NULL;
    state->plan_scratch_alloc = NULL;
    return 0;
  }
  state->plan_scratch_alloc = scratch;
  state->applied = (int *)scratch;
  state->prefix_seen_depth = (size_t *)(void *)(scratch + prefix_offset);
  return 1;
}

static void mutation_state_cleanup_plan_scratch(mutation_stream_state *state) {
  if (state == NULL) {
    return;
  }
  if (state->plan_scratch_alloc != NULL) {
    state->allocator->destroy(state->allocator, state->plan_scratch_alloc);
  }
  state->applied = NULL;
  state->prefix_seen_depth = NULL;
  state->plan_scratch_alloc = NULL;
}

static lql_status mutate_reader_with_supported_plan(
    lql *self, const lql_mutation_plan *plan, lonejson_reader_fn reader_fn,
    void *reader_user, FILE *out, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_status st;
  mutation_stream_state state;
  size_t i;
  int runtime_pooled;
  int out_locked;

  if (plan == NULL || reader_fn == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "plan, reader, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out_locked = 0;
  runtime_pooled = 0;
  runtime = lql_lonejson_acquire(self, &runtime_pooled, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  mutation_stream_state_init(&state);
  state.allocator = lql_allocator_from_receiver(self);
  if (state.allocator == NULL) {
    lql_lonejson_release(self, runtime, runtime_pooled);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "mutation receiver allocator required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  state.plan = plan;
  state.error = &lj_error;
  if (!mutation_state_init_plan_scratch(&state, plan)) {
    lql_lonejson_release(self, runtime, runtime_pooled);
    return LQL_STATUS_NO_MEMORY;
  }
  if (lonejson_writer_init_sink(runtime, &state.writer, file_sink, out,
                                &lj_error) != LONEJSON_STATUS_OK) {
    mutation_state_cleanup_plan_scratch(&state);
    lql_lonejson_release(self, runtime, runtime_pooled);
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  flockfile(out);
  out_locked = 1;
  init_mutation_visitor(&visitor);
  st = lonejson_visit_path_value_reader(runtime, reader_fn, reader_user,
                                        &visitor, &state, &lj_error);
  if (st == LONEJSON_STATUS_OK && (!state.root_seen || !state.root_is_object)) {
    st = LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (st == LONEJSON_STATUS_OK) {
    for (i = 0u; i < plan->count; ++i) {
      if (!state.applied[i] && plan->items[i].can_create_missing_object) {
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
  if (out_locked) {
    funlockfile(out);
  }
  lonejson_writer_cleanup(&state.writer);
  mutation_cleanup_path_frames(&state);
  mutation_key_reset(&state);
  mutation_num_reset(&state);
  mutation_state_cleanup_plan_scratch(&state);
  lql_lonejson_release(self, runtime, runtime_pooled);
  if (st != LONEJSON_STATUS_OK) {
    if (error != NULL && error->code == LQL_STATUS_UNSUPPORTED) {
      return LQL_STATUS_UNSUPPORTED;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  return LQL_STATUS_OK;
}

static lql_status mutate_file_range_with_supported_plan(
    lql *self, const lql_mutation_plan *plan, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, lql_error *error) {
  limited_file_reader reader;
  if (file == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "mutation file is required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (!seek_u64(file, offset)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to seek mutation source range");
    return LQL_STATUS_JSON_ERROR;
  }
  reader.file = file;
  reader.remaining = size;
  return mutate_reader_with_supported_plan(self, plan, limited_read, &reader,
                                           out, error);
}

static lql_status mutate_file_range_root_fields_method(
    lql *self, const lql_mutation_plan *plan, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, lql_error *error) {
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
  return mutate_file_range_with_supported_plan(self, plan, file, offset, size,
                                               out, error);
}

static lql_status mutate_file_range_paths_method(lql *self,
                                                 const lql_mutation_plan *plan,
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
  return mutate_file_range_with_supported_plan(self, plan, file, offset, size,
                                               out, error);
}

static lql_status mutate_source_paths_method(lql *self,
                                             const lql_mutation_plan *plan,
                                             lql_read_fn read, void *read_user,
                                             FILE *out, lql_error *error) {
  mutation_source_reader reader;
  lql_status st;

  if (plan == NULL || read == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "plan, read, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (!mutation_plan_supports_stream_paths(plan)) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "mutation plan requires unsupported path behavior");
    return LQL_STATUS_UNSUPPORTED;
  }
  memset(&reader, 0, sizeof(reader));
  reader.read = read;
  reader.user = read_user;
  st = mutate_reader_with_supported_plan(self, plan, mutation_source_read,
                                         &reader, out, error);
  if (st == LQL_STATUS_JSON_ERROR && reader.error_code != 0) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, "mutation source read failed");
  }
  return st;
}

static lql_status mutate_json_method(lql *self, const lql_mutation_plan *plan,
                                     const char *json, size_t json_len,
                                     FILE *out, lql_error *error) {
  buffer_reader reader;

  if (plan == NULL || json == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "plan, json, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (!mutation_plan_supports_stream_paths(plan)) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "mutation plan requires unsupported path behavior");
    return LQL_STATUS_UNSUPPORTED;
  }
  reader.data = (const unsigned char *)json;
  reader.len = json_len;
  reader.offset = 0u;
  return mutate_reader_with_supported_plan(self, plan, buffer_read, &reader,
                                           out, error);
}

LQL_INTERNAL_SYMBOL void lql_mutation_methods_install(lql *ctx) {
  ctx->mutation_plan_parse = mutation_plan_parse_method;
  ctx->mutation_plan_parse_with_options =
      mutation_plan_parse_with_options_method;
  ctx->mutation_plan_count = mutation_plan_count_method;
  ctx->mutation_plan_destroy = mutation_plan_destroy_method;
  ctx->mutate_file_range_root_fields = mutate_file_range_root_fields_method;
  ctx->mutate_file_range_paths = mutate_file_range_paths_method;
  ctx->mutate_source_paths = mutate_source_paths_method;
  ctx->mutate_json = mutate_json_method;
}
