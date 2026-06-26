#include "lql_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct counting_allocator {
  lql_allocator api;
  lql_allocator *backing;
  size_t alloc_count;
  size_t destroy_count;
  size_t outstanding;
} counting_allocator;

static void *counting_alloc(lql_allocator *self, size_t size) {
  counting_allocator *counter;
  void *ptr;

  counter = (counting_allocator *)self->impl;
  ptr = counter->backing->alloc(counter->backing, size);
  if (ptr != NULL) {
    ++counter->alloc_count;
    ++counter->outstanding;
  }
  return ptr;
}

static void *counting_calloc(lql_allocator *self, size_t count, size_t size) {
  counting_allocator *counter;
  void *ptr;

  counter = (counting_allocator *)self->impl;
  ptr = counter->backing->calloc(counter->backing, count, size);
  if (ptr != NULL) {
    ++counter->alloc_count;
    ++counter->outstanding;
  }
  return ptr;
}

static void *counting_realloc(lql_allocator *self, void *ptr, size_t size) {
  counting_allocator *counter;
  void *next;

  counter = (counting_allocator *)self->impl;
  next = counter->backing->realloc(counter->backing, ptr, size);
  if (next != NULL && ptr == NULL) {
    ++counter->alloc_count;
    ++counter->outstanding;
  }
  return next;
}

static void counting_destroy(lql_allocator *self, void *ptr) {
  counting_allocator *counter;

  if (ptr == NULL) {
    return;
  }
  counter = (counting_allocator *)self->impl;
  ++counter->destroy_count;
  if (counter->outstanding > 0u) {
    --counter->outstanding;
  }
  counter->backing->destroy(counter->backing, ptr);
}

static char *counting_strdup(lql_allocator *self, const char *text) {
  counting_allocator *counter;
  char *ptr;

  counter = (counting_allocator *)self->impl;
  ptr = counter->backing->strdup(counter->backing, text);
  if (ptr != NULL) {
    ++counter->alloc_count;
    ++counter->outstanding;
  }
  return ptr;
}

static void counting_allocator_init(counting_allocator *counter) {
  memset(counter, 0, sizeof(*counter));
  counter->backing = lql_allocator_default();
  counter->api.impl = counter;
  counter->api.alloc = counting_alloc;
  counter->api.calloc = counting_calloc;
  counter->api.realloc = counting_realloc;
  counter->api.destroy = counting_destroy;
  counter->api.strdup = counting_strdup;
}

static int expect_selector_success_uses_allocator(void) {
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;
  size_t outstanding_after_parse;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("selector allocator receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK || selector == NULL) {
    printf("selector allocator parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  if (selector->allocator != &counter.api || counter.alloc_count == 0u) {
    printf("selector allocator was not recorded or used\n");
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  outstanding_after_parse = counter.outstanding;
  matched = 0;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, "{\"status\":\"open\"}",
                         strlen("{\"status\":\"open\"}"), &matched, &error);
  if (st != LQL_STATUS_OK || !matched) {
    printf("selector allocator eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.outstanding != outstanding_after_parse) {
    printf("selector eval allocator cleanup imbalance: before=%lu after=%lu\n",
           (unsigned long)outstanding_after_parse,
           (unsigned long)counter.outstanding);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf(
        "selector allocator cleanup imbalance: outstanding=%lu destroys=%lu\n",
        (unsigned long)counter.outstanding,
        (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_mutation_success_uses_allocator(void) {
  counting_allocator counter;
  lql *ctx;
  lql_mutation_plan *plan;
  const char *exprs[2];
  lql_error error;
  lql_status st;
  size_t outstanding_after_parse;
  FILE *out;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("mutation allocator receiver failed: %s\n", error.message);
    return 1;
  }
  exprs[0] = "/count=+2";
  exprs[1] = "/title=\"done\"";
  plan = NULL;
  lql_error_init(&error);
  st = ctx->mutation_plan_parse(ctx, exprs, 2u, &plan, &error);
  if (st != LQL_STATUS_OK || plan == NULL) {
    printf("mutation allocator parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.alloc_count == 0u ||
      ctx->mutation_plan_count(ctx, plan) != 2u) {
    printf("mutation allocator was not used or count mismatch\n");
    ctx->mutation_plan_destroy(ctx, plan);
    ctx->destroy(ctx);
    return 1;
  }
  outstanding_after_parse = counter.outstanding;
  out = tmpfile();
  if (out == NULL) {
    printf("mutation allocator tmpfile failed\n");
    ctx->mutation_plan_destroy(ctx, plan);
    ctx->destroy(ctx);
    return 1;
  }
  lql_error_init(&error);
  st = ctx->mutate_json(ctx, plan, "{\"count\":3,\"title\":\"old\"}",
                        strlen("{\"count\":3,\"title\":\"old\"}"), out,
                        &error);
  fclose(out);
  if (st != LQL_STATUS_OK) {
    printf("mutation allocator runtime failed: %s\n", error.message);
    ctx->mutation_plan_destroy(ctx, plan);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.outstanding != outstanding_after_parse) {
    printf(
        "mutation runtime allocator cleanup imbalance: before=%lu after=%lu\n",
        (unsigned long)outstanding_after_parse,
        (unsigned long)counter.outstanding);
    ctx->mutation_plan_destroy(ctx, plan);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->mutation_plan_destroy(ctx, plan);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf(
        "mutation allocator cleanup imbalance: outstanding=%lu destroys=%lu\n",
        (unsigned long)counter.outstanding,
        (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_mutation_parse_failure_cleans_allocator(void) {
  counting_allocator counter;
  lql *ctx;
  lql_mutation_plan *plan;
  const char *exprs[1];
  lql_error error;
  lql_status st;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("mutation allocator receiver failed: %s\n", error.message);
    return 1;
  }
  exprs[0] = "/=1";
  plan = NULL;
  lql_error_init(&error);
  st = ctx->mutation_plan_parse(ctx, exprs, 1u, &plan, &error);
  if (st == LQL_STATUS_OK || plan != NULL) {
    printf("mutation allocator parse failure unexpectedly succeeded\n");
    ctx->mutation_plan_destroy(ctx, plan);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->destroy(ctx);
  if (counter.alloc_count == 0u || counter.outstanding != 0u) {
    printf("mutation allocator failure cleanup imbalance: allocs=%lu "
           "outstanding=%lu\n",
           (unsigned long)counter.alloc_count,
           (unsigned long)counter.outstanding);
    return 1;
  }
  return 0;
}

static int expect_selector_parse_failure_cleans_allocator(void) {
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("selector allocator receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(
      ctx, "contains{field=/title,value=urgent,field=/other}", &selector,
      &error);
  if (st == LQL_STATUS_OK || selector != NULL) {
    printf("selector allocator parse failure unexpectedly succeeded\n");
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->destroy(ctx);
  if (counter.alloc_count == 0u || counter.outstanding != 0u) {
    printf("selector allocator failure cleanup imbalance: allocs=%lu "
           "outstanding=%lu\n",
           (unsigned long)counter.alloc_count,
           (unsigned long)counter.outstanding);
    return 1;
  }
  return 0;
}

static int expect_projection_success_uses_allocator(void) {
  counting_allocator counter;
  lql *ctx;
  lql_projection *projection;
  const char *fields[2];
  lql_error error;
  lql_status st;
  int found;
  size_t outstanding_after_parse;
  FILE *out;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("projection allocator receiver failed: %s\n", error.message);
    return 1;
  }
  fields[0] = "/title";
  fields[1] = "/items/0/name";
  projection = NULL;
  lql_error_init(&error);
  st = ctx->projection_parse(ctx, fields, 2u, &projection, &error);
  if (st != LQL_STATUS_OK || projection == NULL) {
    printf("projection allocator parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.alloc_count == 0u) {
    printf("projection allocator was not used\n");
    ctx->projection_destroy(ctx, projection);
    ctx->destroy(ctx);
    return 1;
  }
  outstanding_after_parse = counter.outstanding;
  out = tmpfile();
  if (out == NULL) {
    printf("projection allocator tmpfile failed\n");
    ctx->projection_destroy(ctx, projection);
    ctx->destroy(ctx);
    return 1;
  }
  found = 0;
  lql_error_init(&error);
  st = ctx->project_json(
      ctx, projection,
      "{\"title\":\"T\",\"items\":[{\"name\":\"N\"}],\"ignored\":1}",
      strlen("{\"title\":\"T\",\"items\":[{\"name\":\"N\"}],\"ignored\":1}"),
      out, &found, &error);
  fclose(out);
  if (st != LQL_STATUS_OK || !found) {
    printf("projection allocator project failed: %s\n", error.message);
    ctx->projection_destroy(ctx, projection);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.outstanding != outstanding_after_parse) {
    printf("projection runtime allocator cleanup imbalance: before=%lu "
           "after=%lu\n",
           (unsigned long)outstanding_after_parse,
           (unsigned long)counter.outstanding);
    ctx->projection_destroy(ctx, projection);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->projection_destroy(ctx, projection);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf("projection allocator cleanup imbalance: outstanding=%lu "
           "destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_projection_parse_failure_cleans_allocator(void) {
  counting_allocator counter;
  lql *ctx;
  lql_projection *projection;
  const char *fields[2];
  lql_error error;
  lql_status st;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("projection allocator receiver failed: %s\n", error.message);
    return 1;
  }
  fields[0] = "/items";
  fields[1] = "/items/0/name";
  projection = NULL;
  lql_error_init(&error);
  st = ctx->projection_parse(ctx, fields, 2u, &projection, &error);
  if (st == LQL_STATUS_OK || projection != NULL) {
    printf("projection allocator parse failure unexpectedly succeeded\n");
    ctx->projection_destroy(ctx, projection);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->destroy(ctx);
  if (counter.alloc_count == 0u || counter.outstanding != 0u) {
    printf("projection allocator failure cleanup imbalance: allocs=%lu "
           "outstanding=%lu\n",
           (unsigned long)counter.alloc_count,
           (unsigned long)counter.outstanding);
    return 1;
  }
  return 0;
}

int main(void) {
  int failures;

  failures = 0;
  failures += expect_selector_success_uses_allocator();
  failures += expect_selector_parse_failure_cleans_allocator();
  failures += expect_projection_success_uses_allocator();
  failures += expect_projection_parse_failure_cleans_allocator();
  failures += expect_mutation_success_uses_allocator();
  failures += expect_mutation_parse_failure_cleans_allocator();
  return failures == 0 ? 0 : 1;
}
