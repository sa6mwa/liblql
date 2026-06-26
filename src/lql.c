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
  ctx->selector_parse = lql_selector_parse_impl;
  ctx->selector_parse_or = lql_selector_parse_or_impl;
  ctx->selector_destroy = lql_selector_destroy_impl;
  ctx->selector_is_empty = lql_selector_is_empty_impl;
  ctx->matches_json = lql_matches_json_impl;
  ctx->query_file_decisions = lql_query_file_decisions_impl;
  ctx->query_file_decisions_with_options =
      lql_query_file_decisions_with_options_impl;
  ctx->query_source_decisions = lql_query_source_decisions_impl;
  ctx->query_source_decisions_with_options =
      lql_query_source_decisions_with_options_impl;
  ctx->query_file_matches = lql_query_file_matches_impl;
  ctx->query_file_matches_with_options =
      lql_query_file_matches_with_options_impl;
  ctx->query_source_spooled_matches = lql_query_source_spooled_matches_impl;
  ctx->query_source_spooled_matches_with_options =
      lql_query_source_spooled_matches_with_options_impl;
  ctx->payload_write_json = lql_payload_write_json_impl;
  ctx->payload_write_json_sink = lql_payload_write_json_sink_impl;
  ctx->payload_project_json = lql_payload_project_json_impl;
  ctx->projection_parse = lql_projection_parse_impl;
  ctx->projection_destroy = lql_projection_destroy_impl;
  ctx->project_file_range = lql_project_file_range_impl;
  ctx->project_source = lql_project_source_impl;
  ctx->project_json = lql_project_json_impl;
  ctx->compact_file_range = lql_compact_file_range_impl;
  ctx->compact_source = lql_compact_source_impl;
  ctx->compact_json = lql_compact_json_impl;
  ctx->mutation_plan_parse = lql_mutation_plan_parse_impl;
  ctx->mutation_plan_parse_with_options =
      lql_mutation_plan_parse_with_options_impl;
  ctx->mutation_plan_count = lql_mutation_plan_count_impl;
  ctx->mutation_plan_destroy = lql_mutation_plan_destroy_impl;
  ctx->mutate_file_range_root_fields = lql_mutate_file_range_root_fields_impl;
  ctx->mutate_file_range_paths = lql_mutate_file_range_paths_impl;
  ctx->mutate_file_range_candidates = lql_mutate_file_range_candidates_impl;
  ctx->mutate_file_range_projected_candidates =
      lql_mutate_file_range_projected_candidates_impl;
  ctx->mutate_source_paths = lql_mutate_source_paths_impl;
  ctx->mutate_source_candidates = lql_mutate_source_candidates_impl;
  ctx->mutate_source_projected_candidates =
      lql_mutate_source_projected_candidates_impl;
  ctx->mutate_json = lql_mutate_json_impl;
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

const char *lql_version(void) { return LQL_VERSION; }

void lql_capabilities_get(lql_capabilities *out) {
  if (out == NULL) {
    return;
  }
  memset(out, 0, sizeof(*out));
  out->selector_parse = 1;
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
  return lql_version();
}

static void receiver_capabilities_get(const lql *self, lql_capabilities *out) {
  (void)self;
  lql_capabilities_get(out);
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

LQL_INTERNAL_SYMBOL lql_status lql_selector_parse_impl(lql *self,
                                                       const char *expr,
                                                       lql_selector **out,
                                                       lql_error *error) {
  return lql_parse_selector_internal(lql_allocator_from_receiver(self), expr, 0,
                                     out, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_selector_parse_or_impl(lql *self,
                                                          const char *expr,
                                                          lql_selector **out,
                                                          lql_error *error) {
  return lql_parse_selector_internal(lql_allocator_from_receiver(self), expr, 1,
                                     out, error);
}

LQL_INTERNAL_SYMBOL void lql_selector_destroy_impl(lql *self,
                                                   lql_selector *selector) {
  if (selector != NULL) {
    lql_allocator *allocator;
    allocator = selector->allocator != NULL ? selector->allocator
                                            : lql_allocator_from_receiver(self);
    lql_node_cleanup(allocator, &selector->root);
    allocator->destroy(allocator, selector);
  }
}

LQL_INTERNAL_SYMBOL int
lql_selector_is_empty_impl(const lql *self, const lql_selector *selector) {
  (void)self;
  return selector == NULL || selector->root.kind == LQL_NODE_ALL;
}

LQL_INTERNAL_SYMBOL lql_status
lql_matches_json_impl(lql *self, const lql_selector *selector, const char *json,
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
