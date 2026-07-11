#include "lql_internal.h"

#include <limits.h>
#include <string.h>

typedef struct lql_stream_term {
  const char *key;
  size_t key_len;
  const char *value;
  size_t value_len;
  unsigned long bit;
} lql_stream_term;

struct lql_stream_program {
  lql_stream_term terms[sizeof(unsigned long) * CHAR_BIT];
  size_t term_count;
  unsigned long required_hits;
  int match_all;
  lonejson_field mapped_fields[sizeof(unsigned long) * CHAR_BIT];
  lonejson_map mapped_map;
};

typedef struct lql_stream_state {
  lql *receiver;
  const lql_stream_request *request;
  lql_stream_result *result;
  const lql_stream_program *program;
  lql_error failure;
  size_t object_depth;
  size_t array_depth;
  size_t key_pos;
  unsigned long key_candidates;
  size_t current_term;
  size_t active_term;
  size_t active_pos;
  unsigned long hits;
  int active_mismatch;
  int key_active;
  int active;
  int root_value_started;
} lql_stream_state;

typedef struct lql_stream_member_context {
  lql_stream_state *state;
  size_t term_index;
  size_t value_pos;
  size_t container_depth;
  int active_string;
  int mismatch;
} lql_stream_member_context;

static void stream_lonejson_error(lonejson_error *out, const char *message) {
  if (out == NULL) {
    return;
  }
  lonejson_error_init(out);
  out->code = LONEJSON_STATUS_CALLBACK_FAILED;
  if (message != NULL) {
    strncpy(out->message, message, sizeof(out->message) - 1u);
    out->message[sizeof(out->message) - 1u] = '\0';
  }
}

static void stream_fail(lql_stream_state *state, lql_status status,
                        const char *message) {
  if (state == NULL || state->failure.code != LQL_STATUS_OK) {
    return;
  }
  lql_set_error(&state->failure, status, message);
}

static lql_status stream_compile_term(lql_stream_program *program,
                                      const lql_selector *selector,
                                      lql_error *error) {
  lql_stream_term *term;
  size_t i;
  if (selector == NULL) {
    return LQL_STATUS_OK;
  }
  if (selector->kind == LQL_SELECTOR_KIND_AND) {
    for (i = 0u; i < selector->child_count; ++i) {
      lql_status status = stream_compile_term(program, &selector->children[i],
                                              error);
      if (status != LQL_STATUS_OK) {
        return status;
      }
    }
    return LQL_STATUS_OK;
  }
  if (selector->kind != LQL_SELECTOR_KIND_EQ || selector->field == NULL ||
      selector->value == NULL || selector->value_is_temporal ||
      selector->field[0] != '/' || selector->field[1] == '\0' ||
      strchr(selector->field + 1, '/') != NULL ||
      strchr(selector->field + 1, '~') != NULL) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream probe supports only top-level string equality conjunctions");
    return LQL_STATUS_UNSUPPORTED;
  }
  if (program->term_count >= sizeof(program->terms) / sizeof(program->terms[0])) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream probe selector has too many equality terms");
    return LQL_STATUS_UNSUPPORTED;
  }
  for (i = 0u; i < program->term_count; ++i) {
    if (strcmp(program->terms[i].key, selector->field + 1) == 0) {
      lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                    "direct stream probe does not support duplicate equality paths");
      return LQL_STATUS_UNSUPPORTED;
    }
  }
  term = &program->terms[program->term_count];
  term->key = selector->field + 1;
  term->key_len = strlen(term->key);
  term->value = selector->value;
  term->value_len = strlen(term->value);
  term->bit = 1ul << program->term_count;
  program->required_hits |= term->bit;
  ++program->term_count;
  return LQL_STATUS_OK;
}

static lql_status stream_program_get(lql *self, const lql_selector *selector,
                                     const lql_stream_program **out,
                                     lql_error *error) {
  lql_selector *mutable_selector;
  lql_stream_program *program;
  lql_allocator *allocator;
  lql_status status;
  size_t i;
  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "stream program out required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = NULL;
  if (selector == NULL || selector->kind == LQL_SELECTOR_KIND_ALL) {
    return LQL_STATUS_OK;
  }
  mutable_selector = (lql_selector *)selector;
  if (mutable_selector->stream_program != NULL) {
    *out = mutable_selector->stream_program;
    return LQL_STATUS_OK;
  }
  allocator = lql_allocator_from_receiver(self);
  program = (lql_stream_program *)allocator->calloc(allocator, 1u,
                                                     sizeof(*program));
  if (program == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  status = stream_compile_term(program, selector, error);
  if (status != LQL_STATUS_OK) {
    allocator->destroy(allocator, program);
    return status;
  }
  if (program->term_count == 0u) {
    program->match_all = 1;
  } else {
    for (i = 0u; i < program->term_count; ++i) {
      lonejson_field *field = &program->mapped_fields[i];
      const lql_stream_term *term = &program->terms[i];
      memset(field, 0, sizeof(*field));
      field->json_key = term->key;
      field->json_key_len = term->key_len;
      field->json_key_first = term->key_len == 0u
                                  ? 0u
                                  : (unsigned char)term->key[0];
      field->json_key_last = term->key_len == 0u
                                 ? 0u
                                 : (unsigned char)term->key[term->key_len - 1u];
      field->struct_offset = i * sizeof(lonejson_json_value);
      field->kind = LONEJSON_FIELD_KIND_JSON_VALUE;
      field->storage = LONEJSON_STORAGE_FIXED;
      field->overflow_policy = LONEJSON_OVERFLOW_FAIL;
      field->spool_class = LONEJSON_SPOOL_CLASS_DEFAULT;
    }
    memset(&program->mapped_map, 0, sizeof(program->mapped_map));
    program->mapped_map.name = "lql_stream_program";
    program->mapped_map.struct_size =
        program->term_count * sizeof(lonejson_json_value);
    program->mapped_map.fields = program->mapped_fields;
    program->mapped_map.field_count = program->term_count;
  }
  mutable_selector->stream_program = program;
  *out = program;
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL void lql_stream_program_destroy(lql *self,
                                                     lql_selector *selector) {
  lql_allocator *allocator;
  if (self == NULL || selector == NULL || selector->stream_program == NULL) {
    return;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator != NULL) {
    allocator->destroy(allocator, selector->stream_program);
  }
  selector->stream_program = NULL;
}

static lonejson_read_result stream_read(void *user, unsigned char *buffer,
                                        size_t capacity) {
  lql_stream_state *state;
  lonejson_read_result out;
  size_t len;
  lql_status status;
  state = (lql_stream_state *)user;
  out = lonejson_default_read_result();
  if (state == NULL || state->request == NULL ||
      state->request->reader == NULL) {
    out.error_code = 1;
    return out;
  }
  len = 0u;
  status = state->request->reader(state->request->reader_user, buffer, capacity,
                                  &len, &state->failure);
  if (status != LQL_STATUS_OK) {
    if (state->failure.code == LQL_STATUS_OK) {
      stream_fail(state, status, "stream reader failed");
    }
    out.error_code = 1;
    return out;
  }
  if (len > capacity || len > (size_t)-1 - state->result->bytes_consumed) {
    stream_fail(state, LQL_STATUS_CALLBACK_ERROR,
                "stream reader returned an invalid byte count");
    out.error_code = 1;
    return out;
  }
  out.bytes_read = len;
  out.eof = len == 0u;
  state->result->bytes_consumed += len;
  return out;
}

static lonejson_candidate_callback_result
stream_candidate_begin(void *user, const lonejson_candidate_info *candidate,
                       lonejson_error *error) {
  lql_stream_state *state;
  (void)candidate;
  state = (lql_stream_state *)user;
  if (state->request->limits.max_records != 0u &&
      state->result->records_seen >= state->request->limits.max_records) {
    state->result->stopped_early = 1;
    state->result->stop_reason = LQL_STREAM_STOP_RECORD_LIMIT;
    return LONEJSON_CANDIDATE_STOP;
  }
  state->hits = 0ul;
  state->object_depth = 0u;
  state->array_depth = 0u;
  state->key_pos = 0u;
  state->key_candidates = 0ul;
  state->current_term = (size_t)-1;
  state->active = 0;
  state->key_active = 0;
  state->active_mismatch = 0;
  state->active_pos = 0u;
  state->root_value_started = 0;
  ++state->result->records_seen;
  (void)error;
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lonejson_candidate_callback_result
stream_candidate_end(void *user, const lonejson_candidate_info *candidate,
                     lonejson_error *error) {
  lql_stream_state *state;
  lql_stream_decision decision;
  lql_stream_callback_result callback_result;
  int matched;
  (void)candidate;
  state = (lql_stream_state *)user;
  matched = state->program == NULL || state->program->match_all ||
            state->hits == state->program->required_hits;
  if (matched) {
    ++state->result->records_matched;
  }
  if (state->request->on_decision != NULL) {
    decision.record_index = state->result->records_seen - 1u;
    decision.matched = matched;
    callback_result = state->request->on_decision(state->request->decision_user,
                                                   &decision, &state->failure);
    if (callback_result == LQL_STREAM_CALLBACK_ERROR) {
      if (state->failure.code == LQL_STATUS_OK) {
        stream_fail(state, LQL_STATUS_CALLBACK_ERROR,
                    "stream decision callback failed");
      }
      stream_lonejson_error(error, state->failure.message);
      return LONEJSON_CANDIDATE_ERROR;
    }
    if (callback_result == LQL_STREAM_CALLBACK_STOP) {
      state->result->stopped_early = 1;
      state->result->stop_reason = LQL_STREAM_STOP_CALLBACK;
      return LONEJSON_CANDIDATE_STOP;
    }
  }
  if (matched && state->request->limits.max_matches != 0u &&
      state->result->records_matched >= state->request->limits.max_matches) {
    state->result->stopped_early = 1;
    state->result->stop_reason = LQL_STREAM_STOP_MATCH_LIMIT;
    return LONEJSON_CANDIDATE_STOP;
  }
  return LONEJSON_CANDIDATE_CONTINUE;
}

static void stream_clear_current(lql_stream_state *state) {
  state->current_term = (size_t)-1;
}

static void stream_finish_active(lql_stream_state *state) {
  const lql_stream_term *term;
  if (!state->active) {
    return;
  }
  term = &state->program->terms[state->active_term];
  if (!state->active_mismatch && state->active_pos == term->value_len) {
    state->hits |= term->bit;
  }
  state->active = 0;
}

static void stream_finish_key(lql_stream_state *state) {
  size_t i;
  if (!state->key_active) {
    return;
  }
  for (i = 0u; state->program != NULL && i < state->program->term_count; ++i) {
    const lql_stream_term *term = &state->program->terms[i];
    if ((state->key_candidates & term->bit) != 0ul &&
        state->key_pos == term->key_len) {
      state->current_term = i;
      break;
    }
  }
  state->key_active = 0;
}

static lonejson_status stream_key_end(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  stream_finish_key(state);
  if (state->object_depth == 1u && state->array_depth == 0u &&
      state->current_term == (size_t)-1) {
    return LONEJSON_STATUS_SKIP_VALUE;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_object_begin(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  if (state->object_depth == 1u && state->array_depth == 0u) {
    stream_finish_key(state);
    stream_finish_active(state);
    stream_clear_current(state);
  }
  ++state->object_depth;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_match_all_object_begin(void *user,
                                                      lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  state->root_value_started = 1;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_match_all_array_begin(void *user,
                                                     lonejson_error *error) {
  lql_stream_state *state;
  state = (lql_stream_state *)user;
  if (!state->root_value_started) {
    stream_fail(state, LQL_STATUS_JSON_ERROR,
                "root JSON arrays are not valid NDJSON records");
    stream_lonejson_error(error, state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_object_end(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  if (state->object_depth == 1u && state->array_depth == 0u) {
    stream_finish_key(state);
    stream_finish_active(state);
  }
  if (state->object_depth != 0u) {
    --state->object_depth;
  }
  stream_clear_current(state);
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_array_begin(void *user, lonejson_error *error) {
  lql_stream_state *state;
  state = (lql_stream_state *)user;
  if (state->object_depth == 0u && state->array_depth == 0u) {
    stream_fail(state, LQL_STATUS_JSON_ERROR,
                "root JSON arrays are not valid NDJSON records");
    stream_lonejson_error(error, state->failure.message);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (state->object_depth == 1u && state->array_depth == 0u) {
    stream_finish_key(state);
    stream_finish_active(state);
    stream_clear_current(state);
  }
  ++state->array_depth;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_array_end(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  if (state->array_depth != 0u) {
    --state->array_depth;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_key_begin(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  stream_finish_active(state);
  stream_clear_current(state);
  state->key_active = state->object_depth == 1u && state->array_depth == 0u;
  state->key_pos = 0u;
  state->key_candidates = state->program == NULL ? 0ul : state->program->required_hits;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_key_chunk(void *user, const char *data, size_t len,
                                        lonejson_error *error) {
  lql_stream_state *state;
  size_t i;
  (void)error;
  state = (lql_stream_state *)user;
  if (!state->key_active) {
    return LONEJSON_STATUS_OK;
  }
  for (i = 0u; state->program != NULL && i < state->program->term_count; ++i) {
    const lql_stream_term *term = &state->program->terms[i];
    if ((state->key_candidates & term->bit) != 0ul &&
        (state->key_pos > term->key_len ||
         len > term->key_len - state->key_pos ||
         memcmp(data, term->key + state->key_pos, len) != 0)) {
      state->key_candidates &= ~term->bit;
    }
  }
  state->key_pos += len;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_string_begin(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  stream_finish_key(state);
  state->active = 0;
  if (state->current_term != (size_t)-1 && state->object_depth == 1u &&
      state->array_depth == 0u) {
    state->active_term = state->current_term;
    state->active_pos = 0u;
    state->active_mismatch = 0;
    state->active = 1;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_string_chunk(void *user, const char *data,
                                           size_t len, lonejson_error *error) {
  lql_stream_state *state;
  const lql_stream_term *term;
  (void)error;
  state = (lql_stream_state *)user;
  if (!state->active) {
    return LONEJSON_STATUS_OK;
  }
  term = &state->program->terms[state->active_term];
  if (!state->active_mismatch &&
      (state->active_pos > term->value_len ||
       len > term->value_len - state->active_pos ||
       memcmp(data, term->value + state->active_pos, len) != 0)) {
    state->active_mismatch = 1;
  }
  state->active_pos += len;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_scalar_value(void *user, lonejson_error *error) {
  lql_stream_state *state;
  (void)error;
  state = (lql_stream_state *)user;
  stream_finish_key(state);
  stream_finish_active(state);
  stream_clear_current(state);
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_boolean_value(void *user, int value,
                                            lonejson_error *error) {
  (void)value;
  return stream_scalar_value(user, error);
}

static lonejson_status stream_mapped_object_begin(void *user,
                                                   lonejson_error *error) {
  lql_stream_member_context *member;
  (void)error;
  member = (lql_stream_member_context *)user;
  ++member->container_depth;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_mapped_object_end(void *user,
                                                 lonejson_error *error) {
  lql_stream_member_context *member;
  (void)error;
  member = (lql_stream_member_context *)user;
  if (member->container_depth != 0u) {
    --member->container_depth;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_mapped_string_begin(void *user,
                                                   lonejson_error *error) {
  lql_stream_member_context *member;
  (void)error;
  member = (lql_stream_member_context *)user;
  member->active_string = member->container_depth == 0u;
  member->value_pos = 0u;
  member->mismatch = 0;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_mapped_string_chunk(void *user, const char *data,
                                                   size_t len,
                                                   lonejson_error *error) {
  lql_stream_member_context *member;
  const lql_stream_term *term;
  (void)error;
  member = (lql_stream_member_context *)user;
  if (!member->active_string) {
    return LONEJSON_STATUS_OK;
  }
  term = &member->state->program->terms[member->term_index];
  if (!member->mismatch &&
      (member->value_pos > term->value_len ||
       len > term->value_len - member->value_pos ||
       memcmp(data, term->value + member->value_pos, len) != 0)) {
    member->mismatch = 1;
  }
  member->value_pos += len;
  return LONEJSON_STATUS_OK;
}

static lonejson_status stream_mapped_string_end(void *user,
                                                 lonejson_error *error) {
  lql_stream_member_context *member;
  const lql_stream_term *term;
  (void)error;
  member = (lql_stream_member_context *)user;
  if (!member->active_string) {
    return LONEJSON_STATUS_OK;
  }
  term = &member->state->program->terms[member->term_index];
  if (!member->mismatch && member->value_pos == term->value_len) {
    member->state->hits |= term->bit;
  }
  member->active_string = 0;
  return LONEJSON_STATUS_OK;
}

static lql_status stream_status(lql_stream_state *state, lonejson_status status,
                                const lonejson_error *error, lql_error *out) {
  if (state->failure.code != LQL_STATUS_OK) {
    if (out != NULL) {
      *out = state->failure;
    }
    return state->failure.code;
  }
  if (status == LONEJSON_STATUS_OK) {
    return LQL_STATUS_OK;
  }
  if (status == LONEJSON_STATUS_ALLOCATION_FAILED) {
    lql_set_error(out, LQL_STATUS_NO_MEMORY, "lonejson allocation failed");
    return LQL_STATUS_NO_MEMORY;
  }
  if (status == LONEJSON_STATUS_CALLBACK_FAILED) {
    lql_set_error(out, LQL_STATUS_CALLBACK_ERROR,
                  "lonejson stream callback failed");
    return LQL_STATUS_CALLBACK_ERROR;
  }
  lql_set_error(out, LQL_STATUS_JSON_ERROR,
                error != NULL && error->message[0] != '\0'
                    ? error->message
                    : "invalid JSON stream");
  return LQL_STATUS_JSON_ERROR;
}

static lql_status stream_execute_mapped(lql_stream_state *state,
                                        lonejson *runtime, lql_error *error) {
  lonejson_json_value values[sizeof(unsigned long) * CHAR_BIT];
  lql_stream_member_context members[sizeof(unsigned long) * CHAR_BIT];
  lonejson_value_visitor visitor;
  lonejson_stream *stream;
  lonejson_stream_result stream_result;
  lonejson_error lonejson_error;
  lonejson_status lonejson_status;
  lonejson_candidate_callback_result callback_result;
  lql_status status;
  size_t i;

  visitor = lonejson_default_value_visitor();
  visitor.object_begin = stream_mapped_object_begin;
  visitor.object_end = stream_mapped_object_end;
  visitor.array_begin = stream_mapped_object_begin;
  visitor.array_end = stream_mapped_object_end;
  visitor.string_begin = stream_mapped_string_begin;
  visitor.string_chunk = stream_mapped_string_chunk;
  visitor.string_end = stream_mapped_string_end;
  lonejson_error_init(&lonejson_error);
  for (i = 0u; i < state->program->term_count; ++i) {
    memset(&members[i], 0, sizeof(members[i]));
    members[i].state = state;
    members[i].term_index = i;
    lonejson_json_value_init(runtime, &values[i]);
    lonejson_status = lonejson_json_value_set_parse_visitor(
        &values[i], &visitor, &members[i], &lonejson_error);
    if (lonejson_status != LONEJSON_STATUS_OK) {
      while (i != 0u) {
        --i;
        lonejson_json_value_cleanup(&values[i]);
      }
      return stream_status(state, lonejson_status, &lonejson_error, error);
    }
  }
  stream = lonejson_stream_open_candidates_reader(
      runtime, &state->program->mapped_map, stream_read, state, &lonejson_error);
  if (stream == NULL) {
    for (i = 0u; i < state->program->term_count; ++i) {
      lonejson_json_value_cleanup(&values[i]);
    }
    return stream_status(state, lonejson_error.code, &lonejson_error, error);
  }
  status = LQL_STATUS_OK;
  for (;;) {
    callback_result = stream_candidate_begin(state, NULL, &lonejson_error);
    if (callback_result == LONEJSON_CANDIDATE_STOP) {
      break;
    }
    if (callback_result != LONEJSON_CANDIDATE_CONTINUE) {
      status = stream_status(state, LONEJSON_STATUS_CALLBACK_FAILED,
                             &lonejson_error, error);
      break;
    }
    stream_result = stream->next(stream, values, &lonejson_error);
    if (stream_result == LONEJSON_STREAM_EOF) {
      --state->result->records_seen;
      break;
    }
    if (stream_result == LONEJSON_STREAM_ERROR) {
      status = stream_status(state, stream->error.code, &stream->error, error);
      break;
    }
    if (stream_result == LONEJSON_STREAM_VALUE &&
        stream->root_type == LONEJSON_VALUE_ARRAY) {
      stream_fail(state, LQL_STATUS_JSON_ERROR,
                  "root JSON arrays are not valid NDJSON records");
      if (error != NULL) {
        *error = state->failure;
      }
      status = state->failure.code;
      break;
    }
    callback_result = stream_candidate_end(state, NULL, &lonejson_error);
    if (callback_result == LONEJSON_CANDIDATE_STOP) {
      break;
    }
    if (callback_result != LONEJSON_CANDIDATE_CONTINUE) {
      status = stream_status(state, LONEJSON_STATUS_CALLBACK_FAILED,
                             &lonejson_error, error);
      break;
    }
  }
  stream->close(stream);
  for (i = 0u; i < state->program->term_count; ++i) {
    lonejson_json_value_cleanup(&values[i]);
  }
  if (status == LQL_STATUS_OK && state->failure.code != LQL_STATUS_OK) {
    if (error != NULL) {
      *error = state->failure;
    }
    status = state->failure.code;
  }
  return status;
}

lql_status lql_stream_execute(lql *self, const lql_stream_request *request,
                              lql_stream_result *result, lql_error *error) {
  lql_stream_state state;
  lonejson *runtime;
  lonejson_error lonejson_error;
  lonejson_candidate_stream_options options;
  lonejson_value_visitor visitor;
  lql_status status;
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
  if (request->output_mode != LQL_STREAM_OUTPUT_DECISION_ONLY ||
      request->writer != NULL || request->projection != NULL ||
      request->mutation != NULL) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream probe currently supports decision-only output");
    return LQL_STATUS_UNSUPPORTED;
  }
  if (request->limits.max_bytes != 0u) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "direct stream probe does not yet support byte limits");
    return LQL_STATUS_UNSUPPORTED;
  }
  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.request = request;
  state.result = result;
  lql_error_init(&state.failure);
  status = stream_program_get(self, request->selector, &state.program,
                              &state.failure);
  if (status != LQL_STATUS_OK) {
    if (error != NULL) {
      *error = state.failure;
    }
    return status;
  }
  lonejson_error_init(&lonejson_error);
  runtime = state.program != NULL && !state.program->match_all
                ? lql_lonejson_new_mapped_stream(self, &lonejson_error)
                : lql_lonejson_new(self, &lonejson_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY,
                  lonejson_error.message[0] == '\0' ? "lonejson initialization failed"
                                                     : lonejson_error.message);
    return LQL_STATUS_NO_MEMORY;
  }
  if (state.program != NULL && !state.program->match_all) {
    status = stream_execute_mapped(&state, runtime, error);
    lonejson_free(runtime);
    return status;
  }
  options = lonejson_default_candidate_stream_options();
  options.framing = LONEJSON_CANDIDATE_FRAMING_NDJSON;
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_NONE;
  visitor = lonejson_default_value_visitor();
  if (state.program == NULL || state.program->match_all) {
    visitor.object_begin = stream_match_all_object_begin;
    visitor.array_begin = stream_match_all_array_begin;
  } else {
    visitor.object_begin = stream_object_begin;
    visitor.object_end = stream_object_end;
    visitor.object_key_begin = stream_key_begin;
    visitor.object_key_chunk = stream_key_chunk;
    visitor.object_key_end = stream_key_end;
    visitor.array_begin = stream_array_begin;
    visitor.array_end = stream_array_end;
    visitor.string_begin = stream_string_begin;
    visitor.string_chunk = stream_string_chunk;
    visitor.number_begin = stream_scalar_value;
    visitor.boolean_value = stream_boolean_value;
    visitor.null_value = stream_scalar_value;
  }
  options.visitor = &visitor;
  options.visitor_user = &state;
  options.candidate_begin = stream_candidate_begin;
  options.candidate_end = stream_candidate_end;
  options.candidate_user = &state;
  status = stream_status(&state,
                         lonejson_visit_candidates_reader(runtime, stream_read,
                                                          &state, &options,
                                                          &lonejson_error),
                         &lonejson_error, error);
  lonejson_free(runtime);
  return status;
}
