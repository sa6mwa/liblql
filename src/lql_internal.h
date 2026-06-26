#ifndef LQL_INTERNAL_H
#define LQL_INTERNAL_H

#include "lql/lql.h"

#include <lonejson.h>

#if defined(__GNUC__) || defined(__clang__)
#define LQL_INTERNAL_SYMBOL __attribute__((visibility("hidden")))
#else
#define LQL_INTERNAL_SYMBOL
#endif

typedef enum lql_node_kind {
  LQL_NODE_ALL = 0,
  LQL_NODE_AND,
  LQL_NODE_OR,
  LQL_NODE_NOT,
  LQL_NODE_EQ,
  LQL_NODE_NE,
  LQL_NODE_CONTAINS,
  LQL_NODE_ICONTAINS,
  LQL_NODE_PREFIX,
  LQL_NODE_IPREFIX,
  LQL_NODE_RANGE,
  LQL_NODE_DATE,
  LQL_NODE_IN,
  LQL_NODE_EXISTS
} lql_node_kind;

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

typedef struct lql_term {
  char *field;
  char *value;
  int value_set;
  int ignore_case;
  char **any;
  size_t any_count;
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
} lql_term;

typedef struct lql_node {
  lql_node_kind kind;
  lql_term term;
  struct lql_node *children;
  size_t child_count;
  size_t hit_index;
} lql_node;

struct lql_selector {
  lql_node root;
  size_t hit_count;
};

LQL_INTERNAL_SYMBOL void lql_set_error(lql_error *error, lql_status status,
                                       const char *message);
LQL_INTERNAL_SYMBOL void lql_node_cleanup(lql_node *node);
LQL_INTERNAL_SYMBOL lql_status lql_parse_selector_internal(const char *expr,
                                                           int or_mode,
                                                           lql_selector **out,
                                                           lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_selector_parse_impl(lql *self,
                                                       const char *expr,
                                                       lql_selector **out,
                                                       lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_selector_parse_or_impl(lql *self,
                                                          const char *expr,
                                                          lql_selector **out,
                                                          lql_error *error);
LQL_INTERNAL_SYMBOL void lql_selector_destroy_impl(lql *self,
                                                lql_selector *selector);
LQL_INTERNAL_SYMBOL int
lql_selector_is_empty_impl(const lql *self, const lql_selector *selector);
LQL_INTERNAL_SYMBOL lql_status lql_eval_selector(const lql_selector *selector,
                                                 const char *json,
                                                 size_t json_len,
                                                 int *out_matched,
                                                 lql_error *error);
LQL_INTERNAL_SYMBOL lql_status
lql_matches_json_impl(lql *self, const lql_selector *selector, const char *json,
                      size_t json_len, int *out_matched, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_query_file_decisions_impl(
    lql *self, const lql_selector *selector, FILE *file,
    lql_query_decision_fn on_decision, void *user, lql_query_result *out_result,
    lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_query_file_decisions_with_options_impl(
    lql *self, const lql_selector *selector, FILE *file,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_query_source_decisions_impl(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_query_decision_fn on_decision, void *user, lql_query_result *out_result,
    lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_query_source_decisions_with_options_impl(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_query_source_spooled_matches_impl(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_query_match_fn on_match, void *user, lql_query_result *out_result,
    lql_error *error);
LQL_INTERNAL_SYMBOL lql_status
lql_query_source_spooled_matches_with_options_impl(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status
lql_query_file_matches_impl(lql *self, const lql_selector *selector, FILE *file,
                            lql_query_match_fn on_match, void *user,
                            lql_query_result *out_result, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_query_file_matches_with_options_impl(
    lql *self, const lql_selector *selector, FILE *file,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_payload_write_json_impl(
    lql *self, const lql_payload *payload, FILE *out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_payload_write_json_sink_impl(
    lql *self, const lql_payload *payload, lql_write_fn write, void *write_user,
    lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_eval_query_file_decisions(
    const lql_selector *selector, FILE *file, const lql_query_options *options,
    lql_query_decision_fn on_decision, void *user, lql_query_result *out_result,
    lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_eval_query_source_decisions(
    const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_eval_query_source_spooled_matches(
    const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_eval_query_file_range_spooled_matches(
    const lql_selector *selector, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, int compact, const lql_projection *projection,
    const lql_mutation_plan *mutation_plan, int matches_only,
    lql_query_result *out_result, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_eval_query_file_spooled_matches(
    const lql_selector *selector, FILE *file, FILE *out, int compact,
    const lql_projection *projection, const lql_mutation_plan *mutation_plan,
    int matches_only, lql_query_result *out_result, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_eval_query_source_spooled_rewrite(
    const lql_selector *selector, lql_read_fn read, void *read_user, FILE *out,
    int compact, const lql_projection *projection,
    const lql_mutation_plan *mutation_plan, int matches_only,
    lql_query_result *out_result, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_projection_parse_impl(
    lql *self, const char *const *fields, size_t field_count,
    lql_projection **out, lql_error *error);
LQL_INTERNAL_SYMBOL void lql_projection_destroy_impl(lql *self,
                                                  lql_projection *projection);
LQL_INTERNAL_SYMBOL lql_status lql_project_file_range_impl(
    lql *self, const lql_projection *projection, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, int *out_found, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_project_source_impl(
    lql *self, const lql_projection *projection, lql_read_fn read,
    void *read_user, FILE *out, int *out_found, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_project_json_impl(
    lql *self, const lql_projection *projection, const char *json,
    size_t json_len, FILE *out, int *out_found, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_project_spooled(
    const lql_projection *projection, const lonejson_spooled *spooled,
    FILE *out, int *out_found, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status
lql_compact_file_range_impl(lql *self, FILE *file, lql_uint64 offset,
                            lql_uint64 size, FILE *out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_compact_source_impl(
    lql *self, lql_read_fn read, void *read_user, FILE *out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_compact_json_impl(lql *self,
                                                     const char *json,
                                                     size_t json_len, FILE *out,
                                                     lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_mutation_plan_parse_impl(
    lql *self, const char *const *exprs, size_t expr_count,
    lql_mutation_plan **out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_mutation_plan_parse_with_options_impl(
    lql *self, const char *const *exprs, size_t expr_count,
    const lql_mutation_parse_options *options, lql_mutation_plan **out,
    lql_error *error);
LQL_INTERNAL_SYMBOL size_t
lql_mutation_plan_count_impl(const lql *self, const lql_mutation_plan *plan);
LQL_INTERNAL_SYMBOL void lql_mutation_plan_destroy_impl(lql *self,
                                                     lql_mutation_plan *plan);
lql_status lql_mutate_file_range_root_fields_impl(lql *self,
                                                  const lql_mutation_plan *plan,
                                                  FILE *file, lql_uint64 offset,
                                                  lql_uint64 size, FILE *out,
                                                  lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_mutate_file_range_paths_impl(
    lql *self, const lql_mutation_plan *plan, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_mutate_file_range_candidates_impl(
    lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
    FILE *file, lql_uint64 offset, lql_uint64 size, FILE *out, int compact,
    int matches_only, lql_query_result *out_result, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_mutate_source_paths_impl(
    lql *self, const lql_mutation_plan *plan, lql_read_fn read, void *read_user,
    FILE *out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_mutate_source_candidates_impl(
    lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
    lql_read_fn read, void *read_user, FILE *out, int compact, int matches_only,
    lql_query_result *out_result, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status
lql_mutate_json_impl(lql *self, const lql_mutation_plan *plan, const char *json,
                     size_t json_len, FILE *out, lql_error *error);
LQL_INTERNAL_SYMBOL lql_status lql_mutate_spooled_paths(
    const lql_mutation_plan *plan, const lonejson_spooled *spooled, FILE *out,
    lql_error *error);
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
