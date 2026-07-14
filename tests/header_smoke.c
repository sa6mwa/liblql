#include <lql/lql.h>
#include <lql/version.h>

int main(void) {
  lql *ctx;
  lql_capabilities caps;
  lql_error error;

  lql_error_init(&error);
  if (lql_new(&ctx, &error) != LQL_STATUS_OK) {
    return 1;
  }
  ctx->capabilities_get(ctx, &caps);
  if (LQL_VERSION[0] == '\0' || ctx->version(ctx)[0] == '\0' ||
      !caps.selector_parse || ctx->stream_execute == NULL ||
      ctx->stream_execute_spooled == NULL) {
    ctx->destroy(ctx);
    return 1;
  }
  ctx->destroy(ctx);
  return LQL_STATUS_OK;
}
