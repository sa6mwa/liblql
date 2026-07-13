#include <lql/lql.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct clql_reader {
  FILE *file;
} clql_reader;

typedef struct clql_writer {
  FILE *file;
} clql_writer;

static void usage(FILE *file) {
  fputs("usage: clql [--count] <selector> [file|-]\n"
        "       clql --help\n"
        "       clql --version\n"
        "\n"
        "Reads strict NDJSON from file or stdin and writes compact matching\n"
        "records to stdout, one JSON value per line. Root arrays are errors.\n",
        file);
}

static lql_status clql_read(void *user, unsigned char *buffer, size_t capacity,
                            size_t *out_len, lql_error *error) {
  clql_reader *reader;
  size_t amount;
  (void)error;
  reader = (clql_reader *)user;
  if (reader == NULL || reader->file == NULL || buffer == NULL ||
      out_len == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out_len = 0u;
  if (capacity == 0u) {
    return LQL_STATUS_OK;
  }
  amount = fread(buffer, 1u, capacity, reader->file);
  if (amount == 0u && ferror(reader->file)) {
    return LQL_STATUS_IO_ERROR;
  }
  *out_len = amount;
  return LQL_STATUS_OK;
}

static lql_status clql_write(void *user, const void *data, size_t len,
                             lql_error *error) {
  clql_writer *writer;
  (void)error;
  writer = (clql_writer *)user;
  if (writer == NULL || writer->file == NULL ||
      (len != 0u && data == NULL)) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (len != 0u && fwrite(data, 1u, len, writer->file) != len) {
    return LQL_STATUS_IO_ERROR;
  }
  return LQL_STATUS_OK;
}

static int print_error(const char *context, lql_status status,
                       const lql_error *error) {
  fprintf(stderr, "clql: %s: %s", context, lql_status_string(status));
  if (error != NULL && error->message[0] != '\0') {
    fprintf(stderr, ": %s", error->message);
  }
  fputc('\n', stderr);
  return 1;
}

int main(int argc, char **argv) {
  const char *expr;
  const char *path;
  int count_only;
  int arg;
  FILE *input;
  lql *ctx;
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  lql_status status;
  clql_reader reader;
  clql_writer writer;

  count_only = 0;
  arg = 1;
  if (argc == 2 && strcmp(argv[1], "--help") == 0) {
    usage(stdout);
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    puts(LQL_VERSION);
    return 0;
  }
  if (arg < argc && strcmp(argv[arg], "--count") == 0) {
    count_only = 1;
    ++arg;
  }
  if (argc - arg < 1 || argc - arg > 2) {
    usage(stderr);
    return 2;
  }

  expr = argv[arg++];
  path = arg < argc ? argv[arg] : "-";
  input = stdin;
  if (strcmp(path, "-") != 0) {
    input = fopen(path, "rb");
    if (input == NULL) {
      fprintf(stderr, "clql: unable to open input: %s\n", path);
      return 1;
    }
  }

  ctx = NULL;
  selector = NULL;
  lql_error_init(&error);
  status = lql_new(&ctx, &error);
  if (status != LQL_STATUS_OK) {
    if (input != stdin) {
      fclose(input);
    }
    return print_error("create context", status, &error);
  }
  status = ctx->selector_parse(ctx, expr, &selector, &error);
  if (status != LQL_STATUS_OK) {
    ctx->destroy(ctx);
    if (input != stdin) {
      fclose(input);
    }
    return print_error("parse selector", status, &error);
  }

  memset(&reader, 0, sizeof(reader));
  reader.file = input;
  memset(&writer, 0, sizeof(writer));
  writer.file = stdout;
  memset(&request, 0, sizeof(request));
  request.reader = clql_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.matched_only = 1;
  if (!count_only) {
    request.writer = clql_write;
    request.writer_user = &writer;
    request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  }

  status = lql_stream_execute(ctx, &request, &result, &error);
  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  if (input != stdin) {
    fclose(input);
  }
  if (status != LQL_STATUS_OK) {
    return print_error("execute stream", status, &error);
  }
  if (count_only) {
    printf("%lu\n", (unsigned long)result.records_matched);
  }
  if (fflush(stdout) != 0) {
    fputs("clql: unable to flush stdout\n", stderr);
    return 1;
  }
  return 0;
}
