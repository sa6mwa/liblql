#include <lql/lql.h>
#include <lql/version.h>

static int check_lql_header_cpp(void) {
  lql *ctx = 0;
  lql_error error;
  lql_capabilities capabilities;
  lql_selector *selector = 0;
  lql_status status;

  lql_error_init(&error);
  status = lql_new(&ctx, &error);
  if (status != LQL_STATUS_OK || ctx == 0) {
    return 1;
  }
  ctx->capabilities_get(ctx, &capabilities);
  if (ctx->stream_execute == 0 || ctx->stream_execute_spooled == 0) {
    ctx->destroy(ctx);
    return 1;
  }
  ctx->destroy(ctx);
  (void)status;
  (void)selector;
  (void)LQL_VERSION;
  return capabilities.selector_parse ? 0 : 1;
}

int main(void) { return check_lql_header_cpp(); }
