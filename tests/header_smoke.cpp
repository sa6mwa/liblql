#include <lql/lql.h>
#include <lql/version.h>

static int check_lql_header_cpp(void) {
  lql *ctx = 0;
  lql_error error;
  lql_capabilities capabilities;
  lql_selector *selector = 0;
  lql_status status;

  lql_error_init(&error);
  lql_capabilities_get(&capabilities);
  status = lql_new(&ctx, &error);
  (void)status;
  (void)ctx;
  (void)selector;
  (void)LQL_VERSION;
  return capabilities.selector_parse ? 0 : 1;
}

int main(void) { return check_lql_header_cpp(); }
