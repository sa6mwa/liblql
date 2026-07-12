#include "lql_internal.h"
#include "lql_json_scan.h"

#include <limits.h>
#include <string.h>

#define LQL_FLAT_EQ_TERM_CAPACITY (sizeof(unsigned long) * CHAR_BIT)

typedef struct lql_flat_eq_program {
  lql_json_flat_eq_term terms[LQL_FLAT_EQ_TERM_CAPACITY];
  const lql_selector *selectors[LQL_FLAT_EQ_TERM_CAPACITY];
  size_t term_count;
} lql_flat_eq_program;

typedef struct lql_flat_eq_state {
  const lql_stream_request *request;
  lql_stream_result *result;
  const lql_flat_eq_program *program;
} lql_flat_eq_state;

static int lql_flat_eq_append(lql_flat_eq_program *program,
                              const lql_selector *selector) {
  lql_json_flat_eq_term *term;
  const char *field;
  size_t i;
  if (selector == NULL) {
    return 0;
  }
  if (selector->kind == LQL_SELECTOR_KIND_ALL) {
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_AND ||
      selector->kind == LQL_SELECTOR_KIND_OR ||
      selector->kind == LQL_SELECTOR_KIND_NOT) {
    for (i = 0u; i < selector->child_count; ++i) {
      if (!lql_flat_eq_append(program, &selector->children[i])) {
        return 0;
      }
    }
    return selector->kind != LQL_SELECTOR_KIND_NOT ||
           selector->child_count == 1u;
  }
  if (selector->kind == LQL_SELECTOR_KIND_IN) {
    if (selector->field == NULL || selector->any_count == 0u ||
        selector->any_kinds == NULL) {
      return 0;
    }
    field = selector->field;
    if (field[0] != '/' || field[1] == '\0' || strchr(field + 1, '/') != NULL) {
      return 0;
    }
    for (i = 0u; i < selector->any_count; ++i) {
      if (selector->any_kinds[i] != LQL_SELECTOR_LITERAL_STRING ||
          program->term_count == LQL_FLAT_EQ_TERM_CAPACITY) {
        return 0;
      }
      term = &program->terms[program->term_count];
      term->kind = LQL_JSON_FLAT_TERM_EQ;
      term->field = field + 1;
      term->field_len = strlen(field + 1);
      term->value = selector->any[i];
      term->value_len = selector->any_lens[i];
      program->selectors[program->term_count] = selector;
      ++program->term_count;
    }
    return 1;
  }
  if ((selector->kind != LQL_SELECTOR_KIND_EQ &&
       selector->kind != LQL_SELECTOR_KIND_EXISTS) ||
      selector->field == NULL ||
      program->term_count == LQL_FLAT_EQ_TERM_CAPACITY) {
    return 0;
  }
  field = selector->field;
  if (field[0] != '/' || field[1] == '\0' || strchr(field + 1, '/') != NULL) {
    return 0;
  }
  term = &program->terms[program->term_count];
  term->field = field + 1;
  term->field_len = strlen(field + 1);
  term->kind = selector->kind == LQL_SELECTOR_KIND_EXISTS
                   ? LQL_JSON_FLAT_TERM_EXISTS
                   : LQL_JSON_FLAT_TERM_EQ;
  if (term->kind == LQL_JSON_FLAT_TERM_EQ) {
    if (!selector->value_set || !selector->value_is_string ||
        selector->value_is_temporal || selector->value == NULL) {
      return 0;
    }
    term->value = selector->value;
    term->value_len = strlen(selector->value);
  }
  program->selectors[program->term_count] = selector;
  ++program->term_count;
  return 1;
}

static int lql_flat_eq_matches(const lql_flat_eq_program *program,
                               const lql_selector *selector,
                               unsigned long hits) {
  size_t i;
  if (selector == NULL) {
    return 0;
  }
  if (selector->kind == LQL_SELECTOR_KIND_ALL) {
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_AND) {
    for (i = 0u; i < selector->child_count; ++i) {
      if (!lql_flat_eq_matches(program, &selector->children[i], hits)) {
        return 0;
      }
    }
    return 1;
  }
  if (selector->kind == LQL_SELECTOR_KIND_OR) {
    for (i = 0u; i < selector->child_count; ++i) {
      if (lql_flat_eq_matches(program, &selector->children[i], hits)) {
        return 1;
      }
    }
    return 0;
  }
  if (selector->kind == LQL_SELECTOR_KIND_NOT) {
    return selector->child_count == 1u &&
           !lql_flat_eq_matches(program, &selector->children[0], hits);
  }
  for (i = 0u; i < program->term_count; ++i) {
    if (program->selectors[i] == selector) {
      if ((hits & (1ul << i)) != 0ul) {
        return 1;
      }
    }
  }
  return 0;
}

static lql_status lql_flat_eq_emit(lql_flat_eq_state *state,
                                   const lql_json_spool *spool,
                                   lql_error *error) {
  lql_status status;
  static const char newline[] = "\n";
  status = lql_json_spool_write_to(spool, state->request->writer,
                                   state->request->writer_user, error);
  if (status != LQL_STATUS_OK) {
    if (error != NULL && error->code == LQL_STATUS_OK) {
      lql_set_error(error, status, "stream writer failed");
    }
    return status;
  }
  status = state->request->writer(state->request->writer_user, newline,
                                  sizeof(newline) - 1u, error);
  if (status != LQL_STATUS_OK && error != NULL &&
      error->code == LQL_STATUS_OK) {
    lql_set_error(error, status, "stream writer failed");
  }
  return status;
}

static lql_status lql_flat_eq_record(void *user, size_t record_index,
                                     int root_is_object, unsigned long hits,
                                     const lql_json_spool *spool,
                                     lql_error *error) {
  lql_flat_eq_state *state;
  lql_stream_decision decision;
  lql_stream_value value;
  lql_stream_callback_result callback_result;
  lql_status status;
  int matched;
  state = (lql_flat_eq_state *)user;
  if (state == NULL) {
    lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                  "flat equality scanner callback is unavailable");
    return LQL_STATUS_CALLBACK_ERROR;
  }
  matched = lql_flat_eq_matches(state->program, state->request->selector, hits);
  ++state->result->records_seen;
  if (matched) {
    ++state->result->records_matched;
  }
  if (state->request->on_decision != NULL) {
    decision.record_index = record_index;
    decision.matched = matched;
    callback_result = state->request->on_decision(state->request->decision_user,
                                                  &decision, error);
    if (callback_result == LQL_STREAM_CALLBACK_ERROR) {
      if (error != NULL && error->code == LQL_STATUS_OK) {
        lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                      "stream decision callback failed");
      }
      return LQL_STATUS_CALLBACK_ERROR;
    }
    if (callback_result == LQL_STREAM_CALLBACK_STOP) {
      state->result->stopped_early = 1;
      state->result->stop_reason = LQL_STREAM_STOP_CALLBACK;
      return LQL_STATUS_STOP;
    }
  }
  if (matched && state->request->on_value != NULL) {
    if (spool == NULL) {
      lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                    "matched value capture is unavailable");
      return LQL_STATUS_CALLBACK_ERROR;
    }
    memset(&value, 0, sizeof(value));
    value.storage_kind = LQL_STREAM_VALUE_JSON_SPOOL;
    value.spool = spool;
    callback_result =
        state->request->on_value(state->request->value_user, &value, error);
    if (callback_result == LQL_STREAM_CALLBACK_ERROR) {
      if (error != NULL && error->code == LQL_STATUS_OK) {
        lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                      "stream value callback failed");
      }
      return LQL_STATUS_CALLBACK_ERROR;
    }
    if (callback_result == LQL_STREAM_CALLBACK_STOP) {
      state->result->stopped_early = 1;
      state->result->stop_reason = LQL_STREAM_STOP_CALLBACK;
      return LQL_STATUS_STOP;
    }
  }
  if (state->request->output_mode == LQL_STREAM_OUTPUT_SELECTED_RECORD &&
      (matched || !state->request->matched_only)) {
    if (spool == NULL) {
      lql_set_error(error, LQL_STATUS_CALLBACK_ERROR,
                    "selected record capture is unavailable");
      return LQL_STATUS_CALLBACK_ERROR;
    }
    status = lql_flat_eq_emit(state, spool, error);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  if (matched && state->request->limits.max_matches != 0u &&
      state->result->records_matched >= state->request->limits.max_matches) {
    state->result->stopped_early = 1;
    state->result->stop_reason = LQL_STREAM_STOP_MATCH_LIMIT;
    return LQL_STATUS_STOP;
  }
  (void)root_is_object;
  return LQL_STATUS_OK;
}

static int lql_flat_eq_eligible(const lql_stream_request *request) {
  if (request == NULL || request->selector == NULL ||
      request->limits.max_records != 0u || request->projection != NULL ||
      request->mutation != NULL ||
      (request->output_mode != LQL_STREAM_OUTPUT_DECISION_ONLY &&
       request->output_mode != LQL_STREAM_OUTPUT_SELECTED_RECORD)) {
    return 0;
  }
  return 1;
}

lql_status lql_stream_execute_flat_eq(lql *self,
                                      const lql_stream_request *request,
                                      lql_stream_result *result,
                                      lql_error *error, int *out_handled) {
  lql_json_flat_eq_request scan_request;
  lql_json_spool spool;
  lql_flat_eq_state state;
  lql_flat_eq_program program;
  lql_status status;
  size_t records;
  size_t bytes_read;
  int capture;
  (void)self;
  if (out_handled == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "flat equality state required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out_handled = 0;
  if (!lql_flat_eq_eligible(request)) {
    return LQL_STATUS_OK;
  }
  memset(&program, 0, sizeof(program));
  if (!lql_flat_eq_append(&program, request->selector)) {
    return LQL_STATUS_OK;
  }
  *out_handled = 1;
  capture = request->on_value != NULL ||
            request->output_mode == LQL_STREAM_OUTPUT_SELECTED_RECORD;
  memset(&spool, 0, sizeof(spool));
  if (capture) {
    status = lql_json_spool_init(&spool, error);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  memset(&state, 0, sizeof(state));
  state.request = request;
  state.result = result;
  state.program = &program;
  memset(&scan_request, 0, sizeof(scan_request));
  scan_request.reader = request->reader;
  scan_request.reader_user = request->reader_user;
  scan_request.terms = program.terms;
  scan_request.term_count = program.term_count;
  scan_request.spool = capture ? &spool : NULL;
  scan_request.capture = capture;
  scan_request.record = lql_flat_eq_record;
  scan_request.record_user = &state;
  status =
      lql_json_scan_flat_eq_ndjson(&scan_request, &records, &bytes_read, error);
  result->bytes_consumed = bytes_read;
  if (capture) {
    lql_json_spool_cleanup(&spool);
  }
  if (status == LQL_STATUS_STOP) {
    return LQL_STATUS_OK;
  }
  return status;
}
