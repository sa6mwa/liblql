#include "lql/lql.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct match_count {
  lql_uint64 matched;
} match_count;

static lql_status count_match(void *user, const lql_query_decision *decision) {
  match_count *count;
  count = (match_count *)user;
  if (decision->matched) {
    ++count->matched;
  }
  return LQL_STATUS_OK;
}

static int read_stdin(char **out, size_t *out_len) {
  char *buf;
  size_t cap;
  size_t len;
  size_t n;
  char tmp[4096];
  buf = NULL;
  cap = 0u;
  len = 0u;
  while ((n = fread(tmp, 1u, sizeof(tmp), stdin)) > 0u) {
    if (len + n + 1u > cap) {
      size_t next_cap = cap == 0u ? 8192u : cap * 2u;
      char *next;
      while (next_cap < len + n + 1u) {
        next_cap *= 2u;
      }
      next = (char *)realloc(buf, next_cap);
      if (next == NULL) {
        free(buf);
        return 0;
      }
      buf = next;
      cap = next_cap;
    }
    memcpy(buf + len, tmp, n);
    len += n;
  }
  if (buf == NULL) {
    buf = (char *)malloc(1u);
    if (buf == NULL) {
      return 0;
    }
  }
  buf[len] = '\0';
  *out = buf;
  *out_len = len;
  return 1;
}

static void usage(FILE *out) {
  fprintf(out, "usage: clql [--or|-O] [--matches-only|-M] selector < data.json\n");
  fprintf(out, "       clql --version\n");
}

int main(int argc, char **argv) {
  lql_selector *selector;
  lql_error error;
  char *json;
  const char *selector_expr;
  size_t json_len;
  int matched;
  int matches_only;
  int or_mode;
  int i;
  lql_status st;
  match_count count;
  lql_query_result result;

  or_mode = 0;
  matches_only = 0;
  selector_expr = NULL;
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
      printf("clql 0.0.0\n");
      return 0;
    }
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(stdout);
      return 0;
    }
    if (strcmp(argv[i], "--or") == 0 || strcmp(argv[i], "-O") == 0) {
      or_mode = 1;
    } else if (strcmp(argv[i], "--matches-only") == 0 ||
               strcmp(argv[i], "-M") == 0) {
      matches_only = 1;
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "clql: unknown option %s\n", argv[i]);
      usage(stderr);
      return 2;
    } else if (selector_expr == NULL) {
      selector_expr = argv[i];
    } else {
      usage(stderr);
      return 2;
    }
  }
  if (selector_expr == NULL) {
    usage(stderr);
    return 2;
  }
  lql_error_init(&error);
  st = or_mode ? lql_selector_parse_or(selector_expr, &selector, &error)
               : lql_selector_parse(selector_expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "clql: %s\n", error.message);
    return 2;
  }
  if (matches_only) {
    count.matched = 0u;
    memset(&result, 0, sizeof(result));
    st = lql_query_file_decisions(selector, stdin, count_match, &count, &result,
                                  &error);
    lql_selector_free(selector);
    if (st != LQL_STATUS_OK) {
      fprintf(stderr, "clql: %s\n", error.message);
      return 1;
    }
    return count.matched == 0u ? 1 : 0;
  }
  if (!read_stdin(&json, &json_len)) {
    fprintf(stderr, "clql: failed to read stdin\n");
    lql_selector_free(selector);
    return 1;
  }
  st = lql_matches_json(selector, json, json_len, &matched, &error);
  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "clql: %s\n", error.message);
    free(json);
    lql_selector_free(selector);
    return 1;
  }
  if (matched) {
    fwrite(json, 1u, json_len, stdout);
    if (json_len == 0u || json[json_len - 1u] != '\n') {
      fputc('\n', stdout);
    }
  }
  free(json);
  lql_selector_free(selector);
  return matched ? 0 : 1;
}
