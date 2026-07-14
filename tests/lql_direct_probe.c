#include <lql/lql.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct probe_reader {
  FILE *file;
} probe_reader;

static lql_status probe_discard_write(void *user, const void *data, size_t len,
                                      lql_error *error) {
  (void)user;
  (void)data;
  (void)len;
  (void)error;
  return LQL_STATUS_OK;
}

static lql_status probe_read(void *user, unsigned char *buffer, size_t capacity,
                             size_t *out_len, lql_error *error) {
  probe_reader *reader;
  size_t amount;
  reader = (probe_reader *)user;
  if (reader == NULL || reader->file == NULL || out_len == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  amount = fread(buffer, 1u, capacity, reader->file);
  if (amount == 0u && ferror(reader->file)) {
    lql_error_init(error);
    if (error != NULL) {
      error->code = LQL_STATUS_IO_ERROR;
      strcpy(error->message, "fixture read failed");
    }
    return LQL_STATUS_IO_ERROR;
  }
  *out_len = amount;
  return LQL_STATUS_OK;
}

static int probe_samples(void) {
  const char *text;
  char *end;
  long value;
  text = getenv("LQL_BENCH_SAMPLES");
  if (text == NULL || *text == '\0') {
    return 15;
  }
  errno = 0;
  value = strtol(text, &end, 10);
  if (errno != 0 || *end != '\0' || value < 1 || value > 100) {
    return 15;
  }
  return (int)value;
}

static long probe_elapsed_ns(const struct timespec *start,
                             const struct timespec *end) {
  return ((long)(end->tv_sec - start->tv_sec) * 1000000000L) +
         (long)(end->tv_nsec - start->tv_nsec);
}

int main(int argc, char **argv) {
  const char *fixture;
  const char *expr;
  const char *mutation_expr;
  FILE *file;
  probe_reader reader;
  lql *ctx;
  lql_selector *selector;
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  struct timespec start;
  struct timespec end;
  long best_ns;
  long elapsed_ns;
  int samples;
  int sample;
  int i;

  fixture = NULL;
  expr = "/status=\"open\"";
  mutation_expr = NULL;
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
      fixture = argv[++i];
    } else if (strcmp(argv[i], "--expr") == 0 && i + 1 < argc) {
      expr = argv[++i];
    } else if (strcmp(argv[i], "--mutation") == 0 && i + 1 < argc) {
      mutation_expr = argv[++i];
    } else {
      fprintf(stderr,
              "usage: %s --fixture PATH [--expr SELECTOR] [--mutation ACTION]\n",
              argv[0]);
      return 2;
    }
  }
  if (fixture == NULL) {
    fprintf(stderr, "lql_direct_probe: --fixture is required\n");
    return 2;
  }
  file = fopen(fixture, "rb");
  if (file == NULL) {
    fprintf(stderr, "lql_direct_probe: open fixture failed\n");
    return 1;
  }
  ctx = NULL;
  selector = NULL;
  mutation = NULL;
  lql_error_init(&error);
  if (lql_new(&ctx, &error) != LQL_STATUS_OK ||
      ctx->selector_parse(ctx, expr, &selector, &error) != LQL_STATUS_OK ||
      (mutation_expr != NULL &&
       ctx->mutation_parse(ctx, &mutation_expr, 1u, &mutation, &error) !=
           LQL_STATUS_OK)) {
    fprintf(stderr, "lql_direct_probe: setup failed: %s\n", error.message);
    if (mutation != NULL) {
      ctx->mutation_destroy(ctx, mutation);
    }
    if (ctx != NULL) {
      ctx->destroy(ctx);
    }
    fclose(file);
    return 1;
  }
  memset(&reader, 0, sizeof(reader));
  reader.file = file;
  memset(&request, 0, sizeof(request));
  request.reader = probe_read;
  request.reader_user = &reader;
  request.selector = selector;
  if (mutation != NULL) {
    request.matched_only = 1;
    request.writer = probe_discard_write;
    request.mutation = mutation;
    request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  }
  if ((mutation != NULL
           ? ctx->stream_execute_spooled(ctx, &request, &result, &error)
           : ctx->stream_execute(ctx, &request, &result, &error)) !=
      LQL_STATUS_OK) {
    fprintf(stderr, "lql_direct_probe: warmup failed: %s\n", error.message);
    if (mutation != NULL) {
      ctx->mutation_destroy(ctx, mutation);
    }
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    fclose(file);
    return 1;
  }
  samples = probe_samples();
  best_ns = -1L;
  for (sample = 0; sample < samples; ++sample) {
    if (fseek(file, 0L, SEEK_SET) != 0) {
      fprintf(stderr, "lql_direct_probe: rewind fixture failed\n");
      if (mutation != NULL) {
        ctx->mutation_destroy(ctx, mutation);
      }
      ctx->selector_destroy(ctx, selector);
      ctx->destroy(ctx);
      fclose(file);
      return 1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0 ||
        (mutation != NULL
             ? ctx->stream_execute_spooled(ctx, &request, &result, &error)
             : ctx->stream_execute(ctx, &request, &result, &error)) !=
            LQL_STATUS_OK ||
        clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
      fprintf(stderr, "lql_direct_probe: stream failed: %s\n", error.message);
      if (mutation != NULL) {
        ctx->mutation_destroy(ctx, mutation);
      }
      ctx->selector_destroy(ctx, selector);
      ctx->destroy(ctx);
      fclose(file);
      return 1;
    }
    elapsed_ns = probe_elapsed_ns(&start, &end);
    if (best_ns < 0L || elapsed_ns < best_ns) {
      best_ns = elapsed_ns;
    }
  }
  printf("{\"schema\":\"liblql.direct_probe.v1\",\"impl\":\"c\","
         "\"bytes_per_iter\":%lu,"
         "\"records\":%lu,\"matches\":%lu,\"ns_per_op\":%ld}\n",
         (unsigned long)result.bytes_consumed,
         (unsigned long)result.records_seen, (unsigned long)result.records_matched,
         best_ns);
  ctx->selector_destroy(ctx, selector);
  if (mutation != NULL) {
    ctx->mutation_destroy(ctx, mutation);
  }
  ctx->destroy(ctx);
  fclose(file);
  return 0;
}
