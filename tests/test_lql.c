#include "lql/lql.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

typedef struct stream_seen {
  int calls;
  int matched;
  lql_uint64 offsets[4];
  lql_uint64 sizes[4];
  int stop_after_first;
} stream_seen;

static lql_status record_decision(void *user,
                                  const lql_query_decision *decision) {
  stream_seen *seen = (stream_seen *)user;
  if (seen->calls < 4) {
    seen->offsets[seen->calls] = decision->offset;
    seen->sizes[seen->calls] = decision->size;
  }
  if (decision->matched) {
    ++seen->matched;
  }
  ++seen->calls;
  if (seen->stop_after_first) {
    return LQL_STATUS_STOP;
  }
  return LQL_STATUS_OK;
}

static void expect_match(const char *expr, const char *json, int want) {
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int got;

  lql_error_init(&error);
  st = lql_selector_parse(expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("parse failed for %s: %s\n", expr, error.message);
    ++failures;
    return;
  }
  st = lql_matches_json(selector, json, strlen(json), &got, &error);
  if (st != LQL_STATUS_OK) {
    printf("eval failed for %s: %s\n", expr, error.message);
    ++failures;
  } else if (got != want) {
    printf("match mismatch for %s: got %d want %d\n", expr, got, want);
    ++failures;
  }
  lql_selector_free(selector);
}

static void expect_parse_error(const char *expr) {
  lql_selector *selector;
  lql_error error;
  lql_status st;

  selector = NULL;
  lql_error_init(&error);
  st = lql_selector_parse(expr, &selector, &error);
  if (st == LQL_STATUS_OK) {
    printf("parse unexpectedly succeeded for %s\n", expr);
    lql_selector_free(selector);
    ++failures;
  }
}

static void expect_match_or(const char *expr, const char *json, int want) {
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int got;

  lql_error_init(&error);
  st = lql_selector_parse_or(expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("parse-or failed for %s: %s\n", expr, error.message);
    ++failures;
    return;
  }
  st = lql_matches_json(selector, json, strlen(json), &got, &error);
  if (st != LQL_STATUS_OK) {
    printf("eval-or failed for %s: %s\n", expr, error.message);
    ++failures;
  } else if (got != want) {
    printf("or match mismatch for %s: got %d want %d\n", expr, got, want);
    ++failures;
  }
  lql_selector_free(selector);
}

static void expect_stream_file(void) {
  static const char input[] =
      "{\"status\":\"open\"}\n{\"status\":\"closed\"}\n";
  FILE *fp;
  lql_selector *selector;
  lql_query_result result;
  stream_seen seen;
  lql_error error;
  lql_status st;

  memset(&seen, 0, sizeof(seen));
  lql_error_init(&error);
  st = lql_selector_parse("/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  fp = tmpfile();
  if (fp == NULL) {
    printf("tmpfile failed\n");
    lql_selector_free(selector);
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("tmpfile write/seek failed\n");
    fclose(fp);
    lql_selector_free(selector);
    ++failures;
    return;
  }
  memset(&result, 0, sizeof(result));
  st = lql_query_file_decisions(selector, fp, record_decision, &seen, &result,
                                &error);
  fclose(fp);
  lql_selector_free(selector);
  if (st != LQL_STATUS_OK) {
    printf("stream query failed: %s\n", error.message);
    ++failures;
    return;
  }
  if (seen.calls != 2 || seen.matched != 1 ||
      result.candidates_seen != (lql_uint64)2 ||
      result.candidates_matched != (lql_uint64)1) {
    printf("stream counts mismatch calls=%d matched=%d\n", seen.calls,
           seen.matched);
    ++failures;
  }
  if (seen.offsets[0] != (lql_uint64)0 || seen.sizes[0] != (lql_uint64)17 ||
      seen.offsets[1] != (lql_uint64)18 || seen.sizes[1] != (lql_uint64)19) {
    printf("stream ranges mismatch\n");
    ++failures;
  }
}

static void expect_stream_array_items(void) {
  static const char input[] =
      "[{\"status\":\"open\"}, {\"status\":\"closed\"}]";
  FILE *fp;
  lql_selector *selector;
  lql_query_result result;
  stream_seen seen;
  lql_error error;
  lql_status st;

  memset(&seen, 0, sizeof(seen));
  lql_error_init(&error);
  st = lql_selector_parse("/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("array stream parse failed: %s\n", error.message);
    ++failures;
    return;
  }
  fp = tmpfile();
  if (fp == NULL) {
    printf("array tmpfile failed\n");
    lql_selector_free(selector);
    ++failures;
    return;
  }
  if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    printf("array tmpfile write/seek failed\n");
    fclose(fp);
    lql_selector_free(selector);
    ++failures;
    return;
  }
  memset(&result, 0, sizeof(result));
  st = lql_query_file_decisions(selector, fp, record_decision, &seen, &result,
                                &error);
  fclose(fp);
  lql_selector_free(selector);
  if (st != LQL_STATUS_OK) {
    printf("array stream query failed: %s\n", error.message);
    ++failures;
    return;
  }
  if (seen.calls != 2 || seen.matched != 1 ||
      result.candidates_seen != (lql_uint64)2 ||
      result.candidates_matched != (lql_uint64)1) {
    printf("array stream counts mismatch calls=%d matched=%d\n", seen.calls,
           seen.matched);
    ++failures;
  }
  if (seen.offsets[0] != (lql_uint64)1 || seen.sizes[0] != (lql_uint64)17 ||
      seen.offsets[1] != (lql_uint64)20 || seen.sizes[1] != (lql_uint64)19) {
    printf("array stream ranges mismatch\n");
    ++failures;
  }
}

static void expect_stream_stop_controls(void) {
  static const char input[] =
      "{\"status\":\"open\"}\n{\"status\":\"open\"}\n{\"status\":\"closed\"}\n";
  FILE *fp;
  lql_selector *selector;
  lql_query_options options;
  lql_query_result result;
  stream_seen seen;
  lql_error error;
  lql_status st;

  lql_error_init(&error);
  st = lql_selector_parse("/status=\"open\"", &selector, &error);
  if (st != LQL_STATUS_OK) {
    printf("stop controls parse failed: %s\n", error.message);
    ++failures;
    return;
  }

#define RUN_STOP_CASE(label, setup_options, setup_seen, want_calls, want_matched, want_reason) \
  do {                                                                       \
    fp = tmpfile();                                                          \
    if (fp == NULL) {                                                        \
      printf(label " tmpfile failed\n");                                     \
      ++failures;                                                            \
      break;                                                                 \
    }                                                                        \
    if (fwrite(input, 1u, strlen(input), fp) != strlen(input) ||             \
        fseek(fp, 0L, SEEK_SET) != 0) {                                      \
      printf(label " tmpfile write/seek failed\n");                         \
      fclose(fp);                                                            \
      ++failures;                                                            \
      break;                                                                 \
    }                                                                        \
    memset(&options, 0, sizeof(options));                                    \
    memset(&seen, 0, sizeof(seen));                                          \
    memset(&result, 0, sizeof(result));                                      \
    setup_options;                                                           \
    setup_seen;                                                              \
    st = lql_query_file_decisions_with_options(                              \
        selector, fp, &options, record_decision, &seen, &result, &error);    \
    fclose(fp);                                                              \
    if (st != LQL_STATUS_OK) {                                               \
      printf(label " query failed: %s\n", error.message);                   \
      ++failures;                                                            \
      break;                                                                 \
    }                                                                        \
    if (seen.calls != (want_calls) || seen.matched != (want_matched) ||      \
        !result.stopped_early || result.stop_reason != (want_reason)) {      \
      printf(label " stop mismatch calls=%d matched=%d stopped=%d reason=%d\n", \
             seen.calls, seen.matched, result.stopped_early,                \
             (int)result.stop_reason);                                       \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

  RUN_STOP_CASE("max matches", options.max_matches = 1u, (void)0, 1, 1,
                LQL_QUERY_STOP_MATCH_LIMIT);
  RUN_STOP_CASE("max candidates", options.max_candidates = 2u, (void)0, 2, 2,
                LQL_QUERY_STOP_CANDIDATE_LIMIT);
  RUN_STOP_CASE("max bytes", options.max_bytes_read = 17u, (void)0, 1, 1,
                LQL_QUERY_STOP_BYTE_LIMIT);
  RUN_STOP_CASE("callback stop", (void)0, seen.stop_after_first = 1, 1, 1,
                LQL_QUERY_STOP_CALLBACK);

#undef RUN_STOP_CASE

  lql_selector_free(selector);
}

static int read_tmpfile(FILE *fp, char *buf, size_t cap, size_t *out_len) {
  long end;
  size_t got;
  if (fseek(fp, 0L, SEEK_END) != 0) {
    return 0;
  }
  end = ftell(fp);
  if (end < 0 || (size_t)end + 1u > cap) {
    return 0;
  }
  if (fseek(fp, 0L, SEEK_SET) != 0) {
    return 0;
  }
  got = fread(buf, 1u, (size_t)end, fp);
  if (got != (size_t)end) {
    return 0;
  }
  buf[got] = '\0';
  *out_len = got;
  return 1;
}

static void expect_projection_api(void) {
  static const char first[] =
      "{\"id\":\"a\",\"count\":1,\"nested\":{\"x\":true}}";
  static const char second[] = "{\"id\":\"b\"}";
  const char *fields[2];
  const char *missing[1];
  const char *invalid[1];
  FILE *source;
  FILE *out;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  int found;
  char buf[128];
  size_t len;

  source = tmpfile();
  out = tmpfile();
  if (source == NULL || out == NULL) {
    printf("projection tmpfile failed\n");
    if (source != NULL) {
      fclose(source);
    }
    if (out != NULL) {
      fclose(out);
    }
    ++failures;
    return;
  }
  if (fwrite(first, 1u, strlen(first), source) != strlen(first) ||
      fputc('\n', source) == EOF ||
      fwrite(second, 1u, strlen(second), source) != strlen(second)) {
    printf("projection source write failed\n");
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }

  fields[0] = "/id";
  fields[1] = "/nested";
  projection = NULL;
  lql_error_init(&error);
  st = lql_projection_parse(fields, 2u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("projection parse failed: %s\n", error.message);
    fclose(source);
    fclose(out);
    ++failures;
    return;
  }
  found = 0;
  st = lql_project_file_range(projection, source, 0u,
                              (lql_uint64)strlen(first), out, &found, &error);
  if (st != LQL_STATUS_OK) {
    printf("projection range failed: %s\n", error.message);
    ++failures;
  } else if (!found) {
    printf("projection expected found\n");
    ++failures;
  } else if (!read_tmpfile(out, buf, sizeof(buf), &len) ||
             strcmp(buf, "{\"id\":\"a\",\"nested\":{\"x\":true}}") != 0) {
    printf("projection output mismatch: %s\n", buf);
    ++failures;
  }
  lql_projection_free(projection);
  fclose(out);

  out = tmpfile();
  missing[0] = "/missing";
  projection = NULL;
  lql_error_init(&error);
  st = lql_projection_parse(missing, 1u, &projection, &error);
  if (st != LQL_STATUS_OK) {
    printf("missing projection parse failed: %s\n", error.message);
    ++failures;
  } else {
    found = 1;
    st = lql_project_file_range(projection, source, 0u,
                                (lql_uint64)strlen(first), out, &found,
                                &error);
    if (st != LQL_STATUS_OK) {
      printf("missing projection range failed: %s\n", error.message);
      ++failures;
    } else if (found) {
      printf("missing projection unexpectedly found\n");
      ++failures;
    } else if (!read_tmpfile(out, buf, sizeof(buf), &len) || len != 0u) {
      printf("missing projection wrote output\n");
      ++failures;
    }
  }
  lql_projection_free(projection);
  fclose(out);

  invalid[0] = "/nested/id";
  projection = NULL;
  lql_error_init(&error);
  st = lql_projection_parse(invalid, 1u, &projection, &error);
  if (st == LQL_STATUS_OK) {
    printf("invalid projection path parsed\n");
    lql_projection_free(projection);
    ++failures;
  }
  fclose(source);
}

int main(void) {
  expect_match("/status=\"open\"", "{\"status\":\"open\"}", 1);
  expect_match("/status=\"closed\"", "{\"status\":\"open\"}", 0);
  expect_match("eq{field=/status,field=/status,value=open,value=open}",
               "{\"status\":\"open\"}", 1);
  expect_match("/progress>=50", "{\"progress\":72}", 1);
  expect_match("/progress<50", "{\"progress\":72}", 0);
  expect_match("/timestamp=\"2025-01-01\"",
               "{\"timestamp\":\"2025-01-01T15:00:00Z\"}", 1);
  expect_match("/timestamp=\"2025-01-01\"",
               "{\"timestamp\":\"2025-01-02T00:00:00Z\"}", 0);
  expect_match("/timestamp!=2025-01-01",
               "{\"timestamp\":\"2025-01-01T15:00:00Z\"}", 0);
  expect_match("/timestamp!=2025-01-01", "{\"status\":\"open\"}", 1);
  expect_match("/timestamp>=2026-03-05T10:28:21Z",
               "{\"timestamp\":\"2026-03-05T11:28:21+01:00\"}", 1);
  expect_match("/timestamp>=2026-03-05T10:28:21",
               "{\"timestamp\":\"2026-03-05T11:28:21+01:00\"}", 1);
  expect_match("/timestamp>=2026-03-05T10:28:21",
               "{\"timestamp\":\"2026-03-05T10:28:20Z\"}", 0);
  expect_match("range{field=/progress,gt=10,lte=20}", "{\"progress\":11}", 1);
  expect_match("range{field=/progress,gt=10,lte=20}", "{\"progress\":10}", 0);
  expect_match("range{field=/progress,gt=10,lte=20}", "{\"progress\":20}", 1);
  expect_match("range{field=/progress,gt=10,lte=20}", "{\"progress\":21}", 0);
  expect_match("range{field=/timestamp,gte=2026-03-05T10:28:21Z,lt=2026-03-05T10:30:00Z}",
               "{\"timestamp\":\"2026-03-05T10:29:00Z\"}", 1);
  expect_match("range{field=/timestamp,gte=2026-03-05T10:28:21Z,lt=2026-03-05T10:30:00Z}",
               "{\"timestamp\":\"2026-03-05T10:30:00Z\"}", 0);
  expect_match("date{field=/timestamp,after=2025-01-01,before=2025-01-03}",
               "{\"timestamp\":\"2025-01-02T06:00:00Z\"}", 1);
  expect_match("date{field=/timestamp,after=2025-01-01,before=2025-01-03}",
               "{\"timestamp\":\"2025-01-03T00:00:00Z\"}", 0);
  expect_match("date{f=/timestamp,a=2025-01-01,b=2025-01-03}",
               "{\"timestamp\":\"2025-01-02T06:00:00Z\"}", 1);
  expect_match("date{field=/timestamp,value=2025-01-01}",
               "{\"timestamp\":\"2025-01-01T23:59:59Z\"}", 1);
  expect_match("date{field=/timestamp,value=2025-01-01}",
               "{\"timestamp\":\"2025-01-02T00:00:00Z\"}", 0);
  expect_match("date{field=/timestamp,since=2025-01-01}",
               "{\"timestamp\":\"2025-01-02T00:00:00Z\"}", 1);
  expect_match("date{field=/timestamp,since=now}",
               "{\"timestamp\":\"2099-01-01T00:00:00Z\"}", 1);
  expect_match("date{field=/timestamp,since=now}",
               "{\"timestamp\":\"1970-01-01T00:00:00Z\"}", 0);
  expect_match("date{field=/timestamp,since=TODAY}",
               "{\"timestamp\":\"2099-01-01T00:00:00Z\"}", 1);
  expect_match("date{field=/timestamp,since=yesterday}",
               "{\"timestamp\":\"1970-01-01T00:00:00Z\"}", 0);
  expect_match("date{f=/timestamp,after=2026-03-05T10:28:21.123,before=2026-03-05T10:28:21.123456790}",
               "{\"timestamp\":\"2026-03-05T10:28:21.123456789Z\"}", 1);
  expect_match("date{f=/timestamp,after=2026-03-05T10:28:21.123,before=2026-03-05T10:28:21.123456790}",
               "{\"timestamp\":\"2026-03-05T10:28:21.123+01:00\"}", 0);
  expect_match("contains{field=/message,value=timeout}",
               "{\"message\":\"upstream timeout\"}", 1);
  expect_match("contains{field=/metadata}", "{\"metadata\":{\"etag\":\"x\"}}",
               1);
  expect_match("contains{field=/missing}", "{\"metadata\":{\"etag\":\"x\"}}",
               0);
  expect_match("contains{field=/metadata,value=\"\"}",
               "{\"metadata\":{\"etag\":\"x\"}}", 0);
  expect_match("contains{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("icontains{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("prefix{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("iprefix{f=/,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("contains{f=/*}", "{\"status\":\"open\"}", 1);
  expect_match("icontains{f=/...,v=\"\"}", "{\"status\":\"open\"}", 1);
  expect_match("not.icontains{f=/,v=\"\"}", "{\"status\":\"open\"}", 0);
  expect_match("contains{field=/message,value=TIMEOUT,ignoreCase=true}",
               "{\"message\":\"upstream timeout\"}", 1);
  expect_match("contains{field=/message,value=TIMEOUT,ic=f}",
               "{\"message\":\"upstream timeout\"}", 0);
  expect_match("contains{field=/message,any=timeout|degraded}",
               "{\"message\":\"upstream timeout\"}", 1);
  expect_match("contains{field=/message,any=missing|degraded}",
               "{\"message\":\"upstream timeout\"}", 0);
  expect_match("icontains{field=/message,value=TIMEOUT}",
               "{\"message\":\"upstream timeout\"}", 1);
  expect_match("icontains{f=/message,a=TIMEOUT|DEGRADED}",
               "{\"message\":\"upstream timeout\"}", 1);
  expect_match("prefix{field=/service,value=auth}",
               "{\"service\":\"auth-api\"}", 1);
  expect_match("prefix{field=/metadata}", "{\"metadata\":{\"etag\":\"x\"}}",
               1);
  expect_match("iprefix{field=/metadata}", "{\"metadata\":{\"etag\":\"x\"}}",
               1);
  expect_match("prefix{field=/metadata,value=\"\"}",
               "{\"metadata\":{\"etag\":\"x\"}}", 0);
  expect_match("prefix{field=/service,value=AUTH,ic=t}",
               "{\"service\":\"auth-api\"}", 1);
  expect_match("in{field=/env,any=prod|stage}", "{\"env\":\"prod\"}", 1);
  expect_match("in{field=/env,any=prod|stage}", "{\"env\":\"dev\"}", 0);
  expect_match("contains{f=/hello/*}", "{\"hello\":{\"name\":\"alice\"}}", 1);
  expect_match("contains{f=/hello/[]}", "{\"hello\":{\"0\":\"alice\"}}", 0);
  expect_match("contains{f=/arrays/[]/id}", "{\"arrays\":[{\"id\":1}]}",
               1);
  expect_match("contains{f=/arrays/*/id}", "{\"arrays\":[{\"id\":1}]}", 0);
  expect_match("contains{f=/items[]/sku}", "{\"items\":[{\"sku\":\"a\"}]}",
               1);
  expect_match("contains{f=/items/**/sku}", "{\"items\":[{\"sku\":\"a\"}]}",
               1);
  expect_match("contains{f=/groups/.../sku}",
               "{\"groups\":[{\"items\":[{\"sku\":\"b\"}]}]}", 1);
  expect_match("exists{/metadata/etag}", "{\"metadata\":{\"etag\":\"x\"}}", 1);
  expect_match("exists{/metadata}", "{\"metadata\":{\"etag\":\"x\"}}", 1);
  expect_match("/metadata=\"\"", "{\"metadata\":{\"etag\":\"x\"}}", 0);
  expect_match("/items/0/sku=\"a\"", "{\"items\":[{\"sku\":\"a\"}]}", 1);
  expect_match("/status=\"open\",/progress>=50",
               "{\"status\":\"open\",\"progress\":72}", 1);
  expect_match("/status=\"open\",/progress>=50",
               "{\"status\":\"open\",\"progress\":4}", 0);
  expect_match_or("/status=\"open\",/progress>=50",
                  "{\"status\":\"closed\",\"progress\":72}", 1);
  expect_match_or("/status=\"open\",/progress>=50",
                  "{\"status\":\"closed\",\"progress\":4}", 0);
  expect_match("or.eq{field=/msg,value=warn},or.eq{field=/msg,value=timeout}",
               "{\"msg\":\"timeout\"}", 1);
  expect_match("or.eq{field=/msg,value=warn},or.eq{field=/msg,value=timeout}",
               "{\"msg\":\"ok\"}", 0);
  expect_match("and.eq{field=/status,value=open},and.range{field=/progress,gte=50}",
               "{\"status\":\"open\",\"progress\":72}", 1);
  expect_match("and.eq{field=/status,value=open},and.range{field=/progress,gte=50}",
               "{\"status\":\"open\",\"progress\":4}", 0);
  expect_match("and.0.eq{field=/status,value=open},and.0.range{field=/progress,gte=50}",
               "{\"status\":\"open\",\"progress\":72}", 1);
  expect_match("and.0.eq{field=/status,value=open},and.0.range{field=/progress,gte=50}",
               "{\"status\":\"open\",\"progress\":4}", 0);
  expect_match("or.0.eq{field=/status,value=open},or.0.range{field=/progress,gte=50}",
               "{\"status\":\"open\",\"progress\":72}", 1);
  expect_match("or.0.eq{field=/status,value=open},or.0.range{field=/progress,gte=50}",
               "{\"status\":\"closed\",\"progress\":72}", 0);
  expect_match("not.eq{field=/status,value=closed}", "{\"status\":\"open\"}",
               1);
  expect_match("not.eq{field=/status,value=closed}",
               "{\"status\":\"closed\"}", 0);
  expect_parse_error("contains{field=/message,value=timeout,any=error}");
  expect_parse_error("contains{field=/message,any=}");
  expect_parse_error("contains{field=/message,any=||}");
  expect_parse_error("contains{field=/message,value=timeout,value=error}");
  expect_parse_error("contains{field=/message,value=timeout,ignoreCase=maybe}");
  expect_parse_error("eq{field=/status,f=/other,value=open}");
  expect_parse_error("eq{field=/status,value=open,foo=bar}");
  expect_parse_error("eq{field=/status,value=open,ignoreCase=true}");
  expect_parse_error("or.0.eq{field=/status,value=open},or.0.eq{field=/status,value=closed}");
  expect_parse_error("and.0.eq{field=/status,value=open},and.0.eq{field=/status,value=closed}");
  expect_parse_error("range{field=/progress,gte=10,gte=20}");
  expect_parse_error("range{field=/progress,gte=10,foo=bar}");
  expect_parse_error("range{field=/progress,gte=10,lt=2025-01-01}");
  expect_parse_error("range{field=/timestamp,gte=yesterday}");
  expect_parse_error("/timestamp>=yesterday");
  expect_parse_error("date{field=/timestamp,value=2025-01-01 00:00:00}");
  expect_parse_error("date{field=/timestamp,after=2025-01-01,foo=bar}");
  expect_parse_error("date{field=/timestamp,since=yesterday,after=2025-01-01}");
  expect_parse_error("date{field=/timestamp,after=2025-01-01,gt=2025-01-02}");
  expect_parse_error("date{field=/timestamp,before=2025-01-03,lt=2025-01-02}");
  expect_parse_error("date{after=2025-01-01}");
  expect_parse_error("date{field=/timestamp,since=tomorrowish}");
  expect_parse_error("prefix{field=/service,any=auth|edge}");
  expect_parse_error("in{field=/env}");
  expect_parse_error("in{field=/env,any=prod|stage,a=dev}");
  expect_parse_error("in{field=/env,any=prod|stage,foo=bar}");
  expect_parse_error("range{field=/progress}");
  expect_stream_file();
  expect_stream_array_items();
  expect_stream_stop_controls();
  expect_projection_api();
  return failures == 0 ? 0 : 1;
}
