#include "lql_internal.h"

#include <stdlib.h>
#include <string.h>

static void *system_alloc(lql_allocator *self, size_t size) {
  (void)self;
  return malloc(size);
}

static void *system_calloc(lql_allocator *self, size_t count, size_t size) {
  (void)self;
  return calloc(count, size);
}

static void *system_realloc(lql_allocator *self, void *ptr, size_t size) {
  (void)self;
  return realloc(ptr, size);
}

static void system_destroy(lql_allocator *self, void *ptr) {
  (void)self;
  free(ptr);
}

static char *system_strdup(lql_allocator *self, const char *text) {
  size_t len;
  char *out;
  if (text == NULL) {
    return NULL;
  }
  len = strlen(text);
  out = (char *)self->alloc(self, len + 1u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, text, len + 1u);
  return out;
}

static lql_allocator default_allocator = {NULL,           system_alloc,
                                          system_calloc,  system_realloc,
                                          system_destroy, system_strdup};

LQL_INTERNAL_SYMBOL lql_allocator *lql_allocator_default(void) {
  return &default_allocator;
}

LQL_INTERNAL_SYMBOL lql_allocator *
lql_allocator_from_receiver(const lql *self) {
  const lql_impl *impl;
  if (self == NULL || self->impl == NULL) {
    return NULL;
  }
  impl = (const lql_impl *)self->impl;
  return impl->allocator;
}

LQL_INTERNAL_SYMBOL void *lql_receiver_alloc(lql *self, size_t size) {
  lql_allocator *allocator;
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    return NULL;
  }
  return allocator->alloc(allocator, size);
}

LQL_INTERNAL_SYMBOL void *lql_receiver_calloc(lql *self, size_t count,
                                              size_t size) {
  lql_allocator *allocator;
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    return NULL;
  }
  return allocator->calloc(allocator, count, size);
}

LQL_INTERNAL_SYMBOL void *lql_receiver_realloc(lql *self, void *ptr,
                                               size_t size) {
  lql_allocator *allocator;
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    return NULL;
  }
  return allocator->realloc(allocator, ptr, size);
}

LQL_INTERNAL_SYMBOL char *lql_receiver_strdup(lql *self, const char *text) {
  lql_allocator *allocator;
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL || text == NULL) {
    return NULL;
  }
  return allocator->strdup(allocator, text);
}

LQL_INTERNAL_SYMBOL void lql_receiver_destroy(lql *self, void *ptr) {
  lql_allocator *allocator;
  if (ptr == NULL) {
    return;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator != NULL) {
    allocator->destroy(allocator, ptr);
  }
}
