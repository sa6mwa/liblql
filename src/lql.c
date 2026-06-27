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
static lql_status selector_parse_json_method(lql *self, const void *json,
                                             size_t json_len,
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
static lql_status selector_root_method(const lql *self,
                                       const lql_selector *selector,
                                       lql_selector_node *out,
                                       lql_error *error);
static lql_status selector_node_child_count_method(const lql *self,
                                                   lql_selector_node node,
                                                   size_t *out_count,
                                                   lql_error *error);
static lql_status selector_node_child_method(const lql *self,
                                             lql_selector_node node,
                                             size_t index,
                                             lql_selector_node *out,
                                             lql_error *error);
static lql_status selector_node_string_term_method(
    const lql *self, lql_selector_node node, lql_selector_string_term *out,
    lql_error *error);
static lql_status selector_node_string_term_any_method(
    const lql *self, lql_selector_node node, size_t index,
    lql_string_view *out, lql_error *error);
static lql_status selector_node_range_term_method(
    const lql *self, lql_selector_node node, lql_selector_range_term *out,
    lql_error *error);
static lql_status selector_node_date_term_method(const lql *self,
                                                 lql_selector_node node,
                                                 lql_selector_date_term *out,
                                                 lql_error *error);
static lql_status selector_node_in_term_method(const lql *self,
                                               lql_selector_node node,
                                               lql_selector_in_term *out,
                                               lql_error *error);
static lql_status selector_node_in_term_any_method(const lql *self,
                                                   lql_selector_node node,
                                                   size_t index,
                                                   lql_string_view *out,
                                                   lql_error *error);
static lql_status selector_node_exists_path_method(const lql *self,
                                                   lql_selector_node node,
                                                   lql_string_view *out,
                                                   lql_error *error);
static lql_status selector_write_json_method(lql *self,
                                             const lql_selector *selector,
                                             FILE *out, lql_error *error);
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
  ctx->selector_parse_json = selector_parse_json_method;
  ctx->selector_destroy = selector_destroy_method;
  ctx->selector_is_empty = selector_is_empty_method;
  ctx->selector_capabilities_get = selector_capabilities_get_method;
  ctx->selector_execution_traits_get = selector_execution_traits_get_method;
  ctx->selector_root = selector_root_method;
  ctx->selector_node_child_count = selector_node_child_count_method;
  ctx->selector_node_child = selector_node_child_method;
  ctx->selector_node_string_term = selector_node_string_term_method;
  ctx->selector_node_string_term_any = selector_node_string_term_any_method;
  ctx->selector_node_range_term = selector_node_range_term_method;
  ctx->selector_node_date_term = selector_node_date_term_method;
  ctx->selector_node_in_term = selector_node_in_term_method;
  ctx->selector_node_in_term_any = selector_node_in_term_any_method;
  ctx->selector_node_exists_path = selector_node_exists_path_method;
  ctx->selector_write_json = selector_write_json_method;
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

LQL_INTERNAL_SYMBOL void lql_node_cleanup(lql *self, lql_node *node) {
  lql_allocator *allocator;
  size_t i;
  if (node == NULL) {
    return;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    return;
  }
  allocator->destroy(allocator, node->term.field);
  allocator->destroy(allocator, node->term.value);
  for (i = 0u; i < node->term.any_count; ++i) {
    allocator->destroy(allocator, node->term.any[i]);
  }
  allocator->destroy(allocator, node->term.any);
  allocator->destroy(allocator, node->term.any_lens);
  allocator->destroy(allocator, node->term.range_gt_text);
  allocator->destroy(allocator, node->term.range_gte_text);
  allocator->destroy(allocator, node->term.range_lt_text);
  allocator->destroy(allocator, node->term.range_lte_text);
  allocator->destroy(allocator, node->term.date_value_text);
  allocator->destroy(allocator, node->term.date_since_text);
  allocator->destroy(allocator, node->term.date_after_text);
  allocator->destroy(allocator, node->term.date_before_text);
  allocator->destroy(allocator, node->term.date_gt_text);
  allocator->destroy(allocator, node->term.date_gte_text);
  allocator->destroy(allocator, node->term.date_lt_text);
  allocator->destroy(allocator, node->term.date_lte_text);
  for (i = 0u; i < node->child_count; ++i) {
    lql_node_cleanup(self, &node->children[i]);
  }
  allocator->destroy(allocator, node->children);
  memset(node, 0, sizeof(*node));
}

static lql_status selector_parse_method(lql *self,
                                                       const char *expr,
                                                       lql_selector **out,
                                                       lql_error *error) {
  return lql_parse_selector_internal(self, expr, 0, out, error);
}

static lql_status selector_parse_or_method(lql *self,
                                                          const char *expr,
                                                          lql_selector **out,
                                                          lql_error *error) {
  return lql_parse_selector_internal(self, expr, 1, out, error);
}

static lql_status selector_parse_json_method(lql *self, const void *json,
                                             size_t json_len,
                                             lql_selector **out,
                                             lql_error *error) {
  return lql_parse_selector_json_internal(self, json, json_len, out, error);
}

static void selector_destroy_method(lql *self,
                                                   lql_selector *selector) {
  if (selector != NULL) {
    lql_allocator *allocator;
    lql_node_cleanup(self, &selector->root);
    allocator = lql_allocator_from_receiver(self);
    if (allocator == NULL) {
      return;
    }
    allocator->destroy(allocator, selector);
  }
}

static int
selector_is_empty_method(const lql *self, const lql_selector *selector) {
  (void)self;
  return selector == NULL || selector->root.kind == LQL_NODE_ALL;
}

static lql_selector_node_kind public_node_kind(lql_node_kind kind) {
  switch (kind) {
  case LQL_NODE_ALL:
    return LQL_SELECTOR_NODE_ALL;
  case LQL_NODE_AND:
    return LQL_SELECTOR_NODE_AND;
  case LQL_NODE_OR:
    return LQL_SELECTOR_NODE_OR;
  case LQL_NODE_NOT:
    return LQL_SELECTOR_NODE_NOT;
  case LQL_NODE_EQ:
  case LQL_NODE_NE:
    return LQL_SELECTOR_NODE_EQ;
  case LQL_NODE_CONTAINS:
    return LQL_SELECTOR_NODE_CONTAINS;
  case LQL_NODE_ICONTAINS:
    return LQL_SELECTOR_NODE_ICONTAINS;
  case LQL_NODE_PREFIX:
    return LQL_SELECTOR_NODE_PREFIX;
  case LQL_NODE_IPREFIX:
    return LQL_SELECTOR_NODE_IPREFIX;
  case LQL_NODE_RANGE:
    return LQL_SELECTOR_NODE_RANGE;
  case LQL_NODE_DATE:
    return LQL_SELECTOR_NODE_DATE;
  case LQL_NODE_IN:
    return LQL_SELECTOR_NODE_IN;
  case LQL_NODE_EXISTS:
    return LQL_SELECTOR_NODE_EXISTS;
  }
  return LQL_SELECTOR_NODE_ALL;
}

static lql_selector_node public_node_from_internal(const lql_node *node) {
  lql_selector_node out;
  if (node == NULL) {
    out.kind = LQL_SELECTOR_NODE_ALL;
    out.impl = NULL;
    return out;
  }
  out.kind = public_node_kind(node->kind);
  out.impl = node;
  return out;
}

static const lql_node *internal_node_from_public(lql_selector_node node) {
  return (const lql_node *)node.impl;
}

static lql_string_view lql_view_cstr(const char *text) {
  lql_string_view out;
  out.data = text;
  out.len = text == NULL ? 0u : strlen(text);
  return out;
}

static void lql_string_view_clear(lql_string_view *view) {
  if (view != NULL) {
    view->data = NULL;
    view->len = 0u;
  }
}

static int public_node_is_string_term(lql_selector_node_kind kind) {
  return kind == LQL_SELECTOR_NODE_EQ || kind == LQL_SELECTOR_NODE_CONTAINS ||
         kind == LQL_SELECTOR_NODE_ICONTAINS ||
         kind == LQL_SELECTOR_NODE_PREFIX ||
         kind == LQL_SELECTOR_NODE_IPREFIX;
}

static lql_status selector_root_method(const lql *self,
                                       const lql_selector *selector,
                                       lql_selector_node *out,
                                       lql_error *error) {
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector node required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = selector == NULL ? public_node_from_internal(NULL)
                          : public_node_from_internal(&selector->root);
  return LQL_STATUS_OK;
}

static lql_status selector_node_child_count_method(const lql *self,
                                                   lql_selector_node node,
                                                   size_t *out_count,
                                                   lql_error *error) {
  const lql_node *internal;
  (void)self;
  if (out_count == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out child count required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out_count = 0u;
  internal = internal_node_from_public(node);
  if (internal == NULL) {
    return LQL_STATUS_OK;
  }
  if (node.kind != LQL_SELECTOR_NODE_AND && node.kind != LQL_SELECTOR_NODE_OR &&
      node.kind != LQL_SELECTOR_NODE_NOT) {
    return LQL_STATUS_OK;
  }
  *out_count = internal->child_count;
  return LQL_STATUS_OK;
}

static lql_status selector_node_child_method(const lql *self,
                                             lql_selector_node node,
                                             size_t index,
                                             lql_selector_node *out,
                                             lql_error *error) {
  const lql_node *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector node required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = public_node_from_internal(NULL);
  internal = internal_node_from_public(node);
  if (internal == NULL || (node.kind != LQL_SELECTOR_NODE_AND &&
                           node.kind != LQL_SELECTOR_NODE_OR &&
                           node.kind != LQL_SELECTOR_NODE_NOT)) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node has no children");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (index >= internal->child_count) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector child index out of range");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = public_node_from_internal(&internal->children[index]);
  return LQL_STATUS_OK;
}

static lql_status selector_node_string_term_method(
    const lql *self, lql_selector_node node, lql_selector_string_term *out,
    lql_error *error) {
  const lql_node *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector string term required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  internal = internal_node_from_public(node);
  if (internal == NULL || !public_node_is_string_term(node.kind)) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not a string term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out->field = lql_view_cstr(internal->term.field);
  out->value_present = internal->term.value_set ||
                       (internal->term.value != NULL &&
                        internal->term.value[0] != '\0');
  out->value = lql_view_cstr(internal->term.value);
  out->ignore_case = internal->term.ignore_case;
  out->any_count = internal->term.any_count;
  return LQL_STATUS_OK;
}

static lql_status selector_node_string_term_any_method(
    const lql *self, lql_selector_node node, size_t index,
    lql_string_view *out, lql_error *error) {
  const lql_node *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector any value required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_string_view_clear(out);
  internal = internal_node_from_public(node);
  if (internal == NULL || !public_node_is_string_term(node.kind)) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not a string term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (index >= internal->term.any_count) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector any index out of range");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out->data = internal->term.any[index];
  out->len = internal->term.any_lens[index];
  return LQL_STATUS_OK;
}

static lql_selector_range_bound range_number_bound(double number) {
  lql_selector_range_bound out;
  memset(&out, 0, sizeof(out));
  out.kind = LQL_SELECTOR_BOUND_NUMBER;
  out.number = number;
  return out;
}

static lql_selector_range_bound range_datetime_bound(const char *text) {
  lql_selector_range_bound out;
  memset(&out, 0, sizeof(out));
  out.kind = LQL_SELECTOR_BOUND_DATETIME;
  out.datetime = lql_view_cstr(text);
  return out;
}

static lql_status selector_node_range_term_method(
    const lql *self, lql_selector_node node, lql_selector_range_term *out,
    lql_error *error) {
  const lql_node *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector range term required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  internal = internal_node_from_public(node);
  if (internal == NULL || node.kind != LQL_SELECTOR_NODE_RANGE) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not a range term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out->field = lql_view_cstr(internal->term.field);
  if (internal->term.has_temporal_gt) {
    out->gt = range_datetime_bound(internal->term.range_gt_text);
  } else if (internal->term.has_range_gt) {
    out->gt = range_number_bound(internal->term.range_gt);
  }
  if (internal->term.has_temporal_gte) {
    out->gte = range_datetime_bound(internal->term.range_gte_text);
  } else if (internal->term.has_range_gte) {
    out->gte = range_number_bound(internal->term.range_gte);
  }
  if (internal->term.has_temporal_lt) {
    out->lt = range_datetime_bound(internal->term.range_lt_text);
  } else if (internal->term.has_range_lt) {
    out->lt = range_number_bound(internal->term.range_lt);
  }
  if (internal->term.has_temporal_lte) {
    out->lte = range_datetime_bound(internal->term.range_lte_text);
  } else if (internal->term.has_range_lte) {
    out->lte = range_number_bound(internal->term.range_lte);
  }
  return LQL_STATUS_OK;
}

static lql_selector_since_kind public_since_kind(const lql_term *term) {
  if (term == NULL || term->date_since_text == NULL) {
    return LQL_SELECTOR_SINCE_NONE;
  }
  switch (term->since_macro) {
  case LQL_SINCE_NOW:
    return LQL_SELECTOR_SINCE_NOW;
  case LQL_SINCE_TODAY:
    return LQL_SELECTOR_SINCE_TODAY;
  case LQL_SINCE_YESTERDAY:
    return LQL_SELECTOR_SINCE_YESTERDAY;
  case LQL_SINCE_NONE:
    return LQL_SELECTOR_SINCE_LITERAL;
  }
  return LQL_SELECTOR_SINCE_LITERAL;
}

static lql_status selector_node_date_term_method(const lql *self,
                                                 lql_selector_node node,
                                                 lql_selector_date_term *out,
                                                 lql_error *error) {
  const lql_node *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector date term required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  internal = internal_node_from_public(node);
  if (internal == NULL || node.kind != LQL_SELECTOR_NODE_DATE) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not a date term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out->field = lql_view_cstr(internal->term.field);
  out->value = lql_view_cstr(internal->term.date_value_text);
  out->since = lql_view_cstr(internal->term.date_since_text);
  out->since_kind = public_since_kind(&internal->term);
  out->after = lql_view_cstr(internal->term.date_after_text);
  out->before = lql_view_cstr(internal->term.date_before_text);
  out->gt = lql_view_cstr(internal->term.date_gt_text);
  out->gte = lql_view_cstr(internal->term.date_gte_text);
  out->lt = lql_view_cstr(internal->term.date_lt_text);
  out->lte = lql_view_cstr(internal->term.date_lte_text);
  return LQL_STATUS_OK;
}

static lql_status selector_node_in_term_method(const lql *self,
                                               lql_selector_node node,
                                               lql_selector_in_term *out,
                                               lql_error *error) {
  const lql_node *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector in term required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  internal = internal_node_from_public(node);
  if (internal == NULL || node.kind != LQL_SELECTOR_NODE_IN) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not an in term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out->field = lql_view_cstr(internal->term.field);
  out->any_count = internal->term.any_count;
  return LQL_STATUS_OK;
}

static lql_status selector_node_in_term_any_method(const lql *self,
                                                   lql_selector_node node,
                                                   size_t index,
                                                   lql_string_view *out,
                                                   lql_error *error) {
  const lql_node *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector any value required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_string_view_clear(out);
  internal = internal_node_from_public(node);
  if (internal == NULL || node.kind != LQL_SELECTOR_NODE_IN) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not an in term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (index >= internal->term.any_count) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector any index out of range");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out->data = internal->term.any[index];
  out->len = internal->term.any_lens[index];
  return LQL_STATUS_OK;
}

static lql_status selector_node_exists_path_method(const lql *self,
                                                   lql_selector_node node,
                                                   lql_string_view *out,
                                                   lql_error *error) {
  const lql_node *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector exists path required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_string_view_clear(out);
  internal = internal_node_from_public(node);
  if (internal == NULL || node.kind != LQL_SELECTOR_NODE_EXISTS) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not an exists term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = lql_view_cstr(internal->term.field);
  return LQL_STATUS_OK;
}

static lonejson_status selector_file_sink(void *user, const void *data,
                                          size_t len, lonejson_error *error) {
  FILE *out;
  out = (FILE *)user;
  if (len == 0u) {
    return LONEJSON_STATUS_OK;
  }
  if (out == NULL || fwrite(data, 1u, len, out) != len) {
    if (error != NULL) {
      error->code = LONEJSON_STATUS_IO_ERROR;
      error->message[0] = '\0';
    }
    return LONEJSON_STATUS_IO_ERROR;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status writer_key(lonejson_writer *writer, const char *key,
                                  lonejson_error *error) {
  return lonejson_writer_key(writer, key, strlen(key), error);
}

static lonejson_status writer_string_view(lonejson_writer *writer,
                                          lql_string_view value,
                                          lonejson_error *error) {
  return lonejson_writer_string(writer, value.data == NULL ? "" : value.data,
                                value.len, error);
}

static lonejson_status write_selector_node_json(lonejson_writer *writer,
                                                const lql_node *node,
                                                lonejson_error *error);

static lonejson_status write_string_term_json(lonejson_writer *writer,
                                              const lql_node *node,
                                              lonejson_error *error) {
  size_t i;
  if (lonejson_writer_begin_object(writer, error) != LONEJSON_STATUS_OK ||
      writer_key(writer, "field", error) != LONEJSON_STATUS_OK ||
      lonejson_writer_string(writer, node->term.field, strlen(node->term.field),
                             error) != LONEJSON_STATUS_OK) {
    return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
  }
  if (node->term.value_set ||
      (node->term.value != NULL && node->term.value[0] != '\0')) {
    if (writer_key(writer, "value", error) != LONEJSON_STATUS_OK ||
        lonejson_writer_string(writer,
                               node->term.value == NULL ? "" : node->term.value,
                               node->term.value == NULL
                                   ? 0u
                                   : strlen(node->term.value),
                               error) != LONEJSON_STATUS_OK) {
      return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
    }
  }
  if (node->term.any_count != 0u) {
    if (writer_key(writer, "any", error) != LONEJSON_STATUS_OK ||
        lonejson_writer_begin_array(writer, error) != LONEJSON_STATUS_OK) {
      return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
    }
    for (i = 0u; i < node->term.any_count; ++i) {
      if (lonejson_writer_string(writer, node->term.any[i],
                                 node->term.any_lens[i],
                                 error) != LONEJSON_STATUS_OK) {
        return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
      }
    }
    if (lonejson_writer_end_array(writer, error) != LONEJSON_STATUS_OK) {
      return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
    }
  }
  if (node->term.ignore_case) {
    if (writer_key(writer, "ignoreCase", error) != LONEJSON_STATUS_OK ||
        lonejson_writer_bool(writer, 1, error) != LONEJSON_STATUS_OK) {
      return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
    }
  }
  return lonejson_writer_end_object(writer, error);
}

static lonejson_status write_range_bound_json(lonejson_writer *writer,
                                              const lql_selector_range_bound *b,
                                              lonejson_error *error) {
  if (b->kind == LQL_SELECTOR_BOUND_NUMBER) {
    return lonejson_writer_f64(writer, b->number, error);
  }
  if (b->kind == LQL_SELECTOR_BOUND_DATETIME) {
    return writer_string_view(writer, b->datetime, error);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status write_range_bound_member(lonejson_writer *writer,
                                                const char *key,
                                                lql_selector_range_bound bound,
                                                lonejson_error *error) {
  if (bound.kind == LQL_SELECTOR_BOUND_ABSENT) {
    return LONEJSON_STATUS_OK;
  }
  if (writer_key(writer, key, error) != LONEJSON_STATUS_OK) {
    return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
  }
  return write_range_bound_json(writer, &bound, error);
}

static lonejson_status write_range_term_json(lonejson_writer *writer,
                                             const lql_node *node,
                                             lonejson_error *error) {
  lql_selector_range_term term;
  lql_selector_node public_node;
  public_node = public_node_from_internal(node);
  memset(&term, 0, sizeof(term));
  if (selector_node_range_term_method(NULL, public_node, &term, NULL) !=
      LQL_STATUS_OK) {
    return LONEJSON_STATUS_INTERNAL_ERROR;
  }
  if (lonejson_writer_begin_object(writer, error) != LONEJSON_STATUS_OK ||
      writer_key(writer, "field", error) != LONEJSON_STATUS_OK ||
      writer_string_view(writer, term.field, error) != LONEJSON_STATUS_OK ||
      write_range_bound_member(writer, "gt", term.gt, error) !=
          LONEJSON_STATUS_OK ||
      write_range_bound_member(writer, "gte", term.gte, error) !=
          LONEJSON_STATUS_OK ||
      write_range_bound_member(writer, "lt", term.lt, error) !=
          LONEJSON_STATUS_OK ||
      write_range_bound_member(writer, "lte", term.lte, error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_end_object(writer, error) != LONEJSON_STATUS_OK) {
    return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status write_optional_string_member(lonejson_writer *writer,
                                                    const char *key,
                                                    lql_string_view value,
                                                    lonejson_error *error) {
  if (value.data == NULL) {
    return LONEJSON_STATUS_OK;
  }
  if (writer_key(writer, key, error) != LONEJSON_STATUS_OK ||
      writer_string_view(writer, value, error) != LONEJSON_STATUS_OK) {
    return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status write_date_term_json(lonejson_writer *writer,
                                            const lql_node *node,
                                            lonejson_error *error) {
  lql_selector_date_term term;
  lql_selector_node public_node;
  public_node = public_node_from_internal(node);
  memset(&term, 0, sizeof(term));
  if (selector_node_date_term_method(NULL, public_node, &term, NULL) !=
      LQL_STATUS_OK) {
    return LONEJSON_STATUS_INTERNAL_ERROR;
  }
  if (lonejson_writer_begin_object(writer, error) != LONEJSON_STATUS_OK ||
      writer_key(writer, "field", error) != LONEJSON_STATUS_OK ||
      writer_string_view(writer, term.field, error) != LONEJSON_STATUS_OK ||
      write_optional_string_member(writer, "value", term.value, error) !=
          LONEJSON_STATUS_OK ||
      write_optional_string_member(writer, "since", term.since, error) !=
          LONEJSON_STATUS_OK ||
      write_optional_string_member(writer, "after", term.after, error) !=
          LONEJSON_STATUS_OK ||
      write_optional_string_member(writer, "before", term.before, error) !=
          LONEJSON_STATUS_OK ||
      write_optional_string_member(writer, "gte", term.gte, error) !=
          LONEJSON_STATUS_OK ||
      write_optional_string_member(writer, "gt", term.gt, error) !=
          LONEJSON_STATUS_OK ||
      write_optional_string_member(writer, "lte", term.lte, error) !=
          LONEJSON_STATUS_OK ||
      write_optional_string_member(writer, "lt", term.lt, error) !=
          LONEJSON_STATUS_OK ||
      lonejson_writer_end_object(writer, error) != LONEJSON_STATUS_OK) {
    return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status write_in_term_json(lonejson_writer *writer,
                                          const lql_node *node,
                                          lonejson_error *error) {
  size_t i;
  if (lonejson_writer_begin_object(writer, error) != LONEJSON_STATUS_OK ||
      writer_key(writer, "field", error) != LONEJSON_STATUS_OK ||
      lonejson_writer_string(writer, node->term.field, strlen(node->term.field),
                             error) != LONEJSON_STATUS_OK ||
      writer_key(writer, "any", error) != LONEJSON_STATUS_OK ||
      lonejson_writer_begin_array(writer, error) != LONEJSON_STATUS_OK) {
    return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
  }
  for (i = 0u; i < node->term.any_count; ++i) {
    if (lonejson_writer_string(writer, node->term.any[i], node->term.any_lens[i],
                               error) != LONEJSON_STATUS_OK) {
      return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
    }
  }
  if (lonejson_writer_end_array(writer, error) != LONEJSON_STATUS_OK ||
      lonejson_writer_end_object(writer, error) != LONEJSON_STATUS_OK) {
    return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status write_selector_children_json(lonejson_writer *writer,
                                                    const lql_node *node,
                                                    lonejson_error *error) {
  size_t i;
  if (lonejson_writer_begin_array(writer, error) != LONEJSON_STATUS_OK) {
    return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
  }
  for (i = 0u; i < node->child_count; ++i) {
    if (write_selector_node_json(writer, &node->children[i], error) !=
        LONEJSON_STATUS_OK) {
      return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
    }
  }
  return lonejson_writer_end_array(writer, error);
}

static lonejson_status write_selector_node_json(lonejson_writer *writer,
                                                const lql_node *node,
                                                lonejson_error *error) {
  const char *key;
  if (lonejson_writer_begin_object(writer, error) != LONEJSON_STATUS_OK) {
    return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
  }
  if (node != NULL) {
    key = NULL;
    switch (node->kind) {
    case LQL_NODE_ALL:
      break;
    case LQL_NODE_AND:
      key = "and";
      break;
    case LQL_NODE_OR:
      key = "or";
      break;
    case LQL_NODE_NOT:
      key = "not";
      break;
    case LQL_NODE_EQ:
    case LQL_NODE_NE:
      key = "eq";
      break;
    case LQL_NODE_CONTAINS:
      key = "contains";
      break;
    case LQL_NODE_ICONTAINS:
      key = "icontains";
      break;
    case LQL_NODE_PREFIX:
      key = "prefix";
      break;
    case LQL_NODE_IPREFIX:
      key = "iprefix";
      break;
    case LQL_NODE_RANGE:
      key = "range";
      break;
    case LQL_NODE_DATE:
      key = "date";
      break;
    case LQL_NODE_IN:
      key = "in";
      break;
    case LQL_NODE_EXISTS:
      key = "exists";
      break;
    }
    if (key != NULL) {
      if (writer_key(writer, key, error) != LONEJSON_STATUS_OK) {
        return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
      }
      switch (node->kind) {
      case LQL_NODE_AND:
      case LQL_NODE_OR:
        if (write_selector_children_json(writer, node, error) !=
            LONEJSON_STATUS_OK) {
          return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
        }
        break;
      case LQL_NODE_NOT:
        if (node->child_count == 0u) {
          if (write_selector_node_json(writer, NULL, error) !=
              LONEJSON_STATUS_OK) {
            return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
          }
        } else if (write_selector_node_json(writer, &node->children[0],
                                           error) != LONEJSON_STATUS_OK) {
          return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
        }
        break;
      case LQL_NODE_EQ:
      case LQL_NODE_NE:
      case LQL_NODE_CONTAINS:
      case LQL_NODE_ICONTAINS:
      case LQL_NODE_PREFIX:
      case LQL_NODE_IPREFIX:
        if (write_string_term_json(writer, node, error) != LONEJSON_STATUS_OK) {
          return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
        }
        break;
      case LQL_NODE_RANGE:
        if (write_range_term_json(writer, node, error) != LONEJSON_STATUS_OK) {
          return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
        }
        break;
      case LQL_NODE_DATE:
        if (write_date_term_json(writer, node, error) != LONEJSON_STATUS_OK) {
          return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
        }
        break;
      case LQL_NODE_IN:
        if (write_in_term_json(writer, node, error) != LONEJSON_STATUS_OK) {
          return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
        }
        break;
      case LQL_NODE_EXISTS:
        if (lonejson_writer_string(writer, node->term.field,
                                   strlen(node->term.field),
                                   error) != LONEJSON_STATUS_OK) {
          return error == NULL ? LONEJSON_STATUS_INTERNAL_ERROR : error->code;
        }
        break;
      case LQL_NODE_ALL:
        break;
      }
    }
  }
  return lonejson_writer_end_object(writer, error);
}

static lql_status selector_write_json_method(lql *self,
                                             const lql_selector *selector,
                                             FILE *out, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_writer writer;
  lonejson_status st;
  int writer_ready;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "output file required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, lj_error.message);
    return LQL_STATUS_NO_MEMORY;
  }
  writer_ready = 0;
  st = lonejson_writer_init_sink(runtime, &writer, selector_file_sink, out,
                                 &lj_error);
  if (st == LONEJSON_STATUS_OK) {
    writer_ready = 1;
    st = write_selector_node_json(
        &writer, selector == NULL ? NULL : &selector->root, &lj_error);
  }
  if (st == LONEJSON_STATUS_OK) {
    st = lonejson_writer_finish(&writer, &lj_error);
  }
  if (writer_ready) {
    lonejson_writer_cleanup(&writer);
  }
  lonejson_free(runtime);
  if (st != LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  lj_error.message[0] == '\0' ? "selector JSON write failed"
                                               : lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  return LQL_STATUS_OK;
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
