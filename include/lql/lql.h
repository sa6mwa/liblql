#ifndef LQL_LQL_H
#define LQL_LQL_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum lql_status {
  LQL_STATUS_OK = 0,
  LQL_STATUS_INVALID_ARGUMENT = 1,
  LQL_STATUS_NO_MEMORY = 2,
  LQL_STATUS_PARSE_ERROR = 3,
  LQL_STATUS_JSON_ERROR = 4,
  LQL_STATUS_UNSUPPORTED = 5,
  LQL_STATUS_STOP = 6,
  LQL_STATUS_CALLBACK_ERROR = 7,
  LQL_STATUS_IO_ERROR = 8
} lql_status;

typedef struct lql_error {
  lql_status code;
  char message[256];
} lql_error;

typedef struct lql_selector lql_selector;
typedef struct lql lql;
typedef struct lql_projection lql_projection;
typedef struct lql_mutation lql_mutation;

typedef struct lql_capabilities {
  int selector_parse;
  int selector_inspection;
} lql_capabilities;

typedef struct lql_selector_capabilities {
  int and_;
  int or_;
  int not_;
  int eq;
  int range;
  int date;
  int in;
  int prefix;
  int contains;
  int exists;
  int wildcard_path;
  int recursive_path;
} lql_selector_capabilities;

typedef struct lql_string_view {
  const char *data;
  size_t len;
} lql_string_view;

/** Reads up to `capacity` bytes. A successful zero-byte read signals EOF. */
typedef lql_status (*lql_stream_reader_fn)(void *user, unsigned char *buffer,
                                           size_t capacity, size_t *out_len,
                                           lql_error *error);

/** Writes exactly the supplied output chunk or returns an error status. */
typedef lql_status (*lql_stream_writer_fn)(void *user, const void *data,
                                           size_t len, lql_error *error);

typedef enum lql_stream_callback_result {
  LQL_STREAM_CALLBACK_CONTINUE = 0,
  LQL_STREAM_CALLBACK_STOP = 1,
  LQL_STREAM_CALLBACK_ERROR = 2
} lql_stream_callback_result;

typedef enum lql_stream_output_mode {
  LQL_STREAM_OUTPUT_DECISION_ONLY = 0,
  LQL_STREAM_OUTPUT_SELECTED_RECORD = 1,
  LQL_STREAM_OUTPUT_PROJECTION = 2,
  LQL_STREAM_OUTPUT_MUTATION = 3,
  LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION = 4
} lql_stream_output_mode;

typedef enum lql_stream_stop_reason {
  LQL_STREAM_STOP_NONE = 0,
  LQL_STREAM_STOP_RECORD_LIMIT = 1,
  LQL_STREAM_STOP_MATCH_LIMIT = 2,
  LQL_STREAM_STOP_BYTE_LIMIT = 3,
  LQL_STREAM_STOP_CALLBACK = 4
} lql_stream_stop_reason;

typedef struct lql_stream_decision {
  size_t record_index;
  int matched;
} lql_stream_decision;

/** Invoked after a complete record has been validated and evaluated. */
typedef lql_stream_callback_result (*lql_stream_decision_fn)(
    void *user, const lql_stream_decision *decision, lql_error *error);

typedef struct lql_stream_limits {
  size_t max_records;
  size_t max_matches;
  size_t max_bytes;
} lql_stream_limits;

typedef struct lql_stream_request {
  lql_stream_reader_fn reader;
  void *reader_user;
  lql_stream_writer_fn writer;
  void *writer_user;
  const lql_selector *selector;
  const lql_projection *projection;
  const lql_mutation *mutation;
  lql_stream_output_mode output_mode;
  int matched_only;
  lql_stream_limits limits;
  lql_stream_decision_fn on_decision;
  void *decision_user;
} lql_stream_request;

typedef struct lql_stream_result {
  size_t records_seen;
  size_t records_matched;
  size_t bytes_consumed;
  int stopped_early;
  lql_stream_stop_reason stop_reason;
} lql_stream_result;

typedef enum lql_selector_node_kind {
  LQL_SELECTOR_NODE_ALL = 0,
  LQL_SELECTOR_NODE_AND = 1,
  LQL_SELECTOR_NODE_OR = 2,
  LQL_SELECTOR_NODE_NOT = 3,
  LQL_SELECTOR_NODE_EQ = 4,
  LQL_SELECTOR_NODE_CONTAINS = 5,
  LQL_SELECTOR_NODE_ICONTAINS = 6,
  LQL_SELECTOR_NODE_PREFIX = 7,
  LQL_SELECTOR_NODE_IPREFIX = 8,
  LQL_SELECTOR_NODE_RANGE = 9,
  LQL_SELECTOR_NODE_DATE = 10,
  LQL_SELECTOR_NODE_IN = 11,
  LQL_SELECTOR_NODE_EXISTS = 12
} lql_selector_node_kind;

typedef enum lql_selector_bound_kind {
  LQL_SELECTOR_BOUND_ABSENT = 0,
  LQL_SELECTOR_BOUND_NUMBER = 1,
  LQL_SELECTOR_BOUND_DATETIME = 2
} lql_selector_bound_kind;

typedef enum lql_selector_since_kind {
  LQL_SELECTOR_SINCE_NONE = 0,
  LQL_SELECTOR_SINCE_NOW = 1,
  LQL_SELECTOR_SINCE_TODAY = 2,
  LQL_SELECTOR_SINCE_YESTERDAY = 3,
  LQL_SELECTOR_SINCE_LITERAL = 4
} lql_selector_since_kind;

typedef struct lql_selector_node {
  lql_selector_node_kind kind;
  const void *impl;
} lql_selector_node;

typedef struct lql_selector_string_term {
  lql_string_view field;
  int value_present;
  lql_string_view value;
  int ignore_case;
  size_t any_count;
} lql_selector_string_term;

typedef struct lql_selector_range_bound {
  lql_selector_bound_kind kind;
  double number;
  lql_string_view datetime;
} lql_selector_range_bound;

typedef struct lql_selector_range_term {
  lql_string_view field;
  lql_selector_range_bound gt;
  lql_selector_range_bound gte;
  lql_selector_range_bound lt;
  lql_selector_range_bound lte;
} lql_selector_range_term;

typedef struct lql_selector_date_term {
  lql_string_view field;
  lql_string_view value;
  lql_string_view since;
  lql_selector_since_kind since_kind;
  lql_string_view after;
  lql_string_view before;
  lql_string_view gt;
  lql_string_view gte;
  lql_string_view lt;
  lql_string_view lte;
} lql_selector_date_term;

typedef struct lql_selector_in_term {
  lql_string_view field;
  size_t any_count;
} lql_selector_in_term;

struct lql {
  void *impl;
  const char *(*version)(const lql *self);
  void (*capabilities_get)(const lql *self, lql_capabilities *out);
  lql_status (*selector_parse)(lql *self, const char *expr, lql_selector **out,
                               lql_error *error);
  lql_status (*selector_parse_or)(lql *self, const char *expr,
                                  lql_selector **out, lql_error *error);
  lql_status (*selector_parse_json)(lql *self, const void *json,
                                    size_t json_len, lql_selector **out,
                                    lql_error *error);
  void (*selector_destroy)(lql *self, lql_selector *selector);
  lql_status (*projection_parse)(lql *self, const char *const *paths,
                                 size_t path_count, lql_projection **out,
                                 lql_error *error);
  void (*projection_destroy)(lql *self, lql_projection *projection);
  size_t (*projection_path_count)(const lql *self,
                                  const lql_projection *projection);
  lql_status (*projection_path)(const lql *self,
                                const lql_projection *projection,
                                size_t index, lql_string_view *out,
                                lql_error *error);
  int (*selector_is_empty)(const lql *self, const lql_selector *selector);
  void (*selector_capabilities_get)(const lql *self,
                                    const lql_selector *selector,
                                    lql_selector_capabilities *out);
  lql_status (*selector_root)(const lql *self, const lql_selector *selector,
                              lql_selector_node *out, lql_error *error);
  lql_status (*selector_node_child_count)(const lql *self,
                                          lql_selector_node node,
                                          size_t *out_count, lql_error *error);
  lql_status (*selector_node_child)(const lql *self, lql_selector_node node,
                                    size_t index, lql_selector_node *out,
                                    lql_error *error);
  lql_status (*selector_node_string_term)(const lql *self,
                                          lql_selector_node node,
                                          lql_selector_string_term *out,
                                          lql_error *error);
  lql_status (*selector_node_string_term_any)(const lql *self,
                                              lql_selector_node node,
                                              size_t index,
                                              lql_string_view *out,
                                              lql_error *error);
  lql_status (*selector_node_range_term)(const lql *self,
                                         lql_selector_node node,
                                         lql_selector_range_term *out,
                                         lql_error *error);
  lql_status (*selector_node_date_term)(const lql *self, lql_selector_node node,
                                        lql_selector_date_term *out,
                                        lql_error *error);
  lql_status (*selector_node_in_term)(const lql *self, lql_selector_node node,
                                      lql_selector_in_term *out,
                                      lql_error *error);
  lql_status (*selector_node_in_term_any)(const lql *self,
                                          lql_selector_node node, size_t index,
                                          lql_string_view *out,
                                          lql_error *error);
  lql_status (*selector_node_exists_path)(const lql *self,
                                          lql_selector_node node,
                                          lql_string_view *out,
                                          lql_error *error);
  lql_status (*selector_write_json)(lql *self, const lql_selector *selector,
                                    FILE *out, lql_error *error);
  lql_status (*selector_build_all)(lql *self, lql_selector **out,
                                   lql_error *error);
  lql_status (*selector_build_compound)(lql *self, lql_selector_node_kind kind,
                                        const lql_selector *const *children,
                                        size_t child_count, lql_selector **out,
                                        lql_error *error);
  lql_status (*selector_build_not)(lql *self, const lql_selector *child,
                                   lql_selector **out, lql_error *error);
  lql_status (*selector_build_string)(lql *self, lql_selector_node_kind kind,
                                      const lql_selector_string_term *term,
                                      const lql_string_view *any_values,
                                      lql_selector **out, lql_error *error);
  lql_status (*selector_build_range)(lql *self,
                                     const lql_selector_range_term *term,
                                     lql_selector **out, lql_error *error);
  lql_status (*selector_build_date)(lql *self,
                                    const lql_selector_date_term *term,
                                    lql_selector **out, lql_error *error);
  lql_status (*selector_build_in)(lql *self, const lql_selector_in_term *term,
                                  const lql_string_view *any_values,
                                  lql_selector **out, lql_error *error);
  lql_status (*selector_build_exists)(lql *self, lql_string_view path,
                                      lql_selector **out, lql_error *error);
  void (*destroy)(lql *self);
};

lql_status lql_new(lql **out, lql_error *error);
void lql_error_init(lql_error *error);
const char *lql_status_string(lql_status status);

/**
 * Executes strict NDJSON through liblql's sole stream engine. Root arrays are
 * rejected. Invalid arguments clear `result` before this function returns.
 */
lql_status lql_stream_execute(lql *self, const lql_stream_request *request,
                              lql_stream_result *result, lql_error *error);

#ifdef __cplusplus
}
#endif

#endif
