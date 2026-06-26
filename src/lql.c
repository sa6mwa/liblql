#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "lql_internal.h"

#include <lql/version.h>

#include <string.h>
#include <sys/types.h>

static const char *receiver_version(const lql *self);
static void receiver_capabilities_get(const lql *self, lql_capabilities *out);
static void receiver_destroy(lql *self);
static void capabilities_fill(lql_capabilities *out);
static lql_status selector_parse_method(lql *self, const char *expr,
                                        lql_selector **out, lql_error *error);
static lql_status selector_parse_or_method(lql *self, const char *expr,
                                           lql_selector **out,
                                           lql_error *error);
static void selector_destroy_method(lql *self, lql_selector *selector);
static int selector_is_empty_method(const lql *self,
                                    const lql_selector *selector);
static void selector_capabilities_get_method(
    const lql *self, const lql_selector *selector,
    lql_selector_capabilities *out);
static void selector_execution_traits_get_method(
    const lql *self, const lql_selector *selector,
    lql_selector_execution_traits *out);
static lql_status matches_json_method(lql *self, const lql_selector *selector,
                                      const char *json, size_t json_len,
                                      int *out_matched, lql_error *error);
static void selector_capabilities_visit(lql_selector_capabilities *out,
                                        const lql_node *node);
static void selector_path_capabilities_visit(lql_selector_capabilities *out,
                                             const char *path);
static int selector_segment_is(const char *start, size_t len,
                               const char *literal);

lql_status lql_new(lql **out, lql_error *error) {
  return lql_new_with_allocator(out, lql_allocator_default(), error);
}

LQL_INTERNAL_SYMBOL lql_status lql_new_with_allocator(lql **out,
                                                      lql_allocator *allocator,
                                                      lql_error *error) {
  lql *ctx;
  lql_impl *impl;

  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "out lql required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = NULL;
  if (allocator == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "allocator required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  ctx = (lql *)allocator->calloc(allocator, 1u, sizeof(*ctx));
  if (ctx == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  impl = (lql_impl *)allocator->calloc(allocator, 1u, sizeof(*impl));
  if (impl == NULL) {
    allocator->destroy(allocator, ctx);
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  impl->allocator = allocator;
  ctx->impl = impl;
  ctx->version = receiver_version;
  ctx->capabilities_get = receiver_capabilities_get;
  ctx->selector_parse = selector_parse_method;
  ctx->selector_parse_or = selector_parse_or_method;
  ctx->selector_destroy = selector_destroy_method;
  ctx->selector_is_empty = selector_is_empty_method;
  ctx->selector_capabilities_get = selector_capabilities_get_method;
  ctx->selector_execution_traits_get = selector_execution_traits_get_method;
  ctx->matches_json = matches_json_method;
  lql_eval_methods_install(ctx);
  lql_project_methods_install(ctx);
  lql_mutation_methods_install(ctx);
  ctx->destroy = receiver_destroy;
  *out = ctx;
  return LQL_STATUS_OK;
}

void lql_error_init(lql_error *error) {
  if (error != NULL) {
    error->code = LQL_STATUS_OK;
    error->message[0] = '\0';
  }
}

LQL_INTERNAL_SYMBOL void lql_set_error(lql_error *error, lql_status status,
                                       const char *message) {
  size_t len;
  if (error == NULL) {
    return;
  }
  error->code = status;
  if (message == NULL) {
    error->message[0] = '\0';
    return;
  }
  len = strlen(message);
  if (len >= sizeof(error->message)) {
    len = sizeof(error->message) - 1u;
  }
  memcpy(error->message, message, len);
  error->message[len] = '\0';
}

const char *lql_status_string(lql_status status) {
  switch (status) {
  case LQL_STATUS_OK:
    return "ok";
  case LQL_STATUS_INVALID_ARGUMENT:
    return "invalid argument";
  case LQL_STATUS_NO_MEMORY:
    return "out of memory";
  case LQL_STATUS_PARSE_ERROR:
    return "parse error";
  case LQL_STATUS_JSON_ERROR:
    return "json error";
  case LQL_STATUS_UNSUPPORTED:
    return "unsupported";
  case LQL_STATUS_STOP:
    return "stop";
  }
  return "unknown";
}

static void capabilities_fill(lql_capabilities *out) {
  if (out == NULL) {
    return;
  }
  memset(out, 0, sizeof(*out));
  out->selector_parse = 1;
  out->selector_inspection = 1;
  out->matches_json = 1;
  out->file_decision_stream = 1;
  out->source_decision_stream = 1;
  out->file_match_stream = 1;
  out->seekable_range_payloads = 1;
  out->source_spooled_match_stream = 1;
  out->spooled_payloads = 1;
  out->payload_sink_write = 1;
  out->payload_projection = 1;
  out->projection_file_range = 1;
  out->projection_source = 1;
  out->projection_buffered_json = 1;
  out->compact_file_range = 1;
  out->compact_source = 1;
  out->compact_buffered_json = 1;
  out->mutation_parse = 1;
  out->mutation_file_range = 1;
  out->mutation_file_range_candidates = 1;
  out->mutation_source = 1;
  out->mutation_source_candidates = 1;
  out->mutation_file_range_projected_candidates = 1;
  out->mutation_source_projected_candidates = 1;
  out->mutation_buffered_json = 1;
  out->mutation_file_values = 1;
}

static const char *receiver_version(const lql *self) {
  (void)self;
  return LQL_VERSION;
}

static void receiver_capabilities_get(const lql *self, lql_capabilities *out) {
  (void)self;
  capabilities_fill(out);
}

static void receiver_destroy(lql *self) {
  lql_allocator *allocator;
  lql_impl *impl;
  if (self == NULL) {
    return;
  }
  impl = (lql_impl *)self->impl;
  allocator = lql_allocator_from_receiver(self);
  if (impl != NULL) {
    allocator->destroy(allocator, impl);
  }
  allocator->destroy(allocator, self);
}

LQL_INTERNAL_SYMBOL void lql_node_cleanup(lql_allocator *allocator,
                                          lql_node *node) {
  size_t i;
  if (node == NULL) {
    return;
  }
  allocator->destroy(allocator, node->term.field);
  allocator->destroy(allocator, node->term.value);
  for (i = 0u; i < node->term.any_count; ++i) {
    allocator->destroy(allocator, node->term.any[i]);
  }
  allocator->destroy(allocator, node->term.any);
  for (i = 0u; i < node->child_count; ++i) {
    lql_node_cleanup(allocator, &node->children[i]);
  }
  allocator->destroy(allocator, node->children);
  memset(node, 0, sizeof(*node));
}

static lql_status selector_parse_method(lql *self,
                                                       const char *expr,
                                                       lql_selector **out,
                                                       lql_error *error) {
  return lql_parse_selector_internal(lql_allocator_from_receiver(self), expr, 0,
                                     out, error);
}

static lql_status selector_parse_or_method(lql *self,
                                                          const char *expr,
                                                          lql_selector **out,
                                                          lql_error *error) {
  return lql_parse_selector_internal(lql_allocator_from_receiver(self), expr, 1,
                                     out, error);
}

static void selector_destroy_method(lql *self,
                                                   lql_selector *selector) {
  if (selector != NULL) {
    lql_allocator *allocator;
    allocator = selector->allocator != NULL ? selector->allocator
                                            : lql_allocator_from_receiver(self);
    lql_node_cleanup(allocator, &selector->root);
    allocator->destroy(allocator, selector);
  }
}

static int
selector_is_empty_method(const lql *self, const lql_selector *selector) {
  (void)self;
  return selector == NULL || selector->root.kind == LQL_NODE_ALL;
}

static int selector_segment_is(const char *start, size_t len,
                               const char *literal) {
  return strlen(literal) == len && memcmp(start, literal, len) == 0;
}

static void selector_path_capabilities_visit(lql_selector_capabilities *out,
                                             const char *path) {
  const char *segment;
  const char *slash;
  size_t len;

  if (out == NULL || path == NULL || path[0] != '/') {
    return;
  }
  segment = path + 1;
  while (*segment != '\0') {
    slash = strchr(segment, '/');
    len = slash == NULL ? strlen(segment) : (size_t)(slash - segment);
    if (selector_segment_is(segment, len, "*") ||
        selector_segment_is(segment, len, "[]") ||
        selector_segment_is(segment, len, "**") ||
        selector_segment_is(segment, len, "...")) {
      out->wildcard_path = 1;
    }
    if (selector_segment_is(segment, len, "**") ||
        selector_segment_is(segment, len, "...")) {
      out->recursive_path = 1;
    }
    if (slash == NULL) {
      break;
    }
    segment = slash + 1;
  }
}

static void selector_capabilities_visit(lql_selector_capabilities *out,
                                        const lql_node *node) {
  size_t i;

  if (out == NULL || node == NULL) {
    return;
  }
  selector_path_capabilities_visit(out, node->term.field);
  switch (node->kind) {
  case LQL_NODE_ALL:
    break;
  case LQL_NODE_AND:
    out->and_ = 1;
    break;
  case LQL_NODE_OR:
    out->or_ = 1;
    break;
  case LQL_NODE_NOT:
    out->not_ = 1;
    break;
  case LQL_NODE_EQ:
  case LQL_NODE_NE:
    out->eq = 1;
    break;
  case LQL_NODE_CONTAINS:
  case LQL_NODE_ICONTAINS:
    out->contains = 1;
    break;
  case LQL_NODE_PREFIX:
  case LQL_NODE_IPREFIX:
    out->prefix = 1;
    break;
  case LQL_NODE_RANGE:
    out->range = 1;
    break;
  case LQL_NODE_DATE:
    out->date = 1;
    break;
  case LQL_NODE_IN:
    out->in = 1;
    break;
  case LQL_NODE_EXISTS:
    out->exists = 1;
    break;
  }
  for (i = 0u; i < node->child_count; ++i) {
    selector_capabilities_visit(out, &node->children[i]);
  }
}

static void selector_capabilities_get_method(
    const lql *self, const lql_selector *selector,
    lql_selector_capabilities *out) {
  (void)self;
  if (out == NULL) {
    return;
  }
  memset(out, 0, sizeof(*out));
  if (selector == NULL || selector->root.kind == LQL_NODE_ALL) {
    return;
  }
  selector_capabilities_visit(out, &selector->root);
}

static void selector_execution_traits_get_method(
    const lql *self, const lql_selector *selector,
    lql_selector_execution_traits *out) {
  lql_selector_capabilities capabilities;

  if (out == NULL) {
    return;
  }
  memset(out, 0, sizeof(*out));
  selector_capabilities_get_method(self, selector, &capabilities);
  out->uses_contains_like = capabilities.contains || capabilities.prefix;
  out->uses_recursive_path = capabilities.recursive_path;
  out->uses_wildcard_path = capabilities.wildcard_path;
  out->requires_object_root = !selector_is_empty_method(self, selector);
  out->early_non_match_likely =
      out->requires_object_root && !out->uses_contains_like &&
      !out->uses_recursive_path;
}

static lql_status
matches_json_method(lql *self, const lql_selector *selector, const char *json,
                      size_t json_len, int *out_matched, lql_error *error) {
  if (out_matched == NULL || json == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "json and out_matched are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (selector == NULL || selector->root.kind == LQL_NODE_ALL) {
    *out_matched = 1;
    return LQL_STATUS_OK;
  }
  return lql_eval_selector(self, selector, json, json_len, out_matched, error);
}
