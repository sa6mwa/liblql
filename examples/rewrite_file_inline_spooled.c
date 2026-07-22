#include <lql/lql.h>

#include <stdio.h>
#include <string.h>

static int fail(lql_status status, const lql_error *error) {
  fprintf(stderr, "rewrite_file_inline_spooled: %s", lql_status_string(status));
  if (error != NULL && error->message[0] != '\0') {
    fprintf(stderr, ": %s", error->message);
  }
  fputc('\n', stderr);
  return 1;
}

int main(int argc, char **argv) {
  static const char *const mutations[] = {"/processed=true"};
  lql *ctx;
  lql_selector *selector;
  lql_mutation *mutation;
  lql_file_filter_request request;
  lql_stream_result result;
  lql_error error;
  lql_status status;

  if (argc != 2) {
    fputs("usage: rewrite_file_inline_spooled data.ndjson\n", stderr);
    return 2;
  }
  ctx = NULL;
  selector = NULL;
  mutation = NULL;
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
  status = ctx->mutation_parse(ctx, mutations, 1u, &mutation, &error);
  if (status != LQL_STATUS_OK) {
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return fail(status, &error);
  }
  memset(&request, 0, sizeof(request));
  request.input_path = argv[1];
  request.selector = selector;
  request.mutation = mutation;
  request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  request.matched_only = 0;
  status = ctx->rewrite_file_inline_spooled(ctx, &request, &result, &error);
  ctx->mutation_destroy(ctx, mutation);
  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  return status == LQL_STATUS_OK ? 0 : fail(status, &error);
}
