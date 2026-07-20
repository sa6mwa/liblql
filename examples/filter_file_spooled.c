#include <lql/lql.h>

#include <stdio.h>
#include <string.h>

static int fail(lql_status status, const lql_error *error) {
  fprintf(stderr, "filter_file_spooled: %s", lql_status_string(status));
  if (error != NULL && error->message[0] != '\0') {
    fprintf(stderr, ": %s", error->message);
  }
  fputc('\n', stderr);
  return 1;
}

int main(int argc, char **argv) {
  lql *ctx;
  lql_selector *selector;
  lql_file_filter_request request;
  lql_stream_result result;
  lql_error error;
  lql_status status;
  const char *input;

  input = argc > 1 ? argv[1] : "examples/status.ndjson";
  ctx = NULL;
  selector = NULL;
  lql_error_init(&error);
  status = lql_new(&ctx, &error);
  if (status != LQL_STATUS_OK) {
    return fail(status, &error);
  }
  status = ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error);
  if (status != LQL_STATUS_OK) {
    ctx->destroy(ctx);
    return fail(status, &error);
  }
  memset(&request, 0, sizeof(request));
  request.input_path = input;
  request.output_file = stdout;
  request.selector = selector;
  request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  request.matched_only = 1;
  status = ctx->filter_file_spooled(ctx, &request, &result, &error);
  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  return status == LQL_STATUS_OK ? 0 : fail(status, &error);
}
