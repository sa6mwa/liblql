#include "lql_internal.h"
#include "lql_json_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union allocation {
  size_t size;
  long double alignment;
  void *pointer;
} allocation;

typedef struct tracker {
  lql_allocator allocator;
  size_t attempts;
  size_t fail_at;
  size_t live;
  size_t peak;
} tracker;

static void *allocate(lql_allocator *allocator, size_t size) {
  tracker *t;
  allocation *p;
  t = (tracker *)allocator->impl;
  ++t->attempts;
  if (t->attempts == t->fail_at)
    return NULL;
  p = (allocation *)malloc(sizeof(*p) + size);
  if (p == NULL)
    return NULL;
  p->size = size;
  t->live += size;
  if (t->live > t->peak)
    t->peak = t->live;
  return p + 1;
}

static void release(lql_allocator *allocator, void *ptr) {
  tracker *t;
  allocation *p;
  if (ptr == NULL)
    return;
  t = (tracker *)allocator->impl;
  p = (allocation *)ptr - 1;
  t->live -= p->size;
  free(p);
}

static void *allocate_zero(lql_allocator *allocator, size_t count,
                           size_t size) {
  void *p;
  if (count != 0u && size > (size_t)-1 / count)
    return NULL;
  p = allocate(allocator, count * size);
  if (p != NULL)
    memset(p, 0, count * size);
  return p;
}

static void *resize(lql_allocator *allocator, void *ptr, size_t size) {
  void *next;
  size_t old_size;
  if (ptr == NULL)
    return allocate(allocator, size);
  if (size == 0u) {
    release(allocator, ptr);
    return NULL;
  }
  old_size = ((allocation *)ptr - 1)->size;
  next = allocate(allocator, size);
  if (next != NULL) {
    memcpy(next, ptr, old_size < size ? old_size : size);
    release(allocator, ptr);
  }
  return next;
}

static char *copy_string(lql_allocator *allocator, const char *text) {
  char *p;
  p = (char *)allocate(allocator, strlen(text) + 1u);
  if (p != NULL)
    strcpy(p, text);
  return p;
}

static void tracker_init(tracker *t, size_t fail_at) {
  memset(t, 0, sizeof(*t));
  t->fail_at = fail_at;
  t->allocator.impl = t;
  t->allocator.alloc = allocate;
  t->allocator.calloc = allocate_zero;
  t->allocator.realloc = resize;
  t->allocator.destroy = release;
  t->allocator.strdup = copy_string;
}

typedef struct source {
  const char *data;
  size_t offset;
  size_t written;
} source;

static lql_status read_source(void *user, unsigned char *buffer,
                              size_t capacity, size_t *out_len,
                              lql_error *error) {
  source *s;
  size_t amount;
  (void)error;
  s = (source *)user;
  amount = strlen(s->data) - s->offset;
  if (amount > capacity)
    amount = capacity;
  memcpy(buffer, s->data + s->offset, amount);
  s->offset += amount;
  *out_len = amount;
  return LQL_STATUS_OK;
}

static lql_status write_sink(void *user, const void *data, size_t len,
                             lql_error *error) {
  source *s;
  (void)data;
  (void)error;
  s = (source *)user;
  s->written += len;
  return LQL_STATUS_OK;
}

static lql_status file_open(void *user, lql_string_view path,
                            lql_stream_reader_fn *reader, void **reader_user,
                            lql_error *error) {
  source *s;
  (void)path;
  (void)error;
  s = (source *)user;
  s->offset = 0u;
  *reader = read_source;
  *reader_user = s;
  return LQL_STATUS_OK;
}

static void file_close(void *user, void *reader_user) {
  (void)user;
  (void)reader_user;
}

/* Exercise new workspace allocations through the receiver's quota allocator. */
static int receiver_case(const char *expr, const char *input,
                         const char *mutation_expr, size_t fail_at,
                         size_t *attempts) {
  tracker t;
  lql *client;
  lql_impl *impl;
  lql_allocator *upstream;
  lql_selector *selector;
  lql_mutation *mutation;
  lql_mutation_parse_options options;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  lql_status status;
  source s;
  source file;
  size_t baseline;
  int failed;
  tracker_init(&t, fail_at);
  client = NULL;
  selector = NULL;
  mutation = NULL;
  lql_error_init(&error);
  if (lql_new(&client, &error) != LQL_STATUS_OK)
    return 1;
  impl = (lql_impl *)client->impl;
  baseline = impl->live_bytes;
  upstream = impl->upstream_allocator;
  impl->upstream_allocator = &t.allocator;
  status = client->selector_parse(client, expr, &selector, &error);
  if (status == LQL_STATUS_OK && mutation_expr != NULL) {
    memset(&options, 0, sizeof(options));
    file.data = "file-backed value";
    file.offset = file.written = 0u;
    options.enable_file_values = 1;
    options.file_value_open = file_open;
    options.file_value_close = file_close;
    options.file_value_user = &file;
    status = client->mutation_parse_with_options(client, &mutation_expr, 1u,
                                                 &options, &mutation, &error);
  }
  if (status == LQL_STATUS_OK) {
    memset(&request, 0, sizeof(request));
    s.data = input;
    s.offset = s.written = 0u;
    request.reader = read_source;
    request.reader_user = &s;
    request.selector = selector;
    request.writer = write_sink;
    request.writer_user = &s;
    request.mutation = mutation;
    request.output_mode = mutation == NULL ? LQL_STREAM_OUTPUT_SELECTED_RECORD
                                           : LQL_STREAM_OUTPUT_MUTATION;
    status = client->stream_apply_spooled(client, &request, &result, &error);
    if (status == LQL_STATUS_OK &&
        (result.records_seen != 1u || result.records_matched != 1u ||
         s.written == 0u))
      status = LQL_STATUS_CALLBACK_ERROR;
  }
  client->selector_destroy(client, selector);
  client->mutation_destroy(client, mutation);
  failed = t.live != 0u || impl->live_bytes != baseline ||
           (fail_at != 0u ? status != LQL_STATUS_NO_MEMORY
                          : status != LQL_STATUS_OK);
  if (failed)
    fprintf(stderr, "allocation %lu of %s (%s): status=%d live=%lu: %s\n",
            (unsigned long)fail_at, expr,
            mutation_expr == NULL ? "selected" : mutation_expr, (int)status,
            (unsigned long)t.live, error.message);
  *attempts = t.attempts;
  impl->upstream_allocator = upstream;
  client->destroy(client);
  return failed;
}

static int receiver_failures(void) {
  static const char *const exprs[] = {
      "/kind=\"commit\"", "/.../kind=\"commit\"", "/n=1.5",
      "/timestamp=\"2026-03-05T09:29:00Z\"",
      ("and.eq{f=/kind,v=commit},or.0.eq{f=/kind,v=commit},or.1.eq{f=/"
       "kind,v=other}")};
  static const char *const mutations[] = {NULL, "textfile:/payload=fixture",
                                          "base64file:/payload=fixture",
                                          "file:/payload=fixture", "/n=2"};
  static const char input[] = "{\"kind\":\"commit\",\"n\":1.5,"
                              "\"timestamp\":\"2026-03-05T09:29:00Z\"}\n";
  size_t i;
  size_t j;
  size_t n;
  size_t fail_at;
  size_t attempts;
  for (i = 0u; i < sizeof(exprs) / sizeof(exprs[0]); ++i) {
    for (j = 0u; j < sizeof(mutations) / sizeof(mutations[0]); ++j) {
      if (receiver_case(exprs[i], input, mutations[j], 0u, &n))
        return 1;
      for (fail_at = 1u; fail_at <= n; ++fail_at) {
        if (receiver_case(exprs[i], input, mutations[j], fail_at, &attempts))
          return 1;
      }
    }
  }
  return 0;
}

typedef struct replay_sink {
  const lql_json_spool *spool;
  const char *expected;
  size_t offset;
  int fail;
  int nested;
} replay_sink;

static lql_status check_replay(void *user, const void *data, size_t len,
                               lql_error *error) {
  replay_sink *sink;
  lql_status status;
  source nested;
  sink = (replay_sink *)user;
  if (sink->fail)
    return LQL_STATUS_CALLBACK_ERROR;
  if (sink->offset > 70000u || len > 70000u - sink->offset ||
      memcmp(sink->expected + sink->offset, data, len) != 0)
    return LQL_STATUS_CALLBACK_ERROR;
  if (!sink->nested) {
    sink->nested = 1;
    nested.data = "";
    nested.offset = nested.written = 0u;
    status = lql_json_spool_write_slice(sink->spool, 65000u, 37u, write_sink,
                                        &nested, error);
    if (status != LQL_STATUS_OK || nested.written != 37u)
      return LQL_STATUS_CALLBACK_ERROR;
  }
  sink->offset += len;
  return LQL_STATUS_OK;
}

static int spool_failures(void) {
  tracker t;
  lql_json_spool spool;
  lql_error error;
  source output;
  replay_sink sink;
  char *data;
  unsigned char byte;
  size_t baseline;
  size_t i;
  size_t end;
  int failed;
  tracker_init(&t, 0u);
  data = (char *)malloc(70001u);
  if (data == NULL)
    return 1;
  for (i = 0u; i < 70000u; ++i)
    data[i] = (char)('a' + i % 23u);
  data[70000u] = '\0';
  output.data = data;
  output.offset = output.written = 0u;
  failed = lql_json_spool_init_with_allocator(&spool, &t.allocator, &error) !=
               LQL_STATUS_OK ||
           lql_json_spool_append(&spool, data, 70000u, &error) != LQL_STATUS_OK;
  baseline = t.live;
  t.fail_at = t.attempts + 1u;
  failed |= lql_json_spool_write_to(&spool, write_sink, &output, &error) !=
                LQL_STATUS_NO_MEMORY ||
            t.live != baseline || output.written != 0u;
  t.fail_at = t.attempts + 1u;
  failed |= lql_json_spool_byte_at(&spool, 0u, &byte, &error) !=
                LQL_STATUS_NO_MEMORY ||
            t.live != baseline;
  t.fail_at = t.attempts + 1u;
  failed |= lql_json_spool_find_string_end(&spool, 0u, 70000u, &end, NULL) !=
                LQL_STATUS_NO_MEMORY ||
            t.live != baseline;
  t.fail_at = 0u;
  failed |=
      lql_json_spool_write_to(&spool, write_sink, &output, &error) !=
          LQL_STATUS_OK ||
      output.written != 70000u ||
      lql_json_spool_byte_at(&spool, 69999u, &byte, &error) != LQL_STATUS_OK ||
      byte != (unsigned char)data[69999u];
  baseline = t.live;
  memset(&sink, 0, sizeof(sink));
  sink.spool = &spool;
  sink.expected = data;
  sink.fail = 1;
  failed |= lql_json_spool_write_to(&spool, check_replay, &sink, &error) !=
                LQL_STATUS_CALLBACK_ERROR ||
            t.live != baseline;
  sink.fail = 0;
  failed |= lql_json_spool_write_to(&spool, check_replay, &sink, &error) !=
                LQL_STATUS_OK ||
            sink.offset != 70000u || t.live != baseline;
  lql_json_spool_cleanup(&spool);
  failed |= t.live != 0u;
  free(data);
  if (failed)
    fprintf(stderr, "spool allocation failure/retry cleanup failed\n");
  return failed;
}

static lql_status record_match(void *user, size_t index, int is_object,
                               unsigned long hits, const lql_json_spool *spool,
                               size_t offset, size_t len, int compact,
                               lql_error *error) {
  size_t *matches;
  (void)spool;
  (void)offset;
  (void)len;
  (void)compact;
  (void)error;
  matches = (size_t *)user;
  if (index != 0u || !is_object || hits != 1ul)
    return LQL_STATUS_CALLBACK_ERROR;
  ++*matches;
  return LQL_STATUS_OK;
}

static int decision_workspace(void) {
  tracker t;
  lql_json_flat_eq_request request;
  lql_json_flat_eq_term *term;
  lql_error error;
  source s;
  size_t matches;
  size_t records;
  size_t bytes;
  int failed;
  tracker_init(&t, 0u);
  term = (lql_json_flat_eq_term *)calloc(1u, sizeof(*term));
  if (term == NULL)
    return 1;
  term->kind = LQL_JSON_FLAT_TERM_EQ;
  term->field = "kind";
  term->field_len = 4u;
  term->value = "commit";
  term->value_len = 6u;
  s.data = "{\"kind\":\"commit\"}\n";
  s.offset = s.written = matches = 0u;
  memset(&request, 0, sizeof(request));
  request.allocator = &t.allocator;
  request.reader = read_source;
  request.reader_user = &s;
  request.terms = term;
  request.term_count = 1u;
  request.record = record_match;
  request.record_user = &matches;
  failed = lql_json_scan_flat_eq_ndjson(&request, &records, &bytes, &error) !=
               LQL_STATUS_OK ||
           records != 1u || matches != 1u || bytes != strlen(s.data) ||
           t.live != 0u || t.peak > 32u * 1024u;
  printf("simple decision parser peak workspace=%lu bytes\n",
         (unsigned long)t.peak);
  free(term);
  return failed;
}

int main(void) {
  return receiver_failures() || spool_failures() || decision_workspace();
}
