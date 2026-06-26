#include "lql/lql.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  const char *json = "{\"status\":\"open\",\"progress\":72}";
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  int matched;

  lql_error_init(&error);
  if (lql_new(&ctx, &error) != LQL_STATUS_OK) {
    fprintf(stderr, "%s\n", error.message);
    return 1;
  }
  if (ctx->selector_parse(ctx, "/status=\"open\",/progress>=50", &selector,
                          &error) != LQL_STATUS_OK) {
    fprintf(stderr, "%s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  if (ctx->matches_json(ctx, selector, json, strlen(json), &matched, &error) !=
      LQL_STATUS_OK) {
    fprintf(stderr, "%s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  printf("%s\n", matched ? "match" : "no match");
  return matched ? 0 : 1;
}
