#ifndef LQL_INTERNAL_H
#define LQL_INTERNAL_H

#include "lql/lql.h"

#include <lonejson.h>

#if defined(__GNUC__) || defined(__clang__)
#define LQL_INTERNAL_SYMBOL __attribute__((visibility("hidden")))
#else
#define LQL_INTERNAL_SYMBOL
#endif

typedef struct lql_allocator lql_allocator;
typedef struct lql_impl lql_impl;
typedef struct lql_stream_program lql_stream_program;
typedef struct lql_projection_capture lql_projection_capture;

#define LQL_STREAM_VALUE_LONEJSON_SPOOL 1
#define LQL_STREAM_VALUE_JSON_SPOOL 2

struct lql_stream_value {
  int storage_kind;
  const void *spool;
};

struct lql_allocator {
  void *impl;
  void *(*alloc)(lql_allocator *self, size_t size);
  void *(*calloc)(lql_allocator *self, size_t count, size_t size);
  void *(*realloc)(lql_allocator *self, void *ptr, size_t size);
  void (*destroy)(lql_allocator *self, void *ptr);
  char *(*strdup)(lql_allocator *self, const char *text);
};

struct lql_impl {
  lql_allocator *allocator;
};

LQL_INTERNAL_SYMBOL lql_allocator *lql_allocator_default(void);
LQL_INTERNAL_SYMBOL lql_allocator *lql_allocator_from_receiver(const lql *self);
LQL_INTERNAL_SYMBOL void *lql_receiver_alloc(lql *self, size_t size);
LQL_INTERNAL_SYMBOL void *lql_receiver_calloc(lql *self, size_t count,
                                              size_t size);
LQL_INTERNAL_SYMBOL void *lql_receiver_realloc(lql *self, void *ptr,
                                               size_t size);
LQL_INTERNAL_SYMBOL char *lql_receiver_strdup(lql *self, const char *text);
LQL_INTERNAL_SYMBOL void lql_receiver_destroy(lql *self, void *ptr);
LQL_INTERNAL_SYMBOL lql_status lql_new_with_allocator(lql **out,
                                                      lql_allocator *allocator,
                                                      lql_error *error);
LQL_INTERNAL_SYMBOL lonejson *lql_lonejson_new(lql *self,
                                               lonejson_error *error);
LQL_INTERNAL_SYMBOL lonejson *
lql_lonejson_new_mapped_stream(lql *self, lonejson_error *error);

typedef enum lql_selector_kind {
  LQL_SELECTOR_KIND_ALL = 0,
  LQL_SELECTOR_KIND_AND,
  LQL_SELECTOR_KIND_OR,
  LQL_SELECTOR_KIND_NOT,
  LQL_SELECTOR_KIND_EQ,
  LQL_SELECTOR_KIND_NE,
  LQL_SELECTOR_KIND_CONTAINS,
  LQL_SELECTOR_KIND_ICONTAINS,
  LQL_SELECTOR_KIND_PREFIX,
  LQL_SELECTOR_KIND_IPREFIX,
  LQL_SELECTOR_KIND_RANGE,
  LQL_SELECTOR_KIND_DATE,
  LQL_SELECTOR_KIND_IN,
  LQL_SELECTOR_KIND_EXISTS
} lql_selector_kind;

__extension__ typedef signed long long lql_int64;

typedef struct lql_temporal {
  lql_int64 seconds;
  int nanoseconds;
  int year;
  int month;
  int day;
  int date_only;
} lql_temporal;

typedef enum lql_since_macro {
  LQL_SINCE_NONE = 0,
  LQL_SINCE_NOW,
  LQL_SINCE_TODAY,
  LQL_SINCE_YESTERDAY
} lql_since_macro;

struct lql_selector {
  lql_selector_kind kind;
  char *field;
  char *value;
  int value_set;
  int value_is_string;
  int value_is_temporal;
  int ignore_case;
  char **any;
  size_t *any_lens;
  size_t any_count;
  char *range_gt_text;
  char *range_gte_text;
  char *range_lt_text;
  char *range_lte_text;
  char *date_value_text;
  char *date_since_text;
  char *date_after_text;
  char *date_before_text;
  char *date_gt_text;
  char *date_gte_text;
  char *date_lt_text;
  char *date_lte_text;
  double range_gt;
  double range_gte;
  double range_lt;
  double range_lte;
  lql_temporal temporal_eq;
  lql_temporal temporal_gt;
  lql_temporal temporal_gte;
  lql_temporal temporal_lt;
  lql_temporal temporal_lte;
  int has_range_gt;
  int has_range_gte;
  int has_range_lt;
  int has_range_lte;
  int has_temporal_eq;
  int has_temporal_gt;
  int has_temporal_gte;
  int has_temporal_lt;
  int has_temporal_lte;
  int range_is_temporal;
  lql_since_macro since_macro;
  struct lql_selector *children;
  size_t child_count;
  lql_stream_program *stream_program;
};

typedef struct lql_projection_path {
  char **segments;
  size_t segment_count;
} lql_projection_path;

struct lql_projection {
  char **paths;
  lql_projection_path *compiled_paths;
  size_t path_count;
};

typedef enum lql_mutation_kind {
  LQL_MUTATION_SET = 0,
  LQL_MUTATION_INCREMENT = 1,
  LQL_MUTATION_REMOVE = 2
} lql_mutation_kind;

typedef enum lql_mutation_value_kind {
  LQL_MUTATION_VALUE_STRING = 0,
  LQL_MUTATION_VALUE_NUMBER = 1,
  LQL_MUTATION_VALUE_BOOL = 2,
  LQL_MUTATION_VALUE_NULL = 3
} lql_mutation_value_kind;

typedef struct lql_mutation_action {
  lql_mutation_kind kind;
  char **segments;
  size_t segment_count;
  char *value;
  lql_mutation_value_kind value_kind;
  double delta;
} lql_mutation_action;

struct lql_mutation {
  lql_mutation_action *actions;
  size_t action_count;
};

LQL_INTERNAL_SYMBOL void lql_set_error(lql_error *error, lql_status status,
                                       const char *message);
LQL_INTERNAL_SYMBOL void lql_selector_cleanup(lql *self,
                                              lql_selector *selector);
LQL_INTERNAL_SYMBOL lql_status lql_parse_selector_internal(lql *self,
                                                           const char *expr,
                                                           int or_mode,
                                                           lql_selector **out,
                                                           lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_projection_parse_internal(
    lql *self, const char *const *paths, size_t path_count,
    lql_projection **out, lql_error *error);
LQL_INTERNAL_SYMBOL void
lql_projection_destroy_internal(lql *self, lql_projection *projection);
LQL_INTERNAL_SYMBOL lql_status lql_projection_render_top_level(
    lql *self, const lql_projection *projection, const lonejson_spooled *input,
    lonejson_sink_fn sink, void *sink_user, int *out_emitted, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_projection_capture_create(
    lql *self, const lql_projection *projection, lonejson *runtime,
    lql_projection_capture **out, lql_error *error);
LQL_INTERNAL_SYMBOL void
lql_projection_capture_destroy(lql_projection_capture *capture);
LQL_INTERNAL_SYMBOL void
lql_projection_capture_reset(lql_projection_capture *capture);
LQL_INTERNAL_SYMBOL void
lql_projection_capture_set_root_object(lql_projection_capture *capture);
LQL_INTERNAL_SYMBOL void
lql_projection_capture_visitor(lonejson_path_value_visitor *out);
LQL_INTERNAL_SYMBOL void *
lql_projection_capture_visitor_user(lql_projection_capture *capture);
LQL_INTERNAL_SYMBOL lql_status lql_projection_capture_render(
    lql_projection_capture *capture, lonejson_sink_fn sink, void *sink_user,
    int *out_emitted, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_mutation_parse_internal(
    lql *self, const char *const *expressions, size_t expression_count,
    lql_mutation **out, lql_error *error);
LQL_INTERNAL_SYMBOL void lql_mutation_destroy_internal(lql *self,
                                                       lql_mutation *mutation);
LQL_INTERNAL_SYMBOL lql_status lql_mutation_render(
    lql *self, const lql_mutation *mutation, const lonejson_spooled *input,
    lonejson_sink_fn sink, void *sink_user, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status
lql_parse_selector_json_internal(lql *self, const void *json, size_t json_len,
                                 lql_selector **out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_selector_build_all_internal(
    lql *self, lql_selector **out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_selector_build_compound_internal(
    lql *self, lql_selector_node_kind kind, const lql_selector *const *children,
    size_t child_count, lql_selector **out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_selector_build_not_internal(
    lql *self, const lql_selector *child, lql_selector **out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_selector_build_string_internal(
    lql *self, lql_selector_node_kind kind,
    const lql_selector_string_term *term, const lql_string_view *any_values,
    lql_selector **out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_selector_build_range_internal(
    lql *self, const lql_selector_range_term *term, lql_selector **out,
    lql_error *error);
LQL_INTERNAL_SYMBOL lql_status
lql_selector_build_date_internal(lql *self, const lql_selector_date_term *term,
                                 lql_selector **out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_selector_build_in_internal(
    lql *self, const lql_selector_in_term *term,
    const lql_string_view *any_values, lql_selector **out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_selector_build_exists_internal(
    lql *self, lql_string_view path, lql_selector **out, lql_error *error);
LQL_INTERNAL_SYMBOL int lql_parse_temporal_literal(const char *raw,
                                                   lql_temporal *out);
LQL_INTERNAL_SYMBOL int lql_temporal_compare(const lql_temporal *left,
                                             const lql_temporal *right);
LQL_INTERNAL_SYMBOL int lql_temporal_equal(const lql_temporal *left,
                                           const lql_temporal *right);
LQL_INTERNAL_SYMBOL int
lql_temporal_format_rfc3339_nano(const lql_temporal *value, char *buf,
                                 size_t buf_len);
LQL_INTERNAL_SYMBOL int lql_temporal_now(lql_temporal *out);
LQL_INTERNAL_SYMBOL int lql_temporal_today(lql_temporal *out);
LQL_INTERNAL_SYMBOL int lql_temporal_yesterday(lql_temporal *out);
LQL_INTERNAL_SYMBOL void lql_stream_program_destroy(lql *self,
                                                    lql_selector *selector);
LQL_INTERNAL_SYMBOL lql_status lql_stream_execute_flat_eq(
    lql *self, const lql_stream_request *request, lql_stream_result *result,
    lql_error *error, int *out_handled);

#endif
