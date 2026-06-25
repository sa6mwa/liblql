#ifndef LQL_INTERNAL_H
#define LQL_INTERNAL_H

#include "lql/lql.h"

#include <lonejson.h>

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

void lql_set_error(lql_error *error, lql_status status, const char *message);
void lql_node_cleanup(lql_node *node);
lql_status lql_parse_selector_internal(const char *expr, int or_mode,
                                       lql_selector **out, lql_error *error);
lql_status lql_eval_selector(const lql_selector *selector, const char *json,
                             size_t json_len, int *out_matched,
                             lql_error *error);
lql_status
lql_eval_query_file_decisions(const lql_selector *selector, FILE *file,
                              const lql_query_options *options,
                              lql_query_decision_fn on_decision, void *user,
                              lql_query_result *out_result, lql_error *error);
lql_status lql_eval_query_source_decisions(
    const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error);
lql_status lql_eval_query_source_spooled_matches(
    const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error);
lql_status lql_eval_query_file_spooled_matches(
    const lql_selector *selector, FILE *file, FILE *out, int compact,
    const lql_projection *projection, const lql_mutation_plan *mutation_plan,
    int matches_only, lql_query_result *out_result, lql_error *error);
lql_status lql_project_spooled(const lql_projection *projection,
                               const lonejson_spooled *spooled, FILE *out,
                               int *out_found, lql_error *error);
lql_status lql_mutate_spooled_paths(const lql_mutation_plan *plan,
                                    const lonejson_spooled *spooled, FILE *out,
                                    lql_error *error);
int lql_parse_temporal_literal(const char *raw, lql_temporal *out);
int lql_temporal_compare(const lql_temporal *left, const lql_temporal *right);
int lql_temporal_equal(const lql_temporal *left, const lql_temporal *right);
int lql_temporal_format_rfc3339_nano(const lql_temporal *value, char *buf,
                                     size_t buf_len);
int lql_temporal_now(lql_temporal *out);
int lql_temporal_today(lql_temporal *out);
int lql_temporal_yesterday(lql_temporal *out);

#endif
