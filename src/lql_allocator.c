#include "lql_internal.h"

#include <stdlib.h>
#include <string.h>

static void lonejson_eval_pool_release(void *ctx, void *ptr);

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

static void *pool_user_ptr(lql_pool_block *block) {
  return (void *)(block + 1);
}

static lql_pool_block *pool_block_from_user(void *ptr) {
  return ((lql_pool_block *)ptr) - 1;
}

static void *lonejson_eval_pool_malloc(void *ctx, size_t size) {
  lql_impl *impl;
  lql_pool_block *prev;
  lql_pool_block *block;
  size_t alloc_size;

  impl = (lql_impl *)ctx;
  if (impl == NULL || impl->allocator == NULL) {
    return NULL;
  }
  if (size == 0u) {
    size = 1u;
  }
  prev = NULL;
  block = impl->eval_pool_free;
  while (block != NULL) {
    if (block->size >= size) {
      if (prev == NULL) {
        impl->eval_pool_free = block->free_next;
      } else {
        prev->free_next = block->free_next;
      }
      block->free_next = NULL;
      return pool_user_ptr(block);
    }
    prev = block;
    block = block->free_next;
  }
  alloc_size = sizeof(*block) + size;
  block = (lql_pool_block *)impl->allocator->alloc(impl->allocator, alloc_size);
  if (block == NULL) {
    return NULL;
  }
  block->all_next = impl->eval_pool_all;
  block->free_next = NULL;
  block->size = size;
  impl->eval_pool_all = block;
  return pool_user_ptr(block);
}

static void *lonejson_eval_pool_realloc(void *ctx, void *ptr, size_t size) {
  lql_pool_block *block;
  void *next;

  if (ptr == NULL) {
    return lonejson_eval_pool_malloc(ctx, size);
  }
  if (size == 0u) {
    lonejson_eval_pool_release(ctx, ptr);
    return NULL;
  }
  block = pool_block_from_user(ptr);
  if (block->size >= size) {
    return ptr;
  }
  next = lonejson_eval_pool_malloc(ctx, size);
  if (next == NULL) {
    return NULL;
  }
  memcpy(next, ptr, block->size);
  lonejson_eval_pool_release(ctx, ptr);
  return next;
}

static void lonejson_eval_pool_release(void *ctx, void *ptr) {
  lql_impl *impl;
  lql_pool_block *block;

  if (ptr == NULL) {
    return;
  }
  impl = (lql_impl *)ctx;
  if (impl == NULL) {
    return;
  }
  block = pool_block_from_user(ptr);
  block->free_next = impl->eval_pool_free;
  impl->eval_pool_free = block;
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
  lonejson_allocator allocator;
  lql_impl *impl;

  if (self == NULL || self->impl == NULL ||
      lql_allocator_from_receiver(self) == NULL) {
    if (error != NULL) {
      error->code = LONEJSON_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "lql receiver allocator required");
    }
    return NULL;
  }
  impl = (lql_impl *)self->impl;

  config = lonejson_default_config();
  allocator = lonejson_default_allocator();
  allocator.malloc_fn = lonejson_eval_pool_malloc;
  allocator.realloc_fn = lonejson_eval_pool_realloc;
  allocator.free_fn = lonejson_eval_pool_release;
  allocator.ctx = impl;
  allocator.stats = NULL;
  config.allocator = &allocator;
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
  }
  lonejson_free(runtime);
}
