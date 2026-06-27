#ifndef LQL_LQL_H
#define LQL_LQL_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
typedef unsigned __int64 lql_uint64;
#elif defined(__GNUC__) || defined(__clang__)
__extension__ typedef unsigned long long lql_uint64;
#else
typedef unsigned long lql_uint64;
#endif

typedef enum lql_status {
  LQL_STATUS_OK = 0,
  LQL_STATUS_INVALID_ARGUMENT = 1,
  LQL_STATUS_NO_MEMORY = 2,
  LQL_STATUS_PARSE_ERROR = 3,
  LQL_STATUS_JSON_ERROR = 4,
  LQL_STATUS_UNSUPPORTED = 5,
  LQL_STATUS_STOP = 6
} lql_status;

typedef struct lql_error {
  lql_status code;
  char message[256];
} lql_error;

typedef struct lql_selector lql_selector;
typedef struct lql_projection lql_projection;
typedef struct lql_mutation_plan lql_mutation_plan;
typedef struct lql lql;

typedef struct lql_query_decision {
  int matched;
  lql_uint64 index;
  lql_uint64 offset;
  lql_uint64 size;
} lql_query_decision;

typedef enum lql_payload_kind {
  LQL_PAYLOAD_NONE = 0,
  LQL_PAYLOAD_SEEKABLE_RANGE = 1,
  LQL_PAYLOAD_SPOOLED = 2
} lql_payload_kind;

typedef struct lql_payload {
  lql_payload_kind kind;
  lql_uint64 index;
  lql_uint64 offset;
  lql_uint64 size;
  FILE *source;
  const void *spooled;
} lql_payload;

typedef struct lql_query_match {
  lql_query_decision decision;
  lql_payload payload;
} lql_query_match;

typedef enum lql_query_stop_reason {
  LQL_QUERY_STOP_NONE = 0,
  LQL_QUERY_STOP_MATCH_LIMIT = 1,
  LQL_QUERY_STOP_CANDIDATE_LIMIT = 2,
  LQL_QUERY_STOP_BYTE_LIMIT = 3,
  LQL_QUERY_STOP_CALLBACK = 4
} lql_query_stop_reason;

typedef struct lql_query_options {
  lql_uint64 max_matches;
  lql_uint64 max_candidates;
  lql_uint64 max_bytes_read;
} lql_query_options;

typedef struct lql_read_result {
  size_t bytes_read;
  int eof;
  int error_code;
} lql_read_result;

typedef struct lql_mutation_parse_options {
  int enable_file_values;
  const char *file_value_base_dir;
} lql_mutation_parse_options;

typedef struct lql_query_result {
  lql_uint64 candidates_seen;
  lql_uint64 candidates_matched;
  lql_uint64 bytes_read;
  int stopped_early;
  lql_query_stop_reason stop_reason;
} lql_query_result;

typedef struct lql_capabilities {
  /* Selector parser entry points are available. */
  int selector_parse;
  /* Selector capability and execution-trait inspection is available. */
  int selector_inspection;
  /* In-memory whole-value match helper is available. */
  int matches_json;
  /* FILE * candidate decision streaming is available. */
  int file_decision_stream;
  /* Callback-source candidate decision streaming is available. */
  int source_decision_stream;
  /* FILE * matched-candidate payload streaming is available. */
  int file_match_stream;
  /* Match payloads can identify callback-scoped seekable source ranges. */
  int seekable_range_payloads;
  /* Callback-source matched payload streaming with spooled payloads is
     available. */
  int source_spooled_match_stream;
  /* Match payloads can identify callback-scoped spooled handles. */
  int spooled_payloads;
  /* Match payloads can be written to caller-managed sink callbacks. */
  int payload_sink_write;
  /* Match payloads can be projected while callback-scoped. */
  int payload_projection;
  /* Projection from one seekable file range is available. */
  int projection_file_range;
  /* Projection from caller-provided read callbacks is available. */
  int projection_source;
  /* Projection from caller-buffered JSON is available. */
  int projection_buffered_json;
  /* Compact serialization from one seekable file range is available. */
  int compact_file_range;
  /* Compact serialization from caller-provided read callbacks is available. */
  int compact_source;
  /* Compact serialization from caller-buffered JSON is available. */
  int compact_buffered_json;
  /* Mutation expression parse and plan validation are available. */
  int mutation_parse;
  /* Mutation execution against one seekable file range is available. */
  int mutation_file_range;
  /* Mutation execution against seekable candidate streams is available. */
  int mutation_file_range_candidates;
  /* Mutation execution against caller-provided read callbacks is available. */
  int mutation_source;
  /* Mutation execution against callback-source candidate streams is available.
   */
  int mutation_source_candidates;
  /* Projection-before-mutation execution against seekable candidate streams is
     available. */
  int mutation_file_range_projected_candidates;
  /* Projection-before-mutation execution against callback-source candidate
     streams is available. */
  int mutation_source_projected_candidates;
  /* Mutation execution against caller-buffered JSON is available. */
  int mutation_buffered_json;
  /* Explicit opt-in file-backed mutation values are available. */
  int mutation_file_values;
} lql_capabilities;

typedef struct lql_selector_capabilities {
  /* Selector uses an AND composition. */
  int and_;
  /* Selector uses an OR composition. */
  int or_;
  /* Selector uses a negated clause. */
  int not_;
  /* Selector uses equality comparison. */
  int eq;
  /* Selector uses numeric range comparison. */
  int range;
  /* Selector uses temporal comparison. */
  int date;
  /* Selector uses set membership comparison. */
  int in;
  /* Selector uses prefix comparison. */
  int prefix;
  /* Selector uses substring comparison. */
  int contains;
  /* Selector uses path existence comparison. */
  int exists;
  /* Selector path includes wildcard array/member traversal. */
  int wildcard_path;
  /* Selector path includes recursive traversal. */
  int recursive_path;
} lql_selector_capabilities;

typedef struct lql_selector_execution_traits {
  /* Selector requires contains-like string matching. */
  int uses_contains_like;
  /* Selector requires recursive path traversal. */
  int uses_recursive_path;
  /* Selector requires wildcard path traversal. */
  int uses_wildcard_path;
  /* Selector requires an object-root candidate. */
  int requires_object_root;
  /* Selector is likely to reject non-matches before full traversal. */
  int early_non_match_likely;
} lql_selector_execution_traits;

typedef struct lql_string_view {
  const char *data;
  size_t len;
} lql_string_view;

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

typedef lql_status (*lql_query_decision_fn)(void *user,
                                            const lql_query_decision *decision);
typedef lql_status (*lql_query_match_fn)(void *user,
                                         const lql_query_match *match);
typedef lql_read_result (*lql_read_fn)(void *user, unsigned char *buffer,
                                       size_t capacity);
typedef lql_status (*lql_write_fn)(void *user, const void *data, size_t len);

/* Instantiatable liblql receiver shell. Fields are initialized by lql_new().
   Mutable implementation state, including allocator ownership, is kept behind
   impl. A receiver is not internally synchronized; use one lql instance from
   one active thread at a time, or serialize access externally. Use
   ctx->method(ctx, ...) for all handle operations and cleanup. Standalone
   public functions are limited to construction and diagnostics. */
struct lql {
  /* Private implementation pointer owned by the receiver. */
  void *impl;
  /* Returns the resolved liblql version string for this receiver. */
  const char *(*version)(const lql *self);
  /* Writes the receiver capability set to out; NULL out is accepted. */
  void (*capabilities_get)(const lql *self, lql_capabilities *out);
  /* Parses an AND selector expression; caller destroys *out on success. */
  lql_status (*selector_parse)(lql *self, const char *expr, lql_selector **out,
                               lql_error *error);
  /* Parses an OR selector expression; caller destroys *out on success. */
  lql_status (*selector_parse_or)(lql *self, const char *expr,
                                  lql_selector **out, lql_error *error);
  /* Parses Go-compatible selector AST JSON; caller destroys *out on success. */
  lql_status (*selector_parse_json)(lql *self, const void *json,
                                    size_t json_len, lql_selector **out,
                                    lql_error *error);
  /* Destroys a selector handle; NULL is accepted. */
  void (*selector_destroy)(lql *self, lql_selector *selector);
  /* Reports whether selector is NULL or matches all candidates. */
  int (*selector_is_empty)(const lql *self, const lql_selector *selector);
  /* Writes selector feature-family capabilities to out; NULL out is accepted. */
  void (*selector_capabilities_get)(
      const lql *self, const lql_selector *selector,
      lql_selector_capabilities *out);
  /* Writes selector execution traits to out; NULL out is accepted. */
  void (*selector_execution_traits_get)(
      const lql *self, const lql_selector *selector,
      lql_selector_execution_traits *out);
  /* Borrows the selector root AST node; out is valid while selector lives. */
  lql_status (*selector_root)(const lql *self, const lql_selector *selector,
                              lql_selector_node *out, lql_error *error);
  /* Returns the child count for an AND, OR, or NOT AST node. */
  lql_status (*selector_node_child_count)(const lql *self,
                                          lql_selector_node node,
                                          size_t *out_count,
                                          lql_error *error);
  /* Borrows one child AST node by index from an AND, OR, or NOT node. */
  lql_status (*selector_node_child)(const lql *self, lql_selector_node node,
                                    size_t index, lql_selector_node *out,
                                    lql_error *error);
  /* Borrows string-term data for eq/contains/icontains/prefix/iprefix nodes. */
  lql_status (*selector_node_string_term)(
      const lql *self, lql_selector_node node,
      lql_selector_string_term *out, lql_error *error);
  /* Borrows one any-list value from a string-term node. */
  lql_status (*selector_node_string_term_any)(const lql *self,
                                              lql_selector_node node,
                                              size_t index,
                                              lql_string_view *out,
                                              lql_error *error);
  /* Borrows range-term data for a range node. */
  lql_status (*selector_node_range_term)(const lql *self,
                                         lql_selector_node node,
                                         lql_selector_range_term *out,
                                         lql_error *error);
  /* Borrows date-term data for a date node. */
  lql_status (*selector_node_date_term)(const lql *self,
                                        lql_selector_node node,
                                        lql_selector_date_term *out,
                                        lql_error *error);
  /* Borrows in-term data for an in node. */
  lql_status (*selector_node_in_term)(const lql *self, lql_selector_node node,
                                      lql_selector_in_term *out,
                                      lql_error *error);
  /* Borrows one any-list value from an in node. */
  lql_status (*selector_node_in_term_any)(const lql *self,
                                          lql_selector_node node, size_t index,
                                          lql_string_view *out,
                                          lql_error *error);
  /* Borrows the path for an exists node. */
  lql_status (*selector_node_exists_path)(const lql *self,
                                          lql_selector_node node,
                                          lql_string_view *out,
                                          lql_error *error);
  /* Serializes the selector AST as compact Go-compatible JSON to out. */
  lql_status (*selector_write_json)(lql *self, const lql_selector *selector,
                                    FILE *out, lql_error *error);
  /* Evaluates one caller-buffered JSON value against selector. */
  lql_status (*matches_json)(lql *self, const lql_selector *selector,
                             const char *json, size_t json_len,
                             int *out_matched, lql_error *error);
  /* Streams seekable FILE candidates and invokes on_decision for each. */
  lql_status (*query_file_decisions)(lql *self, const lql_selector *selector,
                                     FILE *file,
                                     lql_query_decision_fn on_decision,
                                     void *user, lql_query_result *out_result,
                                     lql_error *error);
  /* Streams seekable FILE candidates with explicit stop limits. */
  lql_status (*query_file_decisions_with_options)(
      lql *self, const lql_selector *selector, FILE *file,
      const lql_query_options *options, lql_query_decision_fn on_decision,
      void *user, lql_query_result *out_result, lql_error *error);
  /* Streams callback-source candidates without retaining payloads. */
  lql_status (*query_source_decisions)(lql *self, const lql_selector *selector,
                                       lql_read_fn read, void *read_user,
                                       lql_query_decision_fn on_decision,
                                       void *user, lql_query_result *out_result,
                                       lql_error *error);
  /* Streams callback-source candidates with explicit stop limits. */
  lql_status (*query_source_decisions_with_options)(
      lql *self, const lql_selector *selector, lql_read_fn read,
      void *read_user, const lql_query_options *options,
      lql_query_decision_fn on_decision, void *user,
      lql_query_result *out_result, lql_error *error);
  /* Streams seekable FILE matches as callback-scoped payload ranges. */
  lql_status (*query_file_matches)(lql *self, const lql_selector *selector,
                                   FILE *file, lql_query_match_fn on_match,
                                   void *user, lql_query_result *out_result,
                                   lql_error *error);
  /* Streams seekable FILE matches with explicit stop limits. */
  lql_status (*query_file_matches_with_options)(
      lql *self, const lql_selector *selector, FILE *file,
      const lql_query_options *options, lql_query_match_fn on_match, void *user,
      lql_query_result *out_result, lql_error *error);
  /* Streams callback-source matches as callback-scoped spooled payloads. */
  lql_status (*query_source_spooled_matches)(
      lql *self, const lql_selector *selector, lql_read_fn read,
      void *read_user, lql_query_match_fn on_match, void *user,
      lql_query_result *out_result, lql_error *error);
  /* Streams callback-source spooled matches with explicit stop limits. */
  lql_status (*query_source_spooled_matches_with_options)(
      lql *self, const lql_selector *selector, lql_read_fn read,
      void *read_user, const lql_query_options *options,
      lql_query_match_fn on_match, void *user, lql_query_result *out_result,
      lql_error *error);
  /* Writes a callback-scoped payload as compact JSON to FILE out. */
  lql_status (*payload_write_json)(lql *self, const lql_payload *payload,
                                   FILE *out, lql_error *error);
  /* Writes a callback-scoped payload to a caller-managed sink. */
  lql_status (*payload_write_json_sink)(lql *self, const lql_payload *payload,
                                        lql_write_fn write, void *write_user,
                                        lql_error *error);
  /* Projects a callback-scoped payload to FILE out. */
  lql_status (*payload_project_json)(lql *self, const lql_payload *payload,
                                     const lql_projection *projection,
                                     FILE *out, int *out_found,
                                     lql_error *error);
  /* Parses projection paths; caller destroys *out on success. */
  lql_status (*projection_parse)(lql *self, const char *const *fields,
                                 size_t field_count, lql_projection **out,
                                 lql_error *error);
  /* Destroys a projection handle; NULL is accepted. */
  void (*projection_destroy)(lql *self, lql_projection *projection);
  /* Projects one seekable FILE range to FILE out. */
  lql_status (*project_file_range)(lql *self, const lql_projection *projection,
                                   FILE *file, lql_uint64 offset,
                                   lql_uint64 size, FILE *out, int *out_found,
                                   lql_error *error);
  /* Projects one caller-provided source stream to FILE out. */
  lql_status (*project_source)(lql *self, const lql_projection *projection,
                               lql_read_fn read, void *read_user, FILE *out,
                               int *out_found, lql_error *error);
  /* Projects one caller-buffered JSON value to FILE out. */
  lql_status (*project_json)(lql *self, const lql_projection *projection,
                             const char *json, size_t json_len, FILE *out,
                             int *out_found, lql_error *error);
  /* Compacts one seekable FILE range to FILE out. */
  lql_status (*compact_file_range)(lql *self, FILE *file, lql_uint64 offset,
                                   lql_uint64 size, FILE *out,
                                   lql_error *error);
  /* Compacts one caller-provided source stream to FILE out. */
  lql_status (*compact_source)(lql *self, lql_read_fn read, void *read_user,
                               FILE *out, lql_error *error);
  /* Compacts one caller-buffered JSON value to FILE out. */
  lql_status (*compact_json)(lql *self, const char *json, size_t json_len,
                             FILE *out, lql_error *error);
  /* Parses mutation expressions; caller destroys *out on success. */
  lql_status (*mutation_plan_parse)(lql *self, const char *const *exprs,
                                    size_t expr_count, lql_mutation_plan **out,
                                    lql_error *error);
  /* Parses mutation expressions with explicit parse options. */
  lql_status (*mutation_plan_parse_with_options)(
      lql *self, const char *const *exprs, size_t expr_count,
      const lql_mutation_parse_options *options, lql_mutation_plan **out,
      lql_error *error);
  /* Returns the number of expanded mutation operations; NULL returns 0. */
  size_t (*mutation_plan_count)(const lql *self, const lql_mutation_plan *plan);
  /* Destroys a mutation plan handle; NULL is accepted. */
  void (*mutation_plan_destroy)(lql *self, lql_mutation_plan *plan);
  /* Mutates supported root fields in one seekable FILE range. */
  lql_status (*mutate_file_range_root_fields)(lql *self,
                                              const lql_mutation_plan *plan,
                                              FILE *file, lql_uint64 offset,
                                              lql_uint64 size, FILE *out,
                                              lql_error *error);
  /* Mutates supported paths in one seekable FILE range. */
  lql_status (*mutate_file_range_paths)(lql *self,
                                        const lql_mutation_plan *plan,
                                        FILE *file, lql_uint64 offset,
                                        lql_uint64 size, FILE *out,
                                        lql_error *error);
  /* Mutates a seekable candidate stream, preserving unmatched candidates. */
  lql_status (*mutate_file_range_candidates)(
      lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
      FILE *file, lql_uint64 offset, lql_uint64 size, FILE *out, int compact,
      int matches_only, lql_query_result *out_result, lql_error *error);
  /* Mutates a seekable candidate stream with explicit query stop limits. */
  lql_status (*mutate_file_range_candidates_with_options)(
      lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
      FILE *file, lql_uint64 offset, lql_uint64 size, FILE *out, int compact,
      int matches_only, const lql_query_options *options,
      lql_query_result *out_result, lql_error *error);
  /* Projects and mutates matched projections in a seekable candidate stream. */
  lql_status (*mutate_file_range_projected_candidates)(
      lql *self, const lql_selector *selector, const lql_projection *projection,
      const lql_mutation_plan *plan, FILE *file, lql_uint64 offset,
      lql_uint64 size, FILE *out, int compact, int matches_only,
      lql_query_result *out_result, lql_error *error);
  /* Projects and mutates a seekable candidate stream with stop limits. */
  lql_status (*mutate_file_range_projected_candidates_with_options)(
      lql *self, const lql_selector *selector, const lql_projection *projection,
      const lql_mutation_plan *plan, FILE *file, lql_uint64 offset,
      lql_uint64 size, FILE *out, int compact, int matches_only,
      const lql_query_options *options, lql_query_result *out_result,
      lql_error *error);
  /* Mutates supported paths in one caller-provided source stream. */
  lql_status (*mutate_source_paths)(lql *self, const lql_mutation_plan *plan,
                                    lql_read_fn read, void *read_user,
                                    FILE *out, lql_error *error);
  /* Mutates a callback-source candidate stream. */
  lql_status (*mutate_source_candidates)(
      lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
      lql_read_fn read, void *read_user, FILE *out, int compact,
      int matches_only, lql_query_result *out_result, lql_error *error);
  /* Mutates a callback-source candidate stream with explicit stop limits. */
  lql_status (*mutate_source_candidates_with_options)(
      lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
      lql_read_fn read, void *read_user, FILE *out, int compact,
      int matches_only, const lql_query_options *options,
      lql_query_result *out_result, lql_error *error);
  /* Projects and mutates matched projections in a source candidate stream. */
  lql_status (*mutate_source_projected_candidates)(
      lql *self, const lql_selector *selector, const lql_projection *projection,
      const lql_mutation_plan *plan, lql_read_fn read, void *read_user,
      FILE *out, int compact, int matches_only, lql_query_result *out_result,
      lql_error *error);
  /* Projects and mutates a source candidate stream with stop limits. */
  lql_status (*mutate_source_projected_candidates_with_options)(
      lql *self, const lql_selector *selector, const lql_projection *projection,
      const lql_mutation_plan *plan, lql_read_fn read, void *read_user,
      FILE *out, int compact, int matches_only,
      const lql_query_options *options, lql_query_result *out_result,
      lql_error *error);
  /* Mutates one caller-buffered JSON value to FILE out. */
  lql_status (*mutate_json)(lql *self, const lql_mutation_plan *plan,
                            const char *json, size_t json_len, FILE *out,
                            lql_error *error);
  /* Destroys the receiver; NULL behavior is undefined. */
  void (*destroy)(lql *self);
};

/* Allocates and initializes a liblql receiver. On failure, *out is NULL when
   out is non-NULL and error receives an actionable diagnostic. */
lql_status lql_new(lql **out, lql_error *error);
void lql_error_init(lql_error *error);
const char *lql_status_string(lql_status status);

#ifdef __cplusplus
}
#endif

#endif
