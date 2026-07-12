#ifndef LQL_JSON_SCAN_H
#define LQL_JSON_SCAN_H

#include <lql/lql.h>

#include "lql_json_spool.h"

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

typedef lql_status (*lql_json_flat_eq_record_fn)(
    void *user, size_t record_index, int root_is_object, int matched,
    const lql_json_spool *spool, lql_error *error);

typedef struct lql_json_flat_eq_request {
  lql_stream_reader_fn reader;
  void *reader_user;
  const char *field;
  size_t field_len;
  const char *value;
  size_t value_len;
  lql_json_spool *spool;
  int capture;
  lql_json_flat_eq_record_fn record;
  void *record_user;
} lql_json_flat_eq_request;

/*
 * Scans strict NDJSON for a top-level string equality.  The current root is
 * compacted into `spool` and is callback-scoped.  A record callback receives
 * only fully validated roots; returning LQL_STATUS_STOP ends cleanly.
 */
lql_status lql_json_scan_flat_eq_ndjson(const lql_json_flat_eq_request *request,
                                        size_t *out_records,
                                        size_t *out_bytes_read,
                                        lql_error *error);

#endif
