#include <lql/lql.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fuzz_reader {
  const unsigned char *data;
  size_t len;
  size_t offset;
} fuzz_reader;

static lql_status fuzz_read(void *user, unsigned char *buffer, size_t capacity,
                            size_t *out_len, lql_error *error) {
  fuzz_reader *reader;
  size_t amount;
  (void)error;
  reader = (fuzz_reader *)user;
  if (reader == NULL || buffer == NULL || out_len == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out_len = 0u;
  if (reader->offset >= reader->len) {
    return LQL_STATUS_OK;
  }
  amount = reader->len - reader->offset;
  if (amount > capacity) {
    amount = capacity;
  }
  if (amount > 7u) {
    amount = 7u;
  }
  memcpy(buffer, reader->data + reader->offset, amount);
  reader->offset += amount;
  *out_len = amount;
  return LQL_STATUS_OK;
}

static unsigned char *read_input(const char *path, size_t *out_len) {
  FILE *file;
  long len;
  unsigned char *data;
  if (path == NULL || out_len == NULL) {
    return NULL;
  }
  *out_len = 0u;
  file = fopen(path, "rb");
  if (file == NULL) {
    return NULL;
  }
  if (fseek(file, 0L, SEEK_END) != 0 || (len = ftell(file)) < 0L ||
      fseek(file, 0L, SEEK_SET) != 0 || len > 1048576L) {
    fclose(file);
    return NULL;
  }
  data = (unsigned char *)malloc((size_t)len + 1u);
  if (data == NULL) {
    fclose(file);
    return NULL;
  }
  if (len > 0L && fread(data, 1u, (size_t)len, file) != (size_t)len) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  data[(size_t)len] = '\0';
  *out_len = (size_t)len;
  return data;
}

int main(int argc, char **argv) {
  unsigned char *data;
  size_t len;
  fuzz_reader reader;
  lql *ctx;
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  lql_status status;

  if (argc < 2) {
    return 0;
  }
  data = read_input(argv[1], &len);
  if (data == NULL) {
    return 0;
  }

  ctx = NULL;
  selector = NULL;
  lql_error_init(&error);
  if (lql_new(&ctx, &error) != LQL_STATUS_OK ||
      ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error) !=
          LQL_STATUS_OK) {
    if (ctx != NULL) {
      ctx->destroy(ctx);
    }
    free(data);
    return 0;
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = data;
  reader.len = len;
  memset(&request, 0, sizeof(request));
  request.reader = fuzz_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.matched_only = 1;
  status = lql_stream_execute(ctx, &request, &result, &error);
  (void)status;

  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  free(data);
  return 0;
}
