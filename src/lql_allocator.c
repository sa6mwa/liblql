#include "lql_internal.h"

#include <stdlib.h>

LQL_INTERNAL_SYMBOL void *lql_alloc(size_t size) { return malloc(size); }

LQL_INTERNAL_SYMBOL void *lql_calloc(size_t count, size_t size) {
  return calloc(count, size);
}

LQL_INTERNAL_SYMBOL void *lql_realloc(void *ptr, size_t size) {
  return realloc(ptr, size);
}

LQL_INTERNAL_SYMBOL void lql_dealloc(void *ptr) { free(ptr); }
