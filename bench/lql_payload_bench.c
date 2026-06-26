#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
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
  if (expr != NULL &&
      (strstr(expr, "/event=\"session_sync\"") != NULL ||
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
  lql_status st;
  lql_error error;

  counts = (payload_counts *)user;
  if (match->payload.kind != LQL_PAYLOAD_SPOOLED ||
      match->payload.size != match->decision.size) {
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

int main(int argc, char **argv) {
  const char *mode;
  const char *expr;
  const char *fixture_path;
  const char *selector_name;
  FILE *fixture;
  FILE *sink;
  lql *ctx;
  lql_selector *selector;
  lql_projection *projection;
  lql_query_result result;
  lql_mutation_plan *mutation_plan;
  lql_error error;
  lql_status st;
  payload_counts counts;
  bench_source source;
  lql_uint64 fixture_size;
  const char *const *mutation_exprs;
  lql_uint64 mutation_expr_count;
  clock_t start;
  clock_t end;

  if (argc != 4 && argc != 5) {
    fprintf(stderr,
            "usage: lql_payload_bench MODE SELECTOR FIXTURE [SELECTOR_NAME]\n");
    return 2;
  }
  mode = argv[1];
  expr = argv[2];
  fixture_path = argv[3];
  selector_name = argc == 5 ? argv[4] : NULL;
  mutation_expr_count = 1u;
  mutation_exprs =
      mutation_exprs_for_selector(selector_name, expr, &mutation_expr_count);

  lql_error_init(&error);
  ctx = NULL;
  selector = NULL;
  projection = NULL;
  mutation_plan = NULL;
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

  memset(&counts, 0, sizeof(counts));
  counts.ctx = ctx;
  counts.projection = projection;
  memset(&source, 0, sizeof(source));
  source.file = fixture;
  memset(&result, 0, sizeof(result));
  start = clock();
  if (strcmp(mode, "reparse_selector_each_run") == 0) {
    st = ctx->selector_parse(ctx, expr, &selector, &error);
    if (st == LQL_STATUS_OK) {
      st = ctx->query_file_decisions(ctx, selector, fixture, observe_decision,
                                     NULL, &result, &error);
    }
  } else if (strcmp(mode, "decision_only_selector") == 0 ||
             strcmp(mode, "decision_only_plan") == 0 ||
             strcmp(mode, "reuse_selector") == 0) {
    st = ctx->query_file_decisions(ctx, selector, fixture, observe_decision,
                                   NULL, &result, &error);
  } else if (strcmp(mode, "decision_only_source_selector") == 0) {
    st = ctx->query_source_decisions(ctx, selector, read_bench_source, &source,
                                     observe_decision, NULL, &result, &error);
  } else if (strcmp(mode, "plus_value_selector") == 0 ||
             strcmp(mode, "plus_value_plan") == 0) {
    sink = fopen("/dev/null", "wb");
    if (sink == NULL) {
      fprintf(stderr, "lql_payload_bench: failed to open /dev/null\n");
      fclose(fixture);
      ctx->selector_destroy(ctx, selector);
      ctx->destroy(ctx);
      return 1;
    }
    counts.sink = sink;
    st = ctx->query_file_matches(ctx, selector, fixture, count_payload, &counts,
                                 &result, &error);
    fclose(sink);
  } else if (strcmp(mode, "plus_value_source_selector") == 0) {
    sink = fopen("/dev/null", "wb");
    if (sink == NULL) {
      fprintf(stderr, "lql_payload_bench: failed to open /dev/null\n");
      fclose(fixture);
      ctx->selector_destroy(ctx, selector);
      ctx->destroy(ctx);
      return 1;
    }
    counts.sink = sink;
    st = ctx->query_source_spooled_matches(ctx, selector, read_bench_source,
                                           &source, count_spooled_payload,
                                           &counts, &result, &error);
    fclose(sink);
  } else if (strcmp(mode, "plus_value_openjson_selector") == 0 ||
             strcmp(mode, "plus_value_openjson_plan") == 0) {
    sink = fopen("/dev/null", "wb");
    if (sink == NULL) {
      fprintf(stderr, "lql_payload_bench: failed to open /dev/null\n");
      fclose(fixture);
      ctx->selector_destroy(ctx, selector);
      ctx->destroy(ctx);
      return 1;
    }
    counts.sink = sink;
    st = ctx->query_file_matches(ctx, selector, fixture, count_payload, &counts,
                                 &result, &error);
    fclose(sink);
  } else if (strcmp(mode, "project_file_selector") == 0) {
    sink = tmpfile();
    if (sink == NULL) {
      fprintf(stderr, "lql_payload_bench: failed to create projection sink\n");
      fclose(fixture);
      ctx->projection_destroy(ctx, projection);
      ctx->selector_destroy(ctx, selector);
      ctx->destroy(ctx);
      return 1;
    }
    counts.sink = sink;
    st = ctx->query_file_matches(ctx, selector, fixture, count_projected_payload,
                                 &counts, &result, &error);
    fclose(sink);
  } else if (strcmp(mode, "project_source_selector") == 0) {
    sink = tmpfile();
    if (sink == NULL) {
      fprintf(stderr, "lql_payload_bench: failed to create projection sink\n");
      fclose(fixture);
      ctx->projection_destroy(ctx, projection);
      ctx->selector_destroy(ctx, selector);
      ctx->destroy(ctx);
      return 1;
    }
    counts.sink = sink;
    st = ctx->query_source_spooled_matches(ctx, selector, read_bench_source,
                                           &source, count_projected_payload,
                                           &counts, &result, &error);
    fclose(sink);
  } else if (strcmp(mode, "mutate_file_selector") == 0 ||
             strcmp(mode, "mutate_file_plan") == 0) {
    sink = fopen("/dev/null", "wb");
    if (sink == NULL) {
      fprintf(stderr, "lql_payload_bench: failed to open /dev/null\n");
      fclose(fixture);
      ctx->selector_destroy(ctx, selector);
      ctx->destroy(ctx);
      return 1;
    }
    st = ctx->mutation_plan_parse(ctx, mutation_exprs, mutation_expr_count,
                                  &mutation_plan, &error);
    if (st == LQL_STATUS_OK) {
      st = ctx->mutate_file_range_candidates(ctx, selector, mutation_plan,
                                             fixture, 0u, fixture_size, sink,
                                             1, 1, &result, &error);
    }
    fclose(sink);
  } else if (strcmp(mode, "mutate_source_selector") == 0) {
    sink = fopen("/dev/null", "wb");
    if (sink == NULL) {
      fprintf(stderr, "lql_payload_bench: failed to open /dev/null\n");
      fclose(fixture);
      ctx->selector_destroy(ctx, selector);
      ctx->destroy(ctx);
      return 1;
    }
    st = ctx->mutation_plan_parse(ctx, mutation_exprs, mutation_expr_count,
                                  &mutation_plan, &error);
    if (st == LQL_STATUS_OK) {
      st = ctx->mutate_source_candidates(ctx, selector, mutation_plan,
                                         read_bench_source, &source, sink, 1, 1,
                                         &result, &error);
    }
    fclose(sink);
  } else {
    fprintf(stderr, "lql_payload_bench: unsupported mode: %s\n", mode);
    fclose(fixture);
    ctx->selector_destroy(ctx, selector);
    ctx->destroy(ctx);
    return 2;
  }
  end = clock();
  fclose(fixture);
  ctx->mutation_plan_destroy(ctx, mutation_plan);
  ctx->projection_destroy(ctx, projection);
  ctx->selector_destroy(ctx, selector);

  if (st != LQL_STATUS_OK) {
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
