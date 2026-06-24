#include "lql/lql.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  const char *json = "{\"status\":\"open\",\"progress\":72}";
  lql_selector *selector;
  lql_error error;
  int matched;

  lql_error_init(&error);
  if (lql_selector_parse("/status=\"open\",/progress>=50", &selector, &error) !=
      LQL_STATUS_OK) {
    fprintf(stderr, "%s\n", error.message);
    return 1;
  }
  if (lql_matches_json(selector, json, strlen(json), &matched, &error) !=
      LQL_STATUS_OK) {
    fprintf(stderr, "%s\n", error.message);
    lql_selector_free(selector);
    return 1;
  }
  lql_selector_free(selector);
  printf("%s\n", matched ? "match" : "no match");
  return matched ? 0 : 1;
}
