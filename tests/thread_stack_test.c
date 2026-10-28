#define _POSIX_C_SOURCE 200809L
#include <lql/lql.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
  lql *client;
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  const char *input;
  size_t offset;
  size_t length;
  size_t written;
  size_t values;
  size_t file_offset;
  size_t opens;
  size_t closes;
} fixture;

static lql_status read_input(void *user, unsigned char *buffer, size_t capacity,
                             size_t *out_len, lql_error *error) {
  fixture *f;
  size_t amount;
  (void)error;
  f = (fixture *)user;
  amount = f->length - f->offset;
  if (amount > capacity)
    amount = capacity;
  if (amount > 257u)
    amount = 257u;
  memcpy(buffer, f->input + f->offset, amount);
  f->offset += amount;
  *out_len = amount;
  return LQL_STATUS_OK;
}

static lql_status write_output(void *user, const void *data, size_t len,
                               lql_error *error) {
  fixture *f;
  (void)data;
  (void)error;
  f = (fixture *)user;
  f->written += len;
  return LQL_STATUS_OK;
}

static lql_status replay(void *user, size_t offset, size_t len,
                         lql_stream_writer_fn writer, void *writer_user,
                         lql_error *error) {
  fixture *f;
  f = (fixture *)user;
  if (offset > f->length || len > f->length - offset)
    return LQL_STATUS_CALLBACK_ERROR;
  return writer(writer_user, f->input + offset, len, error);
}

static lql_stream_callback_result
value(void *user, const lql_stream_value *selected, lql_error *error) {
  fixture *f;
  f = (fixture *)user;
  ++f->values;
  return lql_stream_value_write_to(selected, write_output, f, error) ==
                 LQL_STATUS_OK
             ? LQL_STREAM_CALLBACK_CONTINUE
             : LQL_STREAM_CALLBACK_ERROR;
}

static lql_status read_file(void *user, unsigned char *buffer, size_t capacity,
                            size_t *out_len, lql_error *error) {
  fixture *f;
  size_t amount;
  (void)error;
  f = (fixture *)user;
  amount = 70001u - f->file_offset;
  if (amount > capacity)
    amount = capacity;
  memset(buffer, 'x', amount);
  f->file_offset += amount;
  *out_len = amount;
  return LQL_STATUS_OK;
}

static lql_status open_file(void *user, lql_string_view path,
                            lql_stream_reader_fn *reader, void **reader_user,
                            lql_error *error) {
  fixture *f;
  (void)path;
  (void)error;
  f = (fixture *)user;
  f->file_offset = 0u;
  ++f->opens;
  *reader = read_file;
  *reader_user = f;
  return LQL_STATUS_OK;
}

static void close_file(void *user, void *reader_user) {
  fixture *f;
  (void)reader_user;
  f = (fixture *)user;
  ++f->closes;
}

static int apply(fixture *f, const char *expr, const char *input, int spooled,
                 int with_value, lql_status expected) {
  lql_status status;
  f->client->selector_destroy(f->client, f->selector);
  f->selector = NULL;
  lql_error_init(&f->error);
  status = f->client->selector_parse(f->client, expr, &f->selector, &f->error);
  if (status != LQL_STATUS_OK)
    goto failed;
  f->input = input;
  f->length = strlen(input);
  f->offset = f->written = f->values = 0u;
  memset(&f->request, 0, sizeof(f->request));
  f->request.reader = read_input;
  f->request.reader_user = f;
  f->request.selector = f->selector;
  if (with_value) {
    f->request.on_value = value;
    f->request.value_user = f;
    if (!spooled) {
      f->request.input_is_compact = 1;
      f->request.range_writer = replay;
      f->request.range_user = f;
    }
  }
  if (f->mutation != NULL || f->projection != NULL) {
    f->request.mutation = f->mutation;
    f->request.projection = f->projection;
    f->request.output_mode = f->mutation != NULL ? LQL_STREAM_OUTPUT_MUTATION
                                                 : LQL_STREAM_OUTPUT_PROJECTION;
    f->request.writer = write_output;
    f->request.writer_user = f;
  }
  status = spooled ? f->client->stream_apply_spooled(f->client, &f->request,
                                                     &f->result, &f->error)
                   : f->client->stream_apply(f->client, &f->request, &f->result,
                                             &f->error);
  if (status == expected &&
      (status != LQL_STATUS_OK ||
       (f->result.records_seen == 1u && f->result.records_matched == 1u &&
        (!with_value || (f->values == 1u && f->written == f->length - 1u)))))
    return 0;
failed:
  fprintf(stderr, "thread case %s: status=%d expected=%d: %s\n", expr,
          (int)status, (int)expected, f->error.message);
  return 1;
}

static char *nested(size_t depth, int arrays) {
  char *text;
  char *cursor;
  size_t i;
  text = (char *)malloc(depth * 6u + 32u);
  if (text == NULL)
    return NULL;
  cursor = text;
  if (arrays) {
    memcpy(cursor, "{\"x\":", 5u);
    cursor += 5u;
  }
  for (i = 0u; i < depth; ++i) {
    if (arrays) {
      *cursor++ = '[';
    } else {
      memcpy(cursor, "{\"x\":", 5u);
      cursor += 5u;
    }
  }
  memcpy(cursor, "{\"kind\":\"commit\"}", 17u);
  cursor += 17u;
  for (i = 0u; i < depth; ++i)
    *cursor++ = arrays ? ']' : '}';
  if (arrays)
    *cursor++ = '}';
  *cursor++ = '\n';
  *cursor = '\0';
  return text;
}

static int deep_cases(fixture *f) {
  static const char *const mutation[] = {"/.../kind=commit"};
  char *input;
  int arrays;
  int failed;
  size_t depth;
  for (arrays = 0; arrays < 2; ++arrays) {
    for (depth = 0u; depth <= 128u; ++depth) {
      input = nested(depth, arrays);
      if (input == NULL)
        return 1;
      failed = apply(f, "/.../kind=\"commit\"", input, 0, 0,
                     depth >= (arrays ? 127u : 128u) ? LQL_STATUS_JSON_ERROR
                                                     : LQL_STATUS_OK);
      if (!failed && depth == (arrays ? 126u : 127u)) {
        failed =
            apply(f, "/.../kind=\"commit\"", input, 0, 1, LQL_STATUS_OK) ||
            apply(f, "/.../kind=\"commit\"", input, 1, 1, LQL_STATUS_OK) ||
            f->client->mutation_parse(f->client, mutation, 1u, &f->mutation,
                                      &f->error) != LQL_STATUS_OK ||
            apply(f, "/.../kind=\"commit\"", input, 1, 0, LQL_STATUS_OK) ||
            f->written != strlen(input);
        f->client->mutation_destroy(f->client, f->mutation);
        f->mutation = NULL;
      }
      free(input);
      if (failed)
        return 1;
    }
  }
  return 0;
}

static int large_cases(fixture *f) {
  static const char *const paths[] = {"/kind"};
  static const char *const expressions[] = {"textfile:/payload=fixture",
                                            "base64file:/payload=fixture",
                                            "file:/payload=fixture"};
  char *input;
  lql_mutation_parse_options options;
  size_t i;
  int failed;
  input = (char *)malloc(100030u);
  if (input == NULL)
    return 1;
  memcpy(input, "{\"kind\":\"commit\",\"data\":\"", 25u);
  memset(input + 25u, 'x', 100000u);
  memcpy(input + 100025u, "\"}\n", 4u);
  failed = apply(f, "/kind=\"commit\"", input, 0, 1, LQL_STATUS_OK) ||
           apply(f, "/kind=\"commit\"", input, 1, 1, LQL_STATUS_OK);
  if (!failed) {
    failed = f->client->projection_parse(f->client, paths, 1u, &f->projection,
                                         &f->error) != LQL_STATUS_OK ||
             apply(f, "/kind=\"commit\"", input, 1, 0, LQL_STATUS_OK) ||
             f->written != 18u;
    f->client->projection_destroy(f->client, f->projection);
    f->projection = NULL;
  }
  free(input);
  if (failed)
    fprintf(stderr, "large replay/projection: bytes=%lu: %s\n",
            (unsigned long)f->written, f->error.message);
  memset(&options, 0, sizeof(options));
  options.enable_file_values = 1;
  options.file_value_open = open_file;
  options.file_value_close = close_file;
  options.file_value_user = f;
  for (i = 0u; i < 3u && !failed; ++i) {
    failed = f->client->mutation_parse_with_options(
                 f->client, expressions + i, 1u, &options, &f->mutation,
                 &f->error) != LQL_STATUS_OK ||
             apply(f, "/kind=\"commit\"", "{\"kind\":\"commit\"}\n", 1, 0,
                   LQL_STATUS_OK) ||
             f->written != (i == 1u ? 93367u : 70032u) || f->opens == 0u ||
             f->opens != f->closes;
    f->client->mutation_destroy(f->client, f->mutation);
    f->mutation = NULL;
    if (failed)
      fprintf(stderr, "file mutation %lu: bytes=%lu opens=%lu closes=%lu: %s\n",
              (unsigned long)i, (unsigned long)f->written,
              (unsigned long)f->opens, (unsigned long)f->closes,
              f->error.message);
  }
  return failed;
}

static void *worker(void *user) {
  fixture *f;
  int *failed;
  failed = (int *)user;
  f = (fixture *)calloc(1u, sizeof(*f));
  if (f == NULL) {
    *failed = 1;
    return NULL;
  }
  *failed = lql_new(&f->client, &f->error) != LQL_STATUS_OK;
  if (!*failed)
    *failed = apply(f, "/kind=\"commit\"", "{\"kind\":\"commit\"}\n", 0, 0,
                    LQL_STATUS_OK) ||
              apply(f, "/n=1e9999999999999999999999999999999999999999",
                    "{\"n\":10e9999999999999999999999999999999999999998}\n", 0,
                    0, LQL_STATUS_OK) ||
              apply(f, "/timestamp=\"2026-03-05T09:29:00Z\"",
                    "{\"timestamp\":\"2026-03-05T09:29:00Z\"}\n", 0, 0,
                    LQL_STATUS_OK) ||
              deep_cases(f) || large_cases(f);
  if (f->client != NULL) {
    f->client->selector_destroy(f->client, f->selector);
    f->client->destroy(f->client);
  }
  free(f);
  return NULL;
}

int main(int argc, char **argv) {
  pthread_attr_t attributes;
  pthread_t threads[4];
  int failed[4];
  size_t stack_size;
  size_t i;
  int result;
  if (pthread_attr_init(&attributes) != 0)
    return 1;
  if (argc == 2 && strcmp(argv[1], "small") == 0 &&
      pthread_attr_setstacksize(&attributes, 64u * 1024u) != 0)
    return 1;
  if (pthread_attr_getstacksize(&attributes, &stack_size) != 0)
    return 1;
  printf("worker stack=%lu bytes; four independent clients\n",
         (unsigned long)stack_size);
  fflush(stdout);
  for (i = 0u; i < 4u; ++i) {
    failed[i] = 1;
    if (pthread_create(threads + i, argc == 2 ? &attributes : NULL, worker,
                       failed + i) != 0)
      return 1;
  }
  result = 0;
  for (i = 0u; i < 4u; ++i) {
    if (pthread_join(threads[i], NULL) != 0 || failed[i])
      result = 1;
  }
  pthread_attr_destroy(&attributes);
  return result;
}
