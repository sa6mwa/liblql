#ifndef LQL_LQL_H
#define LQL_LQL_H

#include <lql/version.h>

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Status returned by every fallible liblql operation. */
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

/**
 * Caller-provided diagnostic storage. Pass a non-NULL error object to receive
 * a status and a NUL-terminated, non-owned message on failure. Call
 * `lql_error_init` before first use or reuse.
 */
typedef struct lql_error {
  /** Status associated with `message`; LQL_STATUS_OK after initialization. */
  lql_status code;
  /** NUL-terminated diagnostic owned by this error object. */
  char message[256];
} lql_error;

typedef struct lql_selector lql_selector;
typedef struct lql lql;
typedef struct lql_projection lql_projection;
typedef struct lql_mutation lql_mutation;
typedef struct lql_stream_value lql_stream_value;

/** Runtime capabilities of this liblql build; zero means unavailable. */
typedef struct lql_capabilities {
  int selector_parse;
  int selector_inspection;
} lql_capabilities;

/** Selector features represented by one parsed or constructed selector. */
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

/**
 * Non-owning UTF-8 or byte-string view. The referenced storage remains owned
 * by the input or liblql object that produced it and must not be freed by the
 * caller.
 */
typedef struct lql_string_view {
  const char *data;
  size_t len;
} lql_string_view;

/**
 * Reads up to `capacity` bytes into the library-owned `buffer`. Set `out_len`
 * to the number written; a successful zero-byte read signals EOF. The callback
 * must not retain `buffer`, and any non-OK status terminates execution.
 */
typedef lql_status (*lql_stream_reader_fn)(void *user, unsigned char *buffer,
                                           size_t capacity, size_t *out_len,
                                           lql_error *error);

/**
 * Consumes exactly one output chunk before returning. The library never retries
 * a short write: return a non-OK status if all `len` bytes cannot be accepted.
 * `data` is borrowed for the duration of the callback only.
 */
typedef lql_status (*lql_stream_writer_fn)(void *user, const void *data,
                                           size_t len, lql_error *error);

/**
 * Writes an immutable, caller-owned source byte range through `writer`.
 * `lql_stream_execute` uses this adapter to deliver selected compact records
 * without retaining them. `offset` and `len` identify the original input byte
 * range, excluding inter-record separators. The adapter must keep that source
 * available until the reader has reached EOF or execution returns, and must
 * propagate writer failures unchanged.
 */
typedef lql_status (*lql_stream_range_writer_fn)(
    void *user, size_t offset, size_t len, lql_stream_writer_fn writer,
    void *writer_user, lql_error *error);

/** Controls continuation after a decision or value callback. */
typedef enum lql_stream_callback_result {
  LQL_STREAM_CALLBACK_CONTINUE = 0,
  LQL_STREAM_CALLBACK_STOP = 1,
  LQL_STREAM_CALLBACK_ERROR = 2
} lql_stream_callback_result;

/** Requested record-output transformation. Zero-initialization selects decisions only. */
typedef enum lql_stream_output_mode {
  LQL_STREAM_OUTPUT_DECISION_ONLY = 0,
  LQL_STREAM_OUTPUT_SELECTED_RECORD = 1,
  LQL_STREAM_OUTPUT_PROJECTION = 2,
  LQL_STREAM_OUTPUT_MUTATION = 3,
  LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION = 4
} lql_stream_output_mode;

/** Observable reason an otherwise successful execution stopped early. */
typedef enum lql_stream_stop_reason {
  LQL_STREAM_STOP_NONE = 0,
  LQL_STREAM_STOP_RECORD_LIMIT = 1,
  LQL_STREAM_STOP_MATCH_LIMIT = 2,
  LQL_STREAM_STOP_BYTE_LIMIT = 3,
  LQL_STREAM_STOP_CALLBACK = 4
} lql_stream_stop_reason;

/** Final selection result for one zero-based NDJSON record index. */
typedef struct lql_stream_decision {
  size_t record_index;
  int matched;
} lql_stream_decision;

/**
 * Invoked after a complete record has been validated and evaluated. Return
 * STOP for a successful early stop or CALLBACK_ERROR after setting `error`.
 * `decision` is borrowed and valid only for the callback.
 */
typedef lql_stream_callback_result (*lql_stream_decision_fn)(
    void *user, const lql_stream_decision *decision, lql_error *error);

/**
 * Receives one completed matched record. The value is callback-scoped.
 * With `lql_stream_execute`, values are caller-owned compact source ranges.
 * `lql_stream_execute_spooled` may instead retain a compact record and spill
 * it to a temporary file. Return STOP for a successful early stop or
 * CALLBACK_ERROR after setting `error`; `value` is invalid on return.
 */
typedef lql_stream_callback_result (*lql_stream_value_fn)(
    void *user, const lql_stream_value *value, lql_error *error);

/** Execution limits; zero for any member means unlimited. */
typedef struct lql_stream_limits {
  size_t max_records;
  size_t max_matches;
  size_t max_bytes;
} lql_stream_limits;

/**
 * Zero-initializable execution request. The caller owns every referenced
 * handle and callback context for the full call. `reader` is required; a
 * non-decision output mode requires `writer`.
 */
typedef struct lql_stream_request {
  /** Required input source; successful zero-byte read is EOF. */
  lql_stream_reader_fn reader;
  /** Opaque context passed to `reader`; never retained after execution. */
  void *reader_user;
  /** Optional caller-owned source replay adapter for true-stream selected values. */
  lql_stream_range_writer_fn range_writer;
  /** Opaque context passed to `range_writer`; never retained after execution. */
  void *range_user;
  /**
   * Non-zero asserts each JSON record is compact (no structural whitespace).
   * `lql_stream_execute` verifies this before replay; a false assertion fails
   * with UNSUPPORTED after validation and emits no part of that record.
   */
  int input_is_compact;
  /** Required for every non-decision output mode; consumes full chunks. */
  lql_stream_writer_fn writer;
  /** Opaque context passed to `writer`; never retained after execution. */
  void *writer_user;
  /** Optional compiled selector; NULL selects every valid record. */
  const lql_selector *selector;
  /** Required only for projection output modes; caller retains ownership. */
  const lql_projection *projection;
  /** Required only for mutation output modes; caller retains ownership. */
  const lql_mutation *mutation;
  /** Zero selects decision-only execution. */
  lql_stream_output_mode output_mode;
  /** Non-zero suppresses unmatched record output; it does not suppress decisions. */
  int matched_only;
  /** Optional record, match, and byte limits; zero values are unlimited. */
  lql_stream_limits limits;
  /** Optional final-decision callback for every accepted record. */
  lql_stream_decision_fn on_decision;
  /** Opaque context passed to `on_decision`. */
  void *decision_user;
  /** Optional final-value callback for each matched record. */
  lql_stream_value_fn on_value;
  /** Opaque context passed to `on_value`. */
  void *value_user;
} lql_stream_request;

/** Zeroed before execution and filled with final counters on return. */
typedef struct lql_stream_result {
  /** Number of fully validated records delivered to the decision stage. */
  size_t records_seen;
  /** Number of `records_seen` records whose selector matched. */
  size_t records_matched;
  /** Bytes consumed from `reader`, including NDJSON separators read. */
  size_t bytes_consumed;
  /** Non-zero only when execution returned success after a configured stop. */
  int stopped_early;
  /** Reason for `stopped_early`, otherwise LQL_STREAM_STOP_NONE. */
  lql_stream_stop_reason stop_reason;
} lql_stream_result;

/** Kinds exposed by the borrowed selector-node traversal API. */
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

/** Discriminant for one range bound. */
typedef enum lql_selector_bound_kind {
  LQL_SELECTOR_BOUND_ABSENT = 0,
  LQL_SELECTOR_BOUND_NUMBER = 1,
  LQL_SELECTOR_BOUND_DATETIME = 2
} lql_selector_bound_kind;

/** Normalized form of a date term's `since` value. */
typedef enum lql_selector_since_kind {
  LQL_SELECTOR_SINCE_NONE = 0,
  LQL_SELECTOR_SINCE_NOW = 1,
  LQL_SELECTOR_SINCE_TODAY = 2,
  LQL_SELECTOR_SINCE_YESTERDAY = 3,
  LQL_SELECTOR_SINCE_LITERAL = 4
} lql_selector_since_kind;

/**
 * Borrowed selector AST cursor. It remains valid only while its owning
 * selector is alive and unchanged; callers must not inspect `impl`.
 */
typedef struct lql_selector_node {
  lql_selector_node_kind kind;
  const void *impl;
} lql_selector_node;

/** Borrowed details for string-like selector terms. */
typedef struct lql_selector_string_term {
  lql_string_view field;
  int value_present;
  lql_string_view value;
  int ignore_case;
  size_t any_count;
} lql_selector_string_term;

/** One numeric or datetime range bound; ABSENT ignores its other fields. */
typedef struct lql_selector_range_bound {
  lql_selector_bound_kind kind;
  double number;
  lql_string_view datetime;
} lql_selector_range_bound;

/** Borrowed details for a range selector term. */
typedef struct lql_selector_range_term {
  lql_string_view field;
  lql_selector_range_bound gt;
  lql_selector_range_bound gte;
  lql_selector_range_bound lt;
  lql_selector_range_bound lte;
} lql_selector_range_term;

/** Borrowed details for a date selector term. */
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

/** Borrowed details for an `in` selector term. */
typedef struct lql_selector_in_term {
  lql_string_view field;
  size_t any_count;
} lql_selector_in_term;

/**
 * Public receiver shell returned by `lql_new`. Every method pointer is
 * initialized on success and takes this receiver first. Methods returning
 * selector, projection, or mutation handles transfer ownership to the caller;
 * destroy methods accept NULL. All inspection views borrow their owning handle.
 */
struct lql {
  /** Private implementation pointer; callers must not read or modify it. */
  void *impl;
  /** Returns a static version string owned by liblql. */
  const char *(*version)(const lql *self);
  /** Writes build capabilities to `out`; `out` is required. */
  void (*capabilities_get)(const lql *self, lql_capabilities *out);
  /** Parses an AND-combined selector expression and transfers `*out` on success. */
  lql_status (*selector_parse)(lql *self, const char *expr, lql_selector **out,
                               lql_error *error);
  /** Parses an OR-combined selector expression and transfers `*out` on success. */
  lql_status (*selector_parse_or)(lql *self, const char *expr,
                                  lql_selector **out, lql_error *error);
  /** Parses Go-compatible selector JSON and transfers `*out` on success. */
  lql_status (*selector_parse_json)(lql *self, const void *json,
                                    size_t json_len, lql_selector **out,
                                    lql_error *error);
  /** Releases a selector returned by parsing or construction; accepts NULL. */
  void (*selector_destroy)(lql *self, lql_selector *selector);
  /** Parses projection paths and transfers `*out` on success. */
  lql_status (*projection_parse)(lql *self, const char *const *paths,
                                 size_t path_count, lql_projection **out,
                                 lql_error *error);
  /** Releases a projection returned by `projection_parse`; accepts NULL. */
  void (*projection_destroy)(lql *self, lql_projection *projection);
  /** Returns the number of normalized paths in `projection`. */
  size_t (*projection_path_count)(const lql *self,
                                  const lql_projection *projection);
  /** Writes a borrowed normalized projection path at `index` to `out`. */
  lql_status (*projection_path)(const lql *self,
                                const lql_projection *projection,
                                size_t index, lql_string_view *out,
                                lql_error *error);
  /**
   * Parses mutation expressions and transfers `*out` on success. Matching
   * outer single or double quotes delimit a string value; their contents are
   * literal LQL text, not JSON escape syntax.
   */
  lql_status (*mutation_parse)(lql *self,
                               const char *const *expressions,
                               size_t expression_count,
                               lql_mutation **out, lql_error *error);
  /** Releases a mutation returned by `mutation_parse`; accepts NULL. */
  void (*mutation_destroy)(lql *self, lql_mutation *mutation);
  /** Returns the number of parsed actions in `mutation`. */
  size_t (*mutation_count)(const lql *self, const lql_mutation *mutation);
  /**
   * Executes strict NDJSON with real producer-to-consumer streaming. Selected
   * output requires caller-owned compact source-range replay; projection and
   * mutation require incremental emitters and otherwise return UNSUPPORTED.
   */
  lql_status (*stream_execute)(lql *self, const lql_stream_request *request,
                               lql_stream_result *result, lql_error *error);
  /**
   * Executes strict NDJSON through the explicitly materialized compatibility
   * path, which may spill one current record to a temporary file.
   */
  lql_status (*stream_execute_spooled)(lql *self,
                                       const lql_stream_request *request,
                                       lql_stream_result *result,
                                       lql_error *error);
  /** Returns non-zero when `selector` is the match-all selector. */
  int (*selector_is_empty)(const lql *self, const lql_selector *selector);
  /** Writes capability flags for `selector` to `out`; `out` is required. */
  void (*selector_capabilities_get)(const lql *self,
                                    const lql_selector *selector,
                                    lql_selector_capabilities *out);
  /** Writes the borrowed root cursor of `selector` to `out`. */
  lql_status (*selector_root)(const lql *self, const lql_selector *selector,
                              lql_selector_node *out, lql_error *error);
  /** Writes the child count of a compound node to `out`. */
  lql_status (*selector_node_child_count)(const lql *self,
                                          lql_selector_node node,
                                          size_t *out_count, lql_error *error);
  /** Writes the borrowed child cursor at `index` to `out`. */
  lql_status (*selector_node_child)(const lql *self, lql_selector_node node,
                                    size_t index, lql_selector_node *out,
                                    lql_error *error);
  /** Writes borrowed details for a string-like term node to `out`. */
  lql_status (*selector_node_string_term)(const lql *self,
                                          lql_selector_node node,
                                          lql_selector_string_term *out,
                                          lql_error *error);
  /** Writes one borrowed `any` member for a string-like term to `out`. */
  lql_status (*selector_node_string_term_any)(const lql *self,
                                              lql_selector_node node,
                                              size_t index,
                                              lql_string_view *out,
                                              lql_error *error);
  /** Writes borrowed details for a range term node to `out`. */
  lql_status (*selector_node_range_term)(const lql *self,
                                         lql_selector_node node,
                                         lql_selector_range_term *out,
                                         lql_error *error);
  /** Writes borrowed details for a date term node to `out`. */
  lql_status (*selector_node_date_term)(const lql *self, lql_selector_node node,
                                        lql_selector_date_term *out,
                                        lql_error *error);
  /** Writes borrowed details for an `in` term node to `out`. */
  lql_status (*selector_node_in_term)(const lql *self, lql_selector_node node,
                                      lql_selector_in_term *out,
                                      lql_error *error);
  /** Writes one borrowed `any` member for an `in` term node to `out`. */
  lql_status (*selector_node_in_term_any)(const lql *self,
                                          lql_selector_node node, size_t index,
                                          lql_string_view *out,
                                          lql_error *error);
  /** Writes the borrowed JSON Pointer path of an `exists` node to `out`. */
  lql_status (*selector_node_exists_path)(const lql *self,
                                          lql_selector_node node,
                                          lql_string_view *out,
                                          lql_error *error);
  /** Serializes `selector` as Go-compatible JSON to caller-owned `out`. */
  lql_status (*selector_write_json)(lql *self, const lql_selector *selector,
                                    FILE *out, lql_error *error);
  /** Builds and transfers the match-all selector to `*out`. */
  lql_status (*selector_build_all)(lql *self, lql_selector **out,
                                   lql_error *error);
  /** Builds an AND or OR selector from borrowed child selectors and transfers `*out`. */
  lql_status (*selector_build_compound)(lql *self, lql_selector_node_kind kind,
                                        const lql_selector *const *children,
                                        size_t child_count, lql_selector **out,
                                        lql_error *error);
  /** Builds a NOT selector from a borrowed child and transfers `*out`. */
  lql_status (*selector_build_not)(lql *self, const lql_selector *child,
                                   lql_selector **out, lql_error *error);
  /** Builds a string-like selector term and transfers `*out`. */
  lql_status (*selector_build_string)(lql *self, lql_selector_node_kind kind,
                                      const lql_selector_string_term *term,
                                      const lql_string_view *any_values,
                                      lql_selector **out, lql_error *error);
  /** Builds a range selector term and transfers `*out`. */
  lql_status (*selector_build_range)(lql *self,
                                     const lql_selector_range_term *term,
                                     lql_selector **out, lql_error *error);
  /** Builds a date selector term and transfers `*out`. */
  lql_status (*selector_build_date)(lql *self,
                                    const lql_selector_date_term *term,
                                    lql_selector **out, lql_error *error);
  /** Builds an `in` selector term and transfers `*out`. */
  lql_status (*selector_build_in)(lql *self, const lql_selector_in_term *term,
                                  const lql_string_view *any_values,
                                  lql_selector **out, lql_error *error);
  /** Builds an `exists` selector from a borrowed JSON Pointer and transfers `*out`. */
  lql_status (*selector_build_exists)(lql *self, lql_string_view path,
                                      lql_selector **out, lql_error *error);
  /** Releases this receiver and every resource it owns; accepts NULL. */
  void (*destroy)(lql *self);
};

/** Allocates and initializes a receiver; caller releases it with `destroy`. */
lql_status lql_new(lql **out, lql_error *error);
/** Clears an error object to LQL_STATUS_OK and an empty message. */
void lql_error_init(lql_error *error);
/** Returns a static, non-owning string for `status`, including unknown values. */
const char *lql_status_string(lql_status status);

/**
 * Compatibility entry point for `self->stream_execute(self, ...)`. Executes
 * strict NDJSON with real producer-to-consumer streaming. Root arrays
 * are rejected. Decision-only execution never retains a record. Selected
 * output and value callbacks require `input_is_compact` plus `range_writer`,
 * so liblql can replay caller-owned source ranges only after validation.
 * Projection and mutation output return LQL_STATUS_UNSUPPORTED until they have
 * an incremental emitter. Invalid arguments clear `result` before return.
 */
lql_status lql_stream_execute(lql *self, const lql_stream_request *request,
                              lql_stream_result *result, lql_error *error);

/**
 * Compatibility entry point for `self->stream_execute_spooled(self, ...)`.
 * Executes strict NDJSON with materialized record handling. This explicit
 * compatibility API may retain one compact record and spill it to a temporary
 * file for selected output, callbacks, projection, or mutation. Do not use it
 * for sensitive inputs or where end-to-end streaming is required.
 */
lql_status lql_stream_execute_spooled(lql *self,
                                      const lql_stream_request *request,
                                      lql_stream_result *result,
                                      lql_error *error);

/** Returns the compact JSON size of one callback-scoped value. */
size_t lql_stream_value_size(const lql_stream_value *value);
/** Streams one callback-scoped value through a caller writer. */
lql_status lql_stream_value_write_to(const lql_stream_value *value,
                                     lql_stream_writer_fn writer,
                                     void *writer_user, lql_error *error);

#ifdef __cplusplus
}
#endif

#endif
