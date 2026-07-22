#include "lql_internal.h"
#include "lql_json_spool.h"

#include <string.h>

size_t lql_stream_value_size(const lql_stream_value *value) {
  if (value == NULL) {
    return 0u;
  }
  if (value->storage_kind == LQL_STREAM_VALUE_SOURCE_RANGE) {
    return value->range_len;
  }
  if (value->storage_kind == LQL_STREAM_VALUE_JSON_SPOOL &&
      value->spool != NULL) {
    return lql_json_spool_size((const lql_json_spool *)value->spool);
  }
  return 0u;
}

lql_status lql_stream_value_write_to(const lql_stream_value *value,
                                     lql_stream_writer_fn writer,
                                     void *writer_user, lql_error *error) {
  lql_status status;
  lql_error_init(error);
  if (value == NULL || writer == NULL ||
      (value->storage_kind != LQL_STREAM_VALUE_SOURCE_RANGE &&
       value->spool == NULL)) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "stream value and writer are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (value->storage_kind == LQL_STREAM_VALUE_SOURCE_RANGE) {
    if (value->range_writer == NULL) {
      lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                    "stream value source range is unavailable");
      return LQL_STATUS_INVALID_ARGUMENT;
    }
    status = value->range_writer(value->range_user, value->range_offset,
                                 value->range_len, writer, writer_user, error);
    if (status != LQL_STATUS_OK && error != NULL &&
        error->code == LQL_STATUS_OK) {
      lql_set_error(error, status, "stream value range writer failed");
    }
    return status;
  }
  if (value->storage_kind == LQL_STREAM_VALUE_JSON_SPOOL) {
    status = lql_json_spool_write_to((const lql_json_spool *)value->spool,
                                     writer, writer_user, error);
    if (status != LQL_STATUS_OK && error != NULL &&
        error->code == LQL_STATUS_OK) {
      lql_set_error(error, status, "stream value writer failed");
    }
    return status;
  }
  lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "invalid stream value");
  return LQL_STATUS_INVALID_ARGUMENT;
}

static lql_status lql_stream_apply_common(lql *self,
                                          const lql_stream_request *request,
                                          lql_stream_result *result,
                                          lql_error *error, int allow_spool) {
  lql_status status;
  int handled;
  if (result != NULL) {
    memset(result, 0, sizeof(*result));
  }
  lql_error_init(error);
  if (self == NULL || request == NULL || result == NULL ||
      request->reader == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "stream receiver, request, result, and reader are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if ((request->output_mode != LQL_STREAM_OUTPUT_DECISION_ONLY &&
       request->output_mode != LQL_STREAM_OUTPUT_SELECTED_RECORD &&
       request->output_mode != LQL_STREAM_OUTPUT_PROJECTION &&
       request->output_mode != LQL_STREAM_OUTPUT_MUTATION &&
       request->output_mode != LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION) ||
      (request->mutation != NULL &&
       request->output_mode != LQL_STREAM_OUTPUT_MUTATION &&
       request->output_mode != LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION)) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream output mode is not implemented");
    return LQL_STATUS_UNSUPPORTED;
  }
  if ((request->output_mode == LQL_STREAM_OUTPUT_SELECTED_RECORD ||
       request->output_mode == LQL_STREAM_OUTPUT_PROJECTION ||
       request->output_mode == LQL_STREAM_OUTPUT_MUTATION ||
       request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION) &&
      request->writer == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "record output requires a writer");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (request->output_mode == LQL_STREAM_OUTPUT_PROJECTION &&
      request->projection == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection output requires a projection handle");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (request->output_mode == LQL_STREAM_OUTPUT_MUTATION &&
      request->mutation == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "mutation output requires a mutation handle");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION &&
      (request->projection == NULL || request->mutation == NULL)) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "combined output requires projection and mutation handles");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (!allow_spool &&
      (request->output_mode == LQL_STREAM_OUTPUT_PROJECTION ||
       request->output_mode == LQL_STREAM_OUTPUT_MUTATION ||
       request->output_mode == LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION)) {
    lql_set_error(
        error, LQL_STATUS_UNSUPPORTED,
        "streaming projection and mutation require an incremental emitter");
    return LQL_STATUS_UNSUPPORTED;
  }
  if (!allow_spool &&
      (request->output_mode == LQL_STREAM_OUTPUT_SELECTED_RECORD ||
       request->on_value != NULL) &&
      (!request->input_is_compact || request->range_writer == NULL)) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "streaming selected values require compact input and a "
                  "source range writer");
    return LQL_STATUS_UNSUPPORTED;
  }
  handled = 0;
  status = lql_stream_apply_flat_eq(self, request, result, error, allow_spool,
                                    &handled);
  if (handled) {
    return status;
  }
  lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                "direct stream selector is not implemented by scanner");
  return LQL_STATUS_UNSUPPORTED;
}

lql_status lql_stream_apply(lql *self, const lql_stream_request *request,
                            lql_stream_result *result, lql_error *error) {
  return lql_stream_apply_common(self, request, result, error, 0);
}

lql_status lql_stream_apply_spooled(lql *self,
                                    const lql_stream_request *request,
                                    lql_stream_result *result,
                                    lql_error *error) {
  return lql_stream_apply_common(self, request, result, error, 1);
}
