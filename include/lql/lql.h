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
  /* Mutation execution against caller-provided read callbacks is available. */
  int mutation_source;
  /* Mutation execution against caller-buffered JSON is available. */
  int mutation_buffered_json;
  /* Explicit opt-in file-backed mutation values are available. */
  int mutation_file_values;
} lql_capabilities;

typedef lql_status (*lql_query_decision_fn)(void *user,
                                            const lql_query_decision *decision);
typedef lql_status (*lql_query_match_fn)(void *user,
                                         const lql_query_match *match);
typedef lql_read_result (*lql_read_fn)(void *user, unsigned char *buffer,
                                       size_t capacity);
typedef lql_status (*lql_write_fn)(void *user, const void *data, size_t len);

void lql_error_init(lql_error *error);
const char *lql_status_string(lql_status status);
/* Returns the resolved liblql semantic version string. */
const char *lql_version(void);
/* Writes the supported public API capability set to out. NULL is accepted. */
void lql_capabilities_get(lql_capabilities *out);

lql_status lql_selector_parse(const char *expr, lql_selector **out,
                              lql_error *error);
lql_status lql_selector_parse_or(const char *expr, lql_selector **out,
                                 lql_error *error);
void lql_selector_free(lql_selector *selector);
int lql_selector_is_empty(const lql_selector *selector);

lql_status lql_matches_json(const lql_selector *selector, const char *json,
                            size_t json_len, int *out_matched,
                            lql_error *error);
lql_status lql_query_file_decisions(const lql_selector *selector, FILE *file,
                                    lql_query_decision_fn on_decision,
                                    void *user, lql_query_result *out_result,
                                    lql_error *error);
lql_status lql_query_file_decisions_with_options(
    const lql_selector *selector, FILE *file, const lql_query_options *options,
    lql_query_decision_fn on_decision, void *user, lql_query_result *out_result,
    lql_error *error);
/* Streams candidate decisions from a caller-provided source callback.
   This is a no-capture decision stream: callback decisions receive candidate
   offsets and byte sizes, but no payload handle is exposed for source readers.
 */
lql_status lql_query_source_decisions(const lql_selector *selector,
                                      lql_read_fn read, void *read_user,
                                      lql_query_decision_fn on_decision,
                                      void *user, lql_query_result *out_result,
                                      lql_error *error);
lql_status lql_query_source_decisions_with_options(
    const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error);
/* Calls on_match for each matched candidate in a caller-provided source
   callback stream. Match payloads are callback-scoped spooled handles; liblql
   does not retain payloads after on_match returns. */
lql_status lql_query_source_spooled_matches(const lql_selector *selector,
                                            lql_read_fn read, void *read_user,
                                            lql_query_match_fn on_match,
                                            void *user,
                                            lql_query_result *out_result,
                                            lql_error *error);
lql_status lql_query_source_spooled_matches_with_options(
    const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error);
/* Calls on_match for each matched candidate in a seekable FILE * stream.
   Match payloads are callback-scoped seekable ranges; liblql does not capture
   or retain candidate JSON. */
lql_status lql_query_file_matches(const lql_selector *selector, FILE *file,
                                  lql_query_match_fn on_match, void *user,
                                  lql_query_result *out_result,
                                  lql_error *error);
lql_status lql_query_file_matches_with_options(
    const lql_selector *selector, FILE *file, const lql_query_options *options,
    lql_query_match_fn on_match, void *user, lql_query_result *out_result,
    lql_error *error);
/* Writes a callback-scoped payload to out. Seekable range payloads preserve the
   source FILE * position; spooled payload handles are valid only for the active
   match callback. */
lql_status lql_payload_write_json(const lql_payload *payload, FILE *out,
                                  lql_error *error);
/* Writes a callback-scoped payload to a caller-managed sink callback. The sink
   receives bounded chunks and must return LQL_STATUS_OK to continue. */
lql_status lql_payload_write_json_sink(const lql_payload *payload,
                                       lql_write_fn write, void *write_user,
                                       lql_error *error);

/* Parses JSON Pointer projection fields into a caller-owned projection handle.
   The root path is rejected, and paths must not start with an array index. */
lql_status lql_projection_parse(const char *const *fields, size_t field_count,
                                lql_projection **out, lql_error *error);
/* Frees a projection handle. NULL is accepted. */
void lql_projection_free(lql_projection *projection);
/* Projects one seekable file range to out. Missing fields write no bytes and
   set out_found to 0; selected values are streamed to out as they are visited.
 */
lql_status lql_project_file_range(const lql_projection *projection, FILE *file,
                                  lql_uint64 offset, lql_uint64 size, FILE *out,
                                  int *out_found, lql_error *error);
/* Projects one JSON value read from a caller-provided source callback to out.
   This streams the source through lonejson and never materializes the document.
   The read callback follows the same contract as source query APIs:
   bytes_read must not exceed capacity, eof marks source completion, and
   non-zero error_code aborts with LQL_STATUS_JSON_ERROR. */
lql_status lql_project_source(const lql_projection *projection,
                              lql_read_fn read, void *read_user, FILE *out,
                              int *out_found, lql_error *error);
/* Projects one caller-buffered JSON value to out. This is an explicitly
   buffered helper; use source-backed APIs for large values that must not be
   materialized by the caller. */
lql_status lql_project_json(const lql_projection *projection, const char *json,
                            size_t json_len, FILE *out, int *out_found,
                            lql_error *error);
/* Compacts one seekable file range to out by streaming it through lonejson.
   The caller owns file positioning before and after the call. */
lql_status lql_compact_file_range(FILE *file, lql_uint64 offset,
                                  lql_uint64 size, FILE *out, lql_error *error);
/* Compacts one JSON value read from a caller-provided source callback to out.
   This streams the source through lonejson and never materializes the document.
   The read callback follows the same contract as source query APIs:
   bytes_read must not exceed capacity, eof marks source completion, and
   non-zero error_code aborts with LQL_STATUS_JSON_ERROR. */
lql_status lql_compact_source(lql_read_fn read, void *read_user, FILE *out,
                              lql_error *error);
/* Compacts one in-memory JSON value to out. */
lql_status lql_compact_json(const char *json, size_t json_len, FILE *out,
                            lql_error *error);
/* Parses CLI-style mutation expressions into a caller-owned mutation plan.
   This validates the mutation language only; execution is a separate API. */
lql_status lql_mutation_plan_parse(const char *const *exprs, size_t expr_count,
                                   lql_mutation_plan **out, lql_error *error);
/* Parses mutation expressions with explicit opt-in behavior for local
   file-backed mutation values. Relative file-backed value paths require
   file_value_base_dir when enable_file_values is non-zero. */
lql_status
lql_mutation_plan_parse_with_options(const char *const *exprs,
                                     size_t expr_count,
                                     const lql_mutation_parse_options *options,
                                     lql_mutation_plan **out, lql_error *error);
/* Returns the number of parsed mutation operations in a plan. */
size_t lql_mutation_plan_count(const lql_mutation_plan *plan);
/* Frees a mutation plan. NULL is accepted. */
void lql_mutation_plan_free(lql_mutation_plan *plan);
/* Applies supported root-object field mutations to one seekable file range.
   This is a streaming rewrite for root object fields only; unsupported plans
   return LQL_STATUS_UNSUPPORTED rather than materializing the document. */
lql_status lql_mutate_file_range_root_fields(const lql_mutation_plan *plan,
                                             FILE *file, lql_uint64 offset,
                                             lql_uint64 size, FILE *out,
                                             lql_error *error);
/* Applies supported concrete-path mutations to one seekable file range.
   This streams the source through lonejson and never materializes the document.
   Supported paths are concrete object/member paths with optional concrete array
   indexes plus existing-position `*` object-child and `[]` array-element
   wildcards. Existing object-member and array-element mutation positions also
   support `**` one-child and `...` recursive path segments. File-backed values
   stream from source-backed paths. */
lql_status lql_mutate_file_range_paths(const lql_mutation_plan *plan,
                                       FILE *file, lql_uint64 offset,
                                       lql_uint64 size, FILE *out,
                                       lql_error *error);
/* Applies supported concrete-path mutations to one JSON value read from a
   caller-provided source callback. This streams the source through lonejson and
   never materializes the document. The read callback follows the same contract
   as source query APIs: bytes_read must not exceed capacity, eof marks source
   completion, and non-zero error_code aborts with LQL_STATUS_JSON_ERROR. */
lql_status lql_mutate_source_paths(const lql_mutation_plan *plan,
                                   lql_read_fn read, void *read_user, FILE *out,
                                   lql_error *error);
/* Applies supported concrete-path mutations to one caller-buffered JSON value.
   This is an explicitly buffered helper; use file/source-backed APIs for large
   values that must not be materialized by the caller. */
lql_status lql_mutate_json(const lql_mutation_plan *plan, const char *json,
                           size_t json_len, FILE *out, lql_error *error);

char *lql_strdup(const char *text);
void lql_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
