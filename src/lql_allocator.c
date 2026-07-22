#include "lql_internal.h"

#include <stdlib.h>
#include <string.h>

typedef union lql_quota_header {
  struct {
    size_t size;
    lql_impl *owner;
  } quota;
  long double alignment;
  void *pointer;
} lql_quota_header;

static void *quota_alloc(lql_allocator *self, size_t size) {
  lql_impl *impl;
  lql_quota_header *header;
  size_t charge;
  if (self == NULL || self->impl == NULL ||
      size > (size_t)-1 - sizeof(*header)) {
    return NULL;
  }
  impl = (lql_impl *)self->impl;
  charge = sizeof(*header) + size;
  if (charge > LQL_INSTANCE_MEMORY_LIMIT_BYTES - impl->live_bytes) {
    return NULL;
  }
  header = (lql_quota_header *)impl->upstream_allocator->alloc(
      impl->upstream_allocator, charge);
  if (header == NULL) {
    return NULL;
  }
  header->quota.size = size;
  header->quota.owner = impl;
  impl->live_bytes += charge;
  impl->quota_allocation_count += 1u;
  return header + 1;
}

static void *quota_calloc(lql_allocator *self, size_t count, size_t size) {
  void *out;
  if (count != 0u && size > (size_t)-1 / count) {
    return NULL;
  }
  out = quota_alloc(self, count * size);
  if (out != NULL) {
    memset(out, 0, count * size);
  }
  return out;
}

static void *quota_realloc(lql_allocator *self, void *ptr, size_t size) {
  lql_impl *impl;
  lql_quota_header *header;
  lql_quota_header *next;
  size_t old_size;
  size_t old_charge;
  size_t new_charge;
  if (ptr == NULL) {
    return quota_alloc(self, size);
  }
  if (self == NULL || self->impl == NULL ||
      size > (size_t)-1 - sizeof(*header)) {
    return NULL;
  }
  impl = (lql_impl *)self->impl;
  header = ((lql_quota_header *)ptr) - 1;
  if (header->quota.owner != impl) {
    return NULL;
  }
  old_size = header->quota.size;
  old_charge = sizeof(*header) + old_size;
  new_charge = sizeof(*header) + size;
  if (size == 0u) {
    impl->live_bytes -= old_charge;
    impl->quota_allocation_count -= 1u;
    impl->upstream_allocator->destroy(impl->upstream_allocator, header);
    lql_allocator_instance_release_if_idle(impl);
    return NULL;
  }
  if (new_charge > old_charge &&
      new_charge - old_charge >
          LQL_INSTANCE_MEMORY_LIMIT_BYTES - impl->live_bytes) {
    return NULL;
  }
  next = (lql_quota_header *)impl->upstream_allocator->realloc(
      impl->upstream_allocator, header, new_charge);
  if (next == NULL && size != 0u) {
    return NULL;
  }
  next->quota.size = size;
  next->quota.owner = impl;
  impl->live_bytes = impl->live_bytes - old_charge + new_charge;
  return next + 1;
}

static void quota_destroy(lql_allocator *self, void *ptr) {
  lql_impl *impl;
  lql_quota_header *header;
  if (self == NULL || self->impl == NULL || ptr == NULL) {
    return;
  }
  header = ((lql_quota_header *)ptr) - 1;
  impl = header->quota.owner;
  if (impl == NULL) {
    return;
  }
  /*
   * The receiver passed to a destroy method may be different from the receiver
   * that created the handle. Quota accounting belongs to the allocation owner
   * stored in the header, and that owner remains alive until this final
   * release.
   */
  impl->live_bytes -= sizeof(*header) + header->quota.size;
  impl->quota_allocation_count -= 1u;
  impl->upstream_allocator->destroy(impl->upstream_allocator, header);
  lql_allocator_instance_release_if_idle(impl);
}

static char *quota_strdup(lql_allocator *self, const char *text) {
  size_t len;
  char *out;
  if (text == NULL) {
    return NULL;
  }
  len = strlen(text);
  if (len == (size_t)-1) {
    return NULL;
  }
  out = (char *)quota_alloc(self, len + 1u);
  if (out != NULL) {
    memcpy(out, text, len + 1u);
  }
  return out;
}

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
  return (lql_allocator *)&impl->allocator;
}

LQL_INTERNAL_SYMBOL void lql_allocator_instance_init(lql_impl *impl,
                                                     lql_allocator *upstream) {
  impl->upstream_allocator = upstream;
  impl->allocator.impl = impl;
  impl->allocator.alloc = quota_alloc;
  impl->allocator.calloc = quota_calloc;
  impl->allocator.realloc = quota_realloc;
  impl->allocator.destroy = quota_destroy;
  impl->allocator.strdup = quota_strdup;
}

LQL_INTERNAL_SYMBOL void
lql_allocator_instance_release_if_idle(lql_impl *impl) {
  if (impl != NULL && impl->receiver_destroyed &&
      impl->quota_allocation_count == 0u) {
    impl->upstream_allocator->destroy(impl->upstream_allocator, impl);
  }
}
