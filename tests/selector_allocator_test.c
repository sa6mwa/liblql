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
  lql_selector *selector;
  lql_error error;
  lql_status st;

  counting_allocator_init(&counter);
  selector = NULL;
  lql_error_init(&error);
  st = lql_parse_selector_internal(
      &counter.api,
      "or.1./status=\"open\",and.1.contains{field=/title,value=\"urgent\"}", 0,
      &selector, &error);
  if (st != LQL_STATUS_OK || selector == NULL) {
    printf("selector allocator parse failed: %s\n", error.message);
    return 1;
  }
  if (selector->allocator != &counter.api || counter.alloc_count == 0u) {
    printf("selector allocator was not recorded or used\n");
    lql_selector_destroy_impl(NULL, selector);
    return 1;
  }
  lql_selector_destroy_impl(NULL, selector);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf(
        "selector allocator cleanup imbalance: outstanding=%lu destroys=%lu\n",
        (unsigned long)counter.outstanding,
        (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_selector_parse_failure_cleans_allocator(void) {
  counting_allocator counter;
  lql_selector *selector;
  lql_error error;
  lql_status st;

  counting_allocator_init(&counter);
  selector = NULL;
  lql_error_init(&error);
  st = lql_parse_selector_internal(
      &counter.api, "contains{field=/title,value=urgent,field=/other}", 0,
      &selector, &error);
  if (st == LQL_STATUS_OK || selector != NULL) {
    printf("selector allocator parse failure unexpectedly succeeded\n");
    lql_selector_destroy_impl(NULL, selector);
    return 1;
  }
  if (counter.alloc_count == 0u || counter.outstanding != 0u) {
    printf("selector allocator failure cleanup imbalance: allocs=%lu "
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
  return failures == 0 ? 0 : 1;
}
