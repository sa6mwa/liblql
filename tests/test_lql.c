#include "lql/lql.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

typedef struct stream_seen {
  int calls;
  int matched;
  lql_uint64 offsets[4];
  lql_uint64 sizes[4];
} stream_seen;

static lql_status record_decision(void *user,
                                  const lql_query_decision *decision) {
  stream_seen *seen = (stream_seen *)user;
  if (seen->calls < 4) {
    seen->offsets[seen->calls] = decision->offset;
    seen->sizes[seen->calls] = decision->size;
  }
  if (decision->matched) {
    ++seen->matched;
  }
  ++seen->calls;
  return LQL_STATUS_OK;
}

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

static void expect_stream_file(void) {
  static const char input[] =
      "{\"status\":\"open\"}\n{\"status\":\"closed\"}\n";
  FILE *fp;
  lql_selector *selector;
  lql_query_result result;
  stream_seen seen;
  lql_error error;
  lql_status st;

  memset(&seen, 0, sizeof(seen));
  lql_error_init(&error);
  st = lql_selector_parse("/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  fp = tmpfile();
  if (fp == NULL) {
    printf("tmpfile failed\n");
    lql_selector_free(selector);
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("tmpfile write/seek failed\n");
    fclose(fp);
    lql_selector_free(selector);
    ++failures;
    return;
  }
  memset(&result, 0, sizeof(result));
  st = lql_query_file_decisions(selector, fp, record_decision, &seen, &result,
                                &error);
  fclose(fp);
  lql_selector_free(selector);
  if (st != LQL_STATUS_OK) {
    printf("stream query failed: %s\n", error.message);
    ++failures;
    return;
  }
  if (seen.calls != 2 || seen.matched != 1 ||
      result.candidates_seen != (lql_uint64)2 ||
      result.candidates_matched != (lql_uint64)1) {
    printf("stream counts mismatch calls=%d matched=%d\n", seen.calls,
           seen.matched);
    ++failures;
  }
  if (seen.offsets[0] != (lql_uint64)0 || seen.sizes[0] != (lql_uint64)17 ||
      seen.offsets[1] != (lql_uint64)18 || seen.sizes[1] != (lql_uint64)19) {
    printf("stream ranges mismatch\n");
    ++failures;
  }
}

static void expect_stream_array_items(void) {
  static const char input[] =
      "[{\"status\":\"open\"}, {\"status\":\"closed\"}]";
  FILE *fp;
  lql_selector *selector;
  lql_query_result result;
  stream_seen seen;
  lql_error error;
  lql_status st;

  memset(&seen, 0, sizeof(seen));
  lql_error_init(&error);
  st = lql_selector_parse("/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("array stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  fp = tmpfile();
  if (fp == NULL) {
    printf("array tmpfile failed\n");
    lql_selector_free(selector);
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("array tmpfile write/seek failed\n");
    fclose(fp);
    lql_selector_free(selector);
    ++failures;
    return;
  }
  memset(&result, 0, sizeof(result));
  st = lql_query_file_decisions(selector, fp, record_decision, &seen, &result,
                                &error);
  fclose(fp);
  lql_selector_free(selector);
  if (st != LQL_STATUS_OK) {
    printf("array stream query failed: %s\n", error.message);
    ++failures;
    return;
  }
  if (seen.calls != 2 || seen.matched != 1 ||
      result.candidates_seen != (lql_uint64)2 ||
      result.candidates_matched != (lql_uint64)1) {
    printf("array stream counts mismatch calls=%d matched=%d\n", seen.calls,
           seen.matched);
    ++failures;
  }
  if (seen.offsets[0] != (lql_uint64)1 || seen.sizes[0] != (lql_uint64)17 ||
      seen.offsets[1] != (lql_uint64)20 || seen.sizes[1] != (lql_uint64)19) {
    printf("array stream ranges mismatch\n");
    ++failures;
  }
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
  expect_match("exists{/metadata}", "{\"metadata\":{\"etag\":\"x\"}}", 1);
  expect_match("/metadata=\"\"", "{\"metadata\":{\"etag\":\"x\"}}", 0);
  expect_match("/items/0/sku=\"a\"", "{\"items\":[{\"sku\":\"a\"}]}", 1);
  expect_match("/status=\"open\",/progress>=50",
               "{\"status\":\"open\",\"progress\":72}", 1);
  expect_match("/status=\"open\",/progress>=50",
               "{\"status\":\"open\",\"progress\":4}", 0);
  expect_stream_file();
  expect_stream_array_items();
  return failures == 0 ? 0 : 1;
}
