#include <lql/lql.h>
#include <lql/version.h>

int main(void) {
  lql_capabilities caps;
  lql_capabilities_get(&caps);
  return LQL_VERSION[0] != '\0' && lql_version()[0] != '\0' &&
                 caps.selector_parse
             ? LQL_STATUS_OK
             : 1;
}
