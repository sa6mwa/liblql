#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "lql/lql.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <time.h>

typedef struct payload_counts {
  lql *ctx;
  lql_uint64 payloads;
  lql_uint64 payload_bytes;
  FILE *sink;
  const lql_projection *projection;
} payload_counts;

typedef struct bench_source {
  FILE *file;
} bench_source;

#if defined(__linux__)
static ssize_t discard_cookie_write(void *cookie, const char *data,
                                    size_t size) {
  (void)cookie;
  (void)data;
  return (ssize_t)size;
}
#endif

static FILE *open_discard_sink(void) {
#if defined(__linux__)
  cookie_io_functions_t io;
  memset(&io, 0, sizeof(io));
  io.write = discard_cookie_write;
  return fopencookie(NULL, "wb", io);
#else
  return fopen("/dev/null", "wb");
#endif
}

static const char *default_mutation_exprs[] = {"/bench/touched=true"};
static const char *numeric_mutation_exprs[] = {"/voucher/lines/10/bench=true"};
static const char *lockd_mutation_exprs[] = {"/processed=true"};
static const char *realworld_dense_mutation_exprs[] = {"/component=lql"};
static const char *realworld_sparse_mutation_exprs[] = {"/event=session_sync"};
static const char *realworld_nested_mutation_exprs[] = {"/query/hash=ff"};
static const char *realworld_increment_mutation_exprs[] = {"/code=+1"};
static const char *realworld_remove_mutation_exprs[] = {"rm:/payload"};
static const char *realworld_create_mutation_exprs[] = {"/meta/bench=true"};
static const char *realworld_multi_mutation_exprs[] = {
    "/component=lql", "/event=session_sync", "/code=+1", "rm:/payload",
    "/meta/bench=true"};

static const char *const *mutation_exprs_for_selector(const char *selector_name,
                                                      const char *expr,
                                                      lql_uint64 *out_count) {
  if (out_count != NULL) {
    *out_count = 1u;
  }
  if (selector_name != NULL) {
    if (strcmp(selector_name, "realworld_eq_sparse") == 0) {
      return realworld_sparse_mutation_exprs;
    }
    if (strcmp(selector_name, "realworld_eq_dense") == 0) {
      return realworld_dense_mutation_exprs;
    }
    if (strcmp(selector_name, "realworld_nested_eq_sparse") == 0) {
      return realworld_nested_mutation_exprs;
    }
    if (strcmp(selector_name, "realworld_range_sparse") == 0) {
      return realworld_increment_mutation_exprs;
    }
    if (strcmp(selector_name, "realworld_recursive_nested_eq_sparse") == 0) {
      return realworld_remove_mutation_exprs;
    }
    if (strcmp(selector_name, "realworld_contains_event_sparse") == 0) {
      return realworld_create_mutation_exprs;
    }
    if (strcmp(selector_name, "realworld_multi_clause_and") == 0) {
      if (out_count != NULL) {
        *out_count = 5u;
      }
      return realworld_multi_mutation_exprs;
    }
  }
  if (expr != NULL && strstr(expr, "/voucher/lines/10/") != NULL) {
    return numeric_mutation_exprs;
  }
  if (expr != NULL && (strstr(expr, "/event=\"session_sync\"") != NULL ||
                       strstr(expr, "/event=\"tabs_update\"") != NULL ||
                       strstr(expr, "/lockd/key") != NULL)) {
    return lockd_mutation_exprs;
  }
  return default_mutation_exprs;
}

static lql_status observe_decision(void *user,
                                   const lql_query_decision *decision) {
  (void)user;
  (void)decision;
  return LQL_STATUS_OK;
}

static lql_read_result read_bench_source(void *user, unsigned char *buffer,
                                         size_t capacity) {
  bench_source *source;
  lql_read_result result;

  memset(&result, 0, sizeof(result));
  source = (bench_source *)user;
  if (source == NULL || source->file == NULL || buffer == NULL ||
      capacity == 0u) {
    result.eof = 1;
    return result;
  }
  result.bytes_read = fread(buffer, 1u, capacity, source->file);
  if (result.bytes_read < capacity && ferror(source->file)) {
    result.error_code = 1;
  }
  if (result.bytes_read == 0u && feof(source->file)) {
    result.eof = 1;
  }
  return result;
}

static void print_u64(lql_uint64 value) {
  char buf[32];
  size_t len;

  len = 0u;
  if (value == 0u) {
    fputc('0', stdout);
    return;
  }
  while (value != 0u && len < sizeof(buf)) {
    buf[len++] = (char)('0' + (int)(value % 10u));
    value /= 10u;
  }
  while (len != 0u) {
    fputc(buf[--len], stdout);
  }
}

static lql_uint64 elapsed_ns(clock_t start, clock_t end) {
  double seconds;

  if (end <= start) {
    return 0u;
  }
  seconds = (double)(end - start) / (double)CLOCKS_PER_SEC;
  return (lql_uint64)(seconds * 1000000000.0);
}

static lql_uint64 peak_rss_bytes(void) {
  struct rusage usage;

  if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
    return 0u;
  }
#ifdef __APPLE__
  return (lql_uint64)usage.ru_maxrss;
#else
  return (lql_uint64)usage.ru_maxrss * 1024u;
#endif
}

static int append_text(char *buf, size_t capacity, size_t *pos,
                       const char *text) {
  size_t len;

  if (buf == NULL || pos == NULL || text == NULL) {
    return 0;
  }
  len = strlen(text);
  if (*pos > capacity || len >= capacity - *pos) {
    return 0;
  }
  memcpy(buf + *pos, text, len);
  *pos += len;
  buf[*pos] = '\0';
  return 1;
}

static int make_file_backed_mutation_expr(const char *mode,
                                          const char *fixture_path, char *buf,
                                          size_t capacity) {
  size_t pos;
  const char *prefix;

  if (mode == NULL || fixture_path == NULL || buf == NULL || capacity == 0u) {
    return 0;
  }
  if (strcmp(mode, "mutate_file_backed_text") == 0) {
    prefix = "textfile:/payload=";
  } else if (strcmp(mode, "mutate_file_backed_base64") == 0) {
    prefix = "base64file:/payload=";
  } else {
    return 0;
  }
  pos = 0u;
  buf[0] = '\0';
  return append_text(buf, capacity, &pos, prefix) &&
         append_text(buf, capacity, &pos, fixture_path) &&
         append_text(buf, capacity, &pos, ".payload");
}

static lql_status seek_end_size(FILE *file, lql_uint64 *out_size) {
  off_t size;

  if (file == NULL || out_size == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (fseeko(file, (off_t)0, SEEK_END) != 0) {
    return LQL_STATUS_JSON_ERROR;
  }
  size = ftello(file);
  if (size < (off_t)0) {
    return LQL_STATUS_JSON_ERROR;
  }
  if (fseeko(file, (off_t)0, SEEK_SET) != 0) {
    return LQL_STATUS_JSON_ERROR;
  }
  *out_size = (lql_uint64)size;
  return LQL_STATUS_OK;
}

static lql_status count_payload(void *user, const lql_query_match *match) {
  payload_counts *counts;
  lql_status st;
  lql_error error;

  counts = (payload_counts *)user;
  if (match->payload.kind != LQL_PAYLOAD_SEEKABLE_RANGE ||
      match->payload.size != match->decision.size ||
      match->payload.offset != match->decision.offset) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_error_init(&error);
  st = counts->ctx->payload_write_json(counts->ctx, &match->payload,
                                       counts->sink, &error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  counts->payloads++;
  counts->payload_bytes += match->payload.size;
  return LQL_STATUS_OK;
}

static lql_status count_spooled_payload(void *user,
                                        const lql_query_match *match) {
  payload_counts *counts;

  counts = (payload_counts *)user;
  if (match->payload.kind != LQL_PAYLOAD_SPOOLED ||
      match->payload.size != match->decision.size) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  /* Match Go's in-memory payload accounting: count delivery without forcing a
     replay copy through the benchmark sink. */
  counts->payloads++;
  counts->payload_bytes += match->payload.size;
  return LQL_STATUS_OK;
}

static lql_status count_projected_payload(void *user,
                                          const lql_query_match *match) {
  payload_counts *counts;
  lql_status st;
  lql_error error;
  int found;
  lql_uint64 before;
  lql_uint64 after;
  long pos;

  counts = (payload_counts *)user;
  if (counts == NULL || counts->projection == NULL || counts->sink == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lql_error_init(&error);
  found = 0;
  pos = ftell(counts->sink);
  before = pos >= 0L ? (lql_uint64)pos : 0u;
  st = counts->ctx->payload_project_json(counts->ctx, &match->payload,
                                         counts->projection, counts->sink,
                                         &found, &error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  pos = ftell(counts->sink);
  after = pos >= 0L ? (lql_uint64)pos : before;
  if (found) {
    counts->payloads++;
    if (after >= before) {
      counts->payload_bytes += after - before;
    }
  }
  return LQL_STATUS_OK;
}

static lql_status
run_payload_pass(lql *ctx, const char *mode, const char *expr,
                 const char *selector_name, FILE *fixture,
                 lql_uint64 fixture_size, lql_selector *selector,
                 lql_projection *projection, const char *const *mutation_exprs,
                 lql_uint64 mutation_expr_count, lql_query_result *result,
                 payload_counts *counts, lql_error *error) {
  FILE *sink;
  lql_mutation_plan *mutation_plan;
  lql_mutation_parse_options mutation_options;
  lql_selector *parsed_selector;
  lql_selector *run_selector;
  bench_source source;
  lql_status st;

  sink = NULL;
  mutation_plan = NULL;
  parsed_selector = NULL;
  run_selector = selector;
  memset(result, 0, sizeof(*result));
  memset(counts, 0, sizeof(*counts));
  counts->ctx = ctx;
  counts->projection = projection;
  memset(&source, 0, sizeof(source));
  source.file = fixture;
  if (fseeko(fixture, (off_t)0, SEEK_SET) != 0) {
    return LQL_STATUS_JSON_ERROR;
  }

  if (strcmp(mode, "reparse_selector_each_run") == 0) {
    st = ctx->selector_parse(ctx, expr, &parsed_selector, error);
    if (st != LQL_STATUS_OK) {
      return st;
    }
    run_selector = parsed_selector;
  }

  if (strcmp(mode, "decision_only_selector") == 0 ||
      strcmp(mode, "decision_only_plan") == 0 ||
      strcmp(mode, "reuse_selector") == 0 ||
      strcmp(mode, "reparse_selector_each_run") == 0) {
    st = ctx->query_file_decisions(ctx, run_selector, fixture, observe_decision,
                                   NULL, result, error);
  } else if (strcmp(mode, "decision_only_source_selector") == 0) {
    st = ctx->query_source_decisions(ctx, run_selector, read_bench_source,
                                     &source, observe_decision, NULL, result,
                                     error);
  } else if (strcmp(mode, "plus_value_selector") == 0 ||
             strcmp(mode, "plus_value_plan") == 0 ||
             strcmp(mode, "plus_value_openjson_selector") == 0 ||
             strcmp(mode, "plus_value_openjson_plan") == 0) {
    sink = open_discard_sink();
    if (sink == NULL) {
      st = LQL_STATUS_JSON_ERROR;
      goto done;
    }
    counts->sink = sink;
    st = ctx->query_file_matches(ctx, run_selector, fixture, count_payload,
                                 counts, result, error);
  } else if (strcmp(mode, "plus_value_source_selector") == 0) {
    sink = open_discard_sink();
    if (sink == NULL) {
      st = LQL_STATUS_JSON_ERROR;
      goto done;
    }
    counts->sink = sink;
    st = ctx->query_source_spooled_matches(ctx, run_selector, read_bench_source,
                                           &source, count_spooled_payload,
                                           counts, result, error);
  } else if (strcmp(mode, "project_file_selector") == 0) {
    sink = tmpfile();
    if (sink == NULL) {
      st = LQL_STATUS_JSON_ERROR;
      goto done;
    }
    counts->sink = sink;
    st =
        ctx->query_file_matches(ctx, run_selector, fixture,
                                count_projected_payload, counts, result, error);
  } else if (strcmp(mode, "project_source_selector") == 0) {
    sink = tmpfile();
    if (sink == NULL) {
      st = LQL_STATUS_JSON_ERROR;
      goto done;
    }
    counts->sink = sink;
    st = ctx->query_source_spooled_matches(ctx, run_selector, read_bench_source,
                                           &source, count_projected_payload,
                                           counts, result, error);
  } else if (strcmp(mode, "mutate_file_selector") == 0 ||
             strcmp(mode, "mutate_file_plan") == 0) {
    sink = open_discard_sink();
    if (sink == NULL) {
      st = LQL_STATUS_JSON_ERROR;
      goto done;
    }
    st = ctx->mutation_plan_parse(ctx, mutation_exprs, mutation_expr_count,
                                  &mutation_plan, error);
    if (st == LQL_STATUS_OK) {
      st = ctx->mutate_file_range_candidates(ctx, run_selector, mutation_plan,
                                             fixture, 0u, fixture_size, sink, 1,
                                             1, result, error);
    }
  } else if (strcmp(mode, "mutate_source_selector") == 0) {
    sink = open_discard_sink();
    if (sink == NULL) {
      st = LQL_STATUS_JSON_ERROR;
      goto done;
    }
    st = ctx->mutation_plan_parse(ctx, mutation_exprs, mutation_expr_count,
                                  &mutation_plan, error);
    if (st == LQL_STATUS_OK) {
      st = ctx->mutate_source_candidates(ctx, run_selector, mutation_plan,
                                         read_bench_source, &source, sink, 1, 1,
                                         result, error);
    }
  } else if (strcmp(mode, "mutate_file_backed_text") == 0 ||
             strcmp(mode, "mutate_file_backed_base64") == 0) {
    sink = open_discard_sink();
    if (sink == NULL) {
      st = LQL_STATUS_JSON_ERROR;
      goto done;
    }
    memset(&mutation_options, 0, sizeof(mutation_options));
    mutation_options.enable_file_values = 1;
    st = ctx->mutation_plan_parse_with_options(
        ctx, mutation_exprs, mutation_expr_count, &mutation_options,
        &mutation_plan, error);
    if (st == LQL_STATUS_OK) {
      st = ctx->mutate_file_range_candidates(ctx, run_selector, mutation_plan,
                                             fixture, 0u, fixture_size, sink, 1,
                                             0, result, error);
    }
  } else {
    st = LQL_STATUS_INVALID_ARGUMENT;
  }

done:
  if (sink != NULL) {
    fclose(sink);
  }
  ctx->mutation_plan_destroy(ctx, mutation_plan);
  if (parsed_selector != NULL) {
    ctx->selector_destroy(ctx, parsed_selector);
  }
  (void)selector_name;
  return st;
}

int main(int argc, char **argv) {
  const char *mode;
  const char *expr;
  const char *fixture_path;
  const char *selector_name;
  const char *submode;
  FILE *fixture;
  lql *ctx;
  lql_selector *selector;
  lql_projection *projection;
  lql_query_result result;
  lql_error error;
  lql_status st;
  payload_counts counts;
  lql_uint64 fixture_size;
  const char *const *mutation_exprs;
  const char *file_backed_mutation_exprs[1];
  lql_uint64 mutation_expr_count;
  char file_backed_mutation_expr[4096];
  clock_t start;
  clock_t end;

  if (argc != 4 && argc != 5 && argc != 6) {
    fprintf(stderr, "usage: lql_payload_bench MODE SELECTOR FIXTURE "
                    "[SELECTOR_NAME] [SUBMODE]\n");
    return 2;
  }
  mode = argv[1];
  expr = argv[2];
  fixture_path = argv[3];
  selector_name = argc >= 5 ? argv[4] : NULL;
  submode = argc == 6 ? argv[5] : "warmup_included";
  if (strcmp(submode, "warmup_included") != 0 &&
      strcmp(submode, "steady_state") != 0) {
    fprintf(stderr, "lql_payload_bench: unsupported submode: %s\n", submode);
    return 2;
  }
  mutation_expr_count = 1u;
  mutation_exprs =
      mutation_exprs_for_selector(selector_name, expr, &mutation_expr_count);
  if (strcmp(mode, "mutate_file_backed_text") == 0 ||
      strcmp(mode, "mutate_file_backed_base64") == 0) {
    if (!make_file_backed_mutation_expr(mode, fixture_path,
                                        file_backed_mutation_expr,
                                        sizeof(file_backed_mutation_expr))) {
      fprintf(stderr,
              "lql_payload_bench: failed to build file-backed mutation expr\n");
      return 1;
    }
    file_backed_mutation_exprs[0] = file_backed_mutation_expr;
    mutation_exprs = file_backed_mutation_exprs;
    mutation_expr_count = 1u;
  }

  lql_error_init(&error);
  ctx = NULL;
  selector = NULL;
  projection = NULL;
  st = lql_new(&ctx, &error);
  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "lql_payload_bench: create lql: %s\n", error.message);
    return 1;
  }
  if (strcmp(mode, "reparse_selector_each_run") != 0) {
    st = ctx->selector_parse(ctx, expr, &selector, &error);
    if (st != LQL_STATUS_OK) {
      fprintf(stderr, "lql_payload_bench: parse selector: %s\n", error.message);
      ctx->destroy(ctx);
      return 1;
    }
  }
  if (strcmp(mode, "project_file_selector") == 0 ||
      strcmp(mode, "project_source_selector") == 0) {
    static const char *projection_fields[] = {"/id"};
    st = ctx->projection_parse(ctx, projection_fields, 1u, &projection, &error);
    if (st != LQL_STATUS_OK) {
      fprintf(stderr, "lql_payload_bench: parse projection: %s\n",
              error.message);
      ctx->selector_destroy(ctx, selector);
      ctx->destroy(ctx);
      return 1;
    }
  }

  fixture = fopen(fixture_path, "rb");
  if (fixture == NULL) {
    fprintf(stderr, "lql_payload_bench: failed to open fixture\n");
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }
  st = seek_end_size(fixture, &fixture_size);
  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "lql_payload_bench: failed to determine fixture size\n");
    fclose(fixture);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 1;
  }

  if (strcmp(submode, "steady_state") == 0) {
    st = run_payload_pass(ctx, mode, expr, selector_name, fixture, fixture_size,
                          selector, projection, mutation_exprs,
                          mutation_expr_count, &result, &counts, &error);
    if (st != LQL_STATUS_OK) {
      fprintf(stderr, "lql_payload_bench: warmup mode %s: %s\n", mode,
              error.message);
      fclose(fixture);
      ctx->projection_destroy(ctx, projection);
      ctx->selector_destroy(ctx, selector);
      ctx->destroy(ctx);
      return 1;
    }
  }

  start = clock();
  st = run_payload_pass(ctx, mode, expr, selector_name, fixture, fixture_size,
                        selector, projection, mutation_exprs,
                        mutation_expr_count, &result, &counts, &error);
  end = clock();
  fclose(fixture);
  ctx->projection_destroy(ctx, projection);
  ctx->selector_destroy(ctx, selector);

  if (st != LQL_STATUS_OK) {
    if (st == LQL_STATUS_INVALID_ARGUMENT) {
      fprintf(stderr, "lql_payload_bench: unsupported mode: %s\n", mode);
      ctx->destroy(ctx);
      return 2;
    }
    fprintf(stderr, "lql_payload_bench: query mode %s: %s\n", mode,
            error.message);
    ctx->destroy(ctx);
    return 1;
  }

  fputs("candidates=", stdout);
  print_u64(result.candidates_seen);
  fputs(" matches=", stdout);
  print_u64(result.candidates_matched);
  fputs(" payloads=", stdout);
  print_u64(counts.payloads);
  fputs(" payload_bytes=", stdout);
  print_u64(counts.payload_bytes);
  fputs(" elapsed_ns=", stdout);
  print_u64(elapsed_ns(start, end));
  fputs(" peak_rss_bytes=", stdout);
  print_u64(peak_rss_bytes());
  fputc('\n', stdout);
  ctx->destroy(ctx);
  return 0;
}
