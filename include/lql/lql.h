#ifndef LQL_LQL_H
#define LQL_LQL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum lql_status {
  LQL_STATUS_OK = 0,
  LQL_STATUS_INVALID_ARGUMENT = 1,
  LQL_STATUS_NO_MEMORY = 2,
  LQL_STATUS_PARSE_ERROR = 3,
  LQL_STATUS_JSON_ERROR = 4,
  LQL_STATUS_UNSUPPORTED = 5
} lql_status;

typedef struct lql_error {
  lql_status code;
  char message[256];
} lql_error;

typedef struct lql_selector lql_selector;

void lql_error_init(lql_error *error);
const char *lql_status_string(lql_status status);

lql_status lql_selector_parse(const char *expr, lql_selector **out,
                              lql_error *error);
lql_status lql_selector_parse_or(const char *expr, lql_selector **out,
                                 lql_error *error);
void lql_selector_free(lql_selector *selector);
int lql_selector_is_empty(const lql_selector *selector);

lql_status lql_matches_json(const lql_selector *selector, const char *json,
                            size_t json_len, int *out_matched,
                            lql_error *error);

char *lql_strdup(const char *text);
void lql_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
