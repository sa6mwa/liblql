#ifndef LQL_INTERNAL_H
#define LQL_INTERNAL_H

#include "lql/lql.h"
#include "lql_temporal_internal.h"

#if defined(__GNUC__)
#define LQL_INTERNAL_SYMBOL __attribute__((visibility("hidden")))
#else
#define LQL_INTERNAL_SYMBOL
#endif

typedef struct lql_allocator lql_allocator;
typedef struct lql_impl lql_impl;
typedef struct lql_stream_program lql_stream_program;
typedef struct lql_projection_capture lql_projection_capture;

#define LQL_INSTANCE_MEMORY_LIMIT_BYTES (8u * 1024u * 1024u)

#define LQL_STREAM_VALUE_JSON_SPOOL 2
#define LQL_STREAM_VALUE_SOURCE_RANGE 3

struct lql_stream_value {
  int storage_kind;
  const void *spool;
  lql_stream_range_writer_fn range_writer;
  void *range_user;
  size_t range_offset;
  size_t range_len;
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
  lql_allocator *upstream_allocator;
  lql_allocator allocator;
  size_t live_bytes;
};

LQL_INTERNAL_SYMBOL lql_allocator *lql_allocator_default(void);
LQL_INTERNAL_SYMBOL void lql_allocator_instance_init(lql_impl *impl,
                                                     lql_allocator *upstream);
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

typedef enum lql_selector_literal_kind {
  LQL_SELECTOR_LITERAL_STRING = 0,
  LQL_SELECTOR_LITERAL_NUMBER = 1,
  LQL_SELECTOR_LITERAL_BOOL = 2,
  LQL_SELECTOR_LITERAL_NULL = 3
} lql_selector_literal_kind;

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
  int value_from_json;
  lql_selector_literal_kind value_kind;
  int value_is_temporal;
  int ignore_case;
  char **any;
  size_t *any_lens;
  lql_selector_literal_kind *any_kinds;
  int *any_from_json;
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
  LQL_MUTATION_VALUE_NULL = 3,
  LQL_MUTATION_VALUE_FILE_AUTO = 4,
  LQL_MUTATION_VALUE_FILE_TEXT = 5,
  LQL_MUTATION_VALUE_FILE_BASE64 = 6
} lql_mutation_value_kind;

typedef struct lql_mutation_action {
  lql_mutation_kind kind;
  char **segments;
  size_t segment_count;
  char *value;
  lql_mutation_file_open_fn file_value_open;
  lql_mutation_file_close_fn file_value_close;
  void *file_value_user;
  lql_mutation_value_kind value_kind;
  double delta;
} lql_mutation_action;

struct lql_mutation {
  lql_mutation_action *actions;
  size_t action_count;
};

LQL_INTERNAL_SYMBOL void lql_set_error(lql_error *error, lql_status status,
                                       const char *message);
LQL_INTERNAL_SYMBOL int lql_number_parse_json(const char *text, size_t len,
                                              double *out);
LQL_INTERNAL_SYMBOL int lql_number_format_json(double value, char *out,
                                               size_t cap);
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
LQL_INTERNAL_SYMBOL lql_status lql_mutation_parse_internal(
    lql *self, const char *const *expressions, size_t expression_count,
    lql_mutation **out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_mutation_parse_internal_with_options(
    lql *self, const char *const *expressions, size_t expression_count,
    const lql_mutation_parse_options *options, lql_mutation **out,
    lql_error *error);
LQL_INTERNAL_SYMBOL void lql_mutation_destroy_internal(lql *self,
                                                       lql_mutation *mutation);
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
LQL_INTERNAL_SYMBOL void lql_stream_program_destroy(lql *self,
                                                    lql_selector *selector);
LQL_INTERNAL_SYMBOL lql_status lql_stream_execute_flat_eq(
    lql *self, const lql_stream_request *request, lql_stream_result *result,
    lql_error *error, int allow_spool, int *out_handled);

#endif
