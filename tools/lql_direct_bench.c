#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <lql/lql.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

typedef struct bench_reader {
  FILE *file;
} bench_reader;

typedef struct bench_writer {
  size_t bytes;
  size_t records;
  int copy_values;
} bench_writer;

typedef struct sha256_state {
  unsigned int h[8];
  unsigned char block[64];
  size_t block_len;
  unsigned long byte_len;
} sha256_state;

static unsigned int sha256_rotr(unsigned int value, unsigned int shift) {
  return (value >> shift) | (value << (32u - shift));
}

static void sha256_transform(sha256_state *state, const unsigned char *data) {
  static const unsigned int round_constants[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  unsigned int words[64];
  unsigned int a;
  unsigned int b;
  unsigned int c;
  unsigned int d;
  unsigned int e;
  unsigned int f;
  unsigned int g;
  unsigned int h;
  unsigned int first;
  unsigned int second;
  size_t i;

  for (i = 0u; i < 16u; ++i) {
    words[i] = ((unsigned int)data[i * 4u] << 24u) |
               ((unsigned int)data[i * 4u + 1u] << 16u) |
               ((unsigned int)data[i * 4u + 2u] << 8u) |
               (unsigned int)data[i * 4u + 3u];
  }
  for (i = 16u; i < 64u; ++i) {
    first = sha256_rotr(words[i - 15u], 7u) ^ sha256_rotr(words[i - 15u], 18u) ^
            (words[i - 15u] >> 3u);
    second = sha256_rotr(words[i - 2u], 17u) ^ sha256_rotr(words[i - 2u], 19u) ^
             (words[i - 2u] >> 10u);
    words[i] = words[i - 16u] + first + words[i - 7u] + second;
  }
  a = state->h[0];
  b = state->h[1];
  c = state->h[2];
  d = state->h[3];
  e = state->h[4];
  f = state->h[5];
  g = state->h[6];
  h = state->h[7];
  for (i = 0u; i < 64u; ++i) {
    first = h +
            (sha256_rotr(e, 6u) ^ sha256_rotr(e, 11u) ^ sha256_rotr(e, 25u)) +
            ((e & f) ^ ((~e) & g)) + round_constants[i] + words[i];
    second = (sha256_rotr(a, 2u) ^ sha256_rotr(a, 13u) ^ sha256_rotr(a, 22u)) +
             ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + first;
    d = c;
    c = b;
    b = a;
    a = first + second;
  }
  state->h[0] += a;
  state->h[1] += b;
  state->h[2] += c;
  state->h[3] += d;
  state->h[4] += e;
  state->h[5] += f;
  state->h[6] += g;
  state->h[7] += h;
}

static void sha256_init(sha256_state *state) {
  memset(state, 0, sizeof(*state));
  state->h[0] = 0x6a09e667u;
  state->h[1] = 0xbb67ae85u;
  state->h[2] = 0x3c6ef372u;
  state->h[3] = 0xa54ff53au;
  state->h[4] = 0x510e527fu;
  state->h[5] = 0x9b05688cu;
  state->h[6] = 0x1f83d9abu;
  state->h[7] = 0x5be0cd19u;
}

static void sha256_update(sha256_state *state, const unsigned char *data,
                          size_t len) {
  while (len != 0u) {
    size_t amount;
    amount = 64u - state->block_len;
    if (amount > len) {
      amount = len;
    }
    memcpy(state->block + state->block_len, data, amount);
    state->block_len += amount;
    data += amount;
    len -= amount;
    state->byte_len += (unsigned long)amount;
    if (state->block_len == 64u) {
      sha256_transform(state, state->block);
      state->block_len = 0u;
    }
  }
}

static void sha256_final(sha256_state *state, char out[65]) {
  static const char hex[] = "0123456789abcdef";
  unsigned long bits;
  size_t i;
  state->block[state->block_len++] = 0x80u;
  if (state->block_len > 56u) {
    while (state->block_len < 64u) {
      state->block[state->block_len++] = 0u;
    }
    sha256_transform(state, state->block);
    state->block_len = 0u;
  }
  while (state->block_len < 56u) {
    state->block[state->block_len++] = 0u;
  }
  bits = state->byte_len * 8ul;
  for (i = 0u; i < 8u; ++i) {
    state->block[63u - i] = (unsigned char)(bits >> (i * 8u));
  }
  sha256_transform(state, state->block);
  for (i = 0u; i < 8u; ++i) {
    unsigned int value;
    size_t j;
    value = state->h[i];
    for (j = 0u; j < 4u; ++j) {
      unsigned char byte;
      byte = (unsigned char)(value >> (24u - j * 8u));
      out[(i * 4u + j) * 2u] = hex[byte >> 4u];
      out[(i * 4u + j) * 2u + 1u] = hex[byte & 15u];
    }
  }
  out[64] = '\0';
}

static int fixture_sha256(FILE *file, char out[65]) {
  unsigned char buffer[8192];
  sha256_state state;
  size_t amount;
  if (fseek(file, 0L, SEEK_SET) != 0) {
    return 0;
  }
  sha256_init(&state);
  while ((amount = fread(buffer, 1u, sizeof(buffer), file)) != 0u) {
    sha256_update(&state, buffer, amount);
  }
  if (ferror(file)) {
    return 0;
  }
  sha256_final(&state, out);
  return fseek(file, 0L, SEEK_SET) == 0;
}

static lql_status bench_read(void *user, unsigned char *buffer, size_t capacity,
                             size_t *out_len, lql_error *error) {
  bench_reader *reader;
  size_t amount;
  reader = (bench_reader *)user;
  if (reader == NULL || reader->file == NULL || out_len == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  amount = fread(buffer, 1u, capacity, reader->file);
  if (amount == 0u && ferror(reader->file)) {
    if (error != NULL) {
      error->code = LQL_STATUS_IO_ERROR;
      strcpy(error->message, "benchmark fixture read failed");
    }
    return LQL_STATUS_IO_ERROR;
  }
  *out_len = amount;
  return LQL_STATUS_OK;
}

static lql_status bench_write(void *user, const void *data, size_t len,
                              lql_error *error) {
  bench_writer *writer;
  const unsigned char *bytes;
  size_t i;
  (void)error;
  writer = (bench_writer *)user;
  if (writer == NULL || (data == NULL && len != 0u) ||
      len > (size_t)-1 - writer->bytes) {
    return LQL_STATUS_CALLBACK_ERROR;
  }
  bytes = (const unsigned char *)data;
  writer->bytes += len;
  for (i = 0u; i < len; ++i) {
    if (bytes[i] == (unsigned char)'\n') {
      ++writer->records;
    }
  }
  return LQL_STATUS_OK;
}

static lql_status bench_discard_write(void *user, const void *data, size_t len,
                                      lql_error *error) {
  (void)user;
  (void)data;
  (void)len;
  (void)error;
  return LQL_STATUS_OK;
}

static lql_status bench_range_write(void *user, size_t offset, size_t len,
                                    lql_stream_writer_fn writer,
                                    void *writer_user, lql_error *error) {
  bench_reader *reader;
  unsigned char buffer[8192];
  long saved;
  lql_status status;
  reader = (bench_reader *)user;
  if (reader == NULL || reader->file == NULL || writer == NULL ||
      offset > (size_t)LONG_MAX) {
    if (error != NULL) {
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "benchmark source range is invalid");
    }
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  saved = ftell(reader->file);
  if (saved < 0L || fseek(reader->file, (long)offset, SEEK_SET) != 0) {
    if (error != NULL) {
      error->code = LQL_STATUS_IO_ERROR;
      strcpy(error->message, "benchmark source range seek failed");
    }
    return LQL_STATUS_IO_ERROR;
  }
  while (len != 0u) {
    size_t amount;
    amount = len > sizeof(buffer) ? sizeof(buffer) : len;
    if (fread(buffer, 1u, amount, reader->file) != amount) {
      if (error != NULL) {
        error->code = LQL_STATUS_IO_ERROR;
        strcpy(error->message, "benchmark source range read failed");
      }
      fseek(reader->file, saved, SEEK_SET);
      return LQL_STATUS_IO_ERROR;
    }
    status = writer(writer_user, buffer, amount, error);
    if (status != LQL_STATUS_OK) {
      fseek(reader->file, saved, SEEK_SET);
      return status;
    }
    len -= amount;
  }
  if (fseek(reader->file, saved, SEEK_SET) != 0) {
    if (error != NULL) {
      error->code = LQL_STATUS_IO_ERROR;
      strcpy(error->message, "benchmark source range restore failed");
    }
    return LQL_STATUS_IO_ERROR;
  }
  return LQL_STATUS_OK;
}

static lql_stream_callback_result
bench_value(void *user, const lql_stream_value *value, lql_error *error) {
  bench_writer *writer;
  size_t size;
  writer = (bench_writer *)user;
  if (writer == NULL || value == NULL)
    return LQL_STREAM_CALLBACK_ERROR;
  size = lql_stream_value_size(value);
  if (size > (size_t)-1 - writer->bytes)
    return LQL_STREAM_CALLBACK_ERROR;
  ++writer->records;
  writer->bytes += size;
  if (!writer->copy_values) {
    return LQL_STREAM_CALLBACK_CONTINUE;
  }
  return lql_stream_value_write_to(value, bench_discard_write, NULL, error) ==
                 LQL_STATUS_OK
             ? LQL_STREAM_CALLBACK_CONTINUE
             : LQL_STREAM_CALLBACK_ERROR;
}

static int mode_is_decision(const char *mode) {
  return strcmp(mode, "decision_only_selector") == 0 ||
         strcmp(mode, "decision_only_plan") == 0 ||
         strcmp(mode, "reuse_selector") == 0 ||
         strcmp(mode, "reparse_selector_each_run") == 0 ||
         strcmp(mode, "decision_only_source_selector") == 0;
}

static int mode_is_selected(const char *mode) {
  return strcmp(mode, "plus_value_selector") == 0 ||
         strcmp(mode, "plus_value_plan") == 0 ||
         strcmp(mode, "plus_value_source_selector") == 0 ||
         strcmp(mode, "plus_value_openjson_selector") == 0 ||
         strcmp(mode, "plus_value_openjson_plan") == 0;
}

static int mode_is_source(const char *mode) {
  return strcmp(mode, "decision_only_source_selector") == 0 ||
         strcmp(mode, "plus_value_source_selector") == 0 ||
         strcmp(mode, "mutate_source_selector") == 0 ||
         strcmp(mode, "project_source_selector") == 0;
}

static int mode_is_projection(const char *mode) {
  return strcmp(mode, "project_file_selector") == 0 ||
         strcmp(mode, "project_source_selector") == 0;
}

static int mode_is_mutation(const char *mode) {
  return strcmp(mode, "mutate_file_selector") == 0 ||
         strcmp(mode, "mutate_file_plan") == 0 ||
         strcmp(mode, "mutate_source_selector") == 0;
}

static int mode_is_project_mutation(const char *mode) {
  return strcmp(mode, "project_mutate_file_selector") == 0;
}

static int mode_is_supported(const char *mode) {
  return strcmp(mode, "decision_only_selector") == 0 ||
         strcmp(mode, "decision_only_plan") == 0 ||
         strcmp(mode, "reuse_selector") == 0 ||
         strcmp(mode, "reparse_selector_each_run") == 0 ||
         strcmp(mode, "decision_only_source_selector") == 0 ||
         strcmp(mode, "plus_value_selector") == 0 ||
         strcmp(mode, "plus_value_plan") == 0 ||
         strcmp(mode, "plus_value_source_selector") == 0 ||
         strcmp(mode, "plus_value_openjson_selector") == 0 ||
         mode_is_projection(mode) || mode_is_project_mutation(mode) ||
         strcmp(mode, "mutate_file_selector") == 0 ||
         strcmp(mode, "mutate_file_plan") == 0 ||
         strcmp(mode, "mutate_source_selector") == 0;
}

static unsigned long benchmark_payload_bytes(const char *mode,
                                             const bench_writer *writer) {
  if (mode_is_projection(mode) || mode_is_project_mutation(mode)) {
    /*
     * The direct writer emits NDJSON.  The Go oracle's projection counter is
     * ProjectFields' JSON value size, before its enclosing stream delimiter.
     */
    if (writer->bytes < writer->records) {
      return 0ul;
    }
    return (unsigned long)(writer->bytes - writer->records);
  }
  return (unsigned long)writer->bytes;
}

static int bench_samples(const char *submode) {
  const char *text;
  char *end;
  long value;
  if (strcmp(submode, "steady_state") != 0) {
    return 1;
  }
  text = getenv("LQL_BENCH_SAMPLES");
  if (text == NULL || *text == '\0') {
    return 15;
  }
  errno = 0;
  value = strtol(text, &end, 10);
  return errno == 0 && *end == '\0' && value >= 1L && value <= 100L ? (int)value
                                                                    : 15;
}

static long elapsed_ns(const struct timespec *start,
                       const struct timespec *end) {
  return (long)(end->tv_sec - start->tv_sec) * 1000000000L +
         (long)(end->tv_nsec - start->tv_nsec);
}

static unsigned long peak_rss_bytes(void) {
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0L) {
    return 0ul;
  }
  return (unsigned long)usage.ru_maxrss * 1024ul;
}

static void json_string(const char *text) {
  const unsigned char *p;
  if (text == NULL) {
    fputs("null", stdout);
    return;
  }
  putchar('"');
  for (p = (const unsigned char *)text; *p != '\0'; ++p) {
    switch (*p) {
    case '"':
      fputs("\\\"", stdout);
      break;
    case '\\':
      fputs("\\\\", stdout);
      break;
    case '\n':
      fputs("\\n", stdout);
      break;
    case '\r':
      fputs("\\r", stdout);
      break;
    case '\t':
      fputs("\\t", stdout);
      break;
    default:
      if (*p < 0x20u) {
        fprintf(stdout, "\\u%04x", (unsigned int)*p);
      } else {
        putchar((int)*p);
      }
      break;
    }
  }
  putchar('"');
}

static const char *const *mutations_for(const char *selector_name,
                                        const char *expr, size_t *count) {
  static const char *const sparse[] = {"/event=session_sync"};
  static const char *const dense[] = {"/component=lql"};
  static const char *const nested[] = {"/query/hash=ff"};
  static const char *const range[] = {"/code=+1"};
  static const char *const top_increment_multi[] = {"/code=+1", "/code=+2"};
  static const char *const top_set_multi[] = {"/processed=true",
                                              "/processed=false"};
  static const char *const nested_increment[] = {"/meta/count=+1"};
  static const char *const same_top_nested_increment[] = {"/meta/count=+1",
                                                          "/meta/state=done"};
  static const char *const remove[] = {"rm:/payload"};
  static const char *const contains[] = {"/meta/bench=true"};
  static const char *const multi[] = {"/component=lql", "/event=session_sync",
                                      "/code=+1", "rm:/payload",
                                      "/meta/bench=true"};
  static const char *const processed[] = {"/processed=true"};
  static const char *const top_multi[] = {"/enabled=false", "/code=+1",
                                          "rm:/payload"};
  static const char *const mixed_nested_multi[] = {
      "/enabled=false", "/code=+1", "rm:/payload", "/meta/bench=true"};
  static const char *const nested_remove[] = {"rm:/meta/state"};
  static const char *const deep_set[] = {"/voucher/lines/10/bench=true"};
  static const char *const same_top_nested_multi[] = {"/meta/bench=true",
                                                      "/meta/state=done"};
  static const char *const fallback[] = {"/bench/touched=true"};
  if (strcmp(selector_name, "realworld_eq_sparse") == 0) {
    *count = 1u;
    return sparse;
  }
  if (strcmp(selector_name, "realworld_eq_dense") == 0) {
    *count = 1u;
    return dense;
  }
  if (strcmp(selector_name, "realworld_nested_eq_sparse") == 0) {
    *count = 1u;
    return nested;
  }
  if (strcmp(selector_name, "realworld_range_sparse") == 0) {
    *count = 1u;
    return range;
  }
  if (strcmp(selector_name, "realworld_recursive_nested_eq_sparse") == 0) {
    *count = 1u;
    return remove;
  }
  if (strcmp(selector_name, "realworld_contains_event_sparse") == 0) {
    *count = 1u;
    return contains;
  }
  if (strcmp(selector_name, "realworld_multi_clause_and") == 0) {
    *count = 5u;
    return multi;
  }
  if (strcmp(selector_name, "eq_status_open_top_set") == 0) {
    *count = 1u;
    return processed;
  }
  if (strcmp(selector_name, "eq_status_open_top_set_multi") == 0) {
    *count = 2u;
    return top_set_multi;
  }
  if (strcmp(selector_name, "eq_status_open_top_remove") == 0) {
    *count = 1u;
    return remove;
  }
  if (strcmp(selector_name, "eq_code_one_top_increment") == 0) {
    *count = 1u;
    return range;
  }
  if (strcmp(selector_name, "eq_code_one_top_increment_multi") == 0) {
    *count = 2u;
    return top_increment_multi;
  }
  if (strcmp(selector_name, "eq_status_open_nested_increment") == 0) {
    *count = 1u;
    return nested_increment;
  }
  if (strcmp(selector_name, "eq_status_open_same_top_nested_increment") == 0) {
    *count = 2u;
    return same_top_nested_increment;
  }
  if (strcmp(selector_name, "eq_code_one_top_multi") == 0) {
    *count = 3u;
    return top_multi;
  }
  if (strcmp(selector_name, "eq_status_open_nested_set") == 0) {
    *count = 1u;
    return fallback;
  }
  if (strcmp(selector_name, "eq_status_open_deep_set") == 0) {
    *count = 1u;
    return deep_set;
  }
  if (strcmp(selector_name, "eq_status_open_same_top_nested_multi") == 0) {
    *count = 2u;
    return same_top_nested_multi;
  }
  if (strcmp(selector_name, "eq_code_one_mixed_nested_multi") == 0) {
    *count = 4u;
    return mixed_nested_multi;
  }
  if (strcmp(selector_name, "eq_status_open_nested_remove") == 0) {
    *count = 1u;
    return nested_remove;
  }
  if (strstr(expr, "/voucher/lines/10/") != NULL) {
    static const char *const voucher[] = {"/voucher/lines/10/bench=true"};
    *count = 1u;
    return voucher;
  }
  if (strstr(expr, "/event=\"session_sync\"") != NULL ||
      strstr(expr, "/event=\"tabs_update\"") != NULL ||
      strstr(expr, "/lockd/key") != NULL) {
    *count = 1u;
    return processed;
  }
  *count = 1u;
  return fallback;
}

static int run_once(FILE *file, lql *ctx, lql_selector *selector,
                    const char *expr, int reparse, const char *mode,
                    lql_projection *projection, lql_mutation *mutation,
                    size_t max_records, size_t max_bytes,
                    lql_stream_result *result, bench_writer *writer,
                    lql_error *error) {
  bench_reader reader;
  lql_stream_request request;
  lql_selector *temporary;
  lql_status status;
  if (fseek(file, 0L, SEEK_SET) != 0) {
    return 0;
  }
  temporary = NULL;
  if (reparse &&
      ctx->selector_parse(ctx, expr, &temporary, error) != LQL_STATUS_OK) {
    return 0;
  }
  memset(&reader, 0, sizeof(reader));
  reader.file = file;
  memset(writer, 0, sizeof(*writer));
  writer->copy_values =
      !(mode_is_selected(mode) && !mode_is_source(mode));
  memset(&request, 0, sizeof(request));
  request.reader = bench_read;
  request.reader_user = &reader;
  if (mode_is_selected(mode) && !mode_is_source(mode)) {
    request.range_writer = bench_range_write;
    request.range_user = &reader;
    request.input_is_compact = 1;
  }
  request.selector = temporary != NULL ? temporary : selector;
  request.matched_only = 1;
  request.limits.max_records = max_records;
  request.limits.max_bytes = max_bytes;
  if (!mode_is_decision(mode)) {
    if (mode_is_mutation(mode)) {
      request.writer = bench_discard_write;
    } else {
      request.writer = bench_write;
      request.writer_user = writer;
    }
  }
  if (mode_is_selected(mode)) {
    request.on_value = bench_value;
    request.value_user = writer;
  } else if (mode_is_projection(mode)) {
    request.projection = projection;
    request.output_mode = LQL_STREAM_OUTPUT_PROJECTION;
  } else if (mode_is_mutation(mode)) {
    request.mutation = mutation;
    request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  } else if (mode_is_project_mutation(mode)) {
    request.projection = projection;
    request.mutation = mutation;
    request.output_mode = LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION;
  }
  status = lql_stream_execute(ctx, &request, result, error);
  if (temporary != NULL) {
    ctx->selector_destroy(ctx, temporary);
  }
  return status == LQL_STATUS_OK;
}

int main(int argc, char **argv) {
  const char *fixture;
  const char *dataset;
  const char *selector_name;
  const char *expr;
  const char *mode;
  const char *submode;
  const char *projection_path;
  const char *projection_paths[1];
  const char *max_records_text;
  char *max_records_end;
  unsigned long max_records_value;
  size_t max_records;
  const char *max_bytes_text;
  char *max_bytes_end;
  unsigned long max_bytes_value;
  size_t max_bytes;
  FILE *file;
  long fixture_bytes;
  char fixture_hash[65];
  lql *ctx;
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
  lql_error error;
  lql_stream_result result;
  bench_writer writer;
  struct timespec start;
  struct timespec end;
  long best_ns;
  int sample_count;
  int sample;
  int reparse;
  int unsupported;
  const char *unsupported_reason;
  size_t mutation_count;
  const char *const *mutations;
  int i;

  fixture = NULL;
  dataset = "large_ndjson";
  selector_name = "eq_status_open";
  expr = "/status=\"open\"";
  mode = "decision_only_selector";
  submode = "steady_state";
  projection_path = "/id";
  max_records = 0u;
  max_bytes = 0u;
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
      fixture = argv[++i];
    } else if (strcmp(argv[i], "--dataset") == 0 && i + 1 < argc) {
      dataset = argv[++i];
    } else if (strcmp(argv[i], "--selector-name") == 0 && i + 1 < argc) {
      selector_name = argv[++i];
    } else if (strcmp(argv[i], "--expr") == 0 && i + 1 < argc) {
      expr = argv[++i];
    } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      mode = argv[++i];
    } else if (strcmp(argv[i], "--submode") == 0 && i + 1 < argc) {
      submode = argv[++i];
    } else if (strcmp(argv[i], "--projection-path") == 0 && i + 1 < argc) {
      projection_path = argv[++i];
    } else if (strcmp(argv[i], "--max-records") == 0 && i + 1 < argc) {
      max_records_text = argv[++i];
      errno = 0;
      max_records_value = strtoul(max_records_text, &max_records_end, 10);
      if (errno != 0 || max_records_end == max_records_text ||
          *max_records_end != '\0' ||
          max_records_value > (unsigned long)((size_t)-1)) {
        fprintf(stderr, "lql_direct_bench: invalid --max-records\n");
        return 2;
      }
      max_records = (size_t)max_records_value;
    } else if (strcmp(argv[i], "--max-bytes") == 0 && i + 1 < argc) {
      max_bytes_text = argv[++i];
      errno = 0;
      max_bytes_value = strtoul(max_bytes_text, &max_bytes_end, 10);
      if (errno != 0 || max_bytes_end == max_bytes_text ||
          *max_bytes_end != '\0' ||
          max_bytes_value > (unsigned long)((size_t)-1)) {
        fprintf(stderr, "lql_direct_bench: invalid --max-bytes\n");
        return 2;
      }
      max_bytes = (size_t)max_bytes_value;
    } else {
      fprintf(stderr,
              "usage: %s --fixture PATH [--dataset NAME] [--selector-name "
              "NAME] [--expr SELECTOR] [--mode MODE] [--submode MODE] "
              "[--projection-path JSON_POINTER] [--max-records N] "
              "[--max-bytes N]\n",
              argv[0]);
      return 2;
    }
  }
  if (fixture == NULL) {
    fprintf(stderr, "lql_direct_bench: --fixture is required\n");
    return 2;
  }
  file = fopen(fixture, "rb");
  if (file == NULL || fseek(file, 0L, SEEK_END) != 0 ||
      (fixture_bytes = ftell(file)) < 0L ||
      !fixture_sha256(file, fixture_hash)) {
    fprintf(stderr, "lql_direct_bench: fixture setup failed\n");
    if (file != NULL) {
      fclose(file);
    }
    return 1;
  }
  unsupported = 0;
  unsupported_reason = "";
  if (!(mode_is_decision(mode) || mode_is_selected(mode) ||
        mode_is_projection(mode) || mode_is_mutation(mode) ||
        mode_is_project_mutation(mode))) {
    unsupported = 1;
    unsupported_reason =
        "mode is not implemented by the direct benchmark runner";
  } else if (!mode_is_supported(mode)) {
    unsupported = 1;
    unsupported_reason =
        "benchmark mode requires a direct public API not implemented yet";
  }
  ctx = NULL;
  selector = NULL;
  projection = NULL;
  mutation = NULL;
  lql_error_init(&error);
  projection_paths[0] = projection_path;
  if (!unsupported &&
      (lql_new(&ctx, &error) != LQL_STATUS_OK ||
       (strcmp(mode, "reparse_selector_each_run") != 0 &&
        ctx->selector_parse(ctx, expr, &selector, &error) != LQL_STATUS_OK) ||
       ((mode_is_projection(mode) || mode_is_project_mutation(mode)) &&
        ctx->projection_parse(ctx, projection_paths, 1u, &projection, &error) !=
            LQL_STATUS_OK) ||
       ((mode_is_mutation(mode) || mode_is_project_mutation(mode)) &&
        ((mutations = mutations_for(selector_name, expr, &mutation_count)),
         ctx->mutation_parse(ctx, mutations, mutation_count, &mutation,
                             &error) != LQL_STATUS_OK)))) {
    fprintf(stderr, "lql_direct_bench: setup failed: %s\n", error.message);
    if (mutation != NULL)
      ctx->mutation_destroy(ctx, mutation);
    if (projection != NULL)
      ctx->projection_destroy(ctx, projection);
    if (selector != NULL)
      ctx->selector_destroy(ctx, selector);
    if (ctx != NULL)
      ctx->destroy(ctx);
    fclose(file);
    return 1;
  }
  reparse = strcmp(mode, "reparse_selector_each_run") == 0;
  if (!unsupported && strcmp(submode, "steady_state") == 0 &&
      !run_once(file, ctx, selector, expr, reparse, mode, projection, mutation,
                max_records, max_bytes, &result, &writer, &error)) {
    fprintf(stderr, "lql_direct_bench: warmup failed: %s\n", error.message);
    return 1;
  }
  best_ns = 0L;
  memset(&result, 0, sizeof(result));
  memset(&writer, 0, sizeof(writer));
  sample_count = unsupported ? 0 : bench_samples(submode);
  for (sample = 0; sample < sample_count; ++sample) {
    lql_stream_result candidate_result;
    bench_writer candidate_writer;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0 ||
        !run_once(file, ctx, selector, expr, reparse, mode, projection,
                  mutation, max_records, max_bytes, &candidate_result,
                  &candidate_writer, &error) ||
        clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
      fprintf(stderr, "lql_direct_bench: stream failed: %s\n", error.message);
      return 1;
    }
    if (sample == 0 || elapsed_ns(&start, &end) < best_ns) {
      best_ns = elapsed_ns(&start, &end);
      result = candidate_result;
      writer = candidate_writer;
    }
  }
  fputs(
      "{\"schema\":\"liblql.parity_benchmark.v1\",\"impl\":\"c\",\"dataset\":",
      stdout);
  json_string(dataset);
  fputs(",\"selector\":", stdout);
  json_string(selector_name);
  fputs(",\"expr\":", stdout);
  json_string(expr);
  fputs(",\"mode\":", stdout);
  json_string(mode);
  fputs(",\"submode\":", stdout);
  json_string(submode);
  fprintf(stdout, ",\"bytes_per_iter\":%ld,\"candidates\":%lu,\"matches\":%lu",
          fixture_bytes, (unsigned long)result.records_seen,
          (unsigned long)result.records_matched);
  fprintf(stdout,
          ",\"payloads\":%lu,\"payload_bytes\":%lu,\"payload_source_type\":",
          mode_is_selected(mode) || mode_is_projection(mode) ||
                  mode_is_project_mutation(mode)
              ? (unsigned long)writer.records
              : 0ul,
          mode_is_selected(mode) || mode_is_projection(mode) ||
                  mode_is_project_mutation(mode)
              ? benchmark_payload_bytes(mode, &writer)
              : 0ul);
  json_string(mode_is_selected(mode)
                  ? (mode_is_source(mode) ? "spooled" : "seekable_range")
                  : ((mode_is_projection(mode) || mode_is_project_mutation(mode))
                         ? "projection"
                         : "none"));
  fputs(",\"fixture_sha256\":", stdout);
  json_string(fixture_hash);
  if (unsupported) {
    fputs(",\"ns_per_op\":null,\"peak_rss_bytes\":null", stdout);
  } else {
    fprintf(stdout, ",\"ns_per_op\":%ld,\"peak_rss_bytes\":%lu", best_ns,
            peak_rss_bytes());
  }
  fputs(",\"allocs_per_op\":null,\"unsupported\":", stdout);
  fputs(unsupported ? "true" : "false", stdout);
  fputs(",\"unsupported_reason\":", stdout);
  json_string(unsupported_reason);
  fputs("}\n", stdout);
  if (mutation != NULL)
    ctx->mutation_destroy(ctx, mutation);
  if (projection != NULL)
    ctx->projection_destroy(ctx, projection);
  if (selector != NULL)
    ctx->selector_destroy(ctx, selector);
  if (ctx != NULL)
    ctx->destroy(ctx);
  fclose(file);
  return 0;
}
