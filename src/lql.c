#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "lql_internal.h"

#include <lql/version.h>

#include <string.h>
#include <sys/types.h>

typedef struct lql_match_adapter {
  FILE *file;
  lql_query_match_fn on_match;
  void *user;
} lql_match_adapter;

typedef struct lql_payload_sink_adapter {
  lql_write_fn write;
  void *user;
  lql_status status;
} lql_payload_sink_adapter;

static const char *receiver_version(const lql *self);
static void receiver_capabilities_get(const lql *self, lql_capabilities *out);
static lql_status receiver_selector_parse(lql *self, const char *expr,
                                          lql_selector **out, lql_error *error);
static lql_status receiver_selector_parse_or(lql *self, const char *expr,
                                             lql_selector **out,
                                             lql_error *error);
static void receiver_selector_free(lql *self, lql_selector *selector);
static int receiver_selector_is_empty(const lql *self,
                                      const lql_selector *selector);
static lql_status receiver_matches_json(lql *self, const lql_selector *selector,
                                        const char *json, size_t json_len,
                                        int *out_matched, lql_error *error);
static lql_status
receiver_query_file_decisions(lql *self, const lql_selector *selector,
                              FILE *file, lql_query_decision_fn on_decision,
                              void *user, lql_query_result *out_result,
                              lql_error *error);
static lql_status receiver_query_file_decisions_with_options(
    lql *self, const lql_selector *selector, FILE *file,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error);
static lql_status
receiver_query_source_decisions(lql *self, const lql_selector *selector,
                                lql_read_fn read, void *read_user,
                                lql_query_decision_fn on_decision, void *user,
                                lql_query_result *out_result, lql_error *error);
static lql_status receiver_query_source_decisions_with_options(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error);
static lql_status
receiver_query_file_matches(lql *self, const lql_selector *selector, FILE *file,
                            lql_query_match_fn on_match, void *user,
                            lql_query_result *out_result, lql_error *error);
static lql_status receiver_query_file_matches_with_options(
    lql *self, const lql_selector *selector, FILE *file,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error);
static lql_status receiver_query_source_spooled_matches(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_query_match_fn on_match, void *user, lql_query_result *out_result,
    lql_error *error);
static lql_status receiver_query_source_spooled_matches_with_options(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error);
static lql_status receiver_payload_write_json(lql *self,
                                              const lql_payload *payload,
                                              FILE *out, lql_error *error);
static lql_status receiver_payload_write_json_sink(lql *self,
                                                   const lql_payload *payload,
                                                   lql_write_fn write,
                                                   void *write_user,
                                                   lql_error *error);
static lql_status receiver_projection_parse(lql *self,
                                            const char *const *fields,
                                            size_t field_count,
                                            lql_projection **out,
                                            lql_error *error);
static void receiver_projection_free(lql *self, lql_projection *projection);
static lql_status receiver_project_file_range(lql *self,
                                              const lql_projection *projection,
                                              FILE *file, lql_uint64 offset,
                                              lql_uint64 size, FILE *out,
                                              int *out_found, lql_error *error);
static lql_status receiver_project_source(lql *self,
                                          const lql_projection *projection,
                                          lql_read_fn read, void *read_user,
                                          FILE *out, int *out_found,
                                          lql_error *error);
static lql_status receiver_project_json(lql *self,
                                        const lql_projection *projection,
                                        const char *json, size_t json_len,
                                        FILE *out, int *out_found,
                                        lql_error *error);
static lql_status receiver_compact_file_range(lql *self, FILE *file,
                                              lql_uint64 offset,
                                              lql_uint64 size, FILE *out,
                                              lql_error *error);
static lql_status receiver_compact_source(lql *self, lql_read_fn read,
                                          void *read_user, FILE *out,
                                          lql_error *error);
static lql_status receiver_compact_json(lql *self, const char *json,
                                        size_t json_len, FILE *out,
                                        lql_error *error);
static lql_status receiver_mutation_plan_parse(lql *self,
                                               const char *const *exprs,
                                               size_t expr_count,
                                               lql_mutation_plan **out,
                                               lql_error *error);
static lql_status receiver_mutation_plan_parse_with_options(
    lql *self, const char *const *exprs, size_t expr_count,
    const lql_mutation_parse_options *options, lql_mutation_plan **out,
    lql_error *error);
static size_t receiver_mutation_plan_count(const lql *self,
                                           const lql_mutation_plan *plan);
static void receiver_mutation_plan_free(lql *self, lql_mutation_plan *plan);
static lql_status receiver_mutate_file_range_root_fields(
    lql *self, const lql_mutation_plan *plan, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, lql_error *error);
static lql_status
receiver_mutate_file_range_paths(lql *self, const lql_mutation_plan *plan,
                                 FILE *file, lql_uint64 offset, lql_uint64 size,
                                 FILE *out, lql_error *error);
static lql_status receiver_mutate_file_range_candidates(
    lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
    FILE *file, lql_uint64 offset, lql_uint64 size, FILE *out, int compact,
    int matches_only, lql_query_result *out_result, lql_error *error);
static lql_status receiver_mutate_source_paths(lql *self,
                                               const lql_mutation_plan *plan,
                                               lql_read_fn read,
                                               void *read_user, FILE *out,
                                               lql_error *error);
static lql_status receiver_mutate_source_candidates(
    lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
    lql_read_fn read, void *read_user, FILE *out, int compact,
    int matches_only, lql_query_result *out_result, lql_error *error);
static lql_status receiver_mutate_json(lql *self, const lql_mutation_plan *plan,
                                       const char *json, size_t json_len,
                                       FILE *out, lql_error *error);
static void receiver_destroy(lql *self);

static int seek_u64(FILE *file, lql_uint64 offset) {
  off_t seek_offset;
  seek_offset = (off_t)offset;
  if (seek_offset < (off_t)0 || (lql_uint64)seek_offset != offset) {
    return 0;
  }
  return fseeko(file, seek_offset, SEEK_SET) == 0;
}

static lql_status copy_range_to_sink(FILE *in, lql_uint64 size,
                                     lql_write_fn write, void *user) {
  char buf[8192];
  size_t want;
  size_t got;
  lql_status st;
  while (size != 0u) {
    want = size > (lql_uint64)sizeof(buf) ? sizeof(buf) : (size_t)size;
    got = fread(buf, 1u, want, in);
    if (got == 0u) {
      return LQL_STATUS_JSON_ERROR;
    }
    st = write(user, buf, got);
    if (st != LQL_STATUS_OK) {
      return st;
    }
    size -= (lql_uint64)got;
  }
  return LQL_STATUS_OK;
}

static lql_status payload_file_write(void *user, const void *data, size_t len) {
  FILE *out;
  out = (FILE *)user;
  return fwrite(data, 1u, len, out) == len ? LQL_STATUS_OK
                                           : LQL_STATUS_JSON_ERROR;
}

static lonejson_status payload_lql_sink(void *user, const void *data,
                                        size_t len, lonejson_error *error) {
  lql_payload_sink_adapter *adapter;
  (void)error;
  adapter = (lql_payload_sink_adapter *)user;
  adapter->status = adapter->write(adapter->user, data, len);
  return adapter->status == LQL_STATUS_OK ? LONEJSON_STATUS_OK
                                          : LONEJSON_STATUS_CALLBACK_FAILED;
}

static lql_status on_match_decision(void *user,
                                    const lql_query_decision *decision) {
  lql_match_adapter *adapter;
  lql_query_match match;

  if (!decision->matched) {
    return LQL_STATUS_OK;
  }
  adapter = (lql_match_adapter *)user;
  memset(&match, 0, sizeof(match));
  match.decision = *decision;
  match.payload.kind = LQL_PAYLOAD_SEEKABLE_RANGE;
  match.payload.index = decision->index;
  match.payload.offset = decision->offset;
  match.payload.size = decision->size;
  match.payload.source = adapter->file;
  return adapter->on_match(adapter->user, &match);
}

lql_status lql_new(lql **out, lql_error *error) {
  lql *ctx;

  if (out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "out lql required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out = NULL;
  ctx = (lql *)lql_calloc(1u, sizeof(*ctx));
  if (ctx == NULL) {
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  ctx->version = receiver_version;
  ctx->capabilities_get = receiver_capabilities_get;
  ctx->selector_parse = receiver_selector_parse;
  ctx->selector_parse_or = receiver_selector_parse_or;
  ctx->selector_free = receiver_selector_free;
  ctx->selector_is_empty = receiver_selector_is_empty;
  ctx->matches_json = receiver_matches_json;
  ctx->query_file_decisions = receiver_query_file_decisions;
  ctx->query_file_decisions_with_options =
      receiver_query_file_decisions_with_options;
  ctx->query_source_decisions = receiver_query_source_decisions;
  ctx->query_source_decisions_with_options =
      receiver_query_source_decisions_with_options;
  ctx->query_file_matches = receiver_query_file_matches;
  ctx->query_file_matches_with_options =
      receiver_query_file_matches_with_options;
  ctx->query_source_spooled_matches = receiver_query_source_spooled_matches;
  ctx->query_source_spooled_matches_with_options =
      receiver_query_source_spooled_matches_with_options;
  ctx->payload_write_json = receiver_payload_write_json;
  ctx->payload_write_json_sink = receiver_payload_write_json_sink;
  ctx->projection_parse = receiver_projection_parse;
  ctx->projection_free = receiver_projection_free;
  ctx->project_file_range = receiver_project_file_range;
  ctx->project_source = receiver_project_source;
  ctx->project_json = receiver_project_json;
  ctx->compact_file_range = receiver_compact_file_range;
  ctx->compact_source = receiver_compact_source;
  ctx->compact_json = receiver_compact_json;
  ctx->mutation_plan_parse = receiver_mutation_plan_parse;
  ctx->mutation_plan_parse_with_options =
      receiver_mutation_plan_parse_with_options;
  ctx->mutation_plan_count = receiver_mutation_plan_count;
  ctx->mutation_plan_free = receiver_mutation_plan_free;
  ctx->mutate_file_range_root_fields = receiver_mutate_file_range_root_fields;
  ctx->mutate_file_range_paths = receiver_mutate_file_range_paths;
  ctx->mutate_file_range_candidates = receiver_mutate_file_range_candidates;
  ctx->mutate_source_paths = receiver_mutate_source_paths;
  ctx->mutate_source_candidates = receiver_mutate_source_candidates;
  ctx->mutate_json = receiver_mutate_json;
  ctx->destroy = receiver_destroy;
  *out = ctx;
  return LQL_STATUS_OK;
}

void lql_error_init(lql_error *error) {
  if (error != NULL) {
    error->code = LQL_STATUS_OK;
    error->message[0] = '\0';
  }
}

LQL_INTERNAL_SYMBOL void lql_set_error(lql_error *error, lql_status status,
                                       const char *message) {
  size_t len;
  if (error == NULL) {
    return;
  }
  error->code = status;
  if (message == NULL) {
    error->message[0] = '\0';
    return;
  }
  len = strlen(message);
  if (len >= sizeof(error->message)) {
    len = sizeof(error->message) - 1u;
  }
  memcpy(error->message, message, len);
  error->message[len] = '\0';
}

const char *lql_status_string(lql_status status) {
  switch (status) {
  case LQL_STATUS_OK:
    return "ok";
  case LQL_STATUS_INVALID_ARGUMENT:
    return "invalid argument";
  case LQL_STATUS_NO_MEMORY:
    return "out of memory";
  case LQL_STATUS_PARSE_ERROR:
    return "parse error";
  case LQL_STATUS_JSON_ERROR:
    return "json error";
  case LQL_STATUS_UNSUPPORTED:
    return "unsupported";
  case LQL_STATUS_STOP:
    return "stop";
  }
  return "unknown";
}

const char *lql_version(void) { return LQL_VERSION; }

void lql_capabilities_get(lql_capabilities *out) {
  if (out == NULL) {
    return;
  }
  memset(out, 0, sizeof(*out));
  out->selector_parse = 1;
  out->matches_json = 1;
  out->file_decision_stream = 1;
  out->source_decision_stream = 1;
  out->file_match_stream = 1;
  out->seekable_range_payloads = 1;
  out->source_spooled_match_stream = 1;
  out->spooled_payloads = 1;
  out->payload_sink_write = 1;
  out->projection_file_range = 1;
  out->projection_source = 1;
  out->projection_buffered_json = 1;
  out->compact_file_range = 1;
  out->compact_source = 1;
  out->compact_buffered_json = 1;
  out->mutation_parse = 1;
  out->mutation_file_range = 1;
  out->mutation_file_range_candidates = 1;
  out->mutation_source = 1;
  out->mutation_source_candidates = 1;
  out->mutation_buffered_json = 1;
  out->mutation_file_values = 1;
}

char *lql_strdup(const char *text) {
  size_t len;
  char *out;
  if (text == NULL) {
    return NULL;
  }
  len = strlen(text);
  out = (char *)lql_alloc(len + 1u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, text, len + 1u);
  return out;
}

static const char *receiver_version(const lql *self) {
  (void)self;
  return lql_version();
}

static void receiver_capabilities_get(const lql *self, lql_capabilities *out) {
  (void)self;
  lql_capabilities_get(out);
}

static lql_status receiver_selector_parse(lql *self, const char *expr,
                                          lql_selector **out,
                                          lql_error *error) {
  (void)self;
  return lql_selector_parse_impl(expr, out, error);
}

static lql_status receiver_selector_parse_or(lql *self, const char *expr,
                                             lql_selector **out,
                                             lql_error *error) {
  (void)self;
  return lql_selector_parse_or_impl(expr, out, error);
}

static void receiver_selector_free(lql *self, lql_selector *selector) {
  (void)self;
  lql_selector_free_impl(selector);
}

static int receiver_selector_is_empty(const lql *self,
                                      const lql_selector *selector) {
  (void)self;
  return lql_selector_is_empty_impl(selector);
}

static lql_status receiver_matches_json(lql *self, const lql_selector *selector,
                                        const char *json, size_t json_len,
                                        int *out_matched, lql_error *error) {
  (void)self;
  return lql_matches_json_impl(selector, json, json_len, out_matched, error);
}

static lql_status
receiver_query_file_decisions(lql *self, const lql_selector *selector,
                              FILE *file, lql_query_decision_fn on_decision,
                              void *user, lql_query_result *out_result,
                              lql_error *error) {
  (void)self;
  return lql_query_file_decisions_impl(selector, file, on_decision, user,
                                       out_result, error);
}

static lql_status receiver_query_file_decisions_with_options(
    lql *self, const lql_selector *selector, FILE *file,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error) {
  (void)self;
  return lql_query_file_decisions_with_options_impl(
      selector, file, options, on_decision, user, out_result, error);
}

static lql_status receiver_query_source_decisions(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_query_decision_fn on_decision, void *user, lql_query_result *out_result,
    lql_error *error) {
  (void)self;
  return lql_query_source_decisions_impl(selector, read, read_user, on_decision,
                                         user, out_result, error);
}

static lql_status receiver_query_source_decisions_with_options(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error) {
  (void)self;
  return lql_query_source_decisions_with_options_impl(
      selector, read, read_user, options, on_decision, user, out_result, error);
}

static lql_status
receiver_query_file_matches(lql *self, const lql_selector *selector, FILE *file,
                            lql_query_match_fn on_match, void *user,
                            lql_query_result *out_result, lql_error *error) {
  (void)self;
  return lql_query_file_matches_impl(selector, file, on_match, user, out_result,
                                     error);
}

static lql_status receiver_query_file_matches_with_options(
    lql *self, const lql_selector *selector, FILE *file,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error) {
  (void)self;
  return lql_query_file_matches_with_options_impl(
      selector, file, options, on_match, user, out_result, error);
}

static lql_status receiver_query_source_spooled_matches(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_query_match_fn on_match, void *user, lql_query_result *out_result,
    lql_error *error) {
  (void)self;
  return lql_query_source_spooled_matches_impl(
      selector, read, read_user, on_match, user, out_result, error);
}

static lql_status receiver_query_source_spooled_matches_with_options(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error) {
  (void)self;
  return lql_query_source_spooled_matches_with_options_impl(
      selector, read, read_user, options, on_match, user, out_result, error);
}

static lql_status receiver_payload_write_json(lql *self,
                                              const lql_payload *payload,
                                              FILE *out, lql_error *error) {
  (void)self;
  return lql_payload_write_json_impl(payload, out, error);
}

static lql_status receiver_payload_write_json_sink(lql *self,
                                                   const lql_payload *payload,
                                                   lql_write_fn write,
                                                   void *write_user,
                                                   lql_error *error) {
  (void)self;
  return lql_payload_write_json_sink_impl(payload, write, write_user, error);
}

static lql_status receiver_projection_parse(lql *self,
                                            const char *const *fields,
                                            size_t field_count,
                                            lql_projection **out,
                                            lql_error *error) {
  (void)self;
  return lql_projection_parse_impl(fields, field_count, out, error);
}

static void receiver_projection_free(lql *self, lql_projection *projection) {
  (void)self;
  lql_projection_free_impl(projection);
}

static lql_status
receiver_project_file_range(lql *self, const lql_projection *projection,
                            FILE *file, lql_uint64 offset, lql_uint64 size,
                            FILE *out, int *out_found, lql_error *error) {
  (void)self;
  return lql_project_file_range_impl(projection, file, offset, size, out,
                                     out_found, error);
}

static lql_status receiver_project_source(lql *self,
                                          const lql_projection *projection,
                                          lql_read_fn read, void *read_user,
                                          FILE *out, int *out_found,
                                          lql_error *error) {
  (void)self;
  return lql_project_source_impl(projection, read, read_user, out, out_found,
                                 error);
}

static lql_status receiver_project_json(lql *self,
                                        const lql_projection *projection,
                                        const char *json, size_t json_len,
                                        FILE *out, int *out_found,
                                        lql_error *error) {
  (void)self;
  return lql_project_json_impl(projection, json, json_len, out, out_found,
                               error);
}

static lql_status receiver_compact_file_range(lql *self, FILE *file,
                                              lql_uint64 offset,
                                              lql_uint64 size, FILE *out,
                                              lql_error *error) {
  (void)self;
  return lql_compact_file_range_impl(file, offset, size, out, error);
}

static lql_status receiver_compact_source(lql *self, lql_read_fn read,
                                          void *read_user, FILE *out,
                                          lql_error *error) {
  (void)self;
  return lql_compact_source_impl(read, read_user, out, error);
}

static lql_status receiver_compact_json(lql *self, const char *json,
                                        size_t json_len, FILE *out,
                                        lql_error *error) {
  (void)self;
  return lql_compact_json_impl(json, json_len, out, error);
}

static lql_status receiver_mutation_plan_parse(lql *self,
                                               const char *const *exprs,
                                               size_t expr_count,
                                               lql_mutation_plan **out,
                                               lql_error *error) {
  (void)self;
  return lql_mutation_plan_parse_impl(exprs, expr_count, out, error);
}

static lql_status receiver_mutation_plan_parse_with_options(
    lql *self, const char *const *exprs, size_t expr_count,
    const lql_mutation_parse_options *options, lql_mutation_plan **out,
    lql_error *error) {
  (void)self;
  return lql_mutation_plan_parse_with_options_impl(exprs, expr_count, options,
                                                   out, error);
}

static size_t receiver_mutation_plan_count(const lql *self,
                                           const lql_mutation_plan *plan) {
  (void)self;
  return lql_mutation_plan_count_impl(plan);
}

static void receiver_mutation_plan_free(lql *self, lql_mutation_plan *plan) {
  (void)self;
  lql_mutation_plan_free_impl(plan);
}

static lql_status receiver_mutate_file_range_root_fields(
    lql *self, const lql_mutation_plan *plan, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, lql_error *error) {
  (void)self;
  return lql_mutate_file_range_root_fields_impl(plan, file, offset, size, out,
                                                error);
}

static lql_status
receiver_mutate_file_range_paths(lql *self, const lql_mutation_plan *plan,
                                 FILE *file, lql_uint64 offset, lql_uint64 size,
                                 FILE *out, lql_error *error) {
  (void)self;
  return lql_mutate_file_range_paths_impl(plan, file, offset, size, out, error);
}

static lql_status receiver_mutate_file_range_candidates(
    lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
    FILE *file, lql_uint64 offset, lql_uint64 size, FILE *out, int compact,
    int matches_only, lql_query_result *out_result, lql_error *error) {
  (void)self;
  if (plan == NULL || file == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "plan, file, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return lql_eval_query_file_range_spooled_matches(
      selector, file, offset, size, out, compact, NULL, plan, matches_only,
      out_result, error);
}

static lql_status receiver_mutate_source_paths(lql *self,
                                               const lql_mutation_plan *plan,
                                               lql_read_fn read,
                                               void *read_user, FILE *out,
                                               lql_error *error) {
  (void)self;
  return lql_mutate_source_paths_impl(plan, read, read_user, out, error);
}

static lql_status receiver_mutate_source_candidates(
    lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
    lql_read_fn read, void *read_user, FILE *out, int compact,
    int matches_only, lql_query_result *out_result, lql_error *error) {
  (void)self;
  if (plan == NULL || read == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "plan, read, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return lql_eval_query_source_spooled_rewrite(selector, read, read_user, out,
                                               compact, NULL, plan,
                                               matches_only, out_result, error);
}

static lql_status receiver_mutate_json(lql *self, const lql_mutation_plan *plan,
                                       const char *json, size_t json_len,
                                       FILE *out, lql_error *error) {
  (void)self;
  return lql_mutate_json_impl(plan, json, json_len, out, error);
}

static void receiver_destroy(lql *self) { lql_dealloc(self); }

LQL_INTERNAL_SYMBOL void lql_node_cleanup(lql_node *node) {
  size_t i;
  if (node == NULL) {
    return;
  }
  lql_dealloc(node->term.field);
  lql_dealloc(node->term.value);
  for (i = 0u; i < node->term.any_count; ++i) {
    lql_dealloc(node->term.any[i]);
  }
  lql_dealloc(node->term.any);
  for (i = 0u; i < node->child_count; ++i) {
    lql_node_cleanup(&node->children[i]);
  }
  lql_dealloc(node->children);
  memset(node, 0, sizeof(*node));
}

LQL_INTERNAL_SYMBOL lql_status lql_selector_parse_impl(const char *expr,
                                                       lql_selector **out,
                                                       lql_error *error) {
  return lql_parse_selector_internal(expr, 0, out, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_selector_parse_or_impl(const char *expr,
                                                          lql_selector **out,
                                                          lql_error *error) {
  return lql_parse_selector_internal(expr, 1, out, error);
}

LQL_INTERNAL_SYMBOL void lql_selector_free_impl(lql_selector *selector) {
  if (selector != NULL) {
    lql_node_cleanup(&selector->root);
    lql_dealloc(selector);
  }
}

LQL_INTERNAL_SYMBOL int
lql_selector_is_empty_impl(const lql_selector *selector) {
  return selector == NULL || selector->root.kind == LQL_NODE_ALL;
}

LQL_INTERNAL_SYMBOL lql_status
lql_matches_json_impl(const lql_selector *selector, const char *json,
                      size_t json_len, int *out_matched, lql_error *error) {
  if (out_matched == NULL || json == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "json and out_matched are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (selector == NULL || selector->root.kind == LQL_NODE_ALL) {
    *out_matched = 1;
    return LQL_STATUS_OK;
  }
  return lql_eval_selector(selector, json, json_len, out_matched, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_query_file_decisions_impl(
    const lql_selector *selector, FILE *file, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error) {
  return lql_query_file_decisions_with_options_impl(
      selector, file, NULL, on_decision, user, out_result, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_query_file_decisions_with_options_impl(
    const lql_selector *selector, FILE *file, const lql_query_options *options,
    lql_query_decision_fn on_decision, void *user, lql_query_result *out_result,
    lql_error *error) {
  if (file == NULL || on_decision == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "file and on_decision are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return lql_eval_query_file_decisions(selector, file, options, on_decision,
                                       user, out_result, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_query_source_decisions_impl(
    const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_query_decision_fn on_decision, void *user, lql_query_result *out_result,
    lql_error *error) {
  return lql_query_source_decisions_with_options_impl(
      selector, read, read_user, NULL, on_decision, user, out_result, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_query_source_decisions_with_options_impl(
    const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error) {
  if (read == NULL || on_decision == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "read and on_decision are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return lql_eval_query_source_decisions(selector, read, read_user, options,
                                         on_decision, user, out_result, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_query_source_spooled_matches_impl(
    const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_query_match_fn on_match, void *user, lql_query_result *out_result,
    lql_error *error) {
  return lql_query_source_spooled_matches_with_options_impl(
      selector, read, read_user, NULL, on_match, user, out_result, error);
}

LQL_INTERNAL_SYMBOL lql_status
lql_query_source_spooled_matches_with_options_impl(
    const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error) {
  if (read == NULL || on_match == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "read and on_match are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return lql_eval_query_source_spooled_matches(
      selector, read, read_user, options, on_match, user, out_result, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_query_file_matches_impl(
    const lql_selector *selector, FILE *file, lql_query_match_fn on_match,
    void *user, lql_query_result *out_result, lql_error *error) {
  return lql_query_file_matches_with_options_impl(
      selector, file, NULL, on_match, user, out_result, error);
}

LQL_INTERNAL_SYMBOL lql_status lql_query_file_matches_with_options_impl(
    const lql_selector *selector, FILE *file, const lql_query_options *options,
    lql_query_match_fn on_match, void *user, lql_query_result *out_result,
    lql_error *error) {
  lql_match_adapter adapter;
  lql_status st;
  if (file == NULL || on_match == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "file and on_match are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  adapter.file = file;
  adapter.on_match = on_match;
  adapter.user = user;
  st = lql_query_file_decisions_with_options_impl(
      selector, file, options, on_match_decision, &adapter, out_result, error);
  if (st != LQL_STATUS_OK && error != NULL &&
      strcmp(error->message, "query decision callback failed") == 0) {
    lql_set_error(error, st, "query match callback failed");
  }
  return st;
}

LQL_INTERNAL_SYMBOL lql_status lql_payload_write_json_impl(
    const lql_payload *payload, FILE *out, lql_error *error) {
  if (payload == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "payload and output file are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return lql_payload_write_json_sink_impl(payload, payload_file_write, out,
                                          error);
}

LQL_INTERNAL_SYMBOL lql_status
lql_payload_write_json_sink_impl(const lql_payload *payload, lql_write_fn write,
                                 void *write_user, lql_error *error) {
  lql_payload_sink_adapter adapter;
  off_t current;
  lql_status copy_status;
  if (payload == NULL || write == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "payload and write callback are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (payload->kind != LQL_PAYLOAD_SEEKABLE_RANGE || payload->source == NULL) {
    if (payload->kind == LQL_PAYLOAD_SPOOLED && payload->spooled != NULL) {
      lonejson_error lj_error;
      memset(&adapter, 0, sizeof(adapter));
      adapter.write = write;
      adapter.user = write_user;
      if (lonejson_spooled_write_to_sink(
              (const lonejson_spooled *)payload->spooled, payload_lql_sink,
              &adapter, &lj_error) == LONEJSON_STATUS_OK) {
        return LQL_STATUS_OK;
      }
      if (adapter.status != LQL_STATUS_OK) {
        lql_set_error(error, adapter.status, "payload sink write failed");
        return adapter.status;
      }
      lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
      return LQL_STATUS_JSON_ERROR;
    }
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "payload is not a seekable source range");
    return LQL_STATUS_UNSUPPORTED;
  }
  current = ftello(payload->source);
  if (current < (off_t)0) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "failed to record source position");
    return LQL_STATUS_UNSUPPORTED;
  }
  if (!seek_u64(payload->source, payload->offset)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to write seekable payload range");
    return LQL_STATUS_JSON_ERROR;
  }
  copy_status =
      copy_range_to_sink(payload->source, payload->size, write, write_user);
  if (fseeko(payload->source, current, SEEK_SET) != 0) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to write seekable payload range");
    return LQL_STATUS_JSON_ERROR;
  }
  if (copy_status != LQL_STATUS_OK) {
    if (copy_status == LQL_STATUS_JSON_ERROR) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "failed to write seekable payload range");
    } else {
      lql_set_error(error, copy_status, "payload sink write failed");
    }
    return copy_status;
  }
  return LQL_STATUS_OK;
}
