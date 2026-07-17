#include <lql/lql.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static lql_string_view sv(const char *s) {
  lql_string_view out;
  out.data = s;
  out.len = s == NULL ? 0u : strlen(s);
  return out;
}

static void fail(const char *name, const char *message) {
  fprintf(stderr, "selector_ast: %s: %s\n", name, message);
  ++failures;
}

static const char *kind_name(lql_selector_node_kind kind) {
  switch (kind) {
  case LQL_SELECTOR_NODE_ALL:
    return "all";
  case LQL_SELECTOR_NODE_AND:
    return "and";
  case LQL_SELECTOR_NODE_OR:
    return "or";
  case LQL_SELECTOR_NODE_NOT:
    return "not";
  case LQL_SELECTOR_NODE_EQ:
    return "eq";
  case LQL_SELECTOR_NODE_CONTAINS:
    return "contains";
  case LQL_SELECTOR_NODE_ICONTAINS:
    return "icontains";
  case LQL_SELECTOR_NODE_PREFIX:
    return "prefix";
  case LQL_SELECTOR_NODE_IPREFIX:
    return "iprefix";
  case LQL_SELECTOR_NODE_RANGE:
    return "range";
  case LQL_SELECTOR_NODE_DATE:
    return "date";
  case LQL_SELECTOR_NODE_IN:
    return "in";
  case LQL_SELECTOR_NODE_EXISTS:
    return "exists";
  default:
    return "unknown";
  }
}

static int view_eq(lql_string_view got, const char *want) {
  size_t len;
  len = want == NULL ? 0u : strlen(want);
  return got.len == len && (len == 0u || memcmp(got.data, want, len) == 0);
}

static int expect_ok(const char *name, lql_status st, lql_error *error) {
  if (st == LQL_STATUS_OK) {
    return 1;
  }
  fprintf(stderr, "selector_ast: %s: unexpected status %s: %s\n", name,
          lql_status_string(st), error != NULL ? error->message : "");
  ++failures;
  return 0;
}

static int expect_fail(const char *name, lql_status st) {
  if (st != LQL_STATUS_OK) {
    return 1;
  }
  fail(name, "expected failure");
  return 0;
}

static int parse_selector(lql *ctx, const char *name, const char *expr,
                          int or_mode, lql_selector **out) {
  lql_error error;
  lql_status st;
  if (or_mode) {
    st = ctx->selector_parse_or(ctx, expr, out, &error);
  } else {
    st = ctx->selector_parse(ctx, expr, out, &error);
  }
  return expect_ok(name, st, &error);
}

static int root_kind(lql *ctx, const char *name, const lql_selector *selector,
                     lql_selector_node_kind kind, lql_selector_node *out) {
  lql_error error;
  if (!expect_ok(name, ctx->selector_root(ctx, selector, out, &error),
                 &error)) {
    return 0;
  }
  if (out->kind != kind) {
    fprintf(stderr, "selector_ast: %s: unexpected root kind: got %s want %s\n",
            name, kind_name(out->kind), kind_name(kind));
    ++failures;
    return 0;
  }
  return 1;
}

static int child_at(lql *ctx, const char *name, lql_selector_node parent,
                    size_t index, lql_selector_node_kind kind,
                    lql_selector_node *out) {
  lql_error error;
  if (!expect_ok(name,
                 ctx->selector_node_child(ctx, parent, index, out, &error),
                 &error)) {
    return 0;
  }
  if (out->kind != kind) {
    fprintf(stderr,
            "selector_ast: %s: unexpected child kind at %lu: got %s want %s\n",
            name, (unsigned long)index, kind_name(out->kind), kind_name(kind));
    ++failures;
    return 0;
  }
  return 1;
}

static int expect_child_count(lql *ctx, const char *name,
                              lql_selector_node node, size_t want) {
  lql_error error;
  size_t got;
  got = 0u;
  if (!expect_ok(name, ctx->selector_node_child_count(ctx, node, &got, &error),
                 &error)) {
    return 0;
  }
  if (got != want) {
    fail(name, "unexpected child count");
    return 0;
  }
  return 1;
}

static int expect_string_term(lql *ctx, const char *name,
                              lql_selector_node node, const char *field,
                              int value_present, const char *value,
                              int ignore_case, size_t any_count) {
  lql_error error;
  lql_selector_string_term term;
  memset(&term, 0, sizeof(term));
  if (!expect_ok(name, ctx->selector_node_string_term(ctx, node, &term, &error),
                 &error)) {
    return 0;
  }
  if (!view_eq(term.field, field) || term.value_present != value_present ||
      !view_eq(term.value, value) || term.ignore_case != ignore_case ||
      term.any_count != any_count) {
    fail(name, "unexpected string term");
    return 0;
  }
  return 1;
}

static int expect_string_any(lql *ctx, const char *name, lql_selector_node node,
                             size_t index, const char *want) {
  lql_error error;
  lql_string_view got;
  memset(&got, 0, sizeof(got));
  if (!expect_ok(
          name,
          ctx->selector_node_string_term_any(ctx, node, index, &got, &error),
          &error)) {
    return 0;
  }
  if (!view_eq(got, want)) {
    fail(name, "unexpected string any value");
    return 0;
  }
  return 1;
}

static int expect_in_any(lql *ctx, const char *name, lql_selector_node node,
                         size_t index, const char *want) {
  lql_error error;
  lql_string_view got;
  memset(&got, 0, sizeof(got));
  if (!expect_ok(name,
                 ctx->selector_node_in_term_any(ctx, node, index, &got, &error),
                 &error)) {
    return 0;
  }
  if (!view_eq(got, want)) {
    fail(name, "unexpected in any value");
    return 0;
  }
  return 1;
}

static char *selector_json(lql *ctx, const char *name,
                           const lql_selector *selector) {
  lql_error error;
  FILE *file;
  long size;
  char *buf;
  size_t read_len;
  file = tmpfile();
  if (file == NULL) {
    fail(name, "tmpfile failed");
    return NULL;
  }
  if (!expect_ok(name, ctx->selector_write_json(ctx, selector, file, &error),
                 &error)) {
    fclose(file);
    return NULL;
  }
  if (fflush(file) != 0 || fseek(file, 0L, SEEK_END) != 0) {
    fclose(file);
    fail(name, "unable to measure JSON output");
    return NULL;
  }
  size = ftell(file);
  if (size < 0 || fseek(file, 0L, SEEK_SET) != 0) {
    fclose(file);
    fail(name, "unable to rewind JSON output");
    return NULL;
  }
  buf = (char *)malloc((size_t)size + 1u);
  if (buf == NULL) {
    fclose(file);
    fail(name, "out of memory");
    return NULL;
  }
  read_len = fread(buf, 1u, (size_t)size, file);
  fclose(file);
  if (read_len != (size_t)size) {
    free(buf);
    fail(name, "unable to read JSON output");
    return NULL;
  }
  buf[(size_t)size] = '\0';
  return buf;
}

static int contains_text(const char *haystack, const char *needle) {
  return haystack != NULL && strstr(haystack, needle) != NULL;
}

static void test_empty_and_wrappers(lql *ctx) {
  lql_selector *selector;
  lql_selector_node root;
  lql_selector_node child;
  lql_selector_node nested;
  lql_selector_node inner;
  selector = NULL;
  if (parse_selector(ctx, "empty_alias", "{}", 0, &selector)) {
    if (!ctx->selector_is_empty(ctx, selector)) {
      fail("empty_alias", "expected empty selector");
    }
    root_kind(ctx, "empty_alias", selector, LQL_SELECTOR_NODE_ALL, &root);
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "implicit_and",
                     "eq{f=/status,v=open},prefix{f=/id,v=req-}", 0,
                     &selector)) {
    if (root_kind(ctx, "implicit_and", selector, LQL_SELECTOR_NODE_AND,
                  &root) &&
        expect_child_count(ctx, "implicit_and", root, 2u)) {
      child_at(ctx, "implicit_and", root, 0u, LQL_SELECTOR_NODE_EQ, &child);
      child_at(ctx, "implicit_and", root, 1u, LQL_SELECTOR_NODE_PREFIX, &child);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "implicit_or",
                     "eq{f=/status,v=open},prefix{f=/id,v=req-}", 1,
                     &selector)) {
    if (root_kind(ctx, "implicit_or", selector, LQL_SELECTOR_NODE_OR, &root)) {
      expect_child_count(ctx, "implicit_or", root, 2u);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "not_wrapper", "not.eq{f=/status,v=open}", 0,
                     &selector)) {
    if (root_kind(ctx, "not_wrapper", selector, LQL_SELECTOR_NODE_NOT, &root)) {
      child_at(ctx, "not_wrapper", root, 0u, LQL_SELECTOR_NODE_EQ, &child);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "whitespace_assignments",
                     "and.eq{\nfield=\"/hello\"\nvalue=\"hi, world\"},"
                     "and.eq{field=/status value=\"okili dokili\"}",
                     0, &selector)) {
    if (root_kind(ctx, "whitespace_assignments", selector,
                  LQL_SELECTOR_NODE_AND, &root)) {
      expect_child_count(ctx, "whitespace_assignments", root, 2u);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "and_with_or_group",
                     "/field=\"value\",/status=\"ok\","
                     "or.eq{field=/msg,value=ok},"
                     "or.1.eq{field=/msg,value=done}",
                     0, &selector)) {
    if (root_kind(ctx, "and_with_or_group", selector, LQL_SELECTOR_NODE_AND,
                  &root) &&
        expect_child_count(ctx, "and_with_or_group", root, 3u)) {
      child_at(ctx, "and_with_or_group", root, 2u, LQL_SELECTOR_NODE_OR,
               &child);
      expect_child_count(ctx, "and_with_or_group", child, 2u);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "deep_indexed_merge",
                     "and.0.or.0.and.0.eq{field=/status,value=open},"
                     "and.0.or.0.and.0.range{field=/progress,gte=10}",
                     0, &selector)) {
    if (root_kind(ctx, "deep_indexed_merge", selector, LQL_SELECTOR_NODE_AND,
                  &root) &&
        child_at(ctx, "deep_indexed_merge", root, 0u, LQL_SELECTOR_NODE_OR,
                 &child) &&
        child_at(ctx, "deep_indexed_merge", child, 0u, LQL_SELECTOR_NODE_AND,
                 &nested)) {
      expect_child_count(ctx, "deep_indexed_merge", nested, 2u);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "deep_indexed_or_and_or_merge",
                     "or.0.and.0.or.0.eq{field=/status,value=open},"
                     "or.0.and.0.or.0.exists{/meta/etag}",
                     0, &selector)) {
    if (root_kind(ctx, "deep_indexed_or_and_or_merge", selector,
                  LQL_SELECTOR_NODE_OR, &root) &&
        child_at(ctx, "deep_indexed_or_and_or_merge", root, 0u,
                 LQL_SELECTOR_NODE_AND, &child) &&
        child_at(ctx, "deep_indexed_or_and_or_merge", child, 0u,
                 LQL_SELECTOR_NODE_OR, &nested) &&
        child_at(ctx, "deep_indexed_or_and_or_merge", nested, 0u,
                 LQL_SELECTOR_NODE_AND, &inner)) {
      expect_child_count(ctx, "deep_indexed_or_and_or_merge", inner, 2u);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "deep_indexed_and_or_not_in",
                     "and.0.or.0.not.in{field=/env,any=prod|stage}", 0,
                     &selector)) {
    if (root_kind(ctx, "deep_indexed_and_or_not_in", selector,
                  LQL_SELECTOR_NODE_AND, &root) &&
        child_at(ctx, "deep_indexed_and_or_not_in", root, 0u,
                 LQL_SELECTOR_NODE_OR, &child) &&
        child_at(ctx, "deep_indexed_and_or_not_in", child, 0u,
                 LQL_SELECTOR_NODE_NOT, &nested)) {
      child_at(ctx, "deep_indexed_and_or_not_in", nested, 0u,
               LQL_SELECTOR_NODE_IN, &inner);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "nested_or_and_not_exists",
                     "or.0.and.0.not.exists{/meta/etag}", 0, &selector)) {
    if (root_kind(ctx, "nested_or_and_not_exists", selector,
                  LQL_SELECTOR_NODE_OR, &root) &&
        child_at(ctx, "nested_or_and_not_exists", root, 0u,
                 LQL_SELECTOR_NODE_AND, &child) &&
        child_at(ctx, "nested_or_and_not_exists", child, 0u,
                 LQL_SELECTOR_NODE_NOT, &nested)) {
      child_at(ctx, "nested_or_and_not_exists", nested, 0u,
               LQL_SELECTOR_NODE_EXISTS, &inner);
    }
    ctx->selector_destroy(ctx, selector);
  }
}

static void test_string_terms(lql *ctx) {
  lql_selector *selector;
  lql_selector_node root;
  char *json;
  selector = NULL;
  if (parse_selector(ctx, "eq_value", "eq{field=/status,value=open}", 0,
                     &selector)) {
    if (root_kind(ctx, "eq_value", selector, LQL_SELECTOR_NODE_EQ, &root)) {
      expect_string_term(ctx, "eq_value", root, "/status", 1, "open", 0, 0u);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "contains_any_ignore",
                     "contains{f=/msg,a=timeout|error,ignoreCase=t}", 0,
                     &selector)) {
    if (root_kind(ctx, "contains_any_ignore", selector,
                  LQL_SELECTOR_NODE_CONTAINS, &root) &&
        expect_string_term(ctx, "contains_any_ignore", root, "/msg", 0, "", 1,
                           2u)) {
      expect_string_any(ctx, "contains_any_ignore", root, 0u, "timeout");
      expect_string_any(ctx, "contains_any_ignore", root, 1u, "error");
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "icontains_omitted", "icontains{f=/msg}", 0,
                     &selector)) {
    if (root_kind(ctx, "icontains_omitted", selector,
                  LQL_SELECTOR_NODE_ICONTAINS, &root)) {
      expect_string_term(ctx, "icontains_omitted", root, "/msg", 0, "", 0, 0u);
    }
    json = selector_json(ctx, "icontains_omitted", selector);
    if (json != NULL) {
      if (contains_text(json, "\"value\"")) {
        fail("icontains_omitted", "omitted value must not serialize");
      }
      free(json);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "contains_unquoted_empty", "contains{f=/msg,value=}",
                     0, &selector)) {
    if (root_kind(ctx, "contains_unquoted_empty", selector,
                  LQL_SELECTOR_NODE_CONTAINS, &root)) {
      expect_string_term(ctx, "contains_unquoted_empty", root, "/msg", 0, "", 0,
                         0u);
    }
    json = selector_json(ctx, "contains_unquoted_empty", selector);
    if (json != NULL) {
      if (contains_text(json, "\"value\"")) {
        fail("contains_unquoted_empty", "unquoted empty value must omit value");
      }
      free(json);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "iprefix_empty", "iprefix{f=/msg,v=\"\"}", 0,
                     &selector)) {
    if (root_kind(ctx, "iprefix_empty", selector, LQL_SELECTOR_NODE_IPREFIX,
                  &root)) {
      expect_string_term(ctx, "iprefix_empty", root, "/msg", 1, "", 0, 0u);
    }
    json = selector_json(ctx, "iprefix_empty", selector);
    if (json != NULL) {
      if (!contains_text(json, "\"value\":\"\"")) {
        fail("iprefix_empty", "explicit empty value must serialize");
      }
      free(json);
    }
    ctx->selector_destroy(ctx, selector);
  }
}

static void test_shorthand_and_capabilities(lql *ctx) {
  lql_selector *selector;
  lql_selector_node root;
  lql_selector_node child;
  lql_selector_range_term range;
  lql_selector_capabilities caps;
  lql_error error;

  selector = NULL;
  if (parse_selector(ctx, "shorthand_eq", "/status=\"open\"", 0, &selector)) {
    if (root_kind(ctx, "shorthand_eq", selector, LQL_SELECTOR_NODE_EQ, &root)) {
      expect_string_term(ctx, "shorthand_eq", root, "/status", 1, "open", 0,
                         0u);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "shorthand_single_quoted_scalar", "/enabled='true'",
                     0, &selector)) {
    if (root_kind(ctx, "shorthand_single_quoted_scalar", selector,
                  LQL_SELECTOR_NODE_EQ, &root)) {
      expect_string_term(ctx, "shorthand_single_quoted_scalar", root,
                         "/enabled", 1, "true", 0, 0u);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "shorthand_not_eq", "/status!=closed", 0,
                     &selector)) {
    if (root_kind(ctx, "shorthand_not_eq", selector, LQL_SELECTOR_NODE_NOT,
                  &root)) {
      child_at(ctx, "shorthand_not_eq", root, 0u, LQL_SELECTOR_NODE_EQ, &child);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "shorthand_range", "/count>=42", 0, &selector)) {
    if (root_kind(ctx, "shorthand_range", selector, LQL_SELECTOR_NODE_RANGE,
                  &root) &&
        expect_ok("shorthand_range",
                  ctx->selector_node_range_term(ctx, root, &range, &error),
                  &error)) {
      if (!view_eq(range.field, "/count") ||
          range.gte.kind != LQL_SELECTOR_BOUND_NUMBER ||
          fabs(range.gte.number - 42.0) > 0.000001) {
        fail("shorthand_range", "unexpected shorthand range");
      }
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "shorthand_eq_spacing", " \n/status = \"open\" \n", 0,
                     &selector)) {
    if (root_kind(ctx, "shorthand_eq_spacing", selector, LQL_SELECTOR_NODE_EQ,
                  &root)) {
      expect_string_term(ctx, "shorthand_eq_spacing", root, "/status", 1,
                         "open", 0, 0u);
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "shorthand_range_spacing", "\n/progress   >=   42\n",
                     0, &selector)) {
    if (root_kind(ctx, "shorthand_range_spacing", selector,
                  LQL_SELECTOR_NODE_RANGE, &root) &&
        expect_ok("shorthand_range_spacing",
                  ctx->selector_node_range_term(ctx, root, &range, &error),
                  &error)) {
      if (!view_eq(range.field, "/progress") ||
          range.gte.kind != LQL_SELECTOR_BOUND_NUMBER ||
          fabs(range.gte.number - 42.0) > 0.000001) {
        fail("shorthand_range_spacing", "unexpected shorthand range");
      }
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "capabilities",
                     "and.eq{field=/status,value=open},"
                     "and.range{field=/progress,gte=5},"
                     "or.in{field=/env,any=prod|stage},"
                     "not.eq{field=/state,value=disabled},"
                     "exists{/meta/.../etag},"
                     "icontains{field=/items[]/msg,value=timeout},"
                     "iprefix{field=/groups/**/service,value=auth}",
                     0, &selector)) {
    memset(&caps, 0, sizeof(caps));
    ctx->selector_capabilities_get(ctx, selector, &caps);
    if (!caps.and_ || !caps.or_ || !caps.not_ || !caps.eq || !caps.range ||
        !caps.in || !caps.contains || !caps.prefix || !caps.exists ||
        !caps.wildcard_path || !caps.recursive_path) {
      fail("capabilities", "missing capability flag");
    }
    ctx->selector_destroy(ctx, selector);
  }
}

static void test_range_date_in_exists(lql *ctx) {
  lql_selector *selector;
  lql_selector_node root;
  lql_selector_range_term range;
  lql_selector_date_term date;
  lql_selector_in_term in_term;
  lql_string_view path;
  lql_error error;

  selector = NULL;
  if (parse_selector(ctx, "range_numeric", "range{f=/progress,gte=10,lt=20}", 0,
                     &selector)) {
    if (root_kind(ctx, "range_numeric", selector, LQL_SELECTOR_NODE_RANGE,
                  &root) &&
        expect_ok("range_numeric",
                  ctx->selector_node_range_term(ctx, root, &range, &error),
                  &error)) {
      if (!view_eq(range.field, "/progress") ||
          range.gte.kind != LQL_SELECTOR_BOUND_NUMBER ||
          range.lt.kind != LQL_SELECTOR_BOUND_NUMBER ||
          fabs(range.gte.number - 10.0) > 0.000001 ||
          fabs(range.lt.number - 20.0) > 0.000001) {
        fail("range_numeric", "unexpected numeric range");
      }
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "range_datetime",
                     "range{f=/timestamp,gte=\" 2026-03-05T10:28:21Z "
                     "\",lt=\"2026-03-05T10:30:00Z\"}",
                     0, &selector)) {
    if (root_kind(ctx, "range_datetime", selector, LQL_SELECTOR_NODE_RANGE,
                  &root) &&
        expect_ok("range_datetime",
                  ctx->selector_node_range_term(ctx, root, &range, &error),
                  &error)) {
      if (range.gte.kind != LQL_SELECTOR_BOUND_DATETIME ||
          !view_eq(range.gte.datetime, "2026-03-05T10:28:21Z") ||
          range.lt.kind != LQL_SELECTOR_BOUND_DATETIME ||
          !view_eq(range.lt.datetime, "2026-03-05T10:30:00Z")) {
        fail("range_datetime", "unexpected datetime range");
      }
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "date_since", "date{f=/timestamp,since=yesterday}", 0,
                     &selector)) {
    if (root_kind(ctx, "date_since", selector, LQL_SELECTOR_NODE_DATE, &root) &&
        expect_ok("date_since",
                  ctx->selector_node_date_term(ctx, root, &date, &error),
                  &error)) {
      if (!view_eq(date.field, "/timestamp") ||
          !view_eq(date.since, "yesterday") ||
          date.since_kind != LQL_SELECTOR_SINCE_YESTERDAY) {
        fail("date_since", "unexpected date since term");
      }
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "date_after_before_aliases",
                     "date{f=/timestamp,a=2025-01-01,b=2025-01-03}", 0,
                     &selector)) {
    if (root_kind(ctx, "date_after_before_aliases", selector,
                  LQL_SELECTOR_NODE_DATE, &root) &&
        expect_ok("date_after_before_aliases",
                  ctx->selector_node_date_term(ctx, root, &date, &error),
                  &error)) {
      if (!view_eq(date.field, "/timestamp") ||
          !view_eq(date.after, "2025-01-01") ||
          !view_eq(date.before, "2025-01-03")) {
        fail("date_after_before_aliases", "unexpected date window");
      }
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "in_any", "in{f=/env,a=prod|stage|dev}", 0,
                     &selector)) {
    if (root_kind(ctx, "in_any", selector, LQL_SELECTOR_NODE_IN, &root) &&
        expect_ok("in_any",
                  ctx->selector_node_in_term(ctx, root, &in_term, &error),
                  &error)) {
      if (!view_eq(in_term.field, "/env") || in_term.any_count != 3u) {
        fail("in_any", "unexpected in term");
      }
      expect_in_any(ctx, "in_any", root, 0u, "prod");
      expect_in_any(ctx, "in_any", root, 1u, "stage");
      expect_in_any(ctx, "in_any", root, 2u, "dev");
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "in_any_phrases",
                     "in{field=/greeting,any=\"hello world|goodbye jupiter\"}",
                     0, &selector)) {
    if (root_kind(ctx, "in_any_phrases", selector, LQL_SELECTOR_NODE_IN,
                  &root) &&
        expect_ok("in_any_phrases",
                  ctx->selector_node_in_term(ctx, root, &in_term, &error),
                  &error)) {
      if (!view_eq(in_term.field, "/greeting") || in_term.any_count != 2u) {
        fail("in_any_phrases", "unexpected in phrase term");
      }
      expect_in_any(ctx, "in_any_phrases", root, 0u, "hello world");
      expect_in_any(ctx, "in_any_phrases", root, 1u, "goodbye jupiter");
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (parse_selector(ctx, "exists_recursive", "exists{/meta/.../etag}", 0,
                     &selector)) {
    if (root_kind(ctx, "exists_recursive", selector, LQL_SELECTOR_NODE_EXISTS,
                  &root) &&
        expect_ok("exists_recursive",
                  ctx->selector_node_exists_path(ctx, root, &path, &error),
                  &error)) {
      if (!view_eq(path, "/meta/.../etag")) {
        fail("exists_recursive", "unexpected exists path");
      }
    }
    ctx->selector_destroy(ctx, selector);
  }
}

static void test_json_import_export(lql *ctx) {
  static const char json[] =
      "{\"and\":[{\"eq\":{\"field\":\"/status\",\"value\":\"open\"}},"
      "{\"or\":[{\"exists\":\"/meta/etag\"},"
      "{\"prefix\":{\"field\":\"/id\",\"value\":\"req-\"}}]}]}";
  lql_selector *selector;
  lql_selector *roundtrip;
  lql_selector_node root;
  lql_selector_node child;
  lql_error error;
  char *out;
  selector = NULL;
  if (!expect_ok(
          "json_import",
          ctx->selector_parse_json(ctx, json, strlen(json), &selector, &error),
          &error)) {
    return;
  }
  if (root_kind(ctx, "json_import", selector, LQL_SELECTOR_NODE_AND, &root) &&
      expect_child_count(ctx, "json_import", root, 2u)) {
    child_at(ctx, "json_import", root, 0u, LQL_SELECTOR_NODE_EQ, &child);
    child_at(ctx, "json_import", root, 1u, LQL_SELECTOR_NODE_OR, &child);
  }
  out = selector_json(ctx, "json_export", selector);
  if (out != NULL) {
    roundtrip = NULL;
    expect_ok(
        "json_roundtrip",
        ctx->selector_parse_json(ctx, out, strlen(out), &roundtrip, &error),
        &error);
    ctx->selector_destroy(ctx, roundtrip);
    free(out);
  }
  ctx->selector_destroy(ctx, selector);

  selector = NULL;
  if (expect_ok(
          "json_string_any_scalar",
          ctx->selector_parse_json(
              ctx,
              "{\"contains\":{\"field\":\"/msg\",\"any\":\"timeout|error\"}}",
              strlen("{\"contains\":{\"field\":\"/"
                     "msg\",\"any\":\"timeout|error\"}}"),
              &selector, &error),
          &error)) {
    if (root_kind(ctx, "json_string_any_scalar", selector,
                  LQL_SELECTOR_NODE_CONTAINS, &root)) {
      expect_string_term(ctx, "json_string_any_scalar", root, "/msg", 0, "", 0,
                         2u);
      expect_string_any(ctx, "json_string_any_scalar", root, 0u, "timeout");
      expect_string_any(ctx, "json_string_any_scalar", root, 1u, "error");
    }
    ctx->selector_destroy(ctx, selector);
  }

  selector = NULL;
  if (expect_ok(
          "json_string_any_array",
          ctx->selector_parse_json(
              ctx,
              "{\"icontains\":{\"field\":\"/msg\",\"any\":[\"warn\",42,true]}}",
              strlen("{\"icontains\":{\"field\":\"/"
                     "msg\",\"any\":[\"warn\",42,true]}}"),
              &selector, &error),
          &error)) {
    if (root_kind(ctx, "json_string_any_array", selector,
                  LQL_SELECTOR_NODE_ICONTAINS, &root)) {
      expect_string_term(ctx, "json_string_any_array", root, "/msg", 0, "", 0,
                         3u);
      expect_string_any(ctx, "json_string_any_array", root, 0u, "warn");
      expect_string_any(ctx, "json_string_any_array", root, 1u, "42");
      expect_string_any(ctx, "json_string_any_array", root, 2u, "true");
    }
    ctx->selector_destroy(ctx, selector);
  }
}

static void test_builders(lql *ctx) {
  lql_error error;
  lql_selector *eq;
  lql_selector *exists;
  lql_selector *children[2];
  lql_selector *compound;
  lql_selector_node root;
  lql_selector_string_term term;
  lql_selector_range_term range;
  lql_selector_date_term date;
  lql_selector_in_term in_term;
  lql_string_view any_values[2];
  char *json;
  memset(&term, 0, sizeof(term));
  term.field = sv("/status");
  term.value_present = 1;
  term.value = sv("open");
  eq = NULL;
  if (!expect_ok("builder_eq",
                 ctx->selector_build_string(ctx, LQL_SELECTOR_NODE_EQ, &term,
                                            NULL, &eq, &error),
                 &error)) {
    return;
  }
  exists = NULL;
  expect_ok("builder_exists",
            ctx->selector_build_exists(ctx, sv("/meta/etag"), &exists, &error),
            &error);
  children[0] = eq;
  children[1] = exists;
  compound = NULL;
  if (expect_ok(
          "builder_and",
          ctx->selector_build_compound(ctx, LQL_SELECTOR_NODE_AND,
                                       (const lql_selector *const *)children,
                                       2u, &compound, &error),
          &error)) {
    if (root_kind(ctx, "builder_and", compound, LQL_SELECTOR_NODE_AND, &root)) {
      expect_child_count(ctx, "builder_and", root, 2u);
    }
    json = selector_json(ctx, "builder_and_json", compound);
    if (json != NULL) {
      if (!contains_text(json, "\"and\"")) {
        fail("builder_and_json", "expected and JSON");
      }
      free(json);
    }
    ctx->selector_destroy(ctx, compound);
  }
  ctx->selector_destroy(ctx, exists);
  ctx->selector_destroy(ctx, eq);

  memset(&term, 0, sizeof(term));
  term.field = sv("/msg");
  term.any_count = 2u;
  any_values[0] = sv("timeout");
  any_values[1] = sv("error");
  eq = NULL;
  if (expect_ok("builder_icontains_any",
                ctx->selector_build_string(ctx, LQL_SELECTOR_NODE_ICONTAINS,
                                           &term, any_values, &eq, &error),
                &error)) {
    if (root_kind(ctx, "builder_icontains_any", eq, LQL_SELECTOR_NODE_ICONTAINS,
                  &root)) {
      expect_string_term(ctx, "builder_icontains_any", root, "/msg", 0, "", 0,
                         2u);
      expect_string_any(ctx, "builder_icontains_any", root, 0u, "timeout");
    }
    ctx->selector_destroy(ctx, eq);
  }

  memset(&range, 0, sizeof(range));
  range.field = sv("/timestamp");
  range.gte.kind = LQL_SELECTOR_BOUND_DATETIME;
  range.gte.datetime = sv(" 2026-03-05T10:28:21Z ");
  range.lt.kind = LQL_SELECTOR_BOUND_DATETIME;
  range.lt.datetime = sv("2026-03-05T10:30:00Z");
  eq = NULL;
  if (expect_ok("builder_range_datetime",
                ctx->selector_build_range(ctx, &range, &eq, &error), &error)) {
    if (root_kind(ctx, "builder_range_datetime", eq, LQL_SELECTOR_NODE_RANGE,
                  &root) &&
        expect_ok("builder_range_datetime",
                  ctx->selector_node_range_term(ctx, root, &range, &error),
                  &error)) {
      if (!view_eq(range.gte.datetime, "2026-03-05T10:28:21Z") ||
          !view_eq(range.lt.datetime, "2026-03-05T10:30:00Z")) {
        fail("builder_range_datetime", "unexpected datetime bounds");
      }
    }
    ctx->selector_destroy(ctx, eq);
  }

  memset(&date, 0, sizeof(date));
  date.field = sv("/timestamp");
  date.since_kind = LQL_SELECTOR_SINCE_TODAY;
  eq = NULL;
  if (expect_ok("builder_date_since_kind",
                ctx->selector_build_date(ctx, &date, &eq, &error), &error)) {
    if (root_kind(ctx, "builder_date_since_kind", eq, LQL_SELECTOR_NODE_DATE,
                  &root) &&
        expect_ok("builder_date_since_kind",
                  ctx->selector_node_date_term(ctx, root, &date, &error),
                  &error)) {
      if (!view_eq(date.since, "today") ||
          date.since_kind != LQL_SELECTOR_SINCE_TODAY) {
        fail("builder_date_since_kind", "unexpected date since");
      }
    }
    ctx->selector_destroy(ctx, eq);
  }

  memset(&in_term, 0, sizeof(in_term));
  in_term.field = sv("/env");
  in_term.any_count = 2u;
  any_values[0] = sv("prod");
  any_values[1] = sv("stage");
  eq = NULL;
  if (expect_ok("builder_in",
                ctx->selector_build_in(ctx, &in_term, any_values, &eq, &error),
                &error)) {
    if (root_kind(ctx, "builder_in", eq, LQL_SELECTOR_NODE_IN, &root)) {
      expect_in_any(ctx, "builder_in", root, 0u, "prod");
      expect_in_any(ctx, "builder_in", root, 1u, "stage");
    }
    ctx->selector_destroy(ctx, eq);
  }
}

static void test_invalid_cases(lql *ctx) {
  lql_error error;
  lql_selector *selector;
  static const char *bad_exprs[] = {
      "contains{field=/msg,value=timeout,any=error}",
      "icontains{field=/msg,value=timeout,any=error}",
      "contains{field=/msg,any=}",
      "contains{field=/msg,any=||}",
      "in{field=/env,any=}",
      "in{field=/env,any= prod | stage }",
      "in{any=prod|stage}",
      "in{field=/env}",
      "prefix{field=/msg,any=foo|bar}",
      "iprefix{field=/msg,any=foo|bar}",
      "contains{field=/msg,value=timeout,ignoreCase=maybe}",
      "eq{field=/status,value=open,foo=bar}",
      "range{field=/progress}",
      "range{gte=10}",
      "range{field=/progress,gte=10,lt=2025-01-01}",
      "range{field=/timestamp,gte=yesterday}",
      "date{field=/timestamp,since=yesterday,after=2025-01-01}",
      "date{field=/timestamp,after=2025-01-01,gt=2025-01-02}",
      "date{field=/timestamp,before=2025-01-03,lt=2025-01-02}",
      "date{after=2025-01-01}",
      "date{field=/timestamp,since=tomorrowish}",
      "exists{}",
      "exists{/meta/etag,field=/status}",
      "exists{/meta/etag,/meta/id}",
      "exists{field=/meta/etag}",
      "and.0.or.0.and.0.eq{field=/status,value=open},"
      "and.0.or.0.and.0.eq{field=/status,value=closed}",
      "or.0.and.0.or.0.exists{/meta/etag},"
      "or.0.and.0.or.0.exists{/meta/id}",
      "and.0.or.0.not.in{field=/env,any=prod|stage},"
      "and.0.or.0.not.in{field=/env,any=dev}",
      "and..eq{field=/status,value=open}",
      "or.0.and.foo.exists{/meta/etag}",
      "nonsense",
      "eq{field=/status,value=\"open}",
      "eq{field=/status,value=open}}",
      "/count>="};
  size_t i;
  for (i = 0u; i < sizeof(bad_exprs) / sizeof(bad_exprs[0]); ++i) {
    selector = NULL;
    if (expect_fail("invalid_expr", ctx->selector_parse(ctx, bad_exprs[i],
                                                        &selector, &error))) {
      ctx->selector_destroy(ctx, selector);
    }
  }
}

int main(void) {
  lql *ctx;
  lql_error error;
  ctx = NULL;
  if (!expect_ok("lql_new", lql_new(&ctx, &error), &error)) {
    return 1;
  }
  test_empty_and_wrappers(ctx);
  test_string_terms(ctx);
  test_shorthand_and_capabilities(ctx);
  test_range_date_in_exists(ctx);
  test_json_import_export(ctx);
  test_builders(ctx);
  test_invalid_cases(ctx);
  ctx->destroy(ctx);
  if (failures != 0) {
    fprintf(stderr, "selector_ast: %d failure(s)\n", failures);
    return 1;
  }
  return 0;
}
