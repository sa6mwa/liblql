#ifndef LQL_JSON_SCAN_H
#define LQL_JSON_SCAN_H

#include <lql/lql.h>

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

#endif
