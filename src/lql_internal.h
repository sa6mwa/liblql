#ifndef LQL_INTERNAL_H
#define LQL_INTERNAL_H

#include "lql/lql.h"

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
  LQL_NODE_IN,
  LQL_NODE_EXISTS
} lql_node_kind;

typedef struct lql_term {
  char *field;
  char *value;
  int value_set;
  int ignore_case;
  char **any;
  size_t any_count;
  double number;
  int has_number;
  int range_op;
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
                              lql_query_decision_fn on_decision, void *user,
                              lql_query_result *out_result, lql_error *error);

#endif
