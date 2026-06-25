#include "lql_internal.h"

#include <stdlib.h>
#include <string.h>

void lql_error_init(lql_error *error) {
  if (error != NULL) {
    error->code = LQL_STATUS_OK;
    error->message[0] = '\0';
  }
}

void lql_set_error(lql_error *error, lql_status status, const char *message) {
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
  }
  return "unknown";
}

char *lql_strdup(const char *text) {
  size_t len;
  char *out;
  if (text == NULL) {
    return NULL;
  }
  len = strlen(text);
  out = (char *)malloc(len + 1u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, text, len + 1u);
  return out;
}

void lql_free(void *ptr) { free(ptr); }

void lql_node_cleanup(lql_node *node) {
  size_t i;
  if (node == NULL) {
    return;
  }
  free(node->term.field);
  free(node->term.value);
  for (i = 0u; i < node->term.any_count; ++i) {
    free(node->term.any[i]);
  }
  free(node->term.any);
  for (i = 0u; i < node->child_count; ++i) {
    lql_node_cleanup(&node->children[i]);
  }
  free(node->children);
  memset(node, 0, sizeof(*node));
}

lql_status lql_selector_parse(const char *expr, lql_selector **out,
                              lql_error *error) {
  return lql_parse_selector_internal(expr, 0, out, error);
}

lql_status lql_selector_parse_or(const char *expr, lql_selector **out,
                                 lql_error *error) {
  return lql_parse_selector_internal(expr, 1, out, error);
}

void lql_selector_free(lql_selector *selector) {
  if (selector != NULL) {
    lql_node_cleanup(&selector->root);
    free(selector);
  }
}

int lql_selector_is_empty(const lql_selector *selector) {
  return selector == NULL || selector->root.kind == LQL_NODE_ALL;
}

lql_status lql_matches_json(const lql_selector *selector, const char *json,
                            size_t json_len, int *out_matched,
                            lql_error *error) {
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

lql_status lql_query_file_decisions(const lql_selector *selector, FILE *file,
                                    lql_query_decision_fn on_decision,
                                    void *user, lql_query_result *out_result,
                                    lql_error *error) {
  if (file == NULL || on_decision == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "file and on_decision are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return lql_eval_query_file_decisions(selector, file, on_decision, user,
                                       out_result, error);
}
