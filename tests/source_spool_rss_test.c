#include "lql_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

#define LARGE_CANDIDATE_COUNT 3u
#define LARGE_CANDIDATE_BLOB_BYTES (1024u * 1024u)
#define LARGE_CANDIDATE_BLOB_PART_BYTES 65536u
#define LARGE_CANDIDATE_RSS_LIMIT_BYTES (8u * 1024u * 1024u)

typedef struct file_reader {
  FILE *file;
} file_reader;

typedef struct spool_counts {
  lql_uint64 matches;
  lql_uint64 spilled;
} spool_counts;

static int write_all(FILE *file, const void *data, size_t len) {
  return fwrite(data, 1u, len, file) == len;
}

static int write_large_candidate(FILE *file) {
  static char blob[65536];
  static int initialized;
  const char *prefix;
  const char *suffix;
  size_t i;

  if (!initialized) {
    memset(blob, 'x', sizeof(blob));
    initialized = 1;
  }
  prefix = "{\"id\":\"large\",\"blob\":[";
  suffix = "]}\n";
  if (!write_all(file, prefix, strlen(prefix))) {
    return 0;
  }
  for (i = 0u; i < LARGE_CANDIDATE_BLOB_BYTES / LARGE_CANDIDATE_BLOB_PART_BYTES;
       ++i) {
    if (!write_all(file, "\"", 1u) ||
        !write_all(file, blob, LARGE_CANDIDATE_BLOB_PART_BYTES) ||
        !write_all(file, "\"", 1u)) {
      return 0;
    }
    if (i + 1u < LARGE_CANDIDATE_BLOB_BYTES / LARGE_CANDIDATE_BLOB_PART_BYTES &&
        !write_all(file, ",", 1u)) {
      return 0;
    }
  }
  return write_all(file, suffix, strlen(suffix));
}

static lql_read_result read_file(void *user, unsigned char *buffer,
                                 size_t capacity) {
  file_reader *reader;
  lql_read_result result;

  memset(&result, 0, sizeof(result));
  reader = (file_reader *)user;
  if (reader == NULL || reader->file == NULL) {
    result.error_code = 1;
    return result;
  }
  result.bytes_read = fread(buffer, 1u, capacity, reader->file);
  if (result.bytes_read < capacity) {
    if (ferror(reader->file)) {
      result.error_code = 1;
    }
    if (feof(reader->file)) {
      result.eof = 1;
    }
  }
  return result;
}

static lql_status observe_spooled_match(void *user,
                                        const lql_query_match *match) {
  spool_counts *counts;
  const lonejson_spooled *spooled;

  counts = (spool_counts *)user;
  if (counts == NULL || match == NULL ||
      match->payload.kind != LQL_PAYLOAD_SPOOLED ||
      match->payload.spooled == NULL ||
      match->payload.size <= LONEJSON_SPOOL_MEMORY_LIMIT) {
    return LQL_STATUS_JSON_ERROR;
  }
  spooled = (const lonejson_spooled *)match->payload.spooled;
  if (!lonejson_spooled_spilled(spooled)) {
    return LQL_STATUS_JSON_ERROR;
  }
  ++counts->matches;
  ++counts->spilled;
  return LQL_STATUS_OK;
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

int main(void) {
  FILE *input;
  file_reader reader;
  spool_counts counts;
  lql *ctx;
  lql_selector *selector;
  lql_error error;
  lql_query_result result;
  lql_status status;
  lql_uint64 rss;
  size_t i;

  input = tmpfile();
  if (input == NULL) {
    printf("large source spool RSS fixture creation failed\n");
    return 1;
  }
  for (i = 0u; i < LARGE_CANDIDATE_COUNT; ++i) {
    if (!write_large_candidate(input)) {
      printf("large source spool RSS fixture write failed\n");
      fclose(input);
      return 1;
    }
  }
  if (fflush(input) != 0 || fseek(input, 0L, SEEK_SET) != 0) {
    printf("large source spool RSS fixture rewind failed\n");
    fclose(input);
    return 1;
  }

  ctx = NULL;
  selector = NULL;
  lql_error_init(&error);
  status = lql_new(&ctx, &error);
  if (status != LQL_STATUS_OK || ctx == NULL) {
    printf("large source spool RSS receiver failed: %s\n", error.message);
    fclose(input);
    return 1;
  }
  status = ctx->selector_parse(ctx, "/id=\"large\"", &selector, &error);
  if (status != LQL_STATUS_OK || selector == NULL) {
    printf("large source spool RSS selector failed: %s\n", error.message);
    ctx->destroy(ctx);
    fclose(input);
    return 1;
  }

  reader.file = input;
  memset(&counts, 0, sizeof(counts));
  memset(&result, 0, sizeof(result));
  status = ctx->query_source_spooled_matches(ctx, selector, read_file, &reader,
                                             observe_spooled_match, &counts,
                                             &result, &error);
  rss = peak_rss_bytes();
  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  fclose(input);

  if (status != LQL_STATUS_OK) {
    printf("large source spool RSS query failed: %s\n", error.message);
    return 1;
  }
  if (counts.matches != LARGE_CANDIDATE_COUNT ||
      counts.spilled != LARGE_CANDIDATE_COUNT ||
      result.candidates_seen != LARGE_CANDIDATE_COUNT ||
      result.candidates_matched != LARGE_CANDIDATE_COUNT) {
    printf("large source spool RSS counts mismatch: matches=%lu spilled=%lu "
           "seen=%lu matched=%lu\n",
           (unsigned long)counts.matches, (unsigned long)counts.spilled,
           (unsigned long)result.candidates_seen,
           (unsigned long)result.candidates_matched);
    return 1;
  }
  if (rss == 0u || rss > LARGE_CANDIDATE_RSS_LIMIT_BYTES) {
    printf("large source spool RSS limit exceeded: rss=%lu limit=%u\n",
           (unsigned long)rss, (unsigned int)LARGE_CANDIDATE_RSS_LIMIT_BYTES);
    return 1;
  }
  return 0;
}
