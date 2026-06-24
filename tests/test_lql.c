#include "lql/lql.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_match(const char *expr, const char *json, int want) {
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int got;

  lql_error_init(&error);
  st = lql_selector_parse(expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("parse failed for %s: %s\n", expr, error.message);
    ++failures;
    return;
  }
  st = lql_matches_json(selector, json, strlen(json), &got, &error);
  if (st != LQL_STATUS_OK) {
    printf("eval failed for %s: %s\n", expr, error.message);
    ++failures;
  } else if (got != want) {
    printf("match mismatch for %s: got %d want %d\n", expr, got, want);
    ++failures;
  }
  lql_selector_free(selector);
}

int main(void) {
  expect_match("/status=\"open\"", "{\"status\":\"open\"}", 1);
  expect_match("/status=\"closed\"", "{\"status\":\"open\"}", 0);
  expect_match("/progress>=50", "{\"progress\":72}", 1);
  expect_match("/progress<50", "{\"progress\":72}", 0);
  expect_match("contains{field=/message,value=timeout}",
               "{\"message\":\"upstream timeout\"}", 1);
  expect_match("icontains{field=/message,value=TIMEOUT}",
               "{\"message\":\"upstream timeout\"}", 1);
  expect_match("prefix{field=/service,value=auth}",
               "{\"service\":\"auth-api\"}", 1);
  expect_match("exists{/metadata/etag}", "{\"metadata\":{\"etag\":\"x\"}}", 1);
  expect_match("/status=\"open\",/progress>=50",
               "{\"status\":\"open\",\"progress\":72}", 1);
  expect_match("/status=\"open\",/progress>=50",
               "{\"status\":\"open\",\"progress\":4}", 0);
  return failures == 0 ? 0 : 1;
}
