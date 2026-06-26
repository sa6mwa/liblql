#include "lql_internal.h"

#include <stdlib.h>

void *lql_alloc(size_t size) { return malloc(size); }

void *lql_calloc(size_t count, size_t size) { return calloc(count, size); }

void *lql_realloc(void *ptr, size_t size) { return realloc(ptr, size); }

void lql_dealloc(void *ptr) { free(ptr); }
