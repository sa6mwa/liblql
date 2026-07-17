#ifndef LQL_JSON_SCAN_H
#define LQL_JSON_SCAN_H

#include <lql/lql.h>

#include <limits.h>

#include "lql_json_spool.h"
#include "lql_temporal_internal.h"

/*
 * Normalizes strict NDJSON through a bounded reader.  Each accepted root is
 * emitted as compact JSON followed by one newline.  Root arrays are rejected;
 * object and scalar roots are valid JSON records.  This is private plumbing
 * for the direct executor, not a public parser API.
 */
typedef struct lql_json_normalize_request {
  lql_stream_reader_fn reader;
  void *reader_user;
  lql_stream_writer_fn writer;
  void *writer_user;
} lql_json_normalize_request;

lql_status lql_json_normalize_ndjson(const lql_json_normalize_request *request,
                                     size_t *out_records,
                                     size_t *out_bytes_read, lql_error *error);

typedef enum lql_json_flat_term_kind {
  LQL_JSON_FLAT_TERM_EQ = 0,
  LQL_JSON_FLAT_TERM_EXISTS = 1,
  LQL_JSON_FLAT_TERM_PREFIX = 2,
  LQL_JSON_FLAT_TERM_NUMBER_EQ = 3,
  LQL_JSON_FLAT_TERM_BOOL_EQ = 4,
  LQL_JSON_FLAT_TERM_NULL_EQ = 5,
  LQL_JSON_FLAT_TERM_CONTAINS = 6,
  LQL_JSON_FLAT_TERM_ICONTAINS = 7,
  LQL_JSON_FLAT_TERM_IPREFIX = 8,
  LQL_JSON_FLAT_TERM_NUMBER_RANGE = 9,
  LQL_JSON_FLAT_TERM_TEMPORAL_RANGE = 10,
  LQL_JSON_FLAT_TERM_TEXT_EQ = 11
} lql_json_flat_term_kind;

#define LQL_JSON_PATH_SEGMENT_CAPACITY (sizeof(unsigned long) * CHAR_BIT)

typedef struct lql_json_flat_eq_term {
  lql_json_flat_term_kind kind;
  const char *field;
  size_t field_len;
  const char *value;
  size_t value_len;
  /* Optional literal JSON Pointer metadata for nested object paths. */
  const char *path;
  size_t path_len;
  size_t path_segment_count;
  unsigned long path_array_segments;
  unsigned long path_object_wildcards;
  unsigned long path_array_wildcards;
  unsigned long path_any_wildcards;
  unsigned long path_recursive_segments;
  const char *path_recursive_match;
  size_t path_recursive_match_len;
  size_t path_recursive_match_segment;
  double range_gt;
  double range_gte;
  double range_lt;
  double range_lte;
  int has_range_gt;
  int has_range_gte;
  int has_range_lt;
  int has_range_lte;
  lql_temporal temporal_gt;
  lql_temporal temporal_gte;
  lql_temporal temporal_lt;
  lql_temporal temporal_lte;
  lql_temporal temporal_eq;
  int has_temporal_gt;
  int has_temporal_gte;
  int has_temporal_lt;
  int has_temporal_lte;
  int has_temporal_eq;
  /* Per-execution KMP table for streaming string contains matching. */
  size_t *contains_failure;
  /* Optional cached numeric path segment indexes. */
  unsigned long path_array_index_cache;
  size_t path_array_index_values[LQL_JSON_PATH_SEGMENT_CAPACITY];
} lql_json_flat_eq_term;

typedef struct lql_json_capture_key {
  const char *const *segments;
  size_t segment_count;
} lql_json_capture_key;

typedef struct lql_json_capture_span {
  size_t offset;
  size_t len;
  int found;
} lql_json_capture_span;

typedef lql_status (*lql_json_flat_eq_record_fn)(
    void *user, size_t record_index, int root_is_object, unsigned long hits,
    const lql_json_spool *spool, size_t source_offset, size_t source_len,
    int source_compact, lql_error *error);

typedef struct lql_json_flat_eq_request {
  lql_stream_reader_fn reader;
  void *reader_user;
  const lql_json_flat_eq_term *terms;
  size_t term_count;
  lql_json_spool *spool;
  int capture;
  int stop_matching_on_hit;
  unsigned long stop_hit_mask;
  const lql_json_capture_key *capture_keys;
  size_t capture_key_count;
  lql_json_capture_span *capture_spans;
  size_t max_records;
  size_t max_bytes;
  lql_stream_cancel_fn cancelled;
  void *cancel_user;
  int *out_cancelled;
  lql_json_flat_eq_record_fn record;
  void *record_user;
} lql_json_flat_eq_request;

/*
 * Scans strict NDJSON for bounded literal object-path predicates.  The current
 * root is compacted into `spool` and is callback-scoped.  A record callback
 * receives hit bits only after full validation; returning LQL_STATUS_STOP ends
 * cleanly.
 */
lql_status lql_json_scan_flat_eq_ndjson(const lql_json_flat_eq_request *request,
                                        size_t *out_records,
                                        size_t *out_bytes_read,
                                        lql_error *error);

#endif
