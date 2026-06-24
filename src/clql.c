#include "lql/lql.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  fprintf(out, "usage: clql selector < data.json\n");
  fprintf(out, "       clql --version\n");
}

int main(int argc, char **argv) {
  lql_selector *selector;
  lql_error error;
  char *json;
  size_t json_len;
  int matched;
  lql_status st;

  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    printf("clql 0.0.0\n");
    return 0;
  }
  if (argc != 2 || strcmp(argv[1], "--help") == 0) {
    usage(argc == 2 ? stdout : stderr);
    return argc == 2 ? 0 : 2;
  }
  lql_error_init(&error);
  st = lql_selector_parse(argv[1], &selector, &error);
  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "clql: %s\n", error.message);
    return 2;
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
