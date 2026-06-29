#include "lql_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct counting_allocator {
  lql_allocator api;
  lql_allocator *backing;
  size_t alloc_count;
  size_t destroy_count;
  size_t outstanding;
  size_t outstanding_bytes;
  size_t peak_outstanding_bytes;
} counting_allocator;

typedef union counting_header {
  struct {
    size_t size;
  } meta;
  void *ptr;
  long l;
  double d;
  long double ld;
} counting_header;

typedef struct memory_reader {
  const char *data;
  size_t len;
  size_t offset;
  size_t chunk_size;
} memory_reader;

static void counting_destroy(lql_allocator *self, void *ptr);
static int make_blob_doc(char *buf, size_t capacity, size_t blob_len);
static int make_number_doc(char *buf, size_t capacity, size_t digit_len);
static int make_long_literal_expr(char *buf, size_t capacity,
                                  const char *prefix, size_t literal_len,
                                  const char *suffix);

static void counting_record_alloc(counting_allocator *counter, size_t size) {
  ++counter->alloc_count;
  ++counter->outstanding;
  counter->outstanding_bytes += size;
  if (counter->outstanding_bytes > counter->peak_outstanding_bytes) {
    counter->peak_outstanding_bytes = counter->outstanding_bytes;
  }
}

static counting_header *counting_header_from_user(void *ptr) {
  return ((counting_header *)ptr) - 1;
}

static void *counting_alloc(lql_allocator *self, size_t size) {
  counting_allocator *counter;
  counting_header *raw;

  counter = (counting_allocator *)self->impl;
  raw = (counting_header *)counter->backing->alloc(counter->backing,
                                                   sizeof(*raw) + size);
  if (raw != NULL) {
    raw->meta.size = size;
    counting_record_alloc(counter, size);
    return (void *)(raw + 1);
  }
  return NULL;
}

static void *counting_calloc(lql_allocator *self, size_t count, size_t size) {
  counting_allocator *counter;
  counting_header *raw;
  size_t total;

  counter = (counting_allocator *)self->impl;
  total = count * size;
  raw = (counting_header *)counter->backing->calloc(counter->backing, 1u,
                                                    sizeof(*raw) + total);
  if (raw != NULL) {
    raw->meta.size = total;
    counting_record_alloc(counter, total);
    return (void *)(raw + 1);
  }
  return NULL;
}

static void *counting_realloc(lql_allocator *self, void *ptr, size_t size) {
  counting_allocator *counter;
  counting_header *raw;
  counting_header *next;
  size_t old_size;

  counter = (counting_allocator *)self->impl;
  if (ptr == NULL) {
    return counting_alloc(self, size);
  }
  if (size == 0u) {
    counting_destroy(self, ptr);
    return NULL;
  }
  raw = counting_header_from_user(ptr);
  old_size = raw->meta.size;
  next = (counting_header *)counter->backing->realloc(counter->backing, raw,
                                                      sizeof(*raw) + size);
  if (next != NULL) {
    next->meta.size = size;
    ++counter->alloc_count;
    if (size >= old_size) {
      counter->outstanding_bytes += size - old_size;
    } else {
      counter->outstanding_bytes -= old_size - size;
    }
    if (counter->outstanding_bytes > counter->peak_outstanding_bytes) {
      counter->peak_outstanding_bytes = counter->outstanding_bytes;
    }
    return (void *)(next + 1);
  }
  return NULL;
}

static void counting_destroy(lql_allocator *self, void *ptr) {
  counting_allocator *counter;
  counting_header *raw;

  if (ptr == NULL) {
    return;
  }
  counter = (counting_allocator *)self->impl;
  raw = counting_header_from_user(ptr);
  ++counter->destroy_count;
  if (counter->outstanding > 0u) {
    --counter->outstanding;
  }
  if (counter->outstanding_bytes >= raw->meta.size) {
    counter->outstanding_bytes -= raw->meta.size;
  } else {
    counter->outstanding_bytes = 0u;
  }
  counter->backing->destroy(counter->backing, raw);
}

static char *counting_strdup(lql_allocator *self, const char *text) {
  char *ptr;
  size_t len;

  if (text == NULL) {
    return NULL;
  }
  len = strlen(text);
  ptr = (char *)counting_alloc(self, len + 1u);
  if (ptr != NULL) {
    memcpy(ptr, text, len + 1u);
  }
  return ptr;
}

static void counting_allocator_init(counting_allocator *counter) {
  memset(counter, 0, sizeof(*counter));
  counter->backing = lql_allocator_default();
  counter->api.impl = counter;
  counter->api.alloc = counting_alloc;
  counter->api.calloc = counting_calloc;
  counter->api.realloc = counting_realloc;
  counter->api.destroy = counting_destroy;
  counter->api.strdup = counting_strdup;
}

static lql_read_result read_memory_chunk(void *user, unsigned char *buffer,
                                         size_t capacity) {
  memory_reader *reader;
  lql_read_result result;
  size_t n;

  reader = (memory_reader *)user;
  memset(&result, 0, sizeof(result));
  if (reader->offset >= reader->len) {
    result.eof = 1;
    return result;
  }
  n = reader->len - reader->offset;
  if (n > capacity) {
    n = capacity;
  }
  if (reader->chunk_size != 0u && n > reader->chunk_size) {
    n = reader->chunk_size;
  }
  memcpy(buffer, reader->data + reader->offset, n);
  reader->offset += n;
  result.bytes_read = n;
  result.eof = reader->offset >= reader->len;
  return result;
}

static lql_status count_matched_decision(void *user,
                                         const lql_query_decision *decision) {
  size_t *matched;

  matched = (size_t *)user;
  if (decision != NULL && decision->matched) {
    ++*matched;
  }
  return LQL_STATUS_OK;
}

static lql_status count_spooled_match(void *user,
                                      const lql_query_match *match) {
  size_t *matched;

  matched = (size_t *)user;
  if (match == NULL || match->payload.kind != LQL_PAYLOAD_SPOOLED ||
      match->payload.spooled == NULL) {
    return LQL_STATUS_JSON_ERROR;
  }
  ++*matched;
  return LQL_STATUS_OK;
}

static int expect_selector_success_uses_allocator(void) {
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;
  size_t alloc_count_after_second_warmup;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("selector allocator receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK || selector == NULL) {
    printf("selector allocator parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.alloc_count == 0u) {
    printf("selector allocator was not used\n");
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  matched = 0;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, "{\"status\":\"open\"}",
                         strlen("{\"status\":\"open\"}"), &matched, &error);
  if (st != LQL_STATUS_OK || !matched) {
    printf("selector allocator eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  matched = 0;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, "{\"status\":\"open\"}",
                         strlen("{\"status\":\"open\"}"), &matched, &error);
  if (st != LQL_STATUS_OK || !matched) {
    printf("selector allocator second warmup failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  alloc_count_after_second_warmup = counter.alloc_count;
  matched = 0;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, "{\"status\":\"open\"}",
                         strlen("{\"status\":\"open\"}"), &matched, &error);
  if (st != LQL_STATUS_OK || !matched) {
    printf("selector allocator steady eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.alloc_count != alloc_count_after_second_warmup) {
    printf("selector steady eval allocated: before=%lu after=%lu\n",
           (unsigned long)alloc_count_after_second_warmup,
           (unsigned long)counter.alloc_count);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf(
        "selector allocator cleanup imbalance: outstanding=%lu destroys=%lu\n",
        (unsigned long)counter.outstanding,
        (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_file_decisions_steady_state_has_no_receiver_alloc(void) {
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  FILE *file;
  const char *json;
  lql_query_result result;
  size_t matched;
  size_t alloc_count_after_second_warmup;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("file decision receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK || selector == NULL) {
    printf("file decision selector parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  file = tmpfile();
  if (file == NULL) {
    printf("file decision tmpfile failed\n");
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  json = "{\"status\":\"open\",\"message\":\"hello\"}\n"
         "{\"status\":\"closed\",\"message\":\"bye\"}\n";
  if (fwrite(json, 1u, strlen(json), file) != strlen(json)) {
    printf("file decision fixture write failed\n");
    fclose(file);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (fseek(file, 0L, SEEK_SET) != 0) {
    printf("file decision fixture seek failed\n");
    fclose(file);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  matched = 0u;
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = ctx->query_file_decisions(ctx, selector, file, count_matched_decision,
                                 &matched, &result, &error);
  if (st != LQL_STATUS_OK || matched != 1u || result.candidates_seen != 2u) {
    printf("file decision first warmup failed: %s\n", error.message);
    fclose(file);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (fseek(file, 0L, SEEK_SET) != 0) {
    printf("file decision second seek failed\n");
    fclose(file);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  matched = 0u;
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = ctx->query_file_decisions(ctx, selector, file, count_matched_decision,
                                 &matched, &result, &error);
  if (st != LQL_STATUS_OK || matched != 1u || result.candidates_seen != 2u) {
    printf("file decision second warmup failed: %s\n", error.message);
    fclose(file);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  alloc_count_after_second_warmup = counter.alloc_count;
  if (fseek(file, 0L, SEEK_SET) != 0) {
    printf("file decision steady seek failed\n");
    fclose(file);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  matched = 0u;
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = ctx->query_file_decisions(ctx, selector, file, count_matched_decision,
                                 &matched, &result, &error);
  fclose(file);
  if (st != LQL_STATUS_OK || matched != 1u || result.candidates_seen != 2u) {
    printf("file decision steady eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.alloc_count != alloc_count_after_second_warmup) {
    printf("file decision steady eval allocated: before=%lu after=%lu\n",
           (unsigned long)alloc_count_after_second_warmup,
           (unsigned long)counter.alloc_count);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf("file decision allocator cleanup imbalance: outstanding=%lu "
           "destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_source_decisions_steady_state_has_no_receiver_alloc(void) {
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  const char *json;
  memory_reader reader;
  lql_query_result result;
  size_t matched;
  size_t alloc_count_after_second_warmup;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("source decision receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK || selector == NULL) {
    printf("source decision selector parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  json = "{\"status\":\"open\",\"message\":\"hello\"}\n"
         "{\"status\":\"closed\",\"message\":\"bye\"}\n";

  reader.data = json;
  reader.len = strlen(json);
  reader.offset = 0u;
  reader.chunk_size = 7u;
  matched = 0u;
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = ctx->query_source_decisions(ctx, selector, read_memory_chunk, &reader,
                                   count_matched_decision, &matched, &result,
                                   &error);
  if (st != LQL_STATUS_OK || matched != 1u || result.candidates_seen != 2u) {
    printf("source decision first warmup failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }

  reader.offset = 0u;
  matched = 0u;
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = ctx->query_source_decisions(ctx, selector, read_memory_chunk, &reader,
                                   count_matched_decision, &matched, &result,
                                   &error);
  if (st != LQL_STATUS_OK || matched != 1u || result.candidates_seen != 2u) {
    printf("source decision second warmup failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  alloc_count_after_second_warmup = counter.alloc_count;

  reader.offset = 0u;
  matched = 0u;
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = ctx->query_source_decisions(ctx, selector, read_memory_chunk, &reader,
                                   count_matched_decision, &matched, &result,
                                   &error);
  if (st != LQL_STATUS_OK || matched != 1u || result.candidates_seen != 2u) {
    printf("source decision steady eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.alloc_count != alloc_count_after_second_warmup) {
    printf("source decision steady eval allocated: before=%lu after=%lu\n",
           (unsigned long)alloc_count_after_second_warmup,
           (unsigned long)counter.alloc_count);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf("source decision allocator cleanup imbalance: outstanding=%lu "
           "destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int
expect_source_array_decisions_steady_state_has_no_receiver_alloc(void) {
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  const char *json;
  memory_reader reader;
  lql_query_result result;
  size_t matched;
  size_t alloc_count_after_second_warmup;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("source array decision receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK || selector == NULL) {
    printf("source array decision selector parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  json = "[{\"status\":\"open\",\"message\":\"hello\"},"
         "{\"status\":\"closed\",\"message\":\"bye\"}]";

  reader.data = json;
  reader.len = strlen(json);
  reader.offset = 0u;
  reader.chunk_size = 7u;
  matched = 0u;
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = ctx->query_source_decisions(ctx, selector, read_memory_chunk, &reader,
                                   count_matched_decision, &matched, &result,
                                   &error);
  if (st != LQL_STATUS_OK || matched != 1u || result.candidates_seen != 2u) {
    printf("source array decision first warmup failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }

  reader.offset = 0u;
  matched = 0u;
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = ctx->query_source_decisions(ctx, selector, read_memory_chunk, &reader,
                                   count_matched_decision, &matched, &result,
                                   &error);
  if (st != LQL_STATUS_OK || matched != 1u || result.candidates_seen != 2u) {
    printf("source array decision second warmup failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  alloc_count_after_second_warmup = counter.alloc_count;

  reader.offset = 0u;
  matched = 0u;
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = ctx->query_source_decisions(ctx, selector, read_memory_chunk, &reader,
                                   count_matched_decision, &matched, &result,
                                   &error);
  if (st != LQL_STATUS_OK || matched != 1u || result.candidates_seen != 2u) {
    printf("source array decision steady eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.alloc_count != alloc_count_after_second_warmup) {
    printf(
        "source array decision steady eval allocated: before=%lu after=%lu\n",
        (unsigned long)alloc_count_after_second_warmup,
        (unsigned long)counter.alloc_count);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf("source array decision allocator cleanup imbalance: outstanding=%lu "
           "destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int make_large_match_doc(char *buf, size_t capacity, size_t blob_len) {
  const char *prefix;
  const char *suffix;
  size_t prefix_len;
  size_t suffix_len;

  prefix = "{\"id\":\"a\",\"blob\":\"";
  suffix = "\"}\n";
  prefix_len = strlen(prefix);
  suffix_len = strlen(suffix);
  if (capacity <= prefix_len + blob_len + suffix_len) {
    return 0;
  }
  memcpy(buf, prefix, prefix_len);
  memset(buf + prefix_len, 'x', blob_len);
  memcpy(buf + prefix_len + blob_len, suffix, suffix_len + 1u);
  return 1;
}

static int run_source_spooled_match_query(lql *ctx, lql_selector *selector,
                                          const char *json, size_t *out_matches,
                                          lql_query_result *out_result,
                                          lql_error *error) {
  memory_reader reader;

  reader.data = json;
  reader.len = strlen(json);
  reader.offset = 0u;
  reader.chunk_size = 4096u;
  *out_matches = 0u;
  memset(out_result, 0, sizeof(*out_result));
  lql_error_init(error);
  return ctx->query_source_spooled_matches(
             ctx, selector, read_memory_chunk, &reader, count_spooled_match,
             out_matches, out_result, error) == LQL_STATUS_OK
             ? 0
             : 1;
}

static int expect_source_spooled_match_peak_is_per_candidate_bounded(void) {
  static char single_doc[90000];
  static char double_doc[180000];
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  lql_query_result result;
  size_t matches;
  size_t baseline_peak;
  size_t single_delta;
  size_t double_delta;
  size_t single_len;

  if (!make_large_match_doc(single_doc, sizeof(single_doc), 65536u)) {
    printf("source spooled single fixture construction failed\n");
    return 1;
  }
  single_len = strlen(single_doc);
  if (sizeof(double_doc) <= single_len * 2u) {
    printf("source spooled double fixture too small\n");
    return 1;
  }
  memcpy(double_doc, single_doc, single_len);
  memcpy(double_doc + single_len, single_doc, single_len + 1u);

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("source spooled receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "/id=\"a\"", &selector, &error);
  if (st != LQL_STATUS_OK || selector == NULL) {
    printf("source spooled selector parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }

  baseline_peak = counter.peak_outstanding_bytes;
  if (run_source_spooled_match_query(ctx, selector, single_doc, &matches,
                                     &result, &error) != 0 ||
      matches != 1u || result.candidates_seen != 1u) {
    printf("source spooled single query failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  single_delta = counter.peak_outstanding_bytes - baseline_peak;

  if (run_source_spooled_match_query(ctx, selector, double_doc, &matches,
                                     &result, &error) != 0 ||
      matches != 2u || result.candidates_seen != 2u) {
    printf("source spooled double query failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  double_delta = counter.peak_outstanding_bytes - baseline_peak;

  if (single_delta == 0u || single_delta > 196608u) {
    printf("source spooled single peak out of bounds: delta=%lu\n",
           (unsigned long)single_delta);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (double_delta > single_delta + 32768u) {
    printf("source spooled peak grew with stream size: single=%lu double=%lu\n",
           (unsigned long)single_delta, (unsigned long)double_delta);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }

  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.outstanding_bytes != 0u ||
      counter.destroy_count == 0u) {
    printf("source spooled allocator cleanup imbalance: outstanding=%lu "
           "bytes=%lu destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.outstanding_bytes,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_mutation_success_uses_allocator(void) {
  counting_allocator counter;
  lql *ctx;
  lql_mutation_plan *plan;
  const char *exprs[2];
  lql_error error;
  lql_status st;
  FILE *out;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("mutation allocator receiver failed: %s\n", error.message);
    return 1;
  }
  exprs[0] = "/count=+2";
  exprs[1] = "/title=\"done\"";
  plan = NULL;
  lql_error_init(&error);
  st = ctx->mutation_plan_parse(ctx, exprs, 2u, &plan, &error);
  if (st != LQL_STATUS_OK || plan == NULL) {
    printf("mutation allocator parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.alloc_count == 0u || ctx->mutation_plan_count(ctx, plan) != 2u) {
    printf("mutation allocator was not used or count mismatch\n");
    ctx->mutation_plan_destroy(ctx, plan);
    ctx->destroy(ctx);
    return 1;
  }
  out = tmpfile();
  if (out == NULL) {
    printf("mutation allocator tmpfile failed\n");
    ctx->mutation_plan_destroy(ctx, plan);
    ctx->destroy(ctx);
    return 1;
  }
  lql_error_init(&error);
  st = ctx->mutate_json(ctx, plan, "{\"count\":3,\"title\":\"old\"}",
                        strlen("{\"count\":3,\"title\":\"old\"}"), out, &error);
  fclose(out);
  if (st != LQL_STATUS_OK) {
    printf("mutation allocator runtime failed: %s\n", error.message);
    ctx->mutation_plan_destroy(ctx, plan);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->mutation_plan_destroy(ctx, plan);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf(
        "mutation allocator cleanup imbalance: outstanding=%lu destroys=%lu\n",
        (unsigned long)counter.outstanding,
        (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_mutation_parse_failure_cleans_allocator(void) {
  counting_allocator counter;
  lql *ctx;
  lql_mutation_plan *plan;
  const char *exprs[1];
  lql_error error;
  lql_status st;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("mutation allocator receiver failed: %s\n", error.message);
    return 1;
  }
  exprs[0] = "/=1";
  plan = NULL;
  lql_error_init(&error);
  st = ctx->mutation_plan_parse(ctx, exprs, 1u, &plan, &error);
  if (st == LQL_STATUS_OK || plan != NULL) {
    printf("mutation allocator parse failure unexpectedly succeeded\n");
    ctx->mutation_plan_destroy(ctx, plan);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->destroy(ctx);
  if (counter.alloc_count == 0u || counter.outstanding != 0u) {
    printf("mutation allocator failure cleanup imbalance: allocs=%lu "
           "outstanding=%lu\n",
           (unsigned long)counter.alloc_count,
           (unsigned long)counter.outstanding);
    return 1;
  }
  return 0;
}

static int mutate_blob_doc(lql *ctx, lql_mutation_plan *plan, const char *json,
                           counting_allocator *counter, size_t *out_delta) {
  lql_error error;
  lql_status st;
  FILE *out;
  size_t before;

  out = tmpfile();
  if (out == NULL) {
    printf("mutation blob tmpfile failed\n");
    return 1;
  }
  before = counter->alloc_count;
  lql_error_init(&error);
  st = ctx->mutate_json(ctx, plan, json, strlen(json), out, &error);
  fclose(out);
  if (st != LQL_STATUS_OK) {
    printf("mutation blob mutate failed: %s\n", error.message);
    return 1;
  }
  *out_delta = counter->alloc_count - before;
  return 0;
}

static int expect_mutation_unselected_large_blob_allocation_stable(void) {
  counting_allocator counter;
  lql *ctx;
  lql_mutation_plan *plan;
  const char *exprs[1];
  lql_error error;
  lql_status st;
  size_t small_delta;
  size_t large_delta;
  size_t small_peak;
  char small_doc[128];
  char large_doc[10064];

  if (!make_blob_doc(small_doc, sizeof(small_doc), 32u) ||
      !make_blob_doc(large_doc, sizeof(large_doc), 9800u)) {
    printf("mutation blob fixture construction failed\n");
    return 1;
  }

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("mutation blob receiver failed: %s\n", error.message);
    return 1;
  }
  exprs[0] = "/id=\"b\"";
  plan = NULL;
  lql_error_init(&error);
  st = ctx->mutation_plan_parse(ctx, exprs, 1u, &plan, &error);
  if (st != LQL_STATUS_OK || plan == NULL) {
    printf("mutation blob parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  if (mutate_blob_doc(ctx, plan, small_doc, &counter, &small_delta) != 0) {
    ctx->mutation_plan_destroy(ctx, plan);
    ctx->destroy(ctx);
    return 1;
  }
  small_peak = counter.peak_outstanding_bytes;
  if (mutate_blob_doc(ctx, plan, large_doc, &counter, &large_delta) != 0) {
    ctx->mutation_plan_destroy(ctx, plan);
    ctx->destroy(ctx);
    return 1;
  }
  if (large_delta > small_delta + 1u) {
    printf("mutation unselected blob allocation grew with input: small=%lu "
           "large=%lu\n",
           (unsigned long)small_delta, (unsigned long)large_delta);
    ctx->mutation_plan_destroy(ctx, plan);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.peak_outstanding_bytes > small_peak + 32768u) {
    printf("mutation unselected blob peak grew with input: small=%lu "
           "large=%lu\n",
           (unsigned long)small_peak,
           (unsigned long)counter.peak_outstanding_bytes);
    ctx->mutation_plan_destroy(ctx, plan);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->mutation_plan_destroy(ctx, plan);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.outstanding_bytes != 0u ||
      counter.destroy_count == 0u) {
    printf("mutation blob allocator cleanup imbalance: outstanding=%lu "
           "bytes=%lu destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.outstanding_bytes,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_selector_parse_failure_cleans_allocator(void) {
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("selector allocator receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx,
                           "contains{field=/title,value=urgent,field=/other}",
                           &selector, &error);
  if (st == LQL_STATUS_OK || selector != NULL) {
    printf("selector allocator parse failure unexpectedly succeeded\n");
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->destroy(ctx);
  if (counter.alloc_count == 0u || counter.outstanding != 0u) {
    printf("selector allocator failure cleanup imbalance: allocs=%lu "
           "outstanding=%lu\n",
           (unsigned long)counter.alloc_count,
           (unsigned long)counter.outstanding);
    return 1;
  }
  return 0;
}

static int expect_projection_success_uses_allocator(void) {
  counting_allocator counter;
  lql *ctx;
  lql_projection *projection;
  const char *fields[2];
  lql_error error;
  lql_status st;
  int found;
  FILE *out;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("projection allocator receiver failed: %s\n", error.message);
    return 1;
  }
  fields[0] = "/title";
  fields[1] = "/items/0/name";
  projection = NULL;
  lql_error_init(&error);
  st = ctx->projection_parse(ctx, fields, 2u, &projection, &error);
  if (st != LQL_STATUS_OK || projection == NULL) {
    printf("projection allocator parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.alloc_count == 0u) {
    printf("projection allocator was not used\n");
    ctx->projection_destroy(ctx, projection);
    ctx->destroy(ctx);
    return 1;
  }
  out = tmpfile();
  if (out == NULL) {
    printf("projection allocator tmpfile failed\n");
    ctx->projection_destroy(ctx, projection);
    ctx->destroy(ctx);
    return 1;
  }
  found = 0;
  lql_error_init(&error);
  st = ctx->project_json(
      ctx, projection,
      "{\"title\":\"T\",\"items\":[{\"name\":\"N\"}],\"ignored\":1}",
      strlen("{\"title\":\"T\",\"items\":[{\"name\":\"N\"}],\"ignored\":1}"),
      out, &found, &error);
  fclose(out);
  if (st != LQL_STATUS_OK || !found) {
    printf("projection allocator project failed: %s\n", error.message);
    ctx->projection_destroy(ctx, projection);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->projection_destroy(ctx, projection);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf("projection allocator cleanup imbalance: outstanding=%lu "
           "destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int make_blob_doc(char *buf, size_t capacity, size_t blob_len) {
  const char *prefix;
  const char *suffix;
  size_t prefix_len;
  size_t suffix_len;

  prefix = "{\"id\":\"a\",\"blob\":\"";
  suffix = "\"}";
  prefix_len = strlen(prefix);
  suffix_len = strlen(suffix);
  if (capacity <= prefix_len + blob_len + suffix_len) {
    return 0;
  }
  memcpy(buf, prefix, prefix_len);
  memset(buf + prefix_len, 'x', blob_len);
  memcpy(buf + prefix_len + blob_len, suffix, suffix_len + 1u);
  return 1;
}

static int make_number_doc(char *buf, size_t capacity, size_t digit_len) {
  const char *prefix;
  const char *suffix;
  size_t prefix_len;
  size_t suffix_len;

  prefix = "{\"n\":";
  suffix = "}";
  prefix_len = strlen(prefix);
  suffix_len = strlen(suffix);
  if (digit_len == 0u || capacity <= prefix_len + digit_len + suffix_len) {
    return 0;
  }
  memcpy(buf, prefix, prefix_len);
  memset(buf + prefix_len, '9', digit_len);
  memcpy(buf + prefix_len + digit_len, suffix, suffix_len + 1u);
  return 1;
}

static int make_long_literal_expr(char *buf, size_t capacity,
                                  const char *prefix, size_t literal_len,
                                  const char *suffix) {
  size_t prefix_len;
  size_t suffix_len;

  prefix_len = strlen(prefix);
  suffix_len = strlen(suffix);
  if (capacity <= prefix_len + literal_len + suffix_len) {
    return 0;
  }
  memcpy(buf, prefix, prefix_len);
  memset(buf + prefix_len, 'x', literal_len);
  memcpy(buf + prefix_len + literal_len, suffix, suffix_len + 1u);
  return 1;
}

static int project_blob_doc(lql *ctx, lql_projection *projection,
                            const char *json, counting_allocator *counter,
                            size_t *out_delta) {
  lql_error error;
  lql_status st;
  FILE *out;
  int found;
  char output[32];
  size_t before;
  size_t nread;

  out = tmpfile();
  if (out == NULL) {
    printf("projection blob tmpfile failed\n");
    return 1;
  }
  found = 0;
  before = counter->alloc_count;
  lql_error_init(&error);
  st = ctx->project_json(ctx, projection, json, strlen(json), out, &found,
                         &error);
  if (st != LQL_STATUS_OK || !found) {
    printf("projection blob project failed: %s\n", error.message);
    fclose(out);
    return 1;
  }
  if (fseek(out, 0L, SEEK_SET) != 0) {
    printf("projection blob output seek failed\n");
    fclose(out);
    return 1;
  }
  memset(output, 0, sizeof(output));
  nread = fread(output, 1u, sizeof(output) - 1u, out);
  fclose(out);
  if (nread != strlen("{\"id\":\"a\"}") ||
      memcmp(output, "{\"id\":\"a\"}", strlen("{\"id\":\"a\"}")) != 0) {
    printf("projection blob output mismatch: %s\n", output);
    return 1;
  }
  *out_delta = counter->alloc_count - before;
  return 0;
}

static int expect_projection_unselected_large_blob_allocation_stable(void) {
  counting_allocator counter;
  lql *ctx;
  lql_projection *projection;
  const char *fields[1];
  lql_error error;
  lql_status st;
  size_t small_delta;
  size_t large_delta;
  size_t small_peak;
  char small_doc[128];
  char large_doc[10064];

  if (!make_blob_doc(small_doc, sizeof(small_doc), 32u) ||
      !make_blob_doc(large_doc, sizeof(large_doc), 9800u)) {
    printf("projection blob fixture construction failed\n");
    return 1;
  }

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("projection blob receiver failed: %s\n", error.message);
    return 1;
  }
  fields[0] = "/id";
  projection = NULL;
  lql_error_init(&error);
  st = ctx->projection_parse(ctx, fields, 1u, &projection, &error);
  if (st != LQL_STATUS_OK || projection == NULL) {
    printf("projection blob parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  if (project_blob_doc(ctx, projection, small_doc, &counter, &small_delta) !=
      0) {
    ctx->projection_destroy(ctx, projection);
    ctx->destroy(ctx);
    return 1;
  }
  small_peak = counter.peak_outstanding_bytes;
  if (project_blob_doc(ctx, projection, large_doc, &counter, &large_delta) !=
      0) {
    ctx->projection_destroy(ctx, projection);
    ctx->destroy(ctx);
    return 1;
  }
  if (large_delta > small_delta + 1u) {
    printf("projection unselected blob allocation grew with input: small=%lu "
           "large=%lu\n",
           (unsigned long)small_delta, (unsigned long)large_delta);
    ctx->projection_destroy(ctx, projection);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.peak_outstanding_bytes > small_peak + 32768u) {
    printf("projection unselected blob peak grew with input: small=%lu "
           "large=%lu\n",
           (unsigned long)small_peak,
           (unsigned long)counter.peak_outstanding_bytes);
    ctx->projection_destroy(ctx, projection);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->projection_destroy(ctx, projection);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf("projection blob allocator cleanup imbalance: outstanding=%lu "
           "destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_selector_contains_large_blob_allocation_stable(void) {
  static char small_doc[128];
  static char large_doc[70064];
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;
  size_t small_peak;

  if (!make_blob_doc(small_doc, sizeof(small_doc), 32u) ||
      !make_blob_doc(large_doc, sizeof(large_doc), 69800u)) {
    printf("selector contains blob fixture construction failed\n");
    return 1;
  }

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("selector contains blob receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "contains{field=/blob,value=missing}",
                           &selector, &error);
  if (st != LQL_STATUS_OK || selector == NULL) {
    printf("selector contains blob parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }

  matched = 1;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, small_doc, strlen(small_doc), &matched,
                         &error);
  if (st != LQL_STATUS_OK || matched) {
    printf("selector contains blob small eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  small_peak = counter.peak_outstanding_bytes;

  matched = 1;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, large_doc, strlen(large_doc), &matched,
                         &error);
  if (st != LQL_STATUS_OK || matched) {
    printf("selector contains blob large eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.peak_outstanding_bytes > small_peak + 32768u) {
    printf("selector contains selected blob peak grew with input: small=%lu "
           "large=%lu\n",
           (unsigned long)small_peak,
           (unsigned long)counter.peak_outstanding_bytes);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }

  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf("selector contains blob allocator cleanup imbalance: "
           "outstanding=%lu destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_selector_prefix_large_blob_allocation_stable(void) {
  static char small_doc[128];
  static char large_doc[70064];
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;
  size_t small_peak;

  if (!make_blob_doc(small_doc, sizeof(small_doc), 32u) ||
      !make_blob_doc(large_doc, sizeof(large_doc), 69800u)) {
    printf("selector prefix blob fixture construction failed\n");
    return 1;
  }

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("selector prefix blob receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "iprefix{field=/blob,value=xx}", &selector,
                           &error);
  if (st != LQL_STATUS_OK || selector == NULL) {
    printf("selector prefix blob parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }

  matched = 0;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, small_doc, strlen(small_doc), &matched,
                         &error);
  if (st != LQL_STATUS_OK || !matched) {
    printf("selector prefix blob small eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  small_peak = counter.peak_outstanding_bytes;

  matched = 0;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, large_doc, strlen(large_doc), &matched,
                         &error);
  if (st != LQL_STATUS_OK || !matched) {
    printf("selector prefix blob large eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.peak_outstanding_bytes > small_peak + 32768u) {
    printf("selector prefix selected blob peak grew with input: small=%lu "
           "large=%lu\n",
           (unsigned long)small_peak,
           (unsigned long)counter.peak_outstanding_bytes);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }

  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf("selector prefix blob allocator cleanup imbalance: "
           "outstanding=%lu destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_selector_exact_large_blob_allocation_stable(void) {
  static char small_doc[128];
  static char large_doc[70064];
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;
  size_t small_peak;

  if (!make_blob_doc(small_doc, sizeof(small_doc), 32u) ||
      !make_blob_doc(large_doc, sizeof(large_doc), 69800u)) {
    printf("selector exact blob fixture construction failed\n");
    return 1;
  }

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("selector exact blob receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "eq{field=/blob,value=missing}", &selector,
                           &error);
  if (st != LQL_STATUS_OK || selector == NULL) {
    printf("selector exact blob parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }

  matched = 1;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, small_doc, strlen(small_doc), &matched,
                         &error);
  if (st != LQL_STATUS_OK || matched) {
    printf("selector exact blob small eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  small_peak = counter.peak_outstanding_bytes;

  matched = 1;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, large_doc, strlen(large_doc), &matched,
                         &error);
  if (st != LQL_STATUS_OK || matched) {
    printf("selector exact blob large eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.peak_outstanding_bytes > small_peak + 32768u) {
    printf("selector exact selected blob peak grew with input: small=%lu "
           "large=%lu\n",
           (unsigned long)small_peak,
           (unsigned long)counter.peak_outstanding_bytes);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }

  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf("selector exact blob allocator cleanup imbalance: "
           "outstanding=%lu destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_selector_in_large_blob_allocation_stable(void) {
  static char small_doc[128];
  static char large_doc[70064];
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;
  size_t small_peak;

  if (!make_blob_doc(small_doc, sizeof(small_doc), 32u) ||
      !make_blob_doc(large_doc, sizeof(large_doc), 69800u)) {
    printf("selector in blob fixture construction failed\n");
    return 1;
  }

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("selector in blob receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "in{field=/blob,any=missing|other}", &selector,
                           &error);
  if (st != LQL_STATUS_OK || selector == NULL) {
    printf("selector in blob parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }

  matched = 1;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, small_doc, strlen(small_doc), &matched,
                         &error);
  if (st != LQL_STATUS_OK || matched) {
    printf("selector in blob small eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  small_peak = counter.peak_outstanding_bytes;

  matched = 1;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, large_doc, strlen(large_doc), &matched,
                         &error);
  if (st != LQL_STATUS_OK || matched) {
    printf("selector in blob large eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.peak_outstanding_bytes > small_peak + 32768u) {
    printf("selector in selected blob peak grew with input: small=%lu "
           "large=%lu\n",
           (unsigned long)small_peak,
           (unsigned long)counter.peak_outstanding_bytes);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }

  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf("selector in blob allocator cleanup imbalance: "
           "outstanding=%lu destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_selector_not_equal_large_blob_allocation_stable(void) {
  static char small_doc[128];
  static char large_doc[70064];
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;
  size_t small_peak;

  if (!make_blob_doc(small_doc, sizeof(small_doc), 32u) ||
      !make_blob_doc(large_doc, sizeof(large_doc), 69800u)) {
    printf("selector not-equal blob fixture construction failed\n");
    return 1;
  }

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("selector not-equal blob receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "/blob!=missing", &selector, &error);
  if (st != LQL_STATUS_OK || selector == NULL) {
    printf("selector not-equal blob parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }

  matched = 0;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, small_doc, strlen(small_doc), &matched,
                         &error);
  if (st != LQL_STATUS_OK || !matched) {
    printf("selector not-equal blob small eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  small_peak = counter.peak_outstanding_bytes;

  matched = 0;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, large_doc, strlen(large_doc), &matched,
                         &error);
  if (st != LQL_STATUS_OK || !matched) {
    printf("selector not-equal blob large eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.peak_outstanding_bytes > small_peak + 32768u) {
    printf("selector not-equal selected blob peak grew with input: small=%lu "
           "large=%lu\n",
           (unsigned long)small_peak,
           (unsigned long)counter.peak_outstanding_bytes);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }

  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf("selector not-equal blob allocator cleanup imbalance: "
           "outstanding=%lu destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_selector_long_literal_allocation_stable(void) {
  static char small_doc[128];
  static char large_doc[70064];
  static char expr[10064];
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;
  size_t small_peak;

  if (!make_blob_doc(small_doc, sizeof(small_doc), 32u) ||
      !make_blob_doc(large_doc, sizeof(large_doc), 69800u)) {
    printf("selector long literal fixture construction failed\n");
    return 1;
  }

#define RUN_LONG_LITERAL_CASE(label, expr_prefix, expr_suffix, expect_small,   \
                              expect_large)                                    \
  do {                                                                         \
    if (!make_long_literal_expr(expr, sizeof(expr), (expr_prefix), 9000u,      \
                                (expr_suffix))) {                              \
      printf("selector long literal expression construction failed: %s\n",     \
             (label));                                                         \
      return 1;                                                                \
    }                                                                          \
    counting_allocator_init(&counter);                                         \
    ctx = NULL;                                                                \
    lql_error_init(&error);                                                    \
    st = lql_new_with_allocator(&ctx, &counter.api, &error);                   \
    if (st != LQL_STATUS_OK || ctx == NULL) {                                  \
      printf("selector long literal receiver failed: %s\n", error.message);    \
      return 1;                                                                \
    }                                                                          \
    selector = NULL;                                                           \
    lql_error_init(&error);                                                    \
    st = ctx->selector_parse(ctx, expr, &selector, &error);                    \
    if (st != LQL_STATUS_OK || selector == NULL) {                             \
      printf("selector long literal parse failed: %s: %s\n", (label),          \
             error.message);                                                   \
      ctx->destroy(ctx);                                                       \
      return 1;                                                                \
    }                                                                          \
    matched = !(expect_small);                                                 \
    lql_error_init(&error);                                                    \
    st = ctx->matches_json(ctx, selector, small_doc, strlen(small_doc),        \
                           &matched, &error);                                  \
    if (st != LQL_STATUS_OK || matched != (expect_small)) {                    \
      printf("selector long literal small eval failed: %s: %s\n", (label),     \
             error.message);                                                   \
      ctx->selector_destroy(ctx, selector);                                    \
      ctx->destroy(ctx);                                                       \
      return 1;                                                                \
    }                                                                          \
    small_peak = counter.peak_outstanding_bytes;                               \
    matched = !(expect_large);                                                 \
    lql_error_init(&error);                                                    \
    st = ctx->matches_json(ctx, selector, large_doc, strlen(large_doc),        \
                           &matched, &error);                                  \
    if (st != LQL_STATUS_OK || matched != (expect_large)) {                    \
      printf("selector long literal large eval failed: %s: %s\n", (label),     \
             error.message);                                                   \
      ctx->selector_destroy(ctx, selector);                                    \
      ctx->destroy(ctx);                                                       \
      return 1;                                                                \
    }                                                                          \
    if (counter.peak_outstanding_bytes > small_peak + 32768u) {                \
      printf("selector long literal peak grew with input: %s small=%lu "       \
             "large=%lu\n",                                                    \
             (label), (unsigned long)small_peak,                               \
             (unsigned long)counter.peak_outstanding_bytes);                   \
      ctx->selector_destroy(ctx, selector);                                    \
      ctx->destroy(ctx);                                                       \
      return 1;                                                                \
    }                                                                          \
    ctx->selector_destroy(ctx, selector);                                      \
    ctx->destroy(ctx);                                                         \
    if (counter.outstanding != 0u || counter.destroy_count == 0u) {            \
      printf("selector long literal cleanup imbalance: %s outstanding=%lu "    \
             "destroys=%lu\n",                                                 \
             (label), (unsigned long)counter.outstanding,                      \
             (unsigned long)counter.destroy_count);                            \
      return 1;                                                                \
    }                                                                          \
  } while (0)

  RUN_LONG_LITERAL_CASE("exact", "eq{field=/blob,value=", "}", 0, 0);
  RUN_LONG_LITERAL_CASE("not-equal", "/blob!=", "", 1, 1);
  RUN_LONG_LITERAL_CASE("prefix", "prefix{field=/blob,value=", "}", 0, 1);
  RUN_LONG_LITERAL_CASE("contains", "contains{field=/blob,value=", "}", 0, 1);
  RUN_LONG_LITERAL_CASE("contains-any", "contains{field=/blob,any=missing|",
                        "}", 0, 1);
  RUN_LONG_LITERAL_CASE("in", "in{field=/blob,any=missing|", "}", 0, 0);

#undef RUN_LONG_LITERAL_CASE

  return 0;
}

static int expect_selector_temporal_large_blob_allocation_stable(void) {
  static char small_doc[128];
  static char large_doc[70064];
  counting_allocator counter;
  lql *ctx;
  lql_selector *eq_selector;
  lql_selector *date_selector;
  lql_selector *range_selector;
  lql_error error;
  lql_status st;
  int matched;
  size_t small_peak;

  if (!make_blob_doc(small_doc, sizeof(small_doc), 32u) ||
      !make_blob_doc(large_doc, sizeof(large_doc), 69800u)) {
    printf("selector temporal blob fixture construction failed\n");
    return 1;
  }

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("selector temporal blob receiver failed: %s\n", error.message);
    return 1;
  }

  eq_selector = NULL;
  date_selector = NULL;
  range_selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "eq{field=/blob,value=2025-01-01}",
                           &eq_selector, &error);
  if (st != LQL_STATUS_OK || eq_selector == NULL) {
    printf("selector temporal eq parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "date{field=/blob,value=2025-01-01}",
                           &date_selector, &error);
  if (st != LQL_STATUS_OK || date_selector == NULL) {
    printf("selector temporal date parse failed: %s\n", error.message);
    ctx->selector_destroy(ctx, eq_selector);
    ctx->destroy(ctx);
    return 1;
  }
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "range{field=/blob,gte=2025-01-01}",
                           &range_selector, &error);
  if (st != LQL_STATUS_OK || range_selector == NULL) {
    printf("selector temporal range parse failed: %s\n", error.message);
    ctx->selector_destroy(ctx, date_selector);
    ctx->selector_destroy(ctx, eq_selector);
    ctx->destroy(ctx);
    return 1;
  }

  matched = 1;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, eq_selector, small_doc, strlen(small_doc),
                         &matched, &error);
  if (st != LQL_STATUS_OK || matched) {
    printf("selector temporal eq small eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, range_selector);
    ctx->selector_destroy(ctx, date_selector);
    ctx->selector_destroy(ctx, eq_selector);
    ctx->destroy(ctx);
    return 1;
  }
  matched = 1;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, date_selector, small_doc, strlen(small_doc),
                         &matched, &error);
  if (st != LQL_STATUS_OK || matched) {
    printf("selector temporal date small eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, range_selector);
    ctx->selector_destroy(ctx, date_selector);
    ctx->selector_destroy(ctx, eq_selector);
    ctx->destroy(ctx);
    return 1;
  }
  matched = 1;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, range_selector, small_doc, strlen(small_doc),
                         &matched, &error);
  if (st != LQL_STATUS_OK || matched) {
    printf("selector temporal range small eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, range_selector);
    ctx->selector_destroy(ctx, date_selector);
    ctx->selector_destroy(ctx, eq_selector);
    ctx->destroy(ctx);
    return 1;
  }
  small_peak = counter.peak_outstanding_bytes;

  matched = 1;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, eq_selector, large_doc, strlen(large_doc),
                         &matched, &error);
  if (st != LQL_STATUS_OK || matched) {
    printf("selector temporal eq large eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, range_selector);
    ctx->selector_destroy(ctx, date_selector);
    ctx->selector_destroy(ctx, eq_selector);
    ctx->destroy(ctx);
    return 1;
  }
  matched = 1;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, date_selector, large_doc, strlen(large_doc),
                         &matched, &error);
  if (st != LQL_STATUS_OK || matched) {
    printf("selector temporal date large eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, range_selector);
    ctx->selector_destroy(ctx, date_selector);
    ctx->selector_destroy(ctx, eq_selector);
    ctx->destroy(ctx);
    return 1;
  }
  matched = 1;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, range_selector, large_doc, strlen(large_doc),
                         &matched, &error);
  if (st != LQL_STATUS_OK || matched) {
    printf("selector temporal range large eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, range_selector);
    ctx->selector_destroy(ctx, date_selector);
    ctx->selector_destroy(ctx, eq_selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.peak_outstanding_bytes > small_peak + 32768u) {
    printf("selector temporal selected blob peak grew with input: small=%lu "
           "large=%lu\n",
           (unsigned long)small_peak,
           (unsigned long)counter.peak_outstanding_bytes);
    ctx->selector_destroy(ctx, range_selector);
    ctx->selector_destroy(ctx, date_selector);
    ctx->selector_destroy(ctx, eq_selector);
    ctx->destroy(ctx);
    return 1;
  }

  ctx->selector_destroy(ctx, range_selector);
  ctx->selector_destroy(ctx, date_selector);
  ctx->selector_destroy(ctx, eq_selector);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf("selector temporal blob allocator cleanup imbalance: "
           "outstanding=%lu destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_selector_numeric_range_large_number_allocation_stable(void) {
  static char small_doc[128];
  static char large_doc[512];
  counting_allocator counter;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;
  size_t small_peak;

  if (!make_number_doc(small_doc, sizeof(small_doc), 3u) ||
      !make_number_doc(large_doc, sizeof(large_doc), 200u)) {
    printf("selector numeric range fixture construction failed\n");
    return 1;
  }

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("selector numeric range receiver failed: %s\n", error.message);
    return 1;
  }
  selector = NULL;
  lql_error_init(&error);
  st = ctx->selector_parse(ctx, "range{field=/n,gt=1}", &selector, &error);
  if (st != LQL_STATUS_OK || selector == NULL) {
    printf("selector numeric range parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }

  matched = 0;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, small_doc, strlen(small_doc), &matched,
                         &error);
  if (st != LQL_STATUS_OK || !matched) {
    printf("selector numeric range small eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  small_peak = counter.peak_outstanding_bytes;

  matched = 0;
  lql_error_init(&error);
  st = ctx->matches_json(ctx, selector, large_doc, strlen(large_doc), &matched,
                         &error);
  if (st != LQL_STATUS_OK || !matched) {
    printf("selector numeric range large eval failed: %s\n", error.message);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  if (counter.peak_outstanding_bytes > small_peak + 16384u) {
    printf("selector numeric range peak grew with input: small=%lu large=%lu\n",
           (unsigned long)small_peak,
           (unsigned long)counter.peak_outstanding_bytes);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }

  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  if (counter.outstanding != 0u || counter.destroy_count == 0u) {
    printf("selector numeric range allocator cleanup imbalance: "
           "outstanding=%lu destroys=%lu\n",
           (unsigned long)counter.outstanding,
           (unsigned long)counter.destroy_count);
    return 1;
  }
  return 0;
}

static int expect_projection_parse_failure_cleans_allocator(void) {
  counting_allocator counter;
  lql *ctx;
  lql_projection *projection;
  const char *fields[2];
  lql_error error;
  lql_status st;

  counting_allocator_init(&counter);
  ctx = NULL;
  lql_error_init(&error);
  st = lql_new_with_allocator(&ctx, &counter.api, &error);
  if (st != LQL_STATUS_OK || ctx == NULL) {
    printf("projection allocator receiver failed: %s\n", error.message);
    return 1;
  }
  fields[0] = "/items";
  fields[1] = "/items/0/name";
  projection = NULL;
  lql_error_init(&error);
  st = ctx->projection_parse(ctx, fields, 2u, &projection, &error);
  if (st == LQL_STATUS_OK || projection != NULL) {
    printf("projection allocator parse failure unexpectedly succeeded\n");
    ctx->projection_destroy(ctx, projection);
    ctx->destroy(ctx);
    return 1;
  }
  ctx->destroy(ctx);
  if (counter.alloc_count == 0u || counter.outstanding != 0u) {
    printf("projection allocator failure cleanup imbalance: allocs=%lu "
           "outstanding=%lu\n",
           (unsigned long)counter.alloc_count,
           (unsigned long)counter.outstanding);
    return 1;
  }
  return 0;
}

int main(void) {
  int failures;

  failures = 0;
  failures += expect_selector_success_uses_allocator();
  failures += expect_file_decisions_steady_state_has_no_receiver_alloc();
  failures += expect_source_decisions_steady_state_has_no_receiver_alloc();
  failures +=
      expect_source_array_decisions_steady_state_has_no_receiver_alloc();
  failures += expect_source_spooled_match_peak_is_per_candidate_bounded();
  failures += expect_selector_parse_failure_cleans_allocator();
  failures += expect_selector_contains_large_blob_allocation_stable();
  failures += expect_selector_prefix_large_blob_allocation_stable();
  failures += expect_selector_exact_large_blob_allocation_stable();
  failures += expect_selector_in_large_blob_allocation_stable();
  failures += expect_selector_not_equal_large_blob_allocation_stable();
  failures += expect_selector_long_literal_allocation_stable();
  failures += expect_selector_temporal_large_blob_allocation_stable();
  failures += expect_selector_numeric_range_large_number_allocation_stable();
  failures += expect_projection_success_uses_allocator();
  failures += expect_projection_unselected_large_blob_allocation_stable();
  failures += expect_projection_parse_failure_cleans_allocator();
  failures += expect_mutation_success_uses_allocator();
  failures += expect_mutation_unselected_large_blob_allocation_stable();
  failures += expect_mutation_parse_failure_cleans_allocator();
  return failures == 0 ? 0 : 1;
}
