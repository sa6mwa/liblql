#include "lql/lql.h"

#include <stdio.h>
#include <string.h>

typedef struct fuzz_source {
  const unsigned char *data;
  size_t len;
  size_t offset;
  size_t chunk;
} fuzz_source;

static int failures = 0;

static lql_read_result fuzz_read(void *user, unsigned char *buffer,
                                 size_t capacity) {
  fuzz_source *source;
  lql_read_result result;
  size_t remaining;
  size_t want;

  memset(&result, 0, sizeof(result));
  source = (fuzz_source *)user;
  if (source == NULL || buffer == NULL || capacity == 0u ||
      source->offset >= source->len) {
    result.eof = 1;
    return result;
  }
  remaining = source->len - source->offset;
  want = remaining < capacity ? remaining : capacity;
  if (source->chunk != 0u && want > source->chunk) {
    want = source->chunk;
  }
  memcpy(buffer, source->data + source->offset, want);
  source->offset += want;
  result.bytes_read = want;
  if (source->offset >= source->len) {
    result.eof = 1;
  }
  return result;
}

static lql_status ignore_decision(void *user,
                                  const lql_query_decision *decision) {
  (void)user;
  (void)decision;
  return LQL_STATUS_OK;
}

static lql_status drain_match(void *user, const lql_query_match *match) {
  lql *ctx;
  lql_error error;
  FILE *out;
  lql_status st;

  ctx = (lql *)user;
  out = tmpfile();
  if (out == NULL) {
    return LQL_STATUS_NO_MEMORY;
  }
  lql_error_init(&error);
  st = ctx->payload_write_json(ctx, &match->payload, out, &error);
  fclose(out);
  return st;
}

static void smoke_selector(lql *ctx, const char *expr, const char *json) {
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;

  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, expr, &selector, &error);
  if (st == LQL_STATUS_OK) {
    matched = 0;
    (void)ctx->matches_json(ctx, selector, json, strlen(json), &matched,
                            &error);
    ctx->selector_destroy(ctx, selector);
  } else if (selector != NULL) {
    printf("fuzz smoke: selector parse failure left handle for %s\n", expr);
    ++failures;
    ctx->selector_destroy(ctx, selector);
  }
}

static void smoke_projection(lql *ctx, const char *field, const char *json) {
  const char *fields[1];
  lql_projection *projection;
  lql_error error;
  lql_status st;
  int found;
  FILE *out;
  fuzz_source source;

  fields[0] = field;
  projection = NULL;
  lql_error_init(&error);
  st = ctx->projection_parse(ctx, fields, 1u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    if (projection != NULL) {
      printf("fuzz smoke: projection parse failure left handle\n");
      ++failures;
      ctx->projection_destroy(ctx, projection);
    }
    return;
  }

  out = tmpfile();
  if (out == NULL) {
    printf("fuzz smoke: projection tmpfile failed\n");
    ++failures;
    ctx->projection_destroy(ctx, projection);
    return;
  }
  found = 0;
  (void)ctx->project_json(ctx, projection, json, strlen(json), out, &found,
                          &error);
  fclose(out);

  memset(&source, 0, sizeof(source));
  source.data = (const unsigned char *)json;
  source.len = strlen(json);
  source.chunk = 3u;
  out = tmpfile();
  if (out != NULL) {
    found = 0;
    (void)ctx->project_source(ctx, projection, fuzz_read, &source, out, &found,
                              &error);
    fclose(out);
  }
  ctx->projection_destroy(ctx, projection);
}

static void smoke_compact(lql *ctx, const unsigned char *data, size_t len) {
  lql_error error;
  fuzz_source source;
  FILE *out;

  out = tmpfile();
  if (out == NULL) {
    printf("fuzz smoke: compact tmpfile failed\n");
    ++failures;
    return;
  }
  lql_error_init(&error);
  (void)ctx->compact_json(ctx, (const char *)data, len, out, &error);
  fclose(out);

  memset(&source, 0, sizeof(source));
  source.data = data;
  source.len = len;
  source.chunk = 2u;
  out = tmpfile();
  if (out != NULL) {
    (void)ctx->compact_source(ctx, fuzz_read, &source, out, &error);
    fclose(out);
  }
}

static void smoke_mutation(lql *ctx, const char *expr, const char *json) {
  const char *exprs[1];
  lql_mutation_plan *plan;
  lql_error error;
  lql_status st;
  FILE *out;
  fuzz_source source;

  exprs[0] = expr;
  plan = NULL;
  lql_error_init(&error);
  st = ctx->mutation_plan_parse(ctx, exprs, 1u, &plan, &error);
  if (st != LQL_STATUS_OK) {
    if (plan != NULL) {
      printf("fuzz smoke: mutation parse failure left handle\n");
      ++failures;
      ctx->mutation_plan_destroy(ctx, plan);
    }
    return;
  }

  out = tmpfile();
  if (out != NULL) {
    (void)ctx->mutate_json(ctx, plan, json, strlen(json), out, &error);
    fclose(out);
  }

  memset(&source, 0, sizeof(source));
  source.data = (const unsigned char *)json;
  source.len = strlen(json);
  source.chunk = 4u;
  out = tmpfile();
  if (out != NULL) {
    (void)ctx->mutate_source_paths(ctx, plan, fuzz_read, &source, out, &error);
    fclose(out);
  }
  ctx->mutation_plan_destroy(ctx, plan);
}

static void smoke_stream(lql *ctx, const char *expr, const char *json) {
  lql_selector *selector;
  lql_error error;
  lql_status st;
  fuzz_source source;
  lql_query_result result;

  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    return;
  }

  memset(&source, 0, sizeof(source));
  source.data = (const unsigned char *)json;
  source.len = strlen(json);
  source.chunk = 5u;
  memset(&result, 0, sizeof(result));
  (void)ctx->query_source_decisions(ctx, selector, fuzz_read, &source,
                                    ignore_decision, NULL, &result, &error);

  memset(&source, 0, sizeof(source));
  source.data = (const unsigned char *)json;
  source.len = strlen(json);
  source.chunk = 3u;
  memset(&result, 0, sizeof(result));
  (void)ctx->query_source_spooled_matches(ctx, selector, fuzz_read, &source,
                                          drain_match, ctx, &result, &error);
  ctx->selector_destroy(ctx, selector);
}

static void expect_valid_seed(lql *ctx) {
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;

  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("fuzz smoke: valid selector failed: %s\n", error.message);
    ++failures;
    return;
  }
  matched = 0;
  st = ctx->matches_json(ctx, selector, "{\"status\":\"open\"}", 17u, &matched,
                         &error);
  if (st != LQL_STATUS_OK || !matched) {
    printf("fuzz smoke: valid match failed: status=%s matched=%d\n",
           lql_status_string(st), matched);
    ++failures;
  }
  ctx->selector_destroy(ctx, selector);
}

int main(void) {
  static const char *selector_seeds[] = {
      "",
      "/status=\"open\"",
      "contains{field=/message,any=open|closed}",
      "and.eq{field=/region,value=us},and.range{field=/metrics/qps,gte=1}",
      "not.exists{field=/deleted}",
      "/items[]/sku=\"x\"",
      "date{field=/timestamp,since=today}",
      "range{field=/n,gte=-10,lte=42}",
      "contains{field=/message,any=bad value}",
      "and.or.1.eq{field=/status,value=open}"};
  static const char *json_seeds[] = {
      "",
      "null",
      "\"scalar\"",
      "{\"status\":\"open\",\"message\":\"opened\",\"metrics\":{\"qps\":7}}",
      "{\"items\":[{\"sku\":\"x\"},{\"sku\":\"y\"}],\"n\":3}",
      "{\"status\":\"closed\"}\n{\"status\":\"open\"}\n",
      "[{\"status\":\"open\"},{\"status\":\"closed\"}]",
      "{\"status\":",
      "{bad json}",
      "{\"large\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"}"};
  static const char *projection_seeds[] = {
      "/status", "/metrics/qps", "/items/0/sku", "", "status", "/", "/0"};
  static const char *mutation_seeds[] = {
      "/status=running",
      "/metrics/qps=+2",
      "/state/old",
      "rm:/state/old",
      "time:/updated=NOW",
      "/state{/owner=\"alice\",/note=\"hi\"}",
      ""};
  lql *ctx;
  lql_error error;
  lql_status st;
  size_t i;
  size_t j;

  ctx = NULL;
  lql_error_init(&error);
  st = lql_new(&ctx, &error);
  if (st != LQL_STATUS_OK) {
    printf("fuzz smoke: lql_new failed: %s\n", error.message);
    return 1;
  }

  expect_valid_seed(ctx);
  for (i = 0u; i < sizeof(selector_seeds) / sizeof(selector_seeds[0]); ++i) {
    for (j = 0u; j < sizeof(json_seeds) / sizeof(json_seeds[0]); ++j) {
      smoke_selector(ctx, selector_seeds[i], json_seeds[j]);
      smoke_stream(ctx, selector_seeds[i], json_seeds[j]);
    }
  }
  for (i = 0u; i < sizeof(projection_seeds) / sizeof(projection_seeds[0]);
       ++i) {
    for (j = 0u; j < sizeof(json_seeds) / sizeof(json_seeds[0]); ++j) {
      smoke_projection(ctx, projection_seeds[i], json_seeds[j]);
    }
  }
  for (i = 0u; i < sizeof(mutation_seeds) / sizeof(mutation_seeds[0]); ++i) {
    for (j = 0u; j < sizeof(json_seeds) / sizeof(json_seeds[0]); ++j) {
      smoke_mutation(ctx, mutation_seeds[i], json_seeds[j]);
    }
  }
  for (i = 0u; i < sizeof(json_seeds) / sizeof(json_seeds[0]); ++i) {
    smoke_compact(ctx, (const unsigned char *)json_seeds[i],
                  strlen(json_seeds[i]));
  }

  ctx->destroy(ctx);
  return failures == 0 ? 0 : 1;
}
