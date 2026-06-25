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

typedef struct lql_query_decision {
  int matched;
  lql_uint64 index;
  lql_uint64 offset;
  lql_uint64 size;
} lql_query_decision;

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

typedef struct lql_query_result {
  lql_uint64 candidates_seen;
  lql_uint64 candidates_matched;
  lql_uint64 bytes_read;
  int stopped_early;
  lql_query_stop_reason stop_reason;
} lql_query_result;

typedef lql_status (*lql_query_decision_fn)(void *user,
                                            const lql_query_decision *decision);

void lql_error_init(lql_error *error);
const char *lql_status_string(lql_status status);

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

char *lql_strdup(const char *text);
void lql_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
