#define _POSIX_C_SOURCE 200809L
#include <lql/lql.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct smoke {
  lql *client;
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  const char *input;
  size_t offset;
  char output[256];
  size_t written;
} smoke;

static const char input[] =
    "{\"kind\":\"commit\",\"n\":1.5}\n{\"kind\":\"other\",\"n\":2}\n";

static lql_status read_input(void *user, unsigned char *buffer, size_t capacity,
                             size_t *out_len, lql_error *error) {
  smoke *s;
  size_t amount;
  (void)error;
  s = (smoke *)user;
  amount = strlen(s->input) - s->offset;
  if (amount > capacity)
    amount = capacity;
  if (amount > 7u)
    amount = 7u;
  memcpy(buffer, s->input + s->offset, amount);
  s->offset += amount;
  *out_len = amount;
  return LQL_STATUS_OK;
}

static lql_status write_output(void *user, const void *data, size_t len,
                               lql_error *error) {
  smoke *s;
  (void)error;
  s = (smoke *)user;
  if (len > sizeof(s->output) - s->written)
    return LQL_STATUS_CALLBACK_ERROR;
  memcpy(s->output + s->written, data, len);
  s->written += len;
  return LQL_STATUS_OK;
}

static int apply(smoke *s, const char *text, lql_stream_output_mode mode,
                 lql_status expected_status, const char *expected) {
  lql_status status;
  s->input = text;
  s->offset = s->written = 0u;
  memset(&s->request, 0, sizeof(s->request));
  s->request.reader = read_input;
  s->request.reader_user = s;
  s->request.writer = write_output;
  s->request.writer_user = s;
  s->request.selector = s->selector;
  s->request.projection =
      mode == LQL_STREAM_OUTPUT_PROJECTION ? s->projection : NULL;
  s->request.mutation = mode == LQL_STREAM_OUTPUT_MUTATION ? s->mutation : NULL;
  s->request.output_mode = mode;
  s->request.matched_only = 1;
  status = mode == LQL_STREAM_OUTPUT_DECISION_ONLY
               ? s->client->stream_apply(s->client, &s->request, &s->result,
                                         &s->error)
               : s->client->stream_apply_spooled(s->client, &s->request,
                                                 &s->result, &s->error);
  if (status != expected_status || s->written != strlen(expected) ||
      memcmp(s->output, expected, s->written) != 0 ||
      (status == LQL_STATUS_OK &&
       (s->result.records_seen != 2u || s->result.records_matched != 1u)) ||
      (status != LQL_STATUS_OK && s->error.code != status)) {
    fprintf(stderr, "SDK smoke mode=%d status=%d expected=%d: %s\n", (int)mode,
            (int)status, (int)expected_status, s->error.message);
    return 1;
  }
  return 0;
}

static int exercise(smoke *s) {
  static const char *const paths[] = {"/n"};
  static const char *const mutations[] = {"/kind=ready"};
  if (lql_new(&s->client, &s->error) != LQL_STATUS_OK ||
      strcmp(s->client->version(s->client), LQL_VERSION) != 0 ||
      s->client->selector_parse(s->client, "/kind=\"commit\"", &s->selector,
                                &s->error) != LQL_STATUS_OK ||
      s->client->projection_parse(s->client, paths, 1u, &s->projection,
                                  &s->error) != LQL_STATUS_OK ||
      s->client->mutation_parse(s->client, mutations, 1u, &s->mutation,
                                &s->error) != LQL_STATUS_OK)
    return 1;
  if (apply(s, input, LQL_STREAM_OUTPUT_DECISION_ONLY, LQL_STATUS_OK, "") ||
      apply(s, input, LQL_STREAM_OUTPUT_SELECTED_RECORD, LQL_STATUS_OK,
            "{\"kind\":\"commit\",\"n\":1.5}\n") ||
      apply(s, input, LQL_STREAM_OUTPUT_PROJECTION, LQL_STATUS_OK,
            "{\"n\":1.5}\n") ||
      apply(s, input, LQL_STREAM_OUTPUT_MUTATION, LQL_STATUS_OK,
            "{\"kind\":\"ready\",\"n\":1.5}\n"))
    return 1;
  s->client->selector_destroy(s->client, s->selector);
  s->selector = NULL;
  if (s->client->selector_parse(s->client, "/n=1.5", &s->selector, &s->error) !=
      LQL_STATUS_OK)
    return 1;
  return apply(s, "{\"kind\":}\n", LQL_STREAM_OUTPUT_DECISION_ONLY,
               LQL_STATUS_JSON_ERROR, "") ||
         apply(s, input, LQL_STREAM_OUTPUT_DECISION_ONLY, LQL_STATUS_OK, "");
}

static void *worker(void *user) {
  smoke *s;
  int *failed;
  failed = (int *)user;
  s = (smoke *)calloc(1u, sizeof(*s));
  *failed = 1;
  if (s == NULL)
    return NULL;
  *failed = exercise(s);
  if (*failed)
    fprintf(stderr, "SDK smoke: %s\n", s->error.message);
  if (s->client != NULL) {
    s->client->selector_destroy(s->client, s->selector);
    s->client->projection_destroy(s->client, s->projection);
    s->client->mutation_destroy(s->client, s->mutation);
    s->client->destroy(s->client);
  }
  free(s);
  return NULL;
}

int main(void) {
  pthread_attr_t attributes;
  pthread_t thread;
  int small;
  int failed;
  int status;
  long minimum;
  size_t stack_size;
  status = pthread_attr_init(&attributes);
  if (status != 0) {
    fprintf(stderr, "pthread_attr_init: %s\n", strerror(status));
    return 1;
  }
  minimum = sysconf(_SC_THREAD_STACK_MIN);
  if (minimum <= 0) {
    fprintf(stderr, "unable to query minimum thread stack\n");
    return 1;
  }
  stack_size = 64u * 1024u;
  if ((unsigned long)minimum > (unsigned long)stack_size)
    stack_size = (size_t)minimum;
  printf("SDK worker stacks: default and %lu bytes (platform minimum %ld)\n",
         (unsigned long)stack_size, minimum);
  status = pthread_attr_setstacksize(&attributes, stack_size);
  if (status != 0) {
    fprintf(stderr, "pthread_attr_setstacksize: %s\n", strerror(status));
    return 1;
  }
  for (small = 0; small < 2; ++small) {
    failed = 1;
    status =
        pthread_create(&thread, small ? &attributes : NULL, worker, &failed);
    if (status != 0) {
      fprintf(stderr, "pthread_create: %s\n", strerror(status));
      return 1;
    }
    status = pthread_join(thread, NULL);
    if (status != 0 || failed) {
      fprintf(stderr, "pthread_join: %s; worker failed=%d\n", strerror(status),
              failed);
      return 1;
    }
  }
  pthread_attr_destroy(&attributes);
  return 0;
}
