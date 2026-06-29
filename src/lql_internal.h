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
typedef struct lql_pool_block lql_pool_block;

struct lql_pool_block {
  union {
    void *ptr;
    long l;
    double d;
    long double ld;
  } align;
  lql_pool_block *all_next;
  lql_pool_block *free_next;
  size_t size;
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
  lql_pool_block *eval_pool_all;
  lql_pool_block *eval_pool_free;
  lonejson *eval_runtime;
  int eval_runtime_in_use;
  unsigned char *eval_hits;
  size_t eval_hits_cap;
  unsigned char *eval_stream_misses;
  size_t eval_stream_misses_cap;
  unsigned char *eval_in_matches;
  size_t eval_in_matches_cap;
  char *eval_contains_tail_buf;
  size_t eval_contains_tail_cap;
  char *eval_val_buf;
  size_t eval_val_cap;
  int *eval_container_types;
  size_t *eval_container_depths;
  size_t eval_container_cap;
  int eval_scratch_in_use;
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
LQL_INTERNAL_SYMBOL lonejson *lql_lonejson_acquire(lql *self, int *out_cached,
                                                   lonejson_error *error);
LQL_INTERNAL_SYMBOL void lql_lonejson_release(lql *self, lonejson *runtime,
                                              int cached);
LQL_INTERNAL_SYMBOL void lql_eval_methods_install(lql *ctx);
LQL_INTERNAL_SYMBOL void lql_project_methods_install(lql *ctx);
LQL_INTERNAL_SYMBOL void lql_mutation_methods_install(lql *ctx);

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
  size_t hit_index;
  size_t hit_count;
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

#endif
