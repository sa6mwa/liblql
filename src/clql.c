#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "lql/lql.h"

#include <lonejson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct field_list {
  char **items;
  size_t count;
} field_list;

typedef struct match_count {
  lql_uint64 matched;
} match_count;

typedef struct output_ranges {
  FILE *source;
  FILE *out;
  const field_list *fields;
  lql_uint64 matched;
} output_ranges;

typedef struct limited_file_reader {
  FILE *file;
  lql_uint64 remaining;
} limited_file_reader;

typedef struct projection_state {
  const field_list *fields;
  lonejson_writer writer;
  lonejson_error *error;
  char *key_buf;
  size_t key_len;
  char *num_buf;
  size_t num_len;
  size_t capture_depth;
  int capturing;
  int in_string;
  int in_number;
  int found;
} projection_state;

static lql_status count_match(void *user, const lql_query_decision *decision) {
  match_count *count;
  count = (match_count *)user;
  if (decision->matched) {
    ++count->matched;
  }
  return LQL_STATUS_OK;
}

static int seek_u64(FILE *file, lql_uint64 offset) {
  return fseeko(file, (off_t)offset, SEEK_SET) == 0;
}

static int copy_range(FILE *in, FILE *out, lql_uint64 size) {
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

static int copy_file(FILE *in, FILE *out) {
  char buf[8192];
  size_t got;
  while ((got = fread(buf, 1u, sizeof(buf), in)) != 0u) {
    if (fwrite(buf, 1u, got, out) != got) {
      return 0;
    }
  }
  return ferror(in) ? 0 : 1;
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

static void free_field_list(field_list *fields) {
  size_t i;
  if (fields == NULL) {
    return;
  }
  for (i = 0u; i < fields->count; ++i) {
    free(fields->items[i]);
  }
  free(fields->items);
  fields->items = NULL;
  fields->count = 0u;
}

static int field_is_array_index(const char *field) {
  size_t i;
  if (field == NULL || field[0] == '\0') {
    return 0;
  }
  for (i = 0u; field[i] != '\0'; ++i) {
    if (field[i] < '0' || field[i] > '9') {
      return 0;
    }
  }
  return 1;
}

static char *decode_root_field_path(const char *path) {
  const char *src;
  char *out;
  size_t len;
  size_t i;
  size_t j;
  if (path == NULL || path[0] != '/' || path[1] == '\0') {
    return NULL;
  }
  src = path + 1;
  len = strlen(src);
  out = (char *)malloc(len + 1u);
  if (out == NULL) {
    return NULL;
  }
  i = 0u;
  j = 0u;
  while (src[i] != '\0') {
    if (src[i] == '/') {
      free(out);
      return NULL;
    }
    if (src[i] == '~' && src[i + 1u] != '\0') {
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
  if (out[0] == '\0' || field_is_array_index(out)) {
    free(out);
    return NULL;
  }
  return out;
}

static int add_field(field_list *fields, const char *path) {
  char *decoded;
  char **next;
  size_t i;
  decoded = decode_root_field_path(path);
  if (decoded == NULL) {
    return 0;
  }
  for (i = 0u; i < fields->count; ++i) {
    if (strcmp(fields->items[i], decoded) == 0) {
      free(decoded);
      return 1;
    }
  }
  next = (char **)realloc(fields->items, sizeof(fields->items[0]) *
                                             (fields->count + 1u));
  if (next == NULL) {
    free(decoded);
    return 0;
  }
  fields->items = next;
  fields->items[fields->count++] = decoded;
  return 1;
}

static int path_is_root_field(const lonejson_value_path *path,
                              const char *field) {
  return path != NULL && path->segment_count == 1u &&
         strlen(field) == path->segments[0].len &&
         memcmp(field, path->segments[0].data, path->segments[0].len) == 0;
}

static const char *selected_field(const field_list *fields,
                                  const lonejson_value_path *path) {
  size_t i;
  if (fields == NULL) {
    return NULL;
  }
  for (i = 0u; i < fields->count; ++i) {
    if (path_is_root_field(path, fields->items[i])) {
      return fields->items[i];
    }
  }
  return NULL;
}

static int append_projection_buf(char **buf, size_t *len, const char *data,
                                 size_t n) {
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

static lonejson_status projection_key(projection_state *state,
                                      const char *key) {
  if (lonejson_writer_key(&state->writer, key, strlen(key), state->error) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  state->found = 1;
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_object_begin(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  projection_state *state;
  const char *field;
  (void)error;
  state = (projection_state *)user;
  field = selected_field(state->fields, path);
  if (field != NULL && !state->capturing) {
    if (projection_key(state, field) != LONEJSON_STATUS_OK ||
        lonejson_writer_begin_object(&state->writer, state->error) !=
            LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->capturing = 1;
    state->capture_depth = path->segment_count;
    return LONEJSON_STATUS_OK;
  }
  if (state->capturing &&
      lonejson_writer_begin_object(&state->writer, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_object_end(void *user,
                                             const lonejson_value_path *path,
                                             lonejson_error *error) {
  projection_state *state;
  (void)error;
  state = (projection_state *)user;
  if (!state->capturing) {
    return LONEJSON_STATUS_OK;
  }
  if (lonejson_writer_end_object(&state->writer, state->error) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (path->segment_count == state->capture_depth) {
    state->capturing = 0;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_array_begin(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  projection_state *state;
  const char *field;
  (void)error;
  state = (projection_state *)user;
  field = selected_field(state->fields, path);
  if (field != NULL && !state->capturing) {
    if (projection_key(state, field) != LONEJSON_STATUS_OK ||
        lonejson_writer_begin_array(&state->writer, state->error) !=
            LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->capturing = 1;
    state->capture_depth = path->segment_count;
    return LONEJSON_STATUS_OK;
  }
  if (state->capturing &&
      lonejson_writer_begin_array(&state->writer, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_array_end(void *user,
                                            const lonejson_value_path *path,
                                            lonejson_error *error) {
  projection_state *state;
  (void)error;
  state = (projection_state *)user;
  if (!state->capturing) {
    return LONEJSON_STATUS_OK;
  }
  if (lonejson_writer_end_array(&state->writer, state->error) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (path->segment_count == state->capture_depth) {
    state->capturing = 0;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_object_key_begin(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  projection_state *state;
  (void)path;
  (void)error;
  state = (projection_state *)user;
  free(state->key_buf);
  state->key_buf = NULL;
  state->key_len = 0u;
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_object_key_chunk(
    void *user, const lonejson_value_path *path, const char *data, size_t len,
    lonejson_error *error) {
  projection_state *state;
  (void)path;
  (void)error;
  state = (projection_state *)user;
  if (!append_projection_buf(&state->key_buf, &state->key_len, data, len)) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_object_key_end(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  projection_state *state;
  (void)path;
  (void)error;
  state = (projection_state *)user;
  if (state->capturing &&
      lonejson_writer_key(&state->writer, state->key_buf,
                          strlen(state->key_buf), state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_string_begin(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  projection_state *state;
  const char *field;
  (void)error;
  state = (projection_state *)user;
  field = selected_field(state->fields, path);
  if (field != NULL && !state->capturing) {
    if (projection_key(state, field) != LONEJSON_STATUS_OK ||
        lonejson_writer_string_begin(&state->writer, state->error) !=
            LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->in_string = 1;
  } else if (state->capturing) {
    if (lonejson_writer_string_begin(&state->writer, state->error) !=
        LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    state->in_string = 1;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_string_chunk(
    void *user, const lonejson_value_path *path, const char *data, size_t len,
    lonejson_error *error) {
  projection_state *state;
  (void)path;
  (void)error;
  state = (projection_state *)user;
  if (state->in_string &&
      lonejson_writer_string_chunk(&state->writer, data, len, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_string_end(void *user,
                                             const lonejson_value_path *path,
                                             lonejson_error *error) {
  projection_state *state;
  (void)path;
  (void)error;
  state = (projection_state *)user;
  if (state->in_string &&
      lonejson_writer_string_end(&state->writer, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  state->in_string = 0;
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_number_begin(
    void *user, const lonejson_value_path *path, lonejson_error *error) {
  projection_state *state;
  (void)error;
  state = (projection_state *)user;
  free(state->num_buf);
  state->num_buf = NULL;
  state->num_len = 0u;
  state->in_number =
      state->capturing || selected_field(state->fields, path) != NULL;
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_number_chunk(
    void *user, const lonejson_value_path *path, const char *data, size_t len,
    lonejson_error *error) {
  projection_state *state;
  (void)path;
  (void)error;
  state = (projection_state *)user;
  if (state->in_number &&
      !append_projection_buf(&state->num_buf, &state->num_len, data, len)) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_number_end(void *user,
                                             const lonejson_value_path *path,
                                             lonejson_error *error) {
  projection_state *state;
  const char *field;
  (void)error;
  state = (projection_state *)user;
  field = selected_field(state->fields, path);
  if (field != NULL && !state->capturing &&
      projection_key(state, field) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (state->in_number &&
      lonejson_writer_number_text(&state->writer, state->num_buf,
                                  state->num_len, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  state->in_number = 0;
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_boolean(void *user,
                                          const lonejson_value_path *path,
                                          int value, lonejson_error *error) {
  projection_state *state;
  const char *field;
  (void)error;
  state = (projection_state *)user;
  field = selected_field(state->fields, path);
  if (field != NULL && !state->capturing &&
      projection_key(state, field) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if ((state->capturing || field != NULL) &&
      lonejson_writer_bool(&state->writer, value, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status projection_null(void *user,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  projection_state *state;
  const char *field;
  (void)error;
  state = (projection_state *)user;
  field = selected_field(state->fields, path);
  if (field != NULL && !state->capturing &&
      projection_key(state, field) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if ((state->capturing || field != NULL) &&
      lonejson_writer_null(&state->writer, state->error) !=
          LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static void init_projection_visitor(lonejson_path_value_visitor *visitor) {
  *visitor = lonejson_default_path_value_visitor();
  visitor->object_begin = projection_object_begin;
  visitor->object_end = projection_object_end;
  visitor->object_key_begin = projection_object_key_begin;
  visitor->object_key_chunk = projection_object_key_chunk;
  visitor->object_key_end = projection_object_key_end;
  visitor->array_begin = projection_array_begin;
  visitor->array_end = projection_array_end;
  visitor->string_begin = projection_string_begin;
  visitor->string_chunk = projection_string_chunk;
  visitor->string_end = projection_string_end;
  visitor->number_begin = projection_number_begin;
  visitor->number_chunk = projection_number_chunk;
  visitor->number_end = projection_number_end;
  visitor->boolean_value = projection_boolean;
  visitor->null_value = projection_null;
}

static int project_range(FILE *source, lql_uint64 offset, lql_uint64 size,
                         const field_list *fields, FILE *out) {
  lonejson *runtime;
  lonejson_error error;
  lonejson_path_value_visitor visitor;
  projection_state state;
  limited_file_reader reader;
  lonejson_status st;
  int found;
  if (!seek_u64(source, offset)) {
    return -1;
  }
  runtime = lonejson_new(NULL, &error);
  if (runtime == NULL) {
    return -1;
  }
  memset(&state, 0, sizeof(state));
  state.fields = fields;
  state.error = &error;
  reader.file = source;
  reader.remaining = size;
  init_projection_visitor(&visitor);
  if (lonejson_writer_init_sink(runtime, &state.writer, file_sink, out,
                                &error) != LONEJSON_STATUS_OK ||
      lonejson_writer_begin_object(&state.writer, &error) !=
          LONEJSON_STATUS_OK) {
    lonejson_free(runtime);
    free(state.key_buf);
    free(state.num_buf);
    return -1;
  }
  st = lonejson_visit_path_value_reader(runtime, limited_read, &reader,
                                        &visitor, &state, &error);
  if (st == LONEJSON_STATUS_OK) {
    st = lonejson_writer_end_object(&state.writer, &error);
  }
  if (st == LONEJSON_STATUS_OK) {
    st = lonejson_writer_finish(&state.writer, &error);
  }
  found = state.found;
  lonejson_writer_cleanup(&state.writer);
  lonejson_free(runtime);
  free(state.key_buf);
  free(state.num_buf);
  if (st != LONEJSON_STATUS_OK) {
    return -1;
  }
  return found ? 1 : 0;
}

static lql_status output_match_range(void *user,
                                     const lql_query_decision *decision) {
  output_ranges *ranges;
  if (!decision->matched) {
    return LQL_STATUS_OK;
  }
  ranges = (output_ranges *)user;
  if (ranges->fields != NULL && ranges->fields->count != 0u) {
    int projected;
    FILE *projected_file;
    projected_file = tmpfile();
    if (projected_file == NULL) {
      return LQL_STATUS_JSON_ERROR;
    }
    projected = project_range(ranges->source, decision->offset, decision->size,
                              ranges->fields, projected_file);
    if (projected < 0) {
      fclose(projected_file);
      return LQL_STATUS_JSON_ERROR;
    }
    if (projected == 0) {
      fclose(projected_file);
      return LQL_STATUS_OK;
    }
    if (fseek(projected_file, 0L, SEEK_SET) != 0 ||
        !copy_file(projected_file, ranges->out)) {
      fclose(projected_file);
      return LQL_STATUS_JSON_ERROR;
    }
    fclose(projected_file);
  } else {
    if (!seek_u64(ranges->source, decision->offset) ||
        !copy_range(ranges->source, ranges->out, decision->size)) {
      return LQL_STATUS_JSON_ERROR;
    }
  }
  if (fputc('\n', ranges->out) == EOF) {
    return LQL_STATUS_JSON_ERROR;
  }
  ++ranges->matched;
  return LQL_STATUS_OK;
}

static int read_stdin(char **out, size_t *out_len) {
  char *buf;
  size_t cap;
  size_t len;
  size_t n;
  char tmp[4096];
  buf = NULL;
  cap = 0u;
  len = 0u;
  while ((n = fread(tmp, 1u, sizeof(tmp), stdin)) > 0u) {
    if (len + n + 1u > cap) {
      size_t next_cap = cap == 0u ? 8192u : cap * 2u;
      char *next;
      while (next_cap < len + n + 1u) {
        next_cap *= 2u;
      }
      next = (char *)realloc(buf, next_cap);
      if (next == NULL) {
        free(buf);
        return 0;
      }
      buf = next;
      cap = next_cap;
    }
    memcpy(buf + len, tmp, n);
    len += n;
  }
  if (buf == NULL) {
    buf = (char *)malloc(1u);
    if (buf == NULL) {
      return 0;
    }
  }
  buf[len] = '\0';
  *out = buf;
  *out_len = len;
  return 1;
}

static FILE *open_input_path(const char *path) {
  if (path == NULL || strcmp(path, "-") == 0) {
    return stdin;
  }
  return fopen(path, "rb");
}

static void close_input_path(FILE *file) {
  if (file != NULL && file != stdin) {
    fclose(file);
  }
}

static void usage(FILE *out) {
  fprintf(out, "usage: clql [--or|-O] [-f field] [--matches-only|-M] selector [data.json]\n");
  fprintf(out, "       clql [--or|-O] [--matches-only|-M] selector < data.json\n");
  fprintf(out, "       clql --version\n");
}

int main(int argc, char **argv) {
  lql_selector *selector;
  lql_error error;
  char *json;
  const char *selector_expr;
  const char *input_path;
  FILE *input;
  FILE *range_source;
  size_t json_len;
  int matched;
  int matches_only;
  int or_mode;
  int i;
  field_list fields;
  lql_status st;
  match_count count;
  output_ranges ranges;
  lql_query_result result;

  memset(&fields, 0, sizeof(fields));
  or_mode = 0;
  matches_only = 0;
  selector_expr = NULL;
  input_path = NULL;
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
      printf("clql 0.0.0\n");
      free_field_list(&fields);
      return 0;
    }
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(stdout);
      free_field_list(&fields);
      return 0;
    }
    if (strcmp(argv[i], "--or") == 0 || strcmp(argv[i], "-O") == 0) {
      or_mode = 1;
    } else if (strcmp(argv[i], "--matches-only") == 0 ||
               strcmp(argv[i], "-M") == 0) {
      matches_only = 1;
    } else if (strcmp(argv[i], "--field") == 0 || strcmp(argv[i], "-f") == 0) {
      if (i + 1 >= argc || !add_field(&fields, argv[++i])) {
        fprintf(stderr, "clql: invalid or unsupported field path\n");
        free_field_list(&fields);
        return 2;
      }
    } else if (strncmp(argv[i], "--field=", 8u) == 0) {
      if (!add_field(&fields, argv[i] + 8u)) {
        fprintf(stderr, "clql: invalid or unsupported field path\n");
        free_field_list(&fields);
        return 2;
      }
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "clql: unknown option %s\n", argv[i]);
      usage(stderr);
      free_field_list(&fields);
      return 2;
    } else if (selector_expr == NULL) {
      selector_expr = argv[i];
    } else if (input_path == NULL) {
      input_path = argv[i];
    } else {
      usage(stderr);
      free_field_list(&fields);
      return 2;
    }
  }
  if (selector_expr == NULL) {
    usage(stderr);
    free_field_list(&fields);
    return 2;
  }
  lql_error_init(&error);
  st = or_mode ? lql_selector_parse_or(selector_expr, &selector, &error)
               : lql_selector_parse(selector_expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "clql: %s\n", error.message);
    free_field_list(&fields);
    return 2;
  }
  if (matches_only) {
    count.matched = 0u;
    memset(&result, 0, sizeof(result));
    input = open_input_path(input_path);
    if (input == NULL) {
      fprintf(stderr, "clql: failed to open input %s\n", input_path);
      lql_selector_free(selector);
      free_field_list(&fields);
      return 1;
    }
    st = lql_query_file_decisions(selector, input, count_match, &count, &result,
                                  &error);
    close_input_path(input);
    lql_selector_free(selector);
    free_field_list(&fields);
    if (st != LQL_STATUS_OK) {
      fprintf(stderr, "clql: %s\n", error.message);
      return 1;
    }
    return count.matched == 0u ? 1 : 0;
  }
  if (input_path != NULL && strcmp(input_path, "-") != 0) {
    input = fopen(input_path, "rb");
    range_source = fopen(input_path, "rb");
    if (input == NULL || range_source == NULL) {
      fprintf(stderr, "clql: failed to open input %s\n", input_path);
      close_input_path(input);
      close_input_path(range_source);
      lql_selector_free(selector);
      free_field_list(&fields);
      return 1;
    }
    memset(&ranges, 0, sizeof(ranges));
    memset(&result, 0, sizeof(result));
    ranges.source = range_source;
    ranges.out = stdout;
    ranges.fields = &fields;
    st = lql_query_file_decisions(selector, input, output_match_range, &ranges,
                                  &result, &error);
    close_input_path(input);
    close_input_path(range_source);
    lql_selector_free(selector);
    free_field_list(&fields);
    if (st != LQL_STATUS_OK) {
      fprintf(stderr, "clql: %s\n", error.message);
      return 1;
    }
    return ranges.matched == 0u ? 1 : 0;
  }
  if (!read_stdin(&json, &json_len)) {
    fprintf(stderr, "clql: failed to read stdin\n");
    lql_selector_free(selector);
    free_field_list(&fields);
    return 1;
  }
  if (fields.count != 0u) {
    fprintf(stderr, "clql: field projection requires a seekable input file\n");
    free(json);
    lql_selector_free(selector);
    free_field_list(&fields);
    return 2;
  }
  st = lql_matches_json(selector, json, json_len, &matched, &error);
  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "clql: %s\n", error.message);
    free(json);
    lql_selector_free(selector);
    free_field_list(&fields);
    return 1;
  }
  if (matched) {
    fwrite(json, 1u, json_len, stdout);
    if (json_len == 0u || json[json_len - 1u] != '\n') {
      fputc('\n', stdout);
    }
  }
  free(json);
  lql_selector_free(selector);
  free_field_list(&fields);
  return matched ? 0 : 1;
}
