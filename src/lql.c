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
static lql_status projection_parse_method(lql *self, const char *const *paths,
                                          size_t path_count,
                                          lql_projection **out,
                                          lql_error *error);
static void projection_destroy_method(lql *self, lql_projection *projection);
static size_t projection_path_count_method(const lql *self,
                                           const lql_projection *projection);
static lql_status projection_path_method(const lql *self,
                                         const lql_projection *projection,
                                         size_t index, lql_string_view *out,
                                         lql_error *error);
static lql_status mutation_parse_method(lql *self,
                                        const char *const *expressions,
                                        size_t expression_count,
                                        lql_mutation **out, lql_error *error);
static void mutation_destroy_method(lql *self, lql_mutation *mutation);
static size_t mutation_count_method(const lql *self,
                                    const lql_mutation *mutation);
static int selector_is_empty_method(const lql *self,
                                    const lql_selector *selector);
static void selector_capabilities_get_method(const lql *self,
                                             const lql_selector *selector,
                                             lql_selector_capabilities *out);
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
static lql_status
selector_node_string_term_method(const lql *self, lql_selector_node node,
                                 lql_selector_string_term *out,
                                 lql_error *error);
static lql_status selector_node_string_term_any_method(const lql *self,
                                                       lql_selector_node node,
                                                       size_t index,
                                                       lql_string_view *out,
                                                       lql_error *error);
static lql_status selector_node_range_term_method(const lql *self,
                                                  lql_selector_node node,
                                                  lql_selector_range_term *out,
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
                                        const lql_selector *selector);
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
  ctx->projection_parse = projection_parse_method;
  ctx->projection_destroy = projection_destroy_method;
  ctx->projection_path_count = projection_path_count_method;
  ctx->projection_path = projection_path_method;
  ctx->mutation_parse = mutation_parse_method;
  ctx->mutation_destroy = mutation_destroy_method;
  ctx->mutation_count = mutation_count_method;
  ctx->stream_execute = lql_stream_execute;
  ctx->stream_execute_spooled = lql_stream_execute_spooled;
  ctx->selector_is_empty = selector_is_empty_method;
  ctx->selector_capabilities_get = selector_capabilities_get_method;
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
  ctx->selector_build_all = lql_selector_build_all_internal;
  ctx->selector_build_compound = lql_selector_build_compound_internal;
  ctx->selector_build_not = lql_selector_build_not_internal;
  ctx->selector_build_string = lql_selector_build_string_internal;
  ctx->selector_build_range = lql_selector_build_range_internal;
  ctx->selector_build_date = lql_selector_build_date_internal;
  ctx->selector_build_in = lql_selector_build_in_internal;
  ctx->selector_build_exists = lql_selector_build_exists_internal;
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
  case LQL_STATUS_CALLBACK_ERROR:
    return "callback error";
  case LQL_STATUS_IO_ERROR:
    return "I/O error";
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

LQL_INTERNAL_SYMBOL void lql_selector_cleanup(lql *self,
                                              lql_selector *selector) {
  lql_allocator *allocator;
  size_t i;
  if (selector == NULL) {
    return;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    return;
  }
  allocator->destroy(allocator, selector->field);
  allocator->destroy(allocator, selector->value);
  for (i = 0u; i < selector->any_count; ++i) {
    allocator->destroy(allocator, selector->any[i]);
  }
  allocator->destroy(allocator, selector->any);
  allocator->destroy(allocator, selector->any_lens);
  allocator->destroy(allocator, selector->any_kinds);
  allocator->destroy(allocator, selector->any_from_json);
  allocator->destroy(allocator, selector->range_gt_text);
  allocator->destroy(allocator, selector->range_gte_text);
  allocator->destroy(allocator, selector->range_lt_text);
  allocator->destroy(allocator, selector->range_lte_text);
  allocator->destroy(allocator, selector->date_value_text);
  allocator->destroy(allocator, selector->date_since_text);
  allocator->destroy(allocator, selector->date_after_text);
  allocator->destroy(allocator, selector->date_before_text);
  allocator->destroy(allocator, selector->date_gt_text);
  allocator->destroy(allocator, selector->date_gte_text);
  allocator->destroy(allocator, selector->date_lt_text);
  allocator->destroy(allocator, selector->date_lte_text);
  lql_stream_program_destroy(self, selector);
  for (i = 0u; i < selector->child_count; ++i) {
    lql_selector_cleanup(self, &selector->children[i]);
  }
  allocator->destroy(allocator, selector->children);
  memset(selector, 0, sizeof(*selector));
}

static lql_status selector_parse_method(lql *self, const char *expr,
                                        lql_selector **out, lql_error *error) {
  return lql_parse_selector_internal(self, expr, 0, out, error);
}

static lql_status selector_parse_or_method(lql *self, const char *expr,
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

static void selector_destroy_method(lql *self, lql_selector *selector) {
  if (selector != NULL) {
    lql_allocator *allocator;
    lql_selector_cleanup(self, selector);
    allocator = lql_allocator_from_receiver(self);
    if (allocator == NULL) {
      return;
    }
    allocator->destroy(allocator, selector);
  }
}

static lql_status projection_parse_method(lql *self, const char *const *paths,
                                          size_t path_count,
                                          lql_projection **out,
                                          lql_error *error) {
  return lql_projection_parse_internal(self, paths, path_count, out, error);
}

static void projection_destroy_method(lql *self, lql_projection *projection) {
  lql_projection_destroy_internal(self, projection);
}

static size_t projection_path_count_method(const lql *self,
                                           const lql_projection *projection) {
  (void)self;
  return projection == NULL ? 0u : projection->path_count;
}

static lql_status projection_path_method(const lql *self,
                                         const lql_projection *projection,
                                         size_t index, lql_string_view *out,
                                         lql_error *error) {
  (void)self;
  if (out == NULL || projection == NULL || index >= projection->path_count) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection path index is invalid");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out->data = projection->paths[index];
  out->len = strlen(projection->paths[index]);
  lql_error_init(error);
  return LQL_STATUS_OK;
}

static lql_status mutation_parse_method(lql *self,
                                        const char *const *expressions,
                                        size_t expression_count,
                                        lql_mutation **out, lql_error *error) {
  return lql_mutation_parse_internal(self, expressions, expression_count, out,
                                     error);
}

static void mutation_destroy_method(lql *self, lql_mutation *mutation) {
  lql_mutation_destroy_internal(self, mutation);
}

static size_t mutation_count_method(const lql *self,
                                    const lql_mutation *mutation) {
  (void)self;
  return mutation == NULL ? 0u : mutation->action_count;
}

static int selector_is_empty_method(const lql *self,
                                    const lql_selector *selector) {
  (void)self;
  return selector == NULL || selector->kind == LQL_SELECTOR_KIND_ALL;
}

static lql_selector_node_kind selector_cursor_kind(lql_selector_kind kind) {
  switch (kind) {
  case LQL_SELECTOR_KIND_ALL:
    return LQL_SELECTOR_NODE_ALL;
  case LQL_SELECTOR_KIND_AND:
    return LQL_SELECTOR_NODE_AND;
  case LQL_SELECTOR_KIND_OR:
    return LQL_SELECTOR_NODE_OR;
  case LQL_SELECTOR_KIND_NOT:
    return LQL_SELECTOR_NODE_NOT;
  case LQL_SELECTOR_KIND_EQ:
  case LQL_SELECTOR_KIND_NE:
    return LQL_SELECTOR_NODE_EQ;
  case LQL_SELECTOR_KIND_CONTAINS:
    return LQL_SELECTOR_NODE_CONTAINS;
  case LQL_SELECTOR_KIND_ICONTAINS:
    return LQL_SELECTOR_NODE_ICONTAINS;
  case LQL_SELECTOR_KIND_PREFIX:
    return LQL_SELECTOR_NODE_PREFIX;
  case LQL_SELECTOR_KIND_IPREFIX:
    return LQL_SELECTOR_NODE_IPREFIX;
  case LQL_SELECTOR_KIND_RANGE:
    return LQL_SELECTOR_NODE_RANGE;
  case LQL_SELECTOR_KIND_DATE:
    return LQL_SELECTOR_NODE_DATE;
  case LQL_SELECTOR_KIND_IN:
    return LQL_SELECTOR_NODE_IN;
  case LQL_SELECTOR_KIND_EXISTS:
    return LQL_SELECTOR_NODE_EXISTS;
  }
  return LQL_SELECTOR_NODE_ALL;
}

static lql_selector_node
selector_cursor_from_selector(const lql_selector *selector) {
  lql_selector_node out;
  if (selector == NULL) {
    out.kind = LQL_SELECTOR_NODE_ALL;
    out.impl = NULL;
    return out;
  }
  out.kind = selector_cursor_kind(selector->kind);
  out.impl = selector;
  return out;
}

static const lql_selector *selector_from_cursor(lql_selector_node node) {
  return (const lql_selector *)node.impl;
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

static int cursor_is_string_predicate(lql_selector_node_kind kind) {
  return kind == LQL_SELECTOR_NODE_EQ || kind == LQL_SELECTOR_NODE_CONTAINS ||
         kind == LQL_SELECTOR_NODE_ICONTAINS ||
         kind == LQL_SELECTOR_NODE_PREFIX || kind == LQL_SELECTOR_NODE_IPREFIX;
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
  *out = selector == NULL ? selector_cursor_from_selector(NULL)
                          : selector_cursor_from_selector(selector);
  return LQL_STATUS_OK;
}

static lql_status selector_node_child_count_method(const lql *self,
                                                   lql_selector_node node,
                                                   size_t *out_count,
                                                   lql_error *error) {
  const lql_selector *internal;
  (void)self;
  if (out_count == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out child count required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out_count = 0u;
  internal = selector_from_cursor(node);
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
  const lql_selector *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector node required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = selector_cursor_from_selector(NULL);
  internal = selector_from_cursor(node);
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
  *out = selector_cursor_from_selector(&internal->children[index]);
  return LQL_STATUS_OK;
}

static lql_status
selector_node_string_term_method(const lql *self, lql_selector_node node,
                                 lql_selector_string_term *out,
                                 lql_error *error) {
  const lql_selector *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector string term required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  internal = selector_from_cursor(node);
  if (internal == NULL || !cursor_is_string_predicate(node.kind)) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not a string term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out->field = lql_view_cstr(internal->field);
  out->value_present = internal->value_set ||
                       (internal->value != NULL && internal->value[0] != '\0');
  out->value = lql_view_cstr(internal->value);
  out->ignore_case = internal->ignore_case;
  out->any_count = internal->any_count;
  return LQL_STATUS_OK;
}

static lql_status selector_node_string_term_any_method(const lql *self,
                                                       lql_selector_node node,
                                                       size_t index,
                                                       lql_string_view *out,
                                                       lql_error *error) {
  const lql_selector *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector any value required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_string_view_clear(out);
  internal = selector_from_cursor(node);
  if (internal == NULL || !cursor_is_string_predicate(node.kind)) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not a string term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (index >= internal->any_count) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector any index out of range");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out->data = internal->any[index];
  out->len = internal->any_lens[index];
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

static lql_status selector_node_range_term_method(const lql *self,
                                                  lql_selector_node node,
                                                  lql_selector_range_term *out,
                                                  lql_error *error) {
  const lql_selector *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector range term required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  internal = selector_from_cursor(node);
  if (internal == NULL || node.kind != LQL_SELECTOR_NODE_RANGE) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not a range term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out->field = lql_view_cstr(internal->field);
  if (internal->has_temporal_gt) {
    out->gt = range_datetime_bound(internal->range_gt_text);
  } else if (internal->has_range_gt) {
    out->gt = range_number_bound(internal->range_gt);
  }
  if (internal->has_temporal_gte) {
    out->gte = range_datetime_bound(internal->range_gte_text);
  } else if (internal->has_range_gte) {
    out->gte = range_number_bound(internal->range_gte);
  }
  if (internal->has_temporal_lt) {
    out->lt = range_datetime_bound(internal->range_lt_text);
  } else if (internal->has_range_lt) {
    out->lt = range_number_bound(internal->range_lt);
  }
  if (internal->has_temporal_lte) {
    out->lte = range_datetime_bound(internal->range_lte_text);
  } else if (internal->has_range_lte) {
    out->lte = range_number_bound(internal->range_lte);
  }
  return LQL_STATUS_OK;
}

static lql_selector_since_kind public_since_kind(const lql_selector *selector) {
  if (selector == NULL || selector->date_since_text == NULL) {
    return LQL_SELECTOR_SINCE_NONE;
  }
  switch (selector->since_macro) {
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
  const lql_selector *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector date term required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  internal = selector_from_cursor(node);
  if (internal == NULL || node.kind != LQL_SELECTOR_NODE_DATE) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not a date term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out->field = lql_view_cstr(internal->field);
  out->value = lql_view_cstr(internal->date_value_text);
  out->since = lql_view_cstr(internal->date_since_text);
  out->since_kind = public_since_kind(internal);
  out->after = lql_view_cstr(internal->date_after_text);
  out->before = lql_view_cstr(internal->date_before_text);
  out->gt = lql_view_cstr(internal->date_gt_text);
  out->gte = lql_view_cstr(internal->date_gte_text);
  out->lt = lql_view_cstr(internal->date_lt_text);
  out->lte = lql_view_cstr(internal->date_lte_text);
  return LQL_STATUS_OK;
}

static lql_status selector_node_in_term_method(const lql *self,
                                               lql_selector_node node,
                                               lql_selector_in_term *out,
                                               lql_error *error) {
  const lql_selector *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector in term required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  internal = selector_from_cursor(node);
  if (internal == NULL || node.kind != LQL_SELECTOR_NODE_IN) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not an in term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out->field = lql_view_cstr(internal->field);
  out->any_count = internal->any_count;
  return LQL_STATUS_OK;
}

static lql_status selector_node_in_term_any_method(const lql *self,
                                                   lql_selector_node node,
                                                   size_t index,
                                                   lql_string_view *out,
                                                   lql_error *error) {
  const lql_selector *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector any value required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_string_view_clear(out);
  internal = selector_from_cursor(node);
  if (internal == NULL || node.kind != LQL_SELECTOR_NODE_IN) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not an in term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (index >= internal->any_count) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector any index out of range");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  out->data = internal->any[index];
  out->len = internal->any_lens[index];
  return LQL_STATUS_OK;
}

static lql_status selector_node_exists_path_method(const lql *self,
                                                   lql_selector_node node,
                                                   lql_string_view *out,
                                                   lql_error *error) {
  const lql_selector *internal;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "out selector exists path required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_string_view_clear(out);
  internal = selector_from_cursor(node);
  if (internal == NULL || node.kind != LQL_SELECTOR_NODE_EXISTS) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "selector node is not an exists term");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = lql_view_cstr(internal->field);
  return LQL_STATUS_OK;
}

static lql_status selector_json_write(FILE *out, const void *data, size_t len,
                                      lql_error *error) {
  if (len == 0u) {
    return LQL_STATUS_OK;
  }
  if (out == NULL || data == NULL || fwrite(data, 1u, len, out) != len) {
    lql_set_error(error, LQL_STATUS_IO_ERROR, "selector JSON write failed");
    return LQL_STATUS_IO_ERROR;
  }
  return LQL_STATUS_OK;
}

static lql_status selector_json_putc(FILE *out, int ch, lql_error *error) {
  unsigned char byte;
  byte = (unsigned char)ch;
  return selector_json_write(out, &byte, 1u, error);
}

static lql_status selector_json_literal(FILE *out, const char *text,
                                        lql_error *error) {
  return selector_json_write(out, text, strlen(text), error);
}

static lql_status selector_json_string(FILE *out, const char *data, size_t len,
                                       lql_error *error) {
  size_t i;
  lql_status status;
  status = selector_json_putc(out, '"', error);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  if (data == NULL) {
    data = "";
    len = 0u;
  }
  for (i = 0u; i < len; ++i) {
    unsigned char ch;
    ch = (unsigned char)data[i];
    switch (ch) {
    case '"':
      status = selector_json_literal(out, "\\\"", error);
      break;
    case '\\':
      status = selector_json_literal(out, "\\\\", error);
      break;
    case '\b':
      status = selector_json_literal(out, "\\b", error);
      break;
    case '\f':
      status = selector_json_literal(out, "\\f", error);
      break;
    case '\n':
      status = selector_json_literal(out, "\\n", error);
      break;
    case '\r':
      status = selector_json_literal(out, "\\r", error);
      break;
    case '\t':
      status = selector_json_literal(out, "\\t", error);
      break;
    default:
      if (ch < 0x20u) {
        if (fprintf(out, "\\u%04x", (unsigned int)ch) < 0) {
          lql_set_error(error, LQL_STATUS_IO_ERROR,
                        "selector JSON write failed");
          return LQL_STATUS_IO_ERROR;
        }
        status = LQL_STATUS_OK;
      } else {
        status = selector_json_putc(out, (int)ch, error);
      }
      break;
    }
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  return selector_json_putc(out, '"', error);
}

static lql_status selector_json_key(FILE *out, const char *key,
                                    lql_error *error) {
  lql_status status;
  status = selector_json_string(out, key, strlen(key), error);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  return selector_json_putc(out, ':', error);
}

static lql_status selector_json_comma(FILE *out, int *first, lql_error *error) {
  if (first == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (*first) {
    *first = 0;
    return LQL_STATUS_OK;
  }
  return selector_json_putc(out, ',', error);
}

static lql_status selector_json_number(FILE *out, double value,
                                       lql_error *error) {
  char buffer[64];
  if (!lql_number_format_json(value, buffer, sizeof(buffer))) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "selector JSON number is not finite");
    return LQL_STATUS_JSON_ERROR;
  }
  if (fprintf(out, "%s", buffer) < 0) {
    lql_set_error(error, LQL_STATUS_IO_ERROR, "selector JSON write failed");
    return LQL_STATUS_IO_ERROR;
  }
  return LQL_STATUS_OK;
}

static lql_status write_selector_json(FILE *out, const lql_selector *selector,
                                      lql_error *error);

static lql_status write_selector_literal_json(FILE *out, const char *value,
                                              size_t len,
                                              lql_selector_literal_kind kind,
                                              lql_error *error) {
  if (kind == LQL_SELECTOR_LITERAL_STRING) {
    return selector_json_string(out, value == NULL ? "" : value,
                                value == NULL ? 0u : len, error);
  }
  /*
   * Selector AST JSON carries scalar term values as JSON scalars. The parser
   * records the literal kind when text or JSON enters liblql, so export uses
   * that metadata instead of quoting everything and changing typed equality.
   */
  return selector_json_literal(out, value == NULL ? "" : value, error);
}

static lql_status write_string_predicate_json(FILE *out,
                                              const lql_selector *selector,
                                              lql_error *error) {
  size_t i;
  int first;
  lql_status status;
  first = 1;
  status = selector_json_putc(out, '{', error);
  if (status != LQL_STATUS_OK ||
      (status = selector_json_comma(out, &first, error)) != LQL_STATUS_OK ||
      (status = selector_json_key(out, "field", error)) != LQL_STATUS_OK ||
      (status = selector_json_string(out, selector->field,
                                     strlen(selector->field), error)) !=
          LQL_STATUS_OK) {
    return status;
  }
  if (selector->value_set ||
      (selector->value != NULL && selector->value[0] != '\0')) {
    status = selector_json_comma(out, &first, error);
    if (status != LQL_STATUS_OK ||
        (status = selector_json_key(out, "value", error)) != LQL_STATUS_OK ||
        (status = write_selector_literal_json(
             out, selector->value == NULL ? "" : selector->value,
             selector->value == NULL ? 0u : strlen(selector->value),
             selector->value_kind, error)) != LQL_STATUS_OK) {
      return status;
    }
  }
  if (selector->any_count != 0u) {
    status = selector_json_comma(out, &first, error);
    if (status != LQL_STATUS_OK ||
        (status = selector_json_key(out, "any", error)) != LQL_STATUS_OK ||
        (status = selector_json_putc(out, '[', error)) != LQL_STATUS_OK) {
      return status;
    }
    for (i = 0u; i < selector->any_count; ++i) {
      if (i != 0u &&
          (status = selector_json_putc(out, ',', error)) != LQL_STATUS_OK) {
        return status;
      }
      status = write_selector_literal_json(out, selector->any[i],
                                           selector->any_lens[i],
                                           selector->any_kinds[i], error);
      if (status != LQL_STATUS_OK) {
        return status;
      }
    }
    status = selector_json_putc(out, ']', error);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  if (selector->ignore_case) {
    status = selector_json_comma(out, &first, error);
    if (status != LQL_STATUS_OK ||
        (status = selector_json_key(out, "ignoreCase", error)) !=
            LQL_STATUS_OK ||
        (status = selector_json_literal(out, "true", error)) != LQL_STATUS_OK) {
      return status;
    }
  }
  return selector_json_putc(out, '}', error);
}

static lql_status write_range_bound_json(FILE *out,
                                         const lql_selector_range_bound *b,
                                         lql_error *error) {
  if (b->kind == LQL_SELECTOR_BOUND_NUMBER) {
    return selector_json_number(out, b->number, error);
  }
  if (b->kind == LQL_SELECTOR_BOUND_DATETIME) {
    return selector_json_string(out, b->datetime.data, b->datetime.len, error);
  }
  return LQL_STATUS_OK;
}

static lql_status write_range_bound_member(FILE *out, const char *key,
                                           lql_selector_range_bound bound,
                                           int *first, lql_error *error) {
  lql_status status;
  if (bound.kind == LQL_SELECTOR_BOUND_ABSENT) {
    return LQL_STATUS_OK;
  }
  status = selector_json_comma(out, first, error);
  if (status != LQL_STATUS_OK ||
      (status = selector_json_key(out, key, error)) != LQL_STATUS_OK) {
    return status;
  }
  return write_range_bound_json(out, &bound, error);
}

static lql_status write_range_predicate_json(FILE *out,
                                             const lql_selector *selector,
                                             lql_error *error) {
  lql_selector_range_bound gt;
  lql_selector_range_bound gte;
  lql_selector_range_bound lt;
  lql_selector_range_bound lte;
  int first;
  lql_status status;

  memset(&gt, 0, sizeof(gt));
  memset(&gte, 0, sizeof(gte));
  memset(&lt, 0, sizeof(lt));
  memset(&lte, 0, sizeof(lte));
  if (selector->has_temporal_gt) {
    gt = range_datetime_bound(selector->range_gt_text);
  } else if (selector->has_range_gt) {
    gt = range_number_bound(selector->range_gt);
  }
  if (selector->has_temporal_gte) {
    gte = range_datetime_bound(selector->range_gte_text);
  } else if (selector->has_range_gte) {
    gte = range_number_bound(selector->range_gte);
  }
  if (selector->has_temporal_lt) {
    lt = range_datetime_bound(selector->range_lt_text);
  } else if (selector->has_range_lt) {
    lt = range_number_bound(selector->range_lt);
  }
  if (selector->has_temporal_lte) {
    lte = range_datetime_bound(selector->range_lte_text);
  } else if (selector->has_range_lte) {
    lte = range_number_bound(selector->range_lte);
  }

  first = 1;
  status = selector_json_putc(out, '{', error);
  if (status != LQL_STATUS_OK ||
      (status = selector_json_comma(out, &first, error)) != LQL_STATUS_OK ||
      (status = selector_json_key(out, "field", error)) != LQL_STATUS_OK ||
      (status = selector_json_string(out, selector->field,
                                     strlen(selector->field), error)) !=
          LQL_STATUS_OK ||
      (status = write_range_bound_member(out, "gt", gt, &first, error)) !=
          LQL_STATUS_OK ||
      (status = write_range_bound_member(out, "gte", gte, &first, error)) !=
          LQL_STATUS_OK ||
      (status = write_range_bound_member(out, "lt", lt, &first, error)) !=
          LQL_STATUS_OK ||
      (status = write_range_bound_member(out, "lte", lte, &first, error)) !=
          LQL_STATUS_OK) {
    return status;
  }
  return selector_json_putc(out, '}', error);
}

static lql_status write_optional_string_member(FILE *out, const char *key,
                                               lql_string_view value,
                                               int *first, lql_error *error) {
  lql_status status;
  if (value.data == NULL) {
    return LQL_STATUS_OK;
  }
  status = selector_json_comma(out, first, error);
  if (status != LQL_STATUS_OK ||
      (status = selector_json_key(out, key, error)) != LQL_STATUS_OK) {
    return status;
  }
  return selector_json_string(out, value.data, value.len, error);
}

static lql_status write_date_predicate_json(FILE *out,
                                            const lql_selector *selector,
                                            lql_error *error) {
  int first;
  lql_status status;
  first = 1;
  status = selector_json_putc(out, '{', error);
  if (status != LQL_STATUS_OK ||
      (status = selector_json_comma(out, &first, error)) != LQL_STATUS_OK ||
      (status = selector_json_key(out, "field", error)) != LQL_STATUS_OK ||
      (status = selector_json_string(out, selector->field,
                                     strlen(selector->field), error)) !=
          LQL_STATUS_OK ||
      (status = write_optional_string_member(
           out, "value", lql_view_cstr(selector->date_value_text), &first,
           error)) != LQL_STATUS_OK ||
      (status = write_optional_string_member(
           out, "since", lql_view_cstr(selector->date_since_text), &first,
           error)) != LQL_STATUS_OK ||
      (status = write_optional_string_member(
           out, "after", lql_view_cstr(selector->date_after_text), &first,
           error)) != LQL_STATUS_OK ||
      (status = write_optional_string_member(
           out, "before", lql_view_cstr(selector->date_before_text), &first,
           error)) != LQL_STATUS_OK ||
      (status = write_optional_string_member(
           out, "gte", lql_view_cstr(selector->date_gte_text), &first,
           error)) != LQL_STATUS_OK ||
      (status = write_optional_string_member(
           out, "gt", lql_view_cstr(selector->date_gt_text), &first, error)) !=
          LQL_STATUS_OK ||
      (status = write_optional_string_member(
           out, "lte", lql_view_cstr(selector->date_lte_text), &first,
           error)) != LQL_STATUS_OK ||
      (status = write_optional_string_member(
           out, "lt", lql_view_cstr(selector->date_lt_text), &first, error)) !=
          LQL_STATUS_OK) {
    return status;
  }
  return selector_json_putc(out, '}', error);
}

static lql_status write_in_predicate_json(FILE *out,
                                          const lql_selector *selector,
                                          lql_error *error) {
  size_t i;
  lql_status status;
  status = selector_json_literal(out, "{\"field\":", error);
  if (status != LQL_STATUS_OK ||
      (status = selector_json_string(out, selector->field,
                                     strlen(selector->field), error)) !=
          LQL_STATUS_OK ||
      (status = selector_json_literal(out, ",\"any\":[", error)) !=
          LQL_STATUS_OK) {
    return status;
  }
  for (i = 0u; i < selector->any_count; ++i) {
    if (i != 0u &&
        (status = selector_json_putc(out, ',', error)) != LQL_STATUS_OK) {
      return status;
    }
    status = write_selector_literal_json(out, selector->any[i],
                                         selector->any_lens[i],
                                         selector->any_kinds[i], error);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  return selector_json_literal(out, "]}", error);
}

static lql_status write_selector_children_json(FILE *out,
                                               const lql_selector *selector,
                                               lql_error *error) {
  size_t i;
  lql_status status;
  status = selector_json_putc(out, '[', error);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  for (i = 0u; i < selector->child_count; ++i) {
    if (i != 0u &&
        (status = selector_json_putc(out, ',', error)) != LQL_STATUS_OK) {
      return status;
    }
    status = write_selector_json(out, &selector->children[i], error);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  return selector_json_putc(out, ']', error);
}

static lql_status write_selector_json(FILE *out, const lql_selector *selector,
                                      lql_error *error) {
  const char *key;
  lql_status status;
  status = selector_json_putc(out, '{', error);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  if (selector != NULL) {
    key = NULL;
    switch (selector->kind) {
    case LQL_SELECTOR_KIND_ALL:
      break;
    case LQL_SELECTOR_KIND_AND:
      key = "and";
      break;
    case LQL_SELECTOR_KIND_OR:
      key = "or";
      break;
    case LQL_SELECTOR_KIND_NOT:
      key = "not";
      break;
    case LQL_SELECTOR_KIND_EQ:
      key = "eq";
      break;
    case LQL_SELECTOR_KIND_NE:
      key = "not";
      break;
    case LQL_SELECTOR_KIND_CONTAINS:
      key = "contains";
      break;
    case LQL_SELECTOR_KIND_ICONTAINS:
      key = "icontains";
      break;
    case LQL_SELECTOR_KIND_PREFIX:
      key = "prefix";
      break;
    case LQL_SELECTOR_KIND_IPREFIX:
      key = "iprefix";
      break;
    case LQL_SELECTOR_KIND_RANGE:
      key = "range";
      break;
    case LQL_SELECTOR_KIND_DATE:
      key = "date";
      break;
    case LQL_SELECTOR_KIND_IN:
      key = "in";
      break;
    case LQL_SELECTOR_KIND_EXISTS:
      key = "exists";
      break;
    }
    if (key != NULL) {
      status = selector_json_key(out, key, error);
      if (status != LQL_STATUS_OK) {
        return status;
      }
      switch (selector->kind) {
      case LQL_SELECTOR_KIND_AND:
      case LQL_SELECTOR_KIND_OR:
        status = write_selector_children_json(out, selector, error);
        if (status != LQL_STATUS_OK) {
          return status;
        }
        break;
      case LQL_SELECTOR_KIND_NOT:
        if (selector->child_count == 0u) {
          status = write_selector_json(out, NULL, error);
        } else {
          status = write_selector_json(out, &selector->children[0], error);
        }
        if (status != LQL_STATUS_OK) {
          return status;
        }
        break;
      case LQL_SELECTOR_KIND_NE: {
        lql_selector eq_selector;
        /*
         * The selector AST JSON object shape has no "ne" operator. If a legacy
         * or future internal producer creates LQL_SELECTOR_KIND_NE, serialize
         * it as the semantic AST form not(eq(...)) so round-trips cannot invert
         * it.
         */
        eq_selector = *selector;
        eq_selector.kind = LQL_SELECTOR_KIND_EQ;
        status = write_selector_json(out, &eq_selector, error);
        if (status != LQL_STATUS_OK) {
          return status;
        }
        break;
      }
      case LQL_SELECTOR_KIND_EQ:
      case LQL_SELECTOR_KIND_CONTAINS:
      case LQL_SELECTOR_KIND_ICONTAINS:
      case LQL_SELECTOR_KIND_PREFIX:
      case LQL_SELECTOR_KIND_IPREFIX:
        status = write_string_predicate_json(out, selector, error);
        if (status != LQL_STATUS_OK) {
          return status;
        }
        break;
      case LQL_SELECTOR_KIND_RANGE:
        status = write_range_predicate_json(out, selector, error);
        if (status != LQL_STATUS_OK) {
          return status;
        }
        break;
      case LQL_SELECTOR_KIND_DATE:
        status = write_date_predicate_json(out, selector, error);
        if (status != LQL_STATUS_OK) {
          return status;
        }
        break;
      case LQL_SELECTOR_KIND_IN:
        status = write_in_predicate_json(out, selector, error);
        if (status != LQL_STATUS_OK) {
          return status;
        }
        break;
      case LQL_SELECTOR_KIND_EXISTS:
        status = selector_json_string(out, selector->field,
                                      strlen(selector->field), error);
        if (status != LQL_STATUS_OK) {
          return status;
        }
        break;
      case LQL_SELECTOR_KIND_ALL:
        break;
      }
    }
  }
  return selector_json_putc(out, '}', error);
}

static lql_status selector_write_json_method(lql *self,
                                             const lql_selector *selector,
                                             FILE *out, lql_error *error) {
  lql_status status;
  (void)self;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "output file required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  status = write_selector_json(out, selector == NULL ? NULL : selector, error);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  if (fflush(out) != 0) {
    lql_set_error(error, LQL_STATUS_IO_ERROR, "selector JSON write failed");
    return LQL_STATUS_IO_ERROR;
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
                                        const lql_selector *selector) {
  size_t i;

  if (out == NULL || selector == NULL) {
    return;
  }
  selector_path_capabilities_visit(out, selector->field);
  switch (selector->kind) {
  case LQL_SELECTOR_KIND_ALL:
    break;
  case LQL_SELECTOR_KIND_AND:
    out->and_ = 1;
    break;
  case LQL_SELECTOR_KIND_OR:
    out->or_ = 1;
    break;
  case LQL_SELECTOR_KIND_NOT:
    out->not_ = 1;
    break;
  case LQL_SELECTOR_KIND_EQ:
  case LQL_SELECTOR_KIND_NE:
    out->eq = 1;
    break;
  case LQL_SELECTOR_KIND_CONTAINS:
  case LQL_SELECTOR_KIND_ICONTAINS:
    out->contains = 1;
    break;
  case LQL_SELECTOR_KIND_PREFIX:
  case LQL_SELECTOR_KIND_IPREFIX:
    out->prefix = 1;
    break;
  case LQL_SELECTOR_KIND_RANGE:
    out->range = 1;
    break;
  case LQL_SELECTOR_KIND_DATE:
    out->date = 1;
    break;
  case LQL_SELECTOR_KIND_IN:
    out->in = 1;
    break;
  case LQL_SELECTOR_KIND_EXISTS:
    out->exists = 1;
    break;
  }
  for (i = 0u; i < selector->child_count; ++i) {
    selector_capabilities_visit(out, &selector->children[i]);
  }
}

static void selector_capabilities_get_method(const lql *self,
                                             const lql_selector *selector,
                                             lql_selector_capabilities *out) {
  (void)self;
  if (out == NULL) {
    return;
  }
  memset(out, 0, sizeof(*out));
  if (selector == NULL || selector->kind == LQL_SELECTOR_KIND_ALL) {
    return;
  }
  selector_capabilities_visit(out, selector);
}
