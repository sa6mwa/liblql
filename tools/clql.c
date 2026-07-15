#include <lql/lql.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct clql_reader {
  FILE *file;
} clql_reader;

typedef struct clql_writer {
  FILE *file;
} clql_writer;

static void usage(FILE *file) {
  fputs("usage: clql [flags] selector... [data.json]\n", file);
  fputs("   or: clql selector... < data.json\n", file);
  fputs("   or: cat data.json | clql selector...\n\n", file);
  fputs("Selection flags:\n", file);
  fputs("  -c, --compact        compact output (currently always compact)\n",
        file);
  fputs("  -O, --or             combine selector arguments with OR\n", file);
  fputs("  -M, --matches-only   output only selector matches\n", file);
  fputs("      --count          output only the number of matches\n", file);
  fputs("  -h, --help           show help\n", file);
  fputs("  -v, --version        show version\n\n", file);
  fputs("Unsupported Go lql flags fail explicitly until supported:\n", file);
  fputs("  -m/--mutate, -f/--field, -t/--theme,\n", file);
  fputs("  -i/--inline, -w/--write, -F/--enable-file-mutations.\n\n", file);
  fputs("Selector examples (shorthand):\n", file);
  fputs("  /status=\"open\"\n", file);
  fputs("  /status!=closed\n", file);
  fputs("  /progress>=50\n", file);
  fputs("  /timestamp>=\"2025-01-01T00:00:00Z\"\n", file);
  fputs("  /devices/0/status=\"online\"\n", file);
  fputs("  /labels/*=\"production\"\n", file);
  fputs("  /items[]/sku=\"ABC-123\"\n", file);
  fputs("  /items/**/sku=\"ABC-123\"\n", file);
  fputs("  /items/.../sku=\"ABC-123\"\n\n", file);
  fputs("Selector examples (full LQL):\n", file);
  fputs("  eq{field=/status,value=open}\n", file);
  fputs("  contains{field=/msg,value=timeout,ic=t}\n", file);
  fputs("  contains{field=/msg,any=timeout|degraded}\n", file);
  fputs("  icontains{field=/msg,value=timeout}\n", file);
  fputs("  icontains{field=/service,a=AUTH|EDGE}\n", file);
  fputs("  iprefix{field=/service,value=auth}\n", file);
  fputs("  date{field=/timestamp,after=2025-01-01,before=2025-02-01}\n", file);
  fputs("  date{f=/timestamp,since=yesterday}\n", file);
  fputs(
      "  and.eq{field=/status,value=open},and.range{field=/progress,gte=50}\n",
      file);
  fputs("  or.eq{field=/region,value=us},or.eq{field=/region,value=eu}\n",
        file);
  fputs("  not.eq{field=/state,value=disabled}\n", file);
  fputs("  exists{/metadata/etag}\n\n", file);
  fputs("Invocation examples:\n", file);
  fputs("  clql -O '/status=\"open\"' '/status=\"queued\"' data.json\n", file);
  fputs("  clql --count '/status=\"open\"' data.json\n", file);
  fputs("  cat data.json | clql '/items[]/sku=\"ABC-123\"'\n\n", file);
  fputs("Unsupported Go lql mutation example (fails explicitly in clql):\n",
        file);
  fputs("  printf '{}\\n' | clql -F \\\n", file);
  fputs("    -m '/filename=notes.txt' -m '/tags/kind=document' \\\n", file);
  fputs("    -m '/tags/source=local' -m 'textfile:/content=notes.txt'\n\n",
        file);
  fputs("Notes:\n", file);
  fputs(
      "  contains/icontains accept value=... or any=/a=... (pipe-delimited).\n",
      file);
  fputs("  range comparisons accept numeric or datetime literals.\n", file);
  fputs("  date supports value/after/before/gt/gte/lt/lte; aliases a=after and "
        "b=before.\n",
        file);
  fputs("  only date{...,since=...} supports relative macros (now, today, "
        "yesterday).\n",
        file);
  fputs("  omitted values for contains/icontains/prefix/iprefix act as path "
        "assertions.\n\n",
        file);
  fputs("Reads strict NDJSON from file or stdin and writes compact matching\n",
        file);
  fputs("records to stdout, one JSON value per line. Root arrays are errors.\n",
        file);
  fputs("Selected output uses liblql's explicitly spooled compatibility path\n",
        file);
  fputs("and may spill the current record to a temporary file.\n", file);
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
  if (writer == NULL || writer->file == NULL || (len != 0u && data == NULL)) {
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

static int file_exists(const char *path) {
  struct stat st;
  return path != NULL && stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

static int is_unsupported_flag_with_value(const char *arg) {
  return strcmp(arg, "-m") == 0 || strcmp(arg, "--mutate") == 0 ||
         strncmp(arg, "--mutate=", 9u) == 0 || strcmp(arg, "-f") == 0 ||
         strcmp(arg, "--field") == 0 || strncmp(arg, "--field=", 8u) == 0 ||
         strcmp(arg, "-t") == 0 || strcmp(arg, "--theme") == 0 ||
         strncmp(arg, "--theme=", 8u) == 0;
}

static int is_unsupported_flag_no_value(const char *arg) {
  return strcmp(arg, "-i") == 0 || strcmp(arg, "--inline") == 0 ||
         strcmp(arg, "-w") == 0 || strcmp(arg, "--write") == 0 ||
         strcmp(arg, "-F") == 0 || strcmp(arg, "--enable-file-mutations") == 0;
}

static char *join_selectors(char **argv, int first, int count,
                            const char *separator) {
  size_t len;
  size_t sep_len;
  int i;
  char *out;
  char *cursor;

  len = 1u;
  sep_len = strlen(separator);
  for (i = 0; i < count; ++i) {
    len += strlen(argv[first + i]);
    if (i + 1 < count) {
      len += sep_len;
    }
  }
  out = (char *)malloc(len);
  if (out == NULL) {
    return NULL;
  }
  cursor = out;
  for (i = 0; i < count; ++i) {
    size_t part_len;
    if (i > 0) {
      memcpy(cursor, separator, sep_len);
      cursor += sep_len;
    }
    part_len = strlen(argv[first + i]);
    memcpy(cursor, argv[first + i], part_len);
    cursor += part_len;
  }
  *cursor = '\0';
  return out;
}

int main(int argc, char **argv) {
  const char *expr;
  const char *path;
  int count_only;
  int or_mode;
  int arg;
  int selector_first;
  int selector_count;
  int remaining;
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
  or_mode = 0;
  arg = 1;

  while (arg < argc) {
    if (strcmp(argv[arg], "--") == 0) {
      ++arg;
      break;
    }
    if (strcmp(argv[arg], "-h") == 0 || strcmp(argv[arg], "--help") == 0) {
      usage(stdout);
      return 0;
    }
    if (strcmp(argv[arg], "-v") == 0 || strcmp(argv[arg], "--version") == 0) {
      puts(LQL_VERSION);
      return 0;
    }
    if (strcmp(argv[arg], "--count") == 0) {
      count_only = 1;
      ++arg;
      continue;
    }
    if (strcmp(argv[arg], "-O") == 0 || strcmp(argv[arg], "--or") == 0) {
      or_mode = 1;
      ++arg;
      continue;
    }
    if (strcmp(argv[arg], "-c") == 0 || strcmp(argv[arg], "--compact") == 0 ||
        strcmp(argv[arg], "-M") == 0 ||
        strcmp(argv[arg], "--matches-only") == 0) {
      ++arg;
      continue;
    }
    if (is_unsupported_flag_no_value(argv[arg]) ||
        is_unsupported_flag_with_value(argv[arg])) {
      fprintf(stderr, "clql: unsupported Go lql flag in current engine: %s\n",
              argv[arg]);
      return 2;
    }
    if (argv[arg][0] == '-') {
      fprintf(stderr, "clql: unknown flag: %s\n", argv[arg]);
      usage(stderr);
      return 2;
    }
    break;
  }

  selector_first = arg;
  remaining = argc - selector_first;
  if (remaining < 1) {
    usage(stderr);
    return 2;
  }
  path = "-";
  selector_count = remaining;
  if (strcmp(argv[argc - 1], "-") == 0 || file_exists(argv[argc - 1])) {
    path = argv[argc - 1];
    selector_count = remaining - 1;
  }
  if (selector_count < 1) {
    usage(stderr);
    return 2;
  }

  expr = join_selectors(argv, selector_first, selector_count,
                        or_mode ? "," : "\n");
  if (expr == NULL) {
    fputs("clql: out of memory\n", stderr);
    return 1;
  }

  input = stdin;
  if (strcmp(path, "-") != 0) {
    input = fopen(path, "rb");
    if (input == NULL) {
      fprintf(stderr, "clql: unable to open input: %s\n", path);
      free((void *)expr);
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
    free((void *)expr);
    return print_error("create context", status, &error);
  }
  if (or_mode) {
    status = ctx->selector_parse_or(ctx, expr, &selector, &error);
  } else {
    status = ctx->selector_parse(ctx, expr, &selector, &error);
  }
  free((void *)expr);
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

  status = ctx->stream_execute_spooled(ctx, &request, &result, &error);
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
