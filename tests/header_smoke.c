#include <lql/lql.h>
#include <lql/version.h>

int main(void) {
  return LQL_VERSION[0] != '\0' && lql_version()[0] != '\0' ? LQL_STATUS_OK : 1;
}
