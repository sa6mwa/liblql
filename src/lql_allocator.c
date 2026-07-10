#include "lql_internal.h"

#include <stdlib.h>
#include <string.h>

#define LQL_LONEJSON_CANDIDATE_READ_BUFFER_SIZE (64u * 1024u)

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

LQL_INTERNAL_SYMBOL int
lql_lonejson_default_runtime_pool_allowed(const lql *self) {
  const lql_impl *impl;
  if (self == NULL || self->impl == NULL) {
    return 0;
  }
  impl = (const lql_impl *)self->impl;
  return impl->allocator == &default_allocator;
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

static void *lonejson_lql_malloc(void *ctx, size_t size) {
  lql_allocator *allocator;
  allocator = (lql_allocator *)ctx;
  return allocator->alloc(allocator, size);
}

static void *lonejson_lql_realloc(void *ctx, void *ptr, size_t size) {
  lql_allocator *allocator;
  allocator = (lql_allocator *)ctx;
  return allocator->realloc(allocator, ptr, size);
}

static void lonejson_lql_release(void *ctx, void *ptr) {
  lql_allocator *allocator;
  allocator = (lql_allocator *)ctx;
  allocator->destroy(allocator, ptr);
}

LQL_INTERNAL_SYMBOL lonejson *lql_lonejson_new(lql *self,
                                               lonejson_error *error) {
  lonejson_config config;
  lonejson_allocator allocator;
  lql_allocator *lql_alloc;

  lql_alloc = lql_allocator_from_receiver(self);
  if (lql_alloc == NULL) {
    if (error != NULL) {
      error->code = LONEJSON_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "lql receiver allocator required");
    }
    return NULL;
  }

  config = lonejson_default_config();
  config.json_value_max_number_bytes = 4096u;
  config.candidate_read_buffer_size = LQL_LONEJSON_CANDIDATE_READ_BUFFER_SIZE;
  allocator = lonejson_default_allocator();
  allocator.malloc_fn = lonejson_lql_malloc;
  allocator.realloc_fn = lonejson_lql_realloc;
  allocator.free_fn = lonejson_lql_release;
  allocator.ctx = lql_alloc;
  allocator.stats = NULL;
  config.allocator = &allocator;
  return lonejson_new(&config, error);
}

static lonejson *lql_lonejson_new_pooled(lql *self, lonejson_error *error) {
  lonejson_config config;

  if (self == NULL || self->impl == NULL ||
      lql_allocator_from_receiver(self) == NULL) {
    if (error != NULL) {
      error->code = LONEJSON_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "lql receiver allocator required");
    }
    return NULL;
  }

  config = lonejson_default_config();
  config.json_value_max_number_bytes = 4096u;
  config.candidate_read_buffer_size = LQL_LONEJSON_CANDIDATE_READ_BUFFER_SIZE;
  return lonejson_new(&config, error);
}

LQL_INTERNAL_SYMBOL lonejson *lql_lonejson_acquire(lql *self, int *out_pooled,
                                                   lonejson_error *error) {
  lql_impl *impl;

  if (out_pooled != NULL) {
    *out_pooled = 0;
  }
  if (self == NULL || self->impl == NULL) {
    if (error != NULL) {
      error->code = LONEJSON_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "lql receiver required");
    }
    return NULL;
  }
  impl = (lql_impl *)self->impl;
  if (!impl->eval_runtime_in_use) {
    if (impl->eval_runtime == NULL) {
      impl->eval_runtime = lql_lonejson_new_pooled(self, error);
      if (impl->eval_runtime == NULL) {
        return NULL;
      }
    }
    impl->eval_runtime_in_use = 1;
    if (out_pooled != NULL) {
      *out_pooled = 1;
    }
    return impl->eval_runtime;
  }
  if (!impl->eval_runtime_nested_in_use) {
    if (impl->eval_runtime_nested == NULL) {
      impl->eval_runtime_nested = lql_lonejson_new_pooled(self, error);
      if (impl->eval_runtime_nested == NULL) {
        return NULL;
      }
    }
    impl->eval_runtime_nested_in_use = 1;
    if (out_pooled != NULL) {
      *out_pooled = 1;
    }
    return impl->eval_runtime_nested;
  }
  return lql_lonejson_new(self, error);
}

LQL_INTERNAL_SYMBOL void lql_lonejson_release(lql *self, lonejson *runtime,
                                              int pooled) {
  lql_impl *impl;

  if (runtime == NULL) {
    return;
  }
  if (pooled && self != NULL && self->impl != NULL) {
    impl = (lql_impl *)self->impl;
    if (impl->eval_runtime == runtime) {
      impl->eval_runtime_in_use = 0;
      return;
    }
    if (impl->eval_runtime_nested == runtime) {
      impl->eval_runtime_nested_in_use = 0;
      return;
    }
  }
  lonejson_free(runtime);
}
