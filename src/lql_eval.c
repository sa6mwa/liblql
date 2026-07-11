#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "lql_internal.h"

#include <ctype.h>
#include <lonejson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define LQL_SOURCE_PREFIX_CAP 1024u
#define LQL_EVAL_FEATURE_PREFIX_CAPTURE 0x80000000u
#define LQL_QUERY_LIMIT_MATCHES 0x01u
#define LQL_QUERY_LIMIT_CANDIDATES 0x02u
#define LQL_QUERY_LIMIT_BYTES 0x04u
#define LQL_ROOT_ARRAY_NDJSON_ERROR                                            \
  "root arrays are not valid NDJSON candidate streams"

typedef struct eval_doc {
  lql_allocator *allocator;
  lql_impl *impl;
  const lql_selector *selector;
  unsigned int *hits;
  size_t hits_cap;
  unsigned int *stream_misses;
  size_t stream_misses_cap;
  const lql_selector **scalar_family_predicates;
  size_t scalar_family_predicates_cap;
  size_t scalar_family_stride;
  size_t scalar_family_counts[LQL_EVAL_FAMILY_COUNT];
  unsigned int *in_matches;
  size_t in_matches_cap;
  size_t in_match_stride;
  size_t *contains_positions;
  size_t contains_positions_cap;
  unsigned int candidate_epoch;
  const lql_selector *const *predicates;
  size_t predicate_count;
  unsigned int scalar_path_features;
  unsigned int scalar_stream_features;
  size_t scalar_len;
  size_t contains_tail_len;
  size_t contains_tail_need;
  char *contains_tail_buf;
  size_t contains_tail_cap;
  char contains_tail[LQL_EVAL_CONTAINS_TAIL_CAP];
  size_t prefix_len;
  size_t prefix_need;
  char prefix_buf[LQL_EVAL_PREFIX_CAP + 1u];
  int number_negative;
  int number_after_decimal;
  int number_in_exp;
  int number_exp_negative;
  int number_nonzero_seen;
  int number_first_sig_integer;
  int number_sig_truncated;
  size_t number_int_digits;
  size_t number_frac_leading_zeros;
  size_t number_sig_len;
  long number_exp_value;
  char number_sig[LQL_EVAL_NUMERIC_SIG_CAP + 1u];
  int *container_types;
  size_t container_cap;
  size_t container_high_water;
  int track_container_types;
  char root_kind;
  int candidate_matched;
  int borrowed_scratch;
  const lql_selector *fast_exact_selector;
  int fast_exact_path_active;
  int fast_exact_hit;
  int fast_exact_miss;
} eval_doc;

typedef struct lql_payload_sink_adapter {
  lql_write_fn write;
  void *user;
  lql_status status;
} lql_payload_sink_adapter;

typedef struct spooled_source_reader {
  lonejson_spooled cursor;
} spooled_source_reader;

static lql_status
execute_query_file_decisions(lql *self, const lql_selector *selector,
                             FILE *file, const lql_query_options *query_options,
                             lql_query_decision_fn on_decision, void *user,
                             lql_query_result *out_result, lql_error *error);
static lql_status
execute_query_file_matches(lql *self, const lql_selector *selector, FILE *file,
                           const lql_query_options *query_options,
                           lql_query_match_fn on_match, void *user,
                           lql_query_result *out_result, lql_error *error);
static lql_status execute_query_source_decisions(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *query_options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error);
static lql_status execute_query_source_decisions_with_base(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_uint64 offset_base, lql_uint64 index_base,
    const lql_query_options *query_options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error);
static lql_status execute_query_source_spooled_matches(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *query_options, lql_query_match_fn on_match,
    void *user, lql_query_result *out_result, lql_error *error);
static lql_status execute_query_source_spooled_matches_with_base(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_uint64 offset_base, lql_uint64 index_base,
    const lql_query_options *query_options, lql_query_match_fn on_match,
    void *user, lql_query_result *out_result, lql_error *error);
static lql_status execute_query_source_candidate_run(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    FILE *out, int compact, const lql_projection *projection,
    const lql_mutation_plan *mutation_plan, int matches_only,
    const lql_query_options *query_options, lql_query_result *out_result,
    lql_error *error);

static void clear_query_result(lql_query_result *out_result) {
  if (out_result != NULL) {
    memset(out_result, 0, sizeof(*out_result));
  }
}

static int payload_seek_u64(FILE *file, lql_uint64 offset) {
  off_t seek_offset;
  seek_offset = (off_t)offset;
  if (seek_offset < (off_t)0 || (lql_uint64)seek_offset != offset) {
    return 0;
  }
  return fseeko(file, seek_offset, SEEK_SET) == 0;
}

static lql_status copy_range_to_sink(FILE *in, lql_uint64 size,
                                     lql_write_fn write, void *user) {
  char buf[8192];
  size_t want;
  size_t got;
  lql_status st;
  while (size != 0u) {
    want = size > (lql_uint64)sizeof(buf) ? sizeof(buf) : (size_t)size;
    got = fread(buf, 1u, want, in);
    if (got == 0u) {
      return LQL_STATUS_JSON_ERROR;
    }
    st = write(user, buf, got);
    if (st != LQL_STATUS_OK) {
      return st;
    }
    size -= (lql_uint64)got;
  }
  return LQL_STATUS_OK;
}

static lql_status copy_fd_range_to_sink(int fd, lql_uint64 offset,
                                        lql_uint64 size, lql_write_fn write,
                                        void *user) {
  char buf[8192];
  size_t want;
  ssize_t got;
  off_t pos;
  lql_status st;

  while (size != 0u) {
    want = size > (lql_uint64)sizeof(buf) ? sizeof(buf) : (size_t)size;
    pos = (off_t)offset;
    if (pos < (off_t)0 || (lql_uint64)pos != offset) {
      return LQL_STATUS_JSON_ERROR;
    }
    got = pread(fd, buf, want, pos);
    if (got <= (ssize_t)0) {
      return LQL_STATUS_JSON_ERROR;
    }
    st = write(user, buf, (size_t)got);
    if (st != LQL_STATUS_OK) {
      return st;
    }
    offset += (lql_uint64)got;
    size -= (lql_uint64)got;
  }
  return LQL_STATUS_OK;
}

static lql_status payload_file_write(void *user, const void *data, size_t len) {
  FILE *out;
  out = (FILE *)user;
  return fwrite(data, 1u, len, out) == len ? LQL_STATUS_OK
                                           : LQL_STATUS_JSON_ERROR;
}

static lql_read_result spooled_source_read(void *user, unsigned char *buffer,
                                           size_t capacity) {
  spooled_source_reader *reader;
  lonejson_read_result lj_result;
  lql_read_result result;
  reader = (spooled_source_reader *)user;
  lj_result = lonejson_spooled_read(&reader->cursor, buffer, capacity);
  result.bytes_read = lj_result.bytes_read;
  result.eof = lj_result.eof;
  result.error_code = lj_result.error_code;
  return result;
}

static lonejson_read_result
spooled_lonejson_read(void *user, unsigned char *buffer, size_t capacity) {
  spooled_source_reader *reader = (spooled_source_reader *)user;
  return lonejson_spooled_read(&reader->cursor, buffer, capacity);
}

static lonejson_status payload_lql_sink(void *user, const void *data,
                                        size_t len, lonejson_error *error) {
  lql_payload_sink_adapter *adapter;
  (void)error;
  adapter = (lql_payload_sink_adapter *)user;
  adapter->status = adapter->write(adapter->user, data, len);
  return adapter->status == LQL_STATUS_OK ? LONEJSON_STATUS_OK
                                          : LONEJSON_STATUS_CALLBACK_FAILED;
}

static lonejson_candidate_callback_result
reject_root_array_candidate(lonejson_error *error) {
  if (error != NULL) {
    error->code = LONEJSON_STATUS_CALLBACK_FAILED;
    strncpy(error->message, LQL_ROOT_ARRAY_NDJSON_ERROR,
            sizeof(error->message) - 1u);
    error->message[sizeof(error->message) - 1u] = '\0';
  }
  return LONEJSON_CANDIDATE_ERROR;
}

static void destroy_doc(eval_doc *doc) {
  if (doc->container_types != NULL && doc->container_high_water != 0u) {
    memset(doc->container_types, 0,
           sizeof(doc->container_types[0]) * doc->container_high_water);
  }
  if (doc->borrowed_scratch && doc->impl != NULL) {
    doc->impl->eval_candidate_epoch = doc->candidate_epoch + 1u;
    if (doc->impl->eval_candidate_epoch == 0u) {
      if (doc->hits != NULL) {
        memset(doc->hits, 0, sizeof(*doc->hits) * doc->hits_cap);
      }
      if (doc->stream_misses != NULL) {
        memset(doc->stream_misses, 0,
               sizeof(*doc->stream_misses) * doc->stream_misses_cap);
      }
      if (doc->in_matches != NULL) {
        memset(doc->in_matches, 0,
               sizeof(*doc->in_matches) * doc->in_matches_cap);
      }
      if (doc->contains_positions != NULL) {
        memset(doc->contains_positions, 0,
               sizeof(*doc->contains_positions) * doc->contains_positions_cap);
      }
      doc->impl->eval_candidate_epoch = 1u;
    }
    doc->impl->eval_hits = doc->hits;
    doc->impl->eval_hits_cap = doc->hits_cap;
    doc->impl->eval_stream_misses = doc->stream_misses;
    doc->impl->eval_stream_misses_cap = doc->stream_misses_cap;
    doc->impl->eval_scalar_family_predicates = doc->scalar_family_predicates;
    doc->impl->eval_scalar_family_predicates_cap =
        doc->scalar_family_predicates_cap;
    doc->impl->eval_in_matches = doc->in_matches;
    doc->impl->eval_in_matches_cap = doc->in_matches_cap;
    doc->impl->eval_contains_positions = doc->contains_positions;
    doc->impl->eval_contains_positions_cap = doc->contains_positions_cap;
    doc->impl->eval_contains_tail_buf = doc->contains_tail_buf;
    doc->impl->eval_contains_tail_cap = doc->contains_tail_cap;
    doc->impl->eval_container_types = doc->container_types;
    doc->impl->eval_container_cap = doc->container_cap;
    doc->impl->eval_scratch_in_use = 0;
  } else {
    doc->allocator->destroy(doc->allocator, doc->hits);
    doc->allocator->destroy(doc->allocator, doc->stream_misses);
    doc->allocator->destroy(doc->allocator, doc->scalar_family_predicates);
    doc->allocator->destroy(doc->allocator, doc->in_matches);
    doc->allocator->destroy(doc->allocator, doc->contains_positions);
    doc->allocator->destroy(doc->allocator, doc->contains_tail_buf);
    doc->allocator->destroy(doc->allocator, doc->container_types);
  }
  memset(doc, 0, sizeof(*doc));
}

static int selector_fast_exact_eligible(const lql_selector *selector) {
  return selector != NULL && selector->kind == LQL_SELECTOR_KIND_EQ &&
         selector->hit_count == 1u && selector->predicate_count == 1u &&
         selector->predicates != NULL && selector->predicates[0] == selector &&
         selector->field_path_direct && selector->field_path_literal &&
         !selector->value_is_temporal && selector->any_count == 0u &&
         (selector->value_set || selector->value != NULL) &&
         selector->observer_family == LQL_EVAL_FAMILY_EXACT;
}

static int init_doc(eval_doc *doc, lql *self, const lql_selector *selector) {
  lql_impl *impl;
  unsigned int *next_hits;
  unsigned int *next_stream_misses;
  const lql_selector **next_scalar_family_predicates;
  unsigned int *next_in_matches;
  size_t *next_contains_positions;
  size_t in_match_need;
  size_t family_need;
  size_t i;
  unsigned int streaming_miss_features;
  int clear_hits;
  int clear_stream_misses;
  int clear_in_matches;
  memset(doc, 0, sizeof(*doc));
  doc->allocator = lql_allocator_from_receiver(self);
  impl = self == NULL ? NULL : (lql_impl *)self->impl;
  doc->impl = impl;
  doc->selector = selector;
  if (doc->allocator == NULL) {
    return 0;
  }
  if (selector_fast_exact_eligible(selector)) {
    doc->fast_exact_selector = selector;
  }
  if (impl != NULL && !impl->eval_scratch_in_use) {
    impl->eval_scratch_in_use = 1;
    doc->borrowed_scratch = 1;
    doc->hits = impl->eval_hits;
    doc->hits_cap = impl->eval_hits_cap;
    doc->stream_misses = impl->eval_stream_misses;
    doc->stream_misses_cap = impl->eval_stream_misses_cap;
    doc->scalar_family_predicates = impl->eval_scalar_family_predicates;
    doc->scalar_family_predicates_cap = impl->eval_scalar_family_predicates_cap;
    doc->in_matches = impl->eval_in_matches;
    doc->in_matches_cap = impl->eval_in_matches_cap;
    doc->contains_positions = impl->eval_contains_positions;
    doc->contains_positions_cap = impl->eval_contains_positions_cap;
    doc->contains_tail_buf = impl->eval_contains_tail_buf;
    doc->contains_tail_cap = impl->eval_contains_tail_cap;
    doc->container_types = impl->eval_container_types;
    doc->container_cap = impl->eval_container_cap;
    impl->eval_hits = NULL;
    impl->eval_hits_cap = 0u;
    impl->eval_stream_misses = NULL;
    impl->eval_stream_misses_cap = 0u;
    impl->eval_scalar_family_predicates = NULL;
    impl->eval_scalar_family_predicates_cap = 0u;
    impl->eval_in_matches = NULL;
    impl->eval_in_matches_cap = 0u;
    impl->eval_contains_positions = NULL;
    impl->eval_contains_positions_cap = 0u;
    impl->eval_contains_tail_buf = NULL;
    impl->eval_contains_tail_cap = 0u;
    impl->eval_container_types = NULL;
    impl->eval_container_cap = 0u;
  }
  if (selector != NULL && selector->hit_count != 0u &&
      doc->fast_exact_selector == NULL) {
    clear_hits = !doc->borrowed_scratch;
    clear_stream_misses = !doc->borrowed_scratch;
    clear_in_matches = !doc->borrowed_scratch;
    doc->in_match_stride = selector->max_in_alternative_count;
    if (doc->hits_cap < selector->hit_count) {
      next_hits = (unsigned int *)doc->allocator->realloc(
          doc->allocator, doc->hits, sizeof(*doc->hits) * selector->hit_count);
      if (next_hits == NULL) {
        destroy_doc(doc);
        return 0;
      }
      doc->hits = next_hits;
      doc->hits_cap = selector->hit_count;
      clear_hits = 1;
    }
    if (clear_hits) {
      memset(doc->hits, 0, sizeof(*doc->hits) * doc->hits_cap);
    }
    streaming_miss_features =
        selector->predicate_features &
        (LQL_SELECTOR_FEATURE_EXACT | LQL_SELECTOR_FEATURE_PREFIX);
    if (streaming_miss_features != 0u &&
        doc->stream_misses_cap < selector->hit_count) {
      next_stream_misses = (unsigned int *)doc->allocator->realloc(
          doc->allocator, doc->stream_misses,
          sizeof(*doc->stream_misses) * selector->hit_count);
      if (next_stream_misses == NULL) {
        destroy_doc(doc);
        return 0;
      }
      doc->stream_misses = next_stream_misses;
      doc->stream_misses_cap = selector->hit_count;
      clear_stream_misses = 1;
    }
    if (clear_stream_misses && doc->stream_misses != NULL) {
      memset(doc->stream_misses, 0,
             sizeof(*doc->stream_misses) * doc->stream_misses_cap);
    }
    if (selector->predicate_count > ((size_t)-1) / LQL_EVAL_FAMILY_COUNT) {
      destroy_doc(doc);
      return 0;
    }
    family_need = selector->predicate_count * LQL_EVAL_FAMILY_COUNT;
    if (doc->scalar_family_predicates_cap < family_need) {
      next_scalar_family_predicates =
          (const lql_selector **)doc->allocator->realloc(
              doc->allocator, doc->scalar_family_predicates,
              sizeof(*doc->scalar_family_predicates) * family_need);
      if (next_scalar_family_predicates == NULL) {
        destroy_doc(doc);
        return 0;
      }
      doc->scalar_family_predicates = next_scalar_family_predicates;
      doc->scalar_family_predicates_cap = family_need;
    }
    doc->scalar_family_stride = selector->predicate_count;
    if (doc->in_match_stride != 0u) {
      in_match_need = selector->hit_count * doc->in_match_stride;
      if (doc->in_matches_cap < in_match_need) {
        next_in_matches = (unsigned int *)doc->allocator->realloc(
            doc->allocator, doc->in_matches,
            sizeof(*doc->in_matches) * in_match_need);
        if (next_in_matches == NULL) {
          destroy_doc(doc);
          return 0;
        }
        doc->in_matches = next_in_matches;
        doc->in_matches_cap = in_match_need;
        clear_in_matches = 1;
      }
      if (clear_in_matches) {
        memset(doc->in_matches, 0,
               sizeof(*doc->in_matches) * doc->in_matches_cap);
      }
    }
    if ((selector->predicate_features & LQL_SELECTOR_FEATURE_CONTAINS) != 0u &&
        doc->contains_positions_cap < selector->hit_count) {
      next_contains_positions = (size_t *)doc->allocator->realloc(
          doc->allocator, doc->contains_positions,
          sizeof(*doc->contains_positions) * selector->hit_count);
      if (next_contains_positions == NULL) {
        destroy_doc(doc);
        return 0;
      }
      doc->contains_positions = next_contains_positions;
      doc->contains_positions_cap = selector->hit_count;
      memset(doc->contains_positions, 0,
             sizeof(*doc->contains_positions) * doc->contains_positions_cap);
    }
    doc->predicates = selector->predicates;
    doc->predicate_count = selector->predicate_count;
    doc->track_container_types = selector->predicate_has_variable_path;
    for (i = 0u; i < selector->predicate_count && !doc->track_container_types;
         ++i) {
      if (selector->predicates[i] == NULL ||
          !selector->predicates[i]->field_path_literal) {
        doc->track_container_types = 1;
      }
    }
  }
  doc->candidate_epoch =
      doc->borrowed_scratch && impl != NULL && impl->eval_candidate_epoch != 0u
          ? impl->eval_candidate_epoch
          : 1u;
  return 1;
}

static void reset_doc(eval_doc *doc) {
  if (doc->selector != NULL && doc->selector->hit_count != 0u) {
    ++doc->candidate_epoch;
    if (doc->candidate_epoch == 0u) {
      memset(doc->hits, 0, sizeof(*doc->hits) * doc->selector->hit_count);
      if (doc->stream_misses != NULL) {
        memset(doc->stream_misses, 0,
               sizeof(*doc->stream_misses) * doc->selector->hit_count);
      }
      if (doc->in_matches != NULL && doc->in_match_stride != 0u) {
        memset(doc->in_matches, 0,
               sizeof(*doc->in_matches) * doc->selector->hit_count *
                   doc->in_match_stride);
      }
      doc->candidate_epoch = 1u;
    }
  }
  doc->scalar_stream_features = 0u;
  doc->scalar_path_features = 0u;
  memset(doc->scalar_family_counts, 0, sizeof(doc->scalar_family_counts));
  doc->scalar_len = 0u;
  doc->contains_tail_len = 0u;
  doc->contains_tail_need = 0u;
  doc->prefix_len = 0u;
  doc->prefix_need = 0u;
  doc->root_kind = '\0';
  doc->candidate_matched = 0;
  doc->fast_exact_path_active = 0;
  doc->fast_exact_hit = 0;
  doc->fast_exact_miss = 0;
}

static char *contains_tail_data(eval_doc *doc) {
  if (doc->contains_tail_need <= LQL_EVAL_CONTAINS_TAIL_CAP) {
    return doc->contains_tail;
  }
  return doc->contains_tail_buf;
}

static int ensure_contains_tail(eval_doc *doc) {
  char *next;
  size_t next_cap;

  if (doc->contains_tail_need <= LQL_EVAL_CONTAINS_TAIL_CAP ||
      doc->contains_tail_need <= doc->contains_tail_cap) {
    return 1;
  }
  next_cap = doc->contains_tail_cap == 0u ? LQL_EVAL_CONTAINS_TAIL_CAP * 2u
                                          : doc->contains_tail_cap;
  while (next_cap < doc->contains_tail_need) {
    next_cap *= 2u;
  }
  next = (char *)doc->allocator->realloc(doc->allocator, doc->contains_tail_buf,
                                         next_cap);
  if (next == NULL) {
    return 0;
  }
  doc->contains_tail_buf = next;
  doc->contains_tail_cap = next_cap;
  return 1;
}

static lonejson_status
push_container(eval_doc *doc, const lonejson_value_path *path, int type) {
  int *next_types;
  size_t depth;
  size_t old_cap;
  size_t next_cap;
  if (!doc->track_container_types) {
    return LONEJSON_STATUS_OK;
  }
  depth = path->segment_count;
  if (depth >= doc->container_cap) {
    old_cap = doc->container_cap;
    next_cap = doc->container_cap == 0u ? 8u : doc->container_cap * 2u;
    while (depth >= next_cap) {
      next_cap *= 2u;
    }
    next_types = (int *)doc->allocator->realloc(
        doc->allocator, doc->container_types, sizeof(int) * next_cap);
    if (next_types == NULL) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    doc->container_types = next_types;
    memset(doc->container_types + old_cap, 0,
           sizeof(doc->container_types[0]) * (next_cap - old_cap));
    doc->container_cap = next_cap;
  }
  doc->container_types[depth] = type;
  if (depth + 1u > doc->container_high_water) {
    doc->container_high_water = depth + 1u;
  }
  return LONEJSON_STATUS_OK;
}

static void pop_container(eval_doc *doc, const lonejson_value_path *path) {
  size_t depth;
  if (!doc->track_container_types) {
    return;
  }
  depth = path->segment_count;
  if (depth >= doc->container_cap) {
    return;
  }
  doc->container_types[depth] = 0;
}

static int ascii_case_equal_prefix(const char *a, const char *b, size_t n) {
  size_t i;
  for (i = 0u; i < n; ++i) {
    unsigned char ca;
    unsigned char cb;
    ca = (unsigned char)a[i];
    cb = (unsigned char)b[i];
    if (ca >= (unsigned char)'A' && ca <= (unsigned char)'Z') {
      ca = (unsigned char)(ca + ((unsigned char)'a' - (unsigned char)'A'));
    }
    if (cb >= (unsigned char)'A' && cb <= (unsigned char)'Z') {
      cb = (unsigned char)(cb + ((unsigned char)'a' - (unsigned char)'A'));
    }
    if (ca != cb) {
      return 0;
    }
  }
  return 1;
}

static unsigned char ascii_lower_byte(unsigned char c) {
  if (c >= (unsigned char)'A' && c <= (unsigned char)'Z') {
    return (unsigned char)(c + ((unsigned char)'a' - (unsigned char)'A'));
  }
  return c;
}

static int ascii_is_digit(unsigned char c) {
  return c >= (unsigned char)'0' && c <= (unsigned char)'9';
}

static int contains_case_len(const char *haystack, size_t h, const char *needle,
                             size_t n, int ignore_case);
static size_t *contains_position_ptr(eval_doc *doc,
                                     const lql_selector *selector);

static int contains_case_len(const char *haystack, size_t h, const char *needle,
                             size_t n, int ignore_case) {
  size_t i;
  const char *cursor;
  const char *end;
  size_t remaining;
  unsigned char first;
  if (n == 0u) {
    return 1;
  }
  if (n > h) {
    return 0;
  }
  first = ignore_case ? ascii_lower_byte((unsigned char)needle[0])
                      : (unsigned char)needle[0];
  if (!ignore_case) {
    cursor = haystack;
    end = haystack + h - n + 1u;
    while (cursor < end) {
      remaining = (size_t)(end - cursor);
      cursor = (const char *)memchr(cursor, first, remaining);
      if (cursor == NULL) {
        return 0;
      }
      if (n == 1u || memcmp(cursor + 1, needle + 1, n - 1u) == 0) {
        return 1;
      }
      ++cursor;
    }
    return 0;
  }
  for (i = 0u; i + n <= h; ++i) {
    if (ascii_lower_byte((unsigned char)haystack[i]) != first) {
      continue;
    }
    if (n == 1u ||
        ascii_case_equal_prefix(haystack + i + 1u, needle + 1u, n - 1u)) {
      return 1;
    }
  }
  return 0;
}

static int contains_any_case_len(const char *haystack, size_t h, char **needles,
                                 const size_t *needle_lens, size_t count,
                                 const unsigned char *firsts,
                                 const unsigned char *first_bitmap,
                                 int ignore_case) {
  size_t i;
  size_t j;
  size_t n;
  size_t n0;
  size_t n1;
  size_t n2;
  unsigned char hay_ch;
  unsigned char raw_ch;
  unsigned char f0;
  unsigned char f1;
  unsigned char f2;
  if (count == 0u) {
    return 0;
  }
  if (count == 1u) {
    return contains_case_len(haystack, h, needles[0], needle_lens[0],
                             ignore_case);
  }
  if (!ignore_case && count <= 3u) {
    for (j = 0u; j < count; ++j) {
      if (contains_case_len(haystack, h, needles[j], needle_lens[j], 0)) {
        return 1;
      }
    }
    return 0;
  }
  if (count == 2u) {
    n0 = needle_lens[0];
    n1 = needle_lens[1];
    if (n0 == 0u || n1 == 0u) {
      return 1;
    }
    f0 = firsts[0];
    f1 = firsts[1];
    for (i = 0u; i < h; ++i) {
      raw_ch = (unsigned char)haystack[i];
      hay_ch = ignore_case ? ascii_lower_byte(raw_ch) : raw_ch;
      if (hay_ch == f0 && n0 <= h - i &&
          (n0 == 1u ||
           (ignore_case
                ? ascii_case_equal_prefix(haystack + i + 1u, needles[0] + 1u,
                                          n0 - 1u)
                : memcmp(haystack + i + 1u, needles[0] + 1u, n0 - 1u) == 0))) {
        return 1;
      }
      if (hay_ch == f1 && n1 <= h - i &&
          (n1 == 1u ||
           (ignore_case
                ? ascii_case_equal_prefix(haystack + i + 1u, needles[1] + 1u,
                                          n1 - 1u)
                : memcmp(haystack + i + 1u, needles[1] + 1u, n1 - 1u) == 0))) {
        return 1;
      }
    }
    return 0;
  }
  if (count == 3u) {
    n0 = needle_lens[0];
    n1 = needle_lens[1];
    n2 = needle_lens[2];
    if (n0 == 0u || n1 == 0u || n2 == 0u) {
      return 1;
    }
    f0 = firsts[0];
    f1 = firsts[1];
    f2 = firsts[2];
    for (i = 0u; i < h; ++i) {
      raw_ch = (unsigned char)haystack[i];
      hay_ch = ignore_case ? ascii_lower_byte(raw_ch) : raw_ch;
      if (hay_ch == f0 && n0 <= h - i &&
          (n0 == 1u ||
           (ignore_case
                ? ascii_case_equal_prefix(haystack + i + 1u, needles[0] + 1u,
                                          n0 - 1u)
                : memcmp(haystack + i + 1u, needles[0] + 1u, n0 - 1u) == 0))) {
        return 1;
      }
      if (hay_ch == f1 && n1 <= h - i &&
          (n1 == 1u ||
           (ignore_case
                ? ascii_case_equal_prefix(haystack + i + 1u, needles[1] + 1u,
                                          n1 - 1u)
                : memcmp(haystack + i + 1u, needles[1] + 1u, n1 - 1u) == 0))) {
        return 1;
      }
      if (hay_ch == f2 && n2 <= h - i &&
          (n2 == 1u ||
           (ignore_case
                ? ascii_case_equal_prefix(haystack + i + 1u, needles[2] + 1u,
                                          n2 - 1u)
                : memcmp(haystack + i + 1u, needles[2] + 1u, n2 - 1u) == 0))) {
        return 1;
      }
    }
    return 0;
  }
  for (j = 0u; j < count; ++j) {
    if (needle_lens[j] == 0u) {
      return 1;
    }
  }
  for (i = 0u; i < h; ++i) {
    raw_ch = (unsigned char)haystack[i];
    hay_ch = ignore_case ? ascii_lower_byte(raw_ch) : raw_ch;
    if ((first_bitmap[hay_ch >> 3] & (unsigned char)(1u << (hay_ch & 7u))) ==
        0u) {
      continue;
    }
    for (j = 0u; j < count; ++j) {
      if (hay_ch != firsts[j]) {
        continue;
      }
      n = needle_lens[j];
      if (n > h - i) {
        continue;
      }
      if (n == 1u) {
        return 1;
      }
      if (ignore_case) {
        if (ascii_case_equal_prefix(haystack + i + 1u, needles[j] + 1u,
                                    n - 1u)) {
          return 1;
        }
      } else if (memcmp(haystack + i + 1u, needles[j] + 1u, n - 1u) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

static int path_segment_matches(const char *start, size_t len,
                                const lonejson_path_segment *segment) {
  size_t i;
  size_t j;
  char ch;
  i = 0u;
  j = 0u;
  while (i < len && j < segment->len) {
    ch = start[i++];
    if (ch == '~' && i < len) {
      if (start[i] == '0') {
        ch = '~';
        ++i;
      } else if (start[i] == '1') {
        ch = '/';
        ++i;
      }
    }
    if (ch != segment->data[j++]) {
      return 0;
    }
  }
  return i == len && j == segment->len;
}

static int parent_container_type(const eval_doc *doc, size_t depth,
                                 int *out_type) {
  if (depth >= doc->container_cap || doc->container_types[depth] == 0) {
    return 0;
  }
  *out_type = doc->container_types[depth];
  return 1;
}

static int pattern_segment_is(const char *start, size_t len, const char *lit) {
  return strlen(lit) == len && memcmp(start, lit, len) == 0;
}

static int pattern_segment_matches(const eval_doc *doc, const char *start,
                                   size_t len, const lonejson_value_path *path,
                                   size_t path_idx) {
  int parent_type;
  if (pattern_segment_is(start, len, "*")) {
    if (path_idx >= path->segment_count ||
        !parent_container_type(doc, path_idx, &parent_type)) {
      return 0;
    }
    return parent_type == '{';
  }
  if (pattern_segment_is(start, len, "[]")) {
    if (path_idx >= path->segment_count ||
        !parent_container_type(doc, path_idx, &parent_type)) {
      return 0;
    }
    return parent_type == '[';
  }
  if (pattern_segment_is(start, len, "**")) {
    if (path_idx >= path->segment_count ||
        !parent_container_type(doc, path_idx, &parent_type)) {
      return 0;
    }
    return parent_type == '{' || parent_type == '[';
  }
  if (path_idx >= path->segment_count) {
    return 0;
  }
  return path_segment_matches(start, len, &path->segments[path_idx]);
}

static int path_matches_from(const eval_doc *doc, const char *seg,
                             const lonejson_value_path *path, size_t path_idx) {
  const char *slash;
  size_t len;
  size_t i;
  if (*seg == '\0') {
    return path_idx == path->segment_count;
  }
  slash = strchr(seg, '/');
  len = slash == NULL ? strlen(seg) : (size_t)(slash - seg);
  if (pattern_segment_is(seg, len, "...")) {
    if (slash == NULL) {
      return 1;
    }
    for (i = path_idx; i <= path->segment_count; ++i) {
      if (path_matches_from(doc, slash + 1, path, i)) {
        return 1;
      }
    }
    return 0;
  }
  if (!pattern_segment_matches(doc, seg, len, path, path_idx)) {
    return 0;
  }
  if (slash == NULL) {
    return path_idx + 1u == path->segment_count;
  }
  return path_matches_from(doc, slash + 1, path, path_idx + 1u);
}

static int path_recursive_literal_suffix_matches(
    const char *pattern, const lonejson_value_path *path, int *out_match) {
  const char *seg;
  const char *slash;
  size_t count;
  size_t len;
  size_t path_idx;
  size_t suffix_idx;

  if (out_match != NULL) {
    *out_match = 0;
  }
  if (pattern == NULL || path == NULL || memcmp(pattern, "/.../", 5u) != 0) {
    return 0;
  }
  seg = pattern + 5u;
  if (*seg == '\0') {
    if (out_match != NULL) {
      *out_match = 1;
    }
    return 1;
  }
  count = 1u;
  for (slash = seg; *slash != '\0'; ++slash) {
    if (*slash == '/') {
      ++count;
    }
  }
  if (count > path->segment_count) {
    return 0;
  }
  path_idx = path->segment_count - count;
  suffix_idx = 0u;
  while (*seg != '\0') {
    slash = strchr(seg, '/');
    len = slash == NULL ? strlen(seg) : (size_t)(slash - seg);
    if (pattern_segment_is(seg, len, "...") ||
        pattern_segment_is(seg, len, "*") ||
        pattern_segment_is(seg, len, "[]") ||
        pattern_segment_is(seg, len, "**")) {
      return 0;
    }
    if (!path_segment_matches(seg, len,
                              &path->segments[path_idx + suffix_idx])) {
      return 1;
    }
    ++suffix_idx;
    if (slash == NULL) {
      break;
    }
    seg = slash + 1;
  }
  if (out_match != NULL) {
    *out_match = 1;
  }
  return 1;
}

static int path_matches(const eval_doc *doc, const char *pattern,
                        const lonejson_value_path *path) {
  const char *seg;
  int suffix_match;
  if (pattern == NULL || path == NULL || pattern[0] != '/') {
    return 0;
  }
  if (pattern[1] == '\0') {
    return path->segment_count == 0u;
  }
  if (pattern[1] == '.' &&
      path_recursive_literal_suffix_matches(pattern, path, &suffix_match)) {
    return suffix_match;
  }
  seg = pattern + 1;
  return path_matches_from(doc, seg, path, 0u);
}

static void stream_miss_clear_fast(eval_doc *doc, const lql_selector *selector);
static int hit_marked_fast(const eval_doc *doc, const lql_selector *selector);
static int eval_selector_tree(const lql_selector *selector,
                              const eval_doc *doc);

static int selector_path_matches(const eval_doc *doc,
                                 const lql_selector *selector,
                                 const lonejson_value_path *path) {
  size_t i;
  size_t offset;
  size_t len;
  int parent_type;
  const lonejson_path_segment *segment;

  if (selector == NULL) {
    return 0;
  }
  if (selector->field_path_recursive_literal_suffix) {
    size_t base;
    if (path == NULL || path->segment_count < selector->field_segment_count) {
      return 0;
    }
    base = path->segment_count - selector->field_segment_count;
    for (i = 0u; i < selector->field_segment_count; ++i) {
      offset = selector->field_segment_offsets[i];
      len = selector->field_segment_lens[i];
      segment = &path->segments[base + i];
      if (segment->len != len ||
          memcmp(selector->field + offset, segment->data, len) != 0) {
        return 0;
      }
    }
    return 1;
  }
  if (!selector->field_path_direct) {
    return path_matches(doc, selector->field, path);
  }
  if (path == NULL || path->segment_count != selector->field_segment_count) {
    return 0;
  }
  if (selector->field_path_literal) {
    for (i = 0u; i < selector->field_segment_count; ++i) {
      offset = selector->field_segment_offsets[i];
      len = selector->field_segment_lens[i];
      segment = &path->segments[i];
      if (segment->len != len ||
          memcmp(selector->field + offset, segment->data, len) != 0) {
        return 0;
      }
    }
    return 1;
  }
  for (i = 0u; i < selector->field_segment_count; ++i) {
    offset = selector->field_segment_offsets[i];
    len = selector->field_segment_lens[i];
    segment = &path->segments[i];
    switch (selector->field_segment_kinds == NULL
                ? LQL_FIELD_SEGMENT_LITERAL
                : selector->field_segment_kinds[i]) {
    case LQL_FIELD_SEGMENT_OBJECT_WILDCARD:
      if (!parent_container_type(doc, i, &parent_type) || parent_type != '{') {
        return 0;
      }
      break;
    case LQL_FIELD_SEGMENT_ARRAY_WILDCARD:
      if (!parent_container_type(doc, i, &parent_type) || parent_type != '[') {
        return 0;
      }
      break;
    case LQL_FIELD_SEGMENT_ANY_WILDCARD:
      if (!parent_container_type(doc, i, &parent_type) ||
          (parent_type != '{' && parent_type != '[')) {
        return 0;
      }
      break;
    default:
      if (segment->len != len ||
          memcmp(selector->field + offset, segment->data, len) != 0) {
        return 0;
      }
      break;
    }
  }
  return 1;
}

static int
selector_top_level_literal_path_match(const lql_selector *selector,
                                      const lonejson_value_path *path,
                                      int *out_match) {
  size_t offset;
  size_t len;

  if (out_match != NULL) {
    *out_match = 0;
  }
  if (selector == NULL || path == NULL || path->segment_count != 1u ||
      !selector->field_path_direct || !selector->field_path_literal ||
      selector->field_segment_count != 1u) {
    return 0;
  }
  offset = selector->field_segment_offsets[0];
  len = selector->field_segment_lens[0];
  if (out_match != NULL) {
    *out_match =
        path->segments[0].len == len &&
        memcmp(selector->field + offset, path->segments[0].data, len) == 0;
  }
  return 1;
}

static int selector_path_depth_possible(const lql_selector *selector,
                                        const lonejson_value_path *path) {
  if (selector == NULL || path == NULL || selector->hit_count == 0u ||
      selector->predicate_has_variable_path) {
    return 1;
  }
  return path->segment_count >= selector->predicate_min_segment_count &&
         path->segment_count <= selector->predicate_max_segment_count;
}

static const lql_selector **scalar_family_begin(eval_doc *doc, size_t family) {
  if (doc == NULL || doc->scalar_family_predicates == NULL ||
      doc->scalar_family_stride == 0u || family >= LQL_EVAL_FAMILY_COUNT) {
    return NULL;
  }
  return doc->scalar_family_predicates + family * doc->scalar_family_stride;
}

static void scalar_family_append_fast(eval_doc *doc,
                                      const lql_selector *selector) {
  size_t family;
  size_t count;
  family = selector->observer_family;
  count = doc->scalar_family_counts[family];
  doc->scalar_family_predicates[family * doc->scalar_family_stride + count] =
      selector;
  doc->scalar_family_counts[family] = count + 1u;
}

static void path_match_prepare(eval_doc *doc, const lonejson_value_path *path,
                               unsigned int feature_mask) {
  const lql_selector *root;
  const lql_selector *selector;
  const lql_selector *const *predicates;
  unsigned int feature;
  size_t end;
  size_t i;
  size_t predicate_index;
  size_t start;
  int path_match;

  root = doc->selector;
  if (doc->fast_exact_selector != NULL) {
    doc->scalar_path_features = 0u;
    doc->fast_exact_path_active = 0;
    doc->fast_exact_miss = 0;
    if (doc->fast_exact_hit) {
      return;
    }
    if (feature_mask != 0u &&
        (LQL_SELECTOR_FEATURE_EXACT & feature_mask) == 0u) {
      return;
    }
    if (selector_path_matches(doc, doc->fast_exact_selector, path)) {
      doc->scalar_path_features = LQL_SELECTOR_FEATURE_EXACT;
      doc->prefix_need = doc->fast_exact_selector->observer_prefix_need;
      doc->fast_exact_path_active = 1;
    }
    return;
  }
  if (root == NULL || root->hit_count == 0u ||
      doc->scalar_family_predicates == NULL) {
    return;
  }
  doc->scalar_path_features = 0u;
  memset(doc->scalar_family_counts, 0, sizeof(doc->scalar_family_counts));
  if (doc->candidate_matched && root->match_sticky_once_true) {
    return;
  }
  if (!selector_path_depth_possible(root, path)) {
    return;
  }
  predicates = doc->predicates;
  start = 0u;
  end = doc->predicate_count;
  if (root->predicate_depth_indexes != NULL &&
      root->predicate_depth_offsets != NULL) {
    if (path == NULL ||
        path->segment_count > root->predicate_max_segment_count) {
      return;
    }
    start = root->predicate_depth_offsets[path->segment_count];
    end = root->predicate_depth_offsets[path->segment_count + 1u];
    for (i = start; i < end; ++i) {
      predicate_index = root->predicate_depth_indexes[i];
      selector = root->predicates[predicate_index];
      if (hit_marked_fast(doc, selector)) {
        continue;
      }
      feature = selector->observer_feature;
      if (feature_mask != 0u && (feature & feature_mask) == 0u) {
        continue;
      }
      if (selector_top_level_literal_path_match(selector, path, &path_match)
              ? path_match
              : selector_path_matches(doc, selector, path)) {
        if (doc->stream_misses != NULL) {
          stream_miss_clear_fast(doc, selector);
        }
        doc->scalar_path_features |= feature;
        scalar_family_append_fast(doc, selector);
        if (selector->observer_contains_tail_need > doc->contains_tail_need) {
          doc->contains_tail_need = selector->observer_contains_tail_need;
        }
        if (selector->observer_prefix_need > doc->prefix_need) {
          doc->prefix_need = selector->observer_prefix_need;
        }
      }
    }
    return;
  }
  for (i = start; i < end; ++i) {
    selector = predicates[i];
    if (hit_marked_fast(doc, selector)) {
      continue;
    }
    feature = selector->observer_feature;
    if (feature_mask != 0u && (feature & feature_mask) == 0u) {
      continue;
    }
    if (selector_top_level_literal_path_match(selector, path, &path_match)
            ? path_match
            : selector_path_matches(doc, selector, path)) {
      if (doc->stream_misses != NULL) {
        stream_miss_clear_fast(doc, selector);
      }
      doc->scalar_path_features |= feature;
      scalar_family_append_fast(doc, selector);
      if (selector->observer_contains_tail_need > doc->contains_tail_need) {
        doc->contains_tail_need = selector->observer_contains_tail_need;
      }
      if (selector->observer_prefix_need > doc->prefix_need) {
        doc->prefix_need = selector->observer_prefix_need;
      }
    }
  }
}

static void scalar_path_match_prepare(eval_doc *doc,
                                      const lonejson_value_path *path) {
  path_match_prepare(doc, path, 0u);
}

static void hit_mark_fast(eval_doc *doc, const lql_selector *selector) {
  if (doc->fast_exact_selector == selector) {
    doc->fast_exact_hit = 1;
    doc->candidate_matched = 1;
    return;
  }
  doc->hits[selector->hit_index] = doc->candidate_epoch;
  if (!doc->candidate_matched && doc->selector->match_sticky_once_true &&
      eval_selector_tree(doc->selector, doc)) {
    doc->candidate_matched = 1;
  }
}

static int hit_marked_fast(const eval_doc *doc, const lql_selector *selector) {
  if (doc->fast_exact_selector == selector) {
    return doc->fast_exact_hit;
  }
  return doc->hits[selector->hit_index] == doc->candidate_epoch;
}

static void stream_miss_mark_fast(eval_doc *doc, const lql_selector *selector) {
  if (doc->fast_exact_selector == selector) {
    doc->fast_exact_miss = 1;
    return;
  }
  doc->stream_misses[selector->hit_index] = doc->candidate_epoch;
}

static void stream_miss_clear_fast(eval_doc *doc,
                                   const lql_selector *selector) {
  if (doc->fast_exact_selector == selector) {
    doc->fast_exact_miss = 0;
    return;
  }
  doc->stream_misses[selector->hit_index] = 0u;
}

static int stream_miss_marked_fast(const eval_doc *doc,
                                   const lql_selector *selector) {
  if (doc->fast_exact_selector == selector) {
    return doc->fast_exact_miss;
  }
  return doc->stream_misses[selector->hit_index] == doc->candidate_epoch;
}

static unsigned int *in_match_row_fast(eval_doc *doc,
                                       const lql_selector *selector) {
  return doc->in_matches + selector->hit_index * doc->in_match_stride;
}

static int resolve_since_macro(lql_since_macro macro, lql_temporal *out) {
  switch (macro) {
  case LQL_SINCE_NOW:
    return lql_temporal_now(out);
  case LQL_SINCE_TODAY:
    return lql_temporal_today(out);
  case LQL_SINCE_YESTERDAY:
    return lql_temporal_yesterday(out);
  default:
    return 0;
  }
}

static void observe_prepared_contains_value(eval_doc *doc, const char *value,
                                            size_t value_len, int is_container,
                                            int is_null) {
  const lql_selector **items;
  const lql_selector *selector;
  size_t i;
  size_t count;
  int ignore_case;

  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_CONTAINS);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_CONTAINS];
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    if (selector->any_count == 0u && !selector->value_set &&
        selector->value == NULL) {
      hit_mark_fast(doc, selector);
      continue;
    }
    if (is_container || is_null) {
      continue;
    }
    ignore_case = selector->observer_ignore_case;
    if (selector->any_count == 0u) {
      if (contains_case_len(value, value_len, selector->value_data,
                            selector->value_len, ignore_case)) {
        hit_mark_fast(doc, selector);
      }
    } else if (contains_any_case_len(value, value_len, selector->any,
                                     selector->any_lens, selector->any_count,
                                     ignore_case ? selector->any_ifirsts
                                                 : selector->any_firsts,
                                     ignore_case ? selector->any_ifirst_bitmap
                                                 : selector->any_first_bitmap,
                                     ignore_case)) {
      hit_mark_fast(doc, selector);
    }
  }
}

static void observe_prepared_prefix_value(eval_doc *doc, const char *value,
                                          size_t value_len, int is_container,
                                          int is_null) {
  const lql_selector **items;
  const lql_selector *selector;
  size_t i;
  size_t count;
  size_t n;

  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_PREFIX);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_PREFIX];
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    if (!selector->value_set && selector->value == NULL) {
      hit_mark_fast(doc, selector);
      continue;
    }
    if (is_container || is_null) {
      continue;
    }
    n = selector->value_len;
    if (value_len >= n &&
        (selector->observer_ignore_case
             ? ascii_case_equal_prefix(value, selector->value, n)
             : memcmp(value, selector->value, n) == 0)) {
      hit_mark_fast(doc, selector);
    }
  }
}

static void observe_prepared_exact_value(eval_doc *doc, const char *value,
                                         size_t value_len, int is_container,
                                         int is_null) {
  const lql_selector **items;
  const lql_selector *selector;
  const char *needle;
  size_t i;
  size_t j;
  size_t count;
  size_t needle_len;

  if (doc->fast_exact_selector != NULL) {
    selector = doc->fast_exact_selector;
    if (!doc->fast_exact_path_active || is_container || is_null ||
        doc->fast_exact_hit) {
      return;
    }
    needle = selector->value_data;
    if (value_len == selector->value_len &&
        (value_len == 0u || memcmp(value, needle, value_len) == 0)) {
      hit_mark_fast(doc, selector);
    }
    return;
  }
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_EXACT);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_EXACT];
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    if (selector->kind == LQL_SELECTOR_KIND_NE && is_null) {
      hit_mark_fast(doc, selector);
      continue;
    }
    if (is_container || is_null) {
      continue;
    }
    if (selector->kind == LQL_SELECTOR_KIND_EQ) {
      needle = selector->value_data;
      if (value_len == selector->value_len &&
          (value_len == 0u || memcmp(value, needle, value_len) == 0)) {
        hit_mark_fast(doc, selector);
      }
      continue;
    }
    if (selector->kind == LQL_SELECTOR_KIND_NE) {
      needle = selector->value_data;
      if (value_len != selector->value_len ||
          (value_len != 0u && memcmp(value, needle, value_len) != 0)) {
        hit_mark_fast(doc, selector);
      }
      continue;
    }
    for (j = 0u; j < selector->any_count; ++j) {
      needle = selector->any[j];
      needle_len = selector->any_lens[j];
      if (value_len == needle_len &&
          (value_len == 0u || memcmp(value, needle, value_len) == 0)) {
        hit_mark_fast(doc, selector);
        break;
      }
    }
  }
}

static void observe_prepared_temporal_value(eval_doc *doc, const char *value,
                                            int is_container, int is_null) {
  const lql_selector **items;
  const lql_selector *selector;
  size_t i;
  size_t count;
  lql_temporal temporal;
  lql_temporal since_macro;
  int parsed;

  if (is_container || is_null) {
    return;
  }
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_TEMPORAL);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_TEMPORAL];
  if (count == 0u) {
    return;
  }
  parsed = lql_parse_temporal_literal(value, &temporal);
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    switch (selector->kind) {
    case LQL_SELECTOR_KIND_EQ:
      if (parsed && lql_temporal_equal(&temporal, &selector->temporal_eq)) {
        hit_mark_fast(doc, selector);
      }
      break;
    case LQL_SELECTOR_KIND_NE:
      if (!parsed || !lql_temporal_equal(&temporal, &selector->temporal_eq)) {
        hit_mark_fast(doc, selector);
      }
      break;
    case LQL_SELECTOR_KIND_RANGE:
      if (parsed &&
          (!selector->has_temporal_gt ||
           lql_temporal_compare(&temporal, &selector->temporal_gt) > 0) &&
          (!selector->has_temporal_gte ||
           lql_temporal_compare(&temporal, &selector->temporal_gte) >= 0) &&
          (!selector->has_temporal_lt ||
           lql_temporal_compare(&temporal, &selector->temporal_lt) < 0) &&
          (!selector->has_temporal_lte ||
           lql_temporal_compare(&temporal, &selector->temporal_lte) <= 0)) {
        hit_mark_fast(doc, selector);
      }
      break;
    case LQL_SELECTOR_KIND_DATE:
      if (parsed &&
          (selector->since_macro == LQL_SINCE_NONE ||
           resolve_since_macro(selector->since_macro, &since_macro)) &&
          (!selector->has_temporal_eq ||
           lql_temporal_equal(&temporal, &selector->temporal_eq)) &&
          (selector->since_macro == LQL_SINCE_NONE ||
           lql_temporal_compare(&temporal, &since_macro) >= 0) &&
          (!selector->has_temporal_gt ||
           lql_temporal_compare(&temporal, &selector->temporal_gt) > 0) &&
          (!selector->has_temporal_gte ||
           lql_temporal_compare(&temporal, &selector->temporal_gte) >= 0) &&
          (!selector->has_temporal_lt ||
           lql_temporal_compare(&temporal, &selector->temporal_lt) < 0) &&
          (!selector->has_temporal_lte ||
           lql_temporal_compare(&temporal, &selector->temporal_lte) <= 0)) {
        hit_mark_fast(doc, selector);
      }
      break;
    default:
      break;
    }
  }
}

static void observe_prepared_numeric_range_value(eval_doc *doc,
                                                 const char *value,
                                                 int is_number,
                                                 int is_container,
                                                 int is_null) {
  const lql_selector **items;
  const lql_selector *selector;
  size_t i;
  size_t count;
  double number;

  if (!is_number || is_container || is_null) {
    return;
  }
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_NUMERIC_RANGE);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_NUMERIC_RANGE];
  if (count == 0u) {
    return;
  }
  number = strtod(value, NULL);
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    if ((!selector->has_range_gt || number > selector->range_gt) &&
        (!selector->has_range_gte || number >= selector->range_gte) &&
        (!selector->has_range_lt || number < selector->range_lt) &&
        (!selector->has_range_lte || number <= selector->range_lte)) {
      hit_mark_fast(doc, selector);
    }
  }
}

static void observe_prepared_exists_value(eval_doc *doc, int is_null) {
  const lql_selector **items;
  size_t i;
  size_t count;

  if (is_null) {
    return;
  }
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_EXISTS);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_EXISTS];
  for (i = 0u; i < count; ++i) {
    hit_mark_fast(doc, items[i]);
  }
}

static void observe_contains_stream_begin(eval_doc *doc,
                                          const lql_selector *selector,
                                          const lonejson_value_path *path) {
  const lql_selector **items;
  size_t *position;
  size_t i;
  size_t j;
  size_t count;

  (void)selector;
  (void)path;
  if (doc->hits == NULL) {
    return;
  }
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_CONTAINS);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_CONTAINS];
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    position = contains_position_ptr(doc, selector);
    if (position != NULL) {
      *position = 0u;
    }
    if (selector->any_count == 0u) {
      if (!selector->value_set && selector->value == NULL) {
        hit_mark_fast(doc, selector);
      } else if (selector->value == NULL || selector->value[0] == '\0') {
        hit_mark_fast(doc, selector);
      }
      continue;
    }
    for (j = 0u; j < selector->any_count; ++j) {
      if (selector->any_lens[j] == 0u) {
        hit_mark_fast(doc, selector);
        break;
      }
    }
  }
}

static void observe_prefix_stream_begin(eval_doc *doc,
                                        const lql_selector *selector,
                                        const lonejson_value_path *path) {
  const lql_selector **items;
  size_t i;
  size_t count;

  (void)selector;
  (void)path;
  if (doc->hits == NULL || doc->stream_misses == NULL) {
    return;
  }
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_PREFIX);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_PREFIX];
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    if (!selector->value_set && selector->value == NULL) {
      hit_mark_fast(doc, selector);
    }
  }
}

static void observe_in_stream_begin(eval_doc *doc, const lql_selector *selector,
                                    const lonejson_value_path *path) {
  const lql_selector **items;
  size_t i;
  size_t j;
  size_t count;
  unsigned int *matches;

  (void)selector;
  (void)path;
  if (doc->in_matches == NULL || doc->in_match_stride == 0u) {
    return;
  }
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_EXACT);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_EXACT];
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    if (selector->kind != LQL_SELECTOR_KIND_IN) {
      continue;
    }
    matches = in_match_row_fast(doc, selector);
    for (j = 0u; j < selector->any_count; ++j) {
      matches[j] = doc->candidate_epoch;
    }
  }
}

static int literal_chunk_matches(const char *literal, size_t literal_len,
                                 size_t offset, const char *data, size_t len,
                                 int ignore_case, size_t limit_len) {
  size_t i;
  size_t compare_len;

  if (offset >= limit_len) {
    return 1;
  }
  compare_len = limit_len - offset;
  if (compare_len > len) {
    compare_len = len;
  }
  if (offset + compare_len > literal_len) {
    return 0;
  }
  if (!ignore_case) {
    return memcmp(data, literal + offset, compare_len) == 0;
  }
  for (i = 0u; i < compare_len; ++i) {
    if (ascii_lower_byte((unsigned char)data[i]) !=
        ascii_lower_byte((unsigned char)literal[offset + i])) {
      return 0;
    }
  }
  return 1;
}

static void observe_prefix_stream_chunk(eval_doc *doc,
                                        const lql_selector *selector,
                                        const lonejson_value_path *path,
                                        const char *data, size_t len) {
  const lql_selector **items;
  size_t i;
  size_t offset;
  size_t value_len;
  size_t count;
  int ignore_case;

  (void)selector;
  (void)path;
  if (doc->hits == NULL || doc->stream_misses == NULL ||
      doc->scalar_len < len) {
    return;
  }
  offset = doc->scalar_len - len;
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_PREFIX);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_PREFIX];
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    if (hit_marked_fast(doc, selector) ||
        stream_miss_marked_fast(doc, selector)) {
      continue;
    }
    value_len = selector->value_len;
    if (value_len == 0u) {
      continue;
    }
    ignore_case = selector->observer_ignore_case;
    if (!literal_chunk_matches(selector->value_data, value_len, offset, data,
                               len, ignore_case, value_len)) {
      stream_miss_mark_fast(doc, selector);
    }
  }
}

static void observe_exact_stream_chunk(eval_doc *doc,
                                       const lql_selector *selector,
                                       const lonejson_value_path *path,
                                       const char *data, size_t len) {
  const lql_selector **items;
  size_t i;
  size_t j;
  size_t offset;
  size_t value_len;
  size_t count;
  const char *value;
  unsigned int *matches;

  (void)selector;
  (void)path;
  if (doc->fast_exact_selector != NULL) {
    selector = doc->fast_exact_selector;
    if (!doc->fast_exact_path_active || doc->fast_exact_hit ||
        doc->fast_exact_miss || doc->scalar_len < len) {
      return;
    }
    offset = doc->scalar_len - len;
    value_len = selector->value_len;
    if (offset >= value_len ||
        !literal_chunk_matches(selector->value_data, value_len, offset, data,
                               len, 0, value_len)) {
      doc->fast_exact_miss = 1;
    }
    return;
  }
  if (doc->hits == NULL || doc->stream_misses == NULL ||
      doc->scalar_len < len) {
    return;
  }
  offset = doc->scalar_len - len;
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_EXACT);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_EXACT];
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    if (hit_marked_fast(doc, selector) ||
        stream_miss_marked_fast(doc, selector)) {
      continue;
    }
    if (selector->kind == LQL_SELECTOR_KIND_IN) {
      matches = in_match_row_fast(doc, selector);
      for (j = 0u; j < selector->any_count; ++j) {
        if (matches[j] == doc->candidate_epoch) {
          value = selector->any[j];
          value_len = selector->any_lens[j];
          if (offset >= value_len) {
            matches[j] = 0u;
            continue;
          }
          if (!literal_chunk_matches(value, value_len, offset, data, len, 0,
                                     value_len)) {
            matches[j] = 0u;
          }
        }
      }
      continue;
    }
    value = selector->value_data;
    value_len = selector->value_len;
    if (offset >= value_len) {
      if (selector->kind == LQL_SELECTOR_KIND_NE) {
        hit_mark_fast(doc, selector);
      } else {
        stream_miss_mark_fast(doc, selector);
      }
      continue;
    }
    if (!literal_chunk_matches(value, value_len, offset, data, len, 0,
                               value_len)) {
      stream_miss_mark_fast(doc, selector);
    }
  }
}

static void observe_scalar_exists_begin(eval_doc *doc,
                                        const lql_selector *selector,
                                        const lonejson_value_path *path) {
  const lql_selector **items;
  size_t i;
  size_t count;

  (void)selector;
  (void)path;
  if (doc->hits == NULL || doc->selector == NULL ||
      (doc->selector->predicate_features & LQL_SELECTOR_FEATURE_EXISTS) == 0u) {
    return;
  }
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_EXISTS);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_EXISTS];
  for (i = 0u; i < count; ++i) {
    hit_mark_fast(doc, items[i]);
  }
}

static void observe_prefix_stream_end(eval_doc *doc,
                                      const lql_selector *selector,
                                      const lonejson_value_path *path) {
  const lql_selector **items;
  size_t i;
  size_t value_len;
  size_t count;
  int ignore_case;

  (void)selector;
  (void)path;
  if (doc->hits == NULL) {
    return;
  }
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_PREFIX);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_PREFIX];
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    if (hit_marked_fast(doc, selector)) {
      continue;
    }
    value_len = selector->value_len;
    if (doc->scalar_len < value_len || stream_miss_marked_fast(doc, selector)) {
      continue;
    }
    if (value_len <= doc->prefix_len) {
      ignore_case = selector->observer_ignore_case;
      if (ignore_case &&
          !ascii_case_equal_prefix(doc->prefix_buf, selector->value_data,
                                   value_len)) {
        continue;
      }
      if (!ignore_case &&
          memcmp(doc->prefix_buf, selector->value_data, value_len) != 0) {
        continue;
      }
    }
    hit_mark_fast(doc, selector);
  }
}

static void observe_exact_stream_end(eval_doc *doc,
                                     const lql_selector *selector,
                                     const lonejson_value_path *path) {
  const lql_selector **items;
  size_t i;
  size_t j;
  size_t value_len;
  size_t count;
  const char *value;
  unsigned int *matches;
  int missed;

  (void)selector;
  (void)path;
  if (doc->fast_exact_selector != NULL) {
    selector = doc->fast_exact_selector;
    if (doc->fast_exact_path_active && !doc->fast_exact_hit &&
        !doc->fast_exact_miss && doc->scalar_len == selector->value_len) {
      hit_mark_fast(doc, selector);
    }
    return;
  }
  if (doc->hits == NULL) {
    return;
  }
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_EXACT);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_EXACT];
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    if (hit_marked_fast(doc, selector)) {
      continue;
    }
    if (selector->kind == LQL_SELECTOR_KIND_EQ ||
        selector->kind == LQL_SELECTOR_KIND_NE) {
      value = selector->value_data;
      value_len = selector->value_len;
      missed = stream_miss_marked_fast(doc, selector);
      if (selector->kind == LQL_SELECTOR_KIND_EQ &&
          doc->scalar_len == value_len && !missed &&
          (value_len > doc->prefix_len ||
           memcmp(doc->prefix_buf, value, value_len) == 0)) {
        hit_mark_fast(doc, selector);
      } else if (selector->kind == LQL_SELECTOR_KIND_NE &&
                 (doc->scalar_len != value_len || missed ||
                  (value_len <= doc->prefix_len &&
                   memcmp(doc->prefix_buf, value, value_len) != 0))) {
        hit_mark_fast(doc, selector);
      }
      continue;
    }
    matches = in_match_row_fast(doc, selector);
    for (j = 0u; j < selector->any_count; ++j) {
      value = selector->any[j];
      value_len = selector->any_lens[j];
      if (doc->scalar_len == value_len &&
          (matches[j] == doc->candidate_epoch ||
           (value_len <= doc->prefix_len &&
            memcmp(doc->prefix_buf, value, value_len) == 0))) {
        hit_mark_fast(doc, selector);
        break;
      }
    }
  }
}

static void observe_temporal_stream_end(eval_doc *doc,
                                        const lql_selector *selector,
                                        const lonejson_value_path *path) {
  const lql_selector **items;
  size_t i;
  size_t count;
  lql_temporal temporal;
  lql_temporal since_macro;
  int parsed;

  (void)selector;
  (void)path;
  if (doc->hits == NULL) {
    return;
  }
  parsed = doc->scalar_len == doc->prefix_len &&
           doc->prefix_len <= LQL_EVAL_TEMPORAL_CAP &&
           lql_parse_temporal_literal(doc->prefix_buf, &temporal);
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_TEMPORAL);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_TEMPORAL];
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    if (hit_marked_fast(doc, selector)) {
      continue;
    }
    switch (selector->kind) {
    case LQL_SELECTOR_KIND_EQ:
      if (parsed && lql_temporal_equal(&temporal, &selector->temporal_eq)) {
        hit_mark_fast(doc, selector);
      }
      break;
    case LQL_SELECTOR_KIND_NE:
      if (!parsed || !lql_temporal_equal(&temporal, &selector->temporal_eq)) {
        hit_mark_fast(doc, selector);
      }
      break;
    case LQL_SELECTOR_KIND_RANGE:
      if (parsed &&
          (!selector->has_temporal_gt ||
           lql_temporal_compare(&temporal, &selector->temporal_gt) > 0) &&
          (!selector->has_temporal_gte ||
           lql_temporal_compare(&temporal, &selector->temporal_gte) >= 0) &&
          (!selector->has_temporal_lt ||
           lql_temporal_compare(&temporal, &selector->temporal_lt) < 0) &&
          (!selector->has_temporal_lte ||
           lql_temporal_compare(&temporal, &selector->temporal_lte) <= 0)) {
        hit_mark_fast(doc, selector);
      }
      break;
    case LQL_SELECTOR_KIND_DATE:
      if (parsed &&
          (selector->since_macro == LQL_SINCE_NONE ||
           resolve_since_macro(selector->since_macro, &since_macro)) &&
          (!selector->has_temporal_eq ||
           lql_temporal_equal(&temporal, &selector->temporal_eq)) &&
          (selector->since_macro == LQL_SINCE_NONE ||
           lql_temporal_compare(&temporal, &since_macro) >= 0) &&
          (!selector->has_temporal_gt ||
           lql_temporal_compare(&temporal, &selector->temporal_gt) > 0) &&
          (!selector->has_temporal_gte ||
           lql_temporal_compare(&temporal, &selector->temporal_gte) >= 0) &&
          (!selector->has_temporal_lt ||
           lql_temporal_compare(&temporal, &selector->temporal_lt) < 0) &&
          (!selector->has_temporal_lte ||
           lql_temporal_compare(&temporal, &selector->temporal_lte) <= 0)) {
        hit_mark_fast(doc, selector);
      }
      break;
    default:
      break;
    }
  }
}

static void numeric_stream_reset(eval_doc *doc) {
  doc->number_negative = 0;
  doc->number_after_decimal = 0;
  doc->number_in_exp = 0;
  doc->number_exp_negative = 0;
  doc->number_nonzero_seen = 0;
  doc->number_first_sig_integer = 0;
  doc->number_sig_truncated = 0;
  doc->number_int_digits = 0u;
  doc->number_frac_leading_zeros = 0u;
  doc->number_sig_len = 0u;
  doc->number_exp_value = 0L;
  doc->number_sig[0] = '\0';
}

static void numeric_stream_record_digit(eval_doc *doc, char c) {
  if (!doc->number_nonzero_seen) {
    if (c == '0') {
      if (doc->number_after_decimal) {
        ++doc->number_frac_leading_zeros;
      }
      return;
    }
    doc->number_nonzero_seen = 1;
    doc->number_first_sig_integer = !doc->number_after_decimal;
  }
  if (doc->number_sig_len < LQL_EVAL_NUMERIC_SIG_CAP) {
    doc->number_sig[doc->number_sig_len] = c;
    ++doc->number_sig_len;
    doc->number_sig[doc->number_sig_len] = '\0';
  } else {
    doc->number_sig_truncated = 1;
  }
}

static void numeric_stream_update(eval_doc *doc, const char *data, size_t len) {
  size_t i;
  unsigned char c;

  for (i = 0u; i < len; ++i) {
    c = (unsigned char)data[i];
    if (doc->number_in_exp) {
      if (c == '-' && doc->number_exp_value == 0L) {
        doc->number_exp_negative = 1;
      } else if (c == '+' && doc->number_exp_value == 0L) {
      } else if (ascii_is_digit(c)) {
        if (doc->number_exp_value < LQL_EVAL_NUMERIC_EXP_CAP) {
          doc->number_exp_value = doc->number_exp_value * 10L + (long)(c - '0');
          if (doc->number_exp_value > LQL_EVAL_NUMERIC_EXP_CAP) {
            doc->number_exp_value = LQL_EVAL_NUMERIC_EXP_CAP;
          }
        }
      }
      continue;
    }
    if (c == '-') {
      doc->number_negative = 1;
    } else if (c == '.') {
      doc->number_after_decimal = 1;
    } else if (c == 'e' || c == 'E') {
      doc->number_in_exp = 1;
    } else if (ascii_is_digit(c)) {
      if (!doc->number_after_decimal) {
        ++doc->number_int_digits;
      }
      numeric_stream_record_digit(doc, (char)c);
    }
  }
}

static double numeric_stream_value(const eval_doc *doc) {
  char number_buf[96];
  long exp10;
  long explicit_exp;

  if (doc->scalar_len == doc->prefix_len) {
    return strtod(doc->prefix_buf, NULL);
  }
  if (!doc->number_nonzero_seen || doc->number_sig_len == 0u) {
    return doc->number_negative ? -0.0 : 0.0;
  }
  explicit_exp =
      doc->number_exp_negative ? -doc->number_exp_value : doc->number_exp_value;
  if (doc->number_first_sig_integer) {
    if (doc->number_int_digits > doc->number_sig_len) {
      exp10 = (long)(doc->number_int_digits - doc->number_sig_len);
    } else {
      exp10 = -(long)(doc->number_sig_len - doc->number_int_digits);
    }
  } else {
    exp10 = -(long)(doc->number_frac_leading_zeros + doc->number_sig_len);
  }
  exp10 += explicit_exp;
  sprintf(number_buf, "%s%se%ld", doc->number_negative ? "-" : "",
          doc->number_sig, exp10);
  return strtod(number_buf, NULL);
}

static void observe_numeric_range_stream_end(eval_doc *doc,
                                             const lql_selector *selector,
                                             const lonejson_value_path *path) {
  const lql_selector **items;
  size_t i;
  size_t count;
  double number;

  (void)selector;
  (void)path;
  if (doc->hits == NULL) {
    return;
  }
  number = numeric_stream_value(doc);
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_NUMERIC_RANGE);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_NUMERIC_RANGE];
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    if (hit_marked_fast(doc, selector)) {
      continue;
    }
    if ((!selector->has_range_gt || number > selector->range_gt) &&
        (!selector->has_range_gte || number >= selector->range_gte) &&
        (!selector->has_range_lt || number < selector->range_lt) &&
        (!selector->has_range_lte || number <= selector->range_lte)) {
      hit_mark_fast(doc, selector);
    }
  }
}

static int contains_stream_boundary_scan(const char *tail, size_t tail_len,
                                         const char *data, size_t len,
                                         const char *needle, size_t needle_len,
                                         int ignore_case) {
  size_t start;
  size_t end;
  size_t data_part;
  size_t min_tail_len;
  size_t tail_part;
  unsigned char a;
  unsigned char first;

  if (tail_len == 0u || len == 0u || needle_len <= 1u) {
    return 0;
  }
  if (tail_len + len < needle_len) {
    return 0;
  }
  start = tail_len >= needle_len ? tail_len - needle_len + 1u : 0u;
  end = tail_len;
  if (needle_len > len) {
    min_tail_len = needle_len - len;
    if (min_tail_len > tail_len) {
      return 0;
    }
    end = tail_len - min_tail_len + 1u;
  }
  if (start >= end) {
    return 0;
  }
  first = (unsigned char)needle[0];
  if (ignore_case) {
    first = ascii_lower_byte(first);
  }
  for (; start < end; ++start) {
    a = (unsigned char)tail[start];
    if (ignore_case) {
      a = ascii_lower_byte(a);
    }
    if (a != first) {
      continue;
    }
    tail_part = tail_len - start;
    data_part = needle_len - tail_part;
    if (tail_part > 1u) {
      if (ignore_case) {
        if (!ascii_case_equal_prefix(tail + start + 1u, needle + 1u,
                                     tail_part - 1u)) {
          continue;
        }
      } else if (memcmp(tail + start + 1u, needle + 1u, tail_part - 1u) != 0) {
        continue;
      }
    }
    if (ignore_case) {
      if (ascii_case_equal_prefix(data, needle + tail_part, data_part)) {
        return 1;
      }
    } else if (memcmp(data, needle + tail_part, data_part) == 0) {
      return 1;
    }
  }
  return 0;
}

static int contains_stream_scan(const char *tail, size_t tail_len,
                                const char *data, size_t len,
                                const char *needle, size_t needle_len,
                                int ignore_case) {
  if (contains_case_len(data, len, needle, needle_len, ignore_case)) {
    return 1;
  }
  return contains_stream_boundary_scan(tail, tail_len, data, len, needle,
                                       needle_len, ignore_case);
}

static size_t *contains_position_ptr(eval_doc *doc,
                                     const lql_selector *selector) {
  if (doc == NULL || selector == NULL || doc->contains_positions == NULL ||
      selector->hit_index >= doc->contains_positions_cap) {
    return NULL;
  }
  return &doc->contains_positions[selector->hit_index];
}

static int contains_stream_kmp_scan(eval_doc *doc, const lql_selector *selector,
                                    const char *data, size_t len) {
  size_t *pos;
  size_t matched;
  size_t i;
  size_t needle_len;
  unsigned char a;
  unsigned char b;
  int ignore_case;

  if (selector == NULL || selector->contains_lps == NULL ||
      selector->contains_lps_len == 0u || selector->value_data == NULL) {
    return 0;
  }
  pos = contains_position_ptr(doc, selector);
  if (pos == NULL) {
    return 0;
  }
  matched = *pos;
  needle_len = selector->contains_lps_len;
  ignore_case = selector->observer_ignore_case;
  for (i = 0u; i < len; ++i) {
    a = (unsigned char)data[i];
    if (ignore_case) {
      a = ascii_lower_byte(a);
    }
    while (matched != 0u) {
      b = (unsigned char)selector->value_data[matched];
      if (ignore_case) {
        b = ascii_lower_byte(b);
      }
      if (a == b) {
        break;
      }
      matched = selector->contains_lps[matched - 1u];
    }
    b = (unsigned char)selector->value_data[matched];
    if (ignore_case) {
      b = ascii_lower_byte(b);
    }
    if (a == b) {
      ++matched;
      if (matched == needle_len) {
        *pos = selector->contains_lps[matched - 1u];
        return 1;
      }
    }
  }
  *pos = matched;
  return 0;
}

static int contains_any_stream_scan(const char *tail, size_t tail_len,
                                    const char *data, size_t len,
                                    char **needles, const size_t *needle_lens,
                                    const unsigned char *firsts,
                                    const unsigned char *first_bitmap,
                                    size_t count, int ignore_case) {
  size_t i;
  size_t needle_len;
  unsigned char ch;

  if (count == 1u) {
    return contains_stream_scan(tail, tail_len, data, len, needles[0],
                                needle_lens[0], ignore_case);
  }
  if (contains_any_case_len(data, len, needles, needle_lens, count, firsts,
                            first_bitmap, ignore_case)) {
    return 1;
  }
  if (tail_len == 0u || len == 0u) {
    return 0;
  }
  for (i = 0u; i < tail_len; ++i) {
    ch = (unsigned char)tail[i];
    if (ignore_case) {
      ch = ascii_lower_byte(ch);
    }
    if ((first_bitmap[ch >> 3] & (unsigned char)(1u << (ch & 7u))) != 0u) {
      break;
    }
  }
  if (i == tail_len) {
    return 0;
  }
  for (i = 0u; i < count; ++i) {
    needle_len = needle_lens[i];
    if (contains_stream_boundary_scan(tail, tail_len, data, len, needles[i],
                                      needle_len, ignore_case)) {
      return 1;
    }
  }
  return 0;
}

static void observe_contains_stream_chunk(eval_doc *doc,
                                          const lql_selector *selector,
                                          const lonejson_value_path *path,
                                          const char *data, size_t len) {
  const lql_selector **items;
  size_t i;
  size_t value_len;
  size_t count;
  const char *tail;
  int ignore_case;

  (void)selector;
  (void)path;
  if (doc->hits == NULL) {
    return;
  }
  tail = contains_tail_data(doc);
  items = scalar_family_begin(doc, LQL_EVAL_FAMILY_CONTAINS);
  count = doc->scalar_family_counts[LQL_EVAL_FAMILY_CONTAINS];
  for (i = 0u; i < count; ++i) {
    selector = items[i];
    if (hit_marked_fast(doc, selector)) {
      continue;
    }
    ignore_case = selector->observer_ignore_case;
    if (selector->any_count == 0u) {
      value_len = selector->value_len;
      if (contains_stream_kmp_scan(doc, selector, data, len) ||
          (selector->contains_lps == NULL &&
           contains_stream_scan(tail, doc->contains_tail_len, data, len,
                                selector->value_data, value_len,
                                ignore_case))) {
        hit_mark_fast(doc, selector);
      }
    } else if (selector->any_count == 1u) {
      if (contains_stream_scan(tail, doc->contains_tail_len, data, len,
                               selector->any[0], selector->any_lens[0],
                               ignore_case)) {
        hit_mark_fast(doc, selector);
      }
    } else if (contains_any_stream_scan(
                   tail, doc->contains_tail_len, data, len, selector->any,
                   selector->any_lens,
                   ignore_case ? selector->any_ifirsts : selector->any_firsts,
                   ignore_case ? selector->any_ifirst_bitmap
                               : selector->any_first_bitmap,
                   selector->any_count, ignore_case)) {
      hit_mark_fast(doc, selector);
    }
  }
}

static void contains_stream_update_tail(eval_doc *doc, const char *data,
                                        size_t len) {
  char *tail;
  size_t drop_len;
  size_t total_len;
  size_t keep_len;

  if (doc->contains_tail_need == 0u) {
    doc->contains_tail_len = 0u;
    return;
  }
  tail = contains_tail_data(doc);
  if (tail == NULL) {
    doc->contains_tail_len = 0u;
    return;
  }
  if (len >= doc->contains_tail_need) {
    memcpy(tail, data + len - doc->contains_tail_need, doc->contains_tail_need);
    doc->contains_tail_len = doc->contains_tail_need;
    return;
  }
  total_len = doc->contains_tail_len + len;
  keep_len = total_len;
  if (keep_len > doc->contains_tail_need) {
    keep_len = doc->contains_tail_need;
  }
  if (total_len > keep_len) {
    drop_len = total_len - keep_len;
    if (drop_len >= doc->contains_tail_len) {
      memcpy(tail, data + drop_len - doc->contains_tail_len, keep_len);
      doc->contains_tail_len = keep_len;
      return;
    }
    memmove(tail, tail + drop_len, doc->contains_tail_len - drop_len);
    doc->contains_tail_len -= drop_len;
  }
  memcpy(tail + doc->contains_tail_len, data, len);
  doc->contains_tail_len = keep_len;
}

static void prefix_stream_update(eval_doc *doc, const char *data, size_t len) {
  size_t keep_len;

  if (doc->prefix_need == 0u || doc->prefix_len >= doc->prefix_need) {
    return;
  }
  keep_len = doc->prefix_need - doc->prefix_len;
  if (keep_len > len) {
    keep_len = len;
  }
  memcpy(doc->prefix_buf + doc->prefix_len, data, keep_len);
  doc->prefix_len += keep_len;
  if (doc->prefix_len <= LQL_EVAL_PREFIX_CAP) {
    doc->prefix_buf[doc->prefix_len] = '\0';
  }
}

static void observe_prepared_value(eval_doc *doc, const char *value,
                                   int is_number, int is_container,
                                   int is_null) {
  unsigned int features;
  size_t value_len;
  if (doc->selector == NULL || doc->selector->kind == LQL_SELECTOR_KIND_ALL) {
    return;
  }
  features = doc->scalar_path_features;
  value_len = 0u;
  if ((features & (LQL_SELECTOR_FEATURE_CONTAINS | LQL_SELECTOR_FEATURE_PREFIX |
                   LQL_SELECTOR_FEATURE_EXACT)) != 0u) {
    value_len = is_container || is_null ? 0u : strlen(value);
  }
  if ((features & LQL_SELECTOR_FEATURE_CONTAINS) != 0u) {
    observe_prepared_contains_value(doc, value, value_len, is_container,
                                    is_null);
  }
  if ((features & LQL_SELECTOR_FEATURE_PREFIX) != 0u) {
    observe_prepared_prefix_value(doc, value, value_len, is_container, is_null);
  }
  if ((features & LQL_SELECTOR_FEATURE_EXACT) != 0u) {
    observe_prepared_exact_value(doc, value, value_len, is_container, is_null);
  }
  if ((features & LQL_SELECTOR_FEATURE_TEMPORAL) != 0u) {
    observe_prepared_temporal_value(doc, value, is_container, is_null);
  }
  if ((features & LQL_SELECTOR_FEATURE_NUMERIC_RANGE) != 0u) {
    observe_prepared_numeric_range_value(doc, value, is_number, is_container,
                                         is_null);
  }
  if ((features & LQL_SELECTOR_FEATURE_EXISTS) != 0u) {
    observe_prepared_exists_value(doc, is_null);
  }
}

static void observe_container_value(eval_doc *doc,
                                    const lonejson_value_path *path) {
  unsigned int container_features;

  if (doc->selector == NULL || doc->selector->kind == LQL_SELECTOR_KIND_ALL) {
    return;
  }
  container_features = doc->selector->container_observer_features;
  if (container_features == 0u) {
    return;
  }
  path_match_prepare(doc, path, container_features);
  if (doc->scalar_path_features == 0u) {
    return;
  }
  observe_prepared_value(doc, "", 0, 1, 0);
}

static int eval_selector_tree(const lql_selector *selector,
                              const eval_doc *doc) {
  size_t i;
  switch (selector->kind) {
  case LQL_SELECTOR_KIND_ALL:
    return 1;
  case LQL_SELECTOR_KIND_AND:
    for (i = 0u; i < selector->child_count; ++i) {
      if (!eval_selector_tree(&selector->children[i], doc)) {
        return 0;
      }
    }
    return 1;
  case LQL_SELECTOR_KIND_OR:
    for (i = 0u; i < selector->child_count; ++i) {
      if (eval_selector_tree(&selector->children[i], doc)) {
        return 1;
      }
    }
    return 0;
  case LQL_SELECTOR_KIND_NOT:
    return selector->child_count == 0u
               ? 1
               : !eval_selector_tree(&selector->children[0], doc);
  default:
    return doc != NULL &&
           (doc->fast_exact_selector == selector || doc->hits != NULL) &&
           hit_marked_fast(doc, selector);
  }
}

static int eval_doc_matches(const lql_selector *selector, const eval_doc *doc) {
  if (selector == NULL || selector->kind == LQL_SELECTOR_KIND_ALL) {
    return 1;
  }
  if (doc != NULL && doc->candidate_matched &&
      selector->match_sticky_once_true) {
    return 1;
  }
  return eval_selector_tree(selector, doc);
}

static lonejson_status on_object_begin(void *user,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  if (path->segment_count == 0u) {
    doc->root_kind = '{';
  }
  observe_container_value(doc, path);
  return push_container(doc, path, '{');
}

static lonejson_status
on_object_begin_root_only(void *user, const lonejson_value_path *path,
                          lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  if (path->segment_count == 0u) {
    doc->root_kind = '{';
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_object_end(void *user,
                                     const lonejson_value_path *path,
                                     lonejson_error *error) {
  (void)error;
  pop_container((eval_doc *)user, path);
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_array_begin(void *user,
                                      const lonejson_value_path *path,
                                      lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  if (path->segment_count == 0u) {
    doc->root_kind = '[';
  }
  observe_container_value(doc, path);
  return push_container(doc, path, '[');
}

static lonejson_status on_array_begin_root_only(void *user,
                                                const lonejson_value_path *path,
                                                lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  if (path->segment_count == 0u) {
    doc->root_kind = '[';
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_array_end(void *user, const lonejson_value_path *path,
                                    lonejson_error *error) {
  (void)error;
  pop_container((eval_doc *)user, path);
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_string_begin(void *user,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  doc->scalar_len = 0u;
  doc->contains_tail_len = 0u;
  doc->contains_tail_need = 0u;
  doc->prefix_len = 0u;
  doc->prefix_need = 0u;
  scalar_path_match_prepare(doc, path);
  if (doc->scalar_path_features == 0u) {
    doc->scalar_stream_features = 0u;
    return LONEJSON_STATUS_OK;
  }
  if (doc->prefix_need != 0u) {
    doc->prefix_buf[0] = '\0';
  }
  observe_scalar_exists_begin(doc, doc->selector, path);
  doc->scalar_stream_features =
      doc->scalar_path_features &
      (LQL_SELECTOR_FEATURE_CONTAINS | LQL_SELECTOR_FEATURE_PREFIX |
       LQL_SELECTOR_FEATURE_EXACT | LQL_SELECTOR_FEATURE_TEMPORAL);
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_PREFIX) == 0u &&
      (doc->scalar_stream_features &
       (LQL_SELECTOR_FEATURE_EXACT | LQL_SELECTOR_FEATURE_TEMPORAL)) != 0u) {
    doc->scalar_stream_features |= LQL_EVAL_FEATURE_PREFIX_CAPTURE;
  }
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_CONTAINS) != 0u) {
    if (!ensure_contains_tail(doc)) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    observe_contains_stream_begin(doc, doc->selector, path);
  }
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_PREFIX) != 0u) {
    observe_prefix_stream_begin(doc, doc->selector, path);
  }
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_EXACT) != 0u) {
    observe_in_stream_begin(doc, doc->selector, path);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_string_chunk(void *user,
                                       const lonejson_value_path *path,
                                       const char *data, size_t len,
                                       lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  if (doc->scalar_stream_features == 0u) {
    return LONEJSON_STATUS_OK;
  }
  doc->scalar_len += len;
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_CONTAINS) != 0u) {
    observe_contains_stream_chunk(doc, doc->selector, path, data, len);
    contains_stream_update_tail(doc, data, len);
  }
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_PREFIX) != 0u) {
    observe_prefix_stream_chunk(doc, doc->selector, path, data, len);
    prefix_stream_update(doc, data, len);
  }
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_EXACT) != 0u) {
    observe_exact_stream_chunk(doc, doc->selector, path, data, len);
  }
  if ((doc->scalar_stream_features & LQL_EVAL_FEATURE_PREFIX_CAPTURE) != 0u) {
    prefix_stream_update(doc, data, len);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_string_end(void *user,
                                     const lonejson_value_path *path,
                                     lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  if (path->segment_count == 0u) {
    doc->root_kind = 's';
  }
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_PREFIX) != 0u) {
    observe_prefix_stream_end(doc, doc->selector, path);
  }
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_EXACT) != 0u) {
    observe_exact_stream_end(doc, doc->selector, path);
  }
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_TEMPORAL) != 0u) {
    observe_temporal_stream_end(doc, doc->selector, path);
  }
  doc->scalar_len = 0u;
  doc->scalar_stream_features = 0u;
  doc->contains_tail_len = 0u;
  doc->contains_tail_need = 0u;
  doc->prefix_len = 0u;
  doc->prefix_need = 0u;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_number_begin(void *user,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  eval_doc *doc;
  lonejson_status st;

  st = on_string_begin(user, path, error);
  if (st != LONEJSON_STATUS_OK) {
    return st;
  }
  doc = (eval_doc *)user;
  if (doc->scalar_path_features == 0u) {
    return LONEJSON_STATUS_OK;
  }
  doc->scalar_stream_features |=
      doc->scalar_path_features & LQL_SELECTOR_FEATURE_NUMERIC_RANGE;
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_NUMERIC_RANGE) !=
      0u) {
    if ((doc->scalar_stream_features &
         (LQL_SELECTOR_FEATURE_PREFIX | LQL_SELECTOR_FEATURE_EXACT |
          LQL_SELECTOR_FEATURE_TEMPORAL)) == 0u) {
      doc->scalar_stream_features |= LQL_EVAL_FEATURE_PREFIX_CAPTURE;
    }
    numeric_stream_reset(doc);
    if (doc->prefix_need < LQL_EVAL_NUMERIC_PREFIX_CAP) {
      doc->prefix_need = LQL_EVAL_NUMERIC_PREFIX_CAP;
      doc->prefix_buf[0] = '\0';
    }
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_number_chunk(void *user,
                                       const lonejson_value_path *path,
                                       const char *data, size_t len,
                                       lonejson_error *error) {
  eval_doc *doc;
  lonejson_status st;

  st = on_string_chunk(user, path, data, len, error);
  if (st != LONEJSON_STATUS_OK) {
    return st;
  }
  doc = (eval_doc *)user;
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_NUMERIC_RANGE) !=
      0u) {
    numeric_stream_update(doc, data, len);
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_number_end(void *user,
                                     const lonejson_value_path *path,
                                     lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  if (path->segment_count == 0u) {
    doc->root_kind = 'n';
  }
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_PREFIX) != 0u) {
    observe_prefix_stream_end(doc, doc->selector, path);
  }
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_EXACT) != 0u) {
    observe_exact_stream_end(doc, doc->selector, path);
  }
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_TEMPORAL) != 0u) {
    observe_temporal_stream_end(doc, doc->selector, path);
  }
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_NUMERIC_RANGE) !=
      0u) {
    observe_numeric_range_stream_end(doc, doc->selector, path);
  }
  doc->scalar_len = 0u;
  doc->scalar_stream_features = 0u;
  doc->contains_tail_len = 0u;
  doc->contains_tail_need = 0u;
  doc->prefix_len = 0u;
  doc->prefix_need = 0u;
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_boolean(void *user, const lonejson_value_path *path,
                                  int value, lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  if (path->segment_count == 0u) {
    doc->root_kind = 'b';
  }
  scalar_path_match_prepare(doc, path);
  if (doc->scalar_path_features == 0u) {
    return LONEJSON_STATUS_OK;
  }
  observe_prepared_value(doc, value ? "true" : "false", 0, 0, 0);
  return LONEJSON_STATUS_OK;
}

static lonejson_status on_null(void *user, const lonejson_value_path *path,
                               lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  if (path->segment_count == 0u) {
    doc->root_kind = '0';
  }
  scalar_path_match_prepare(doc, path);
  if (doc->scalar_path_features == 0u) {
    return LONEJSON_STATUS_OK;
  }
  observe_prepared_value(doc, "", 0, 0, 1);
  return LONEJSON_STATUS_OK;
}

static void init_eval_visitor(lonejson_path_value_visitor *visitor) {
  *visitor = lonejson_default_path_value_visitor();
  visitor->object_begin = on_object_begin;
  visitor->object_end = on_object_end;
  visitor->array_begin = on_array_begin;
  visitor->array_end = on_array_end;
  visitor->string_begin = on_string_begin;
  visitor->string_chunk = on_string_chunk;
  visitor->string_end = on_string_end;
  visitor->number_begin = on_number_begin;
  visitor->number_chunk = on_number_chunk;
  visitor->number_end = on_number_end;
  visitor->boolean_value = on_boolean;
  visitor->null_value = on_null;
}

static void configure_eval_visitor_for_doc(lonejson_path_value_visitor *visitor,
                                           const eval_doc *doc) {
  if (visitor != NULL && doc != NULL && !doc->track_container_types &&
      (doc->selector == NULL ||
       doc->selector->container_observer_features == 0u)) {
    visitor->object_begin = on_object_begin_root_only;
    visitor->array_begin = on_array_begin_root_only;
    visitor->object_end = NULL;
    visitor->array_end = NULL;
    return;
  }
  if (visitor != NULL && doc != NULL && !doc->track_container_types) {
    visitor->object_end = NULL;
    visitor->array_end = NULL;
  }
}

typedef struct query_stream_state {
  lql *receiver;
  FILE *file;
  lonejson *capture_runtime;
  lql_uint64 offset_base;
  lql_uint64 index_base;
  const lql_selector *selector;
  lql_query_options options;
  unsigned int limit_flags;
  lql_query_decision_fn on_decision;
  lql_query_match_fn on_match;
  void *user;
  lql_query_result result;
  lql_status callback_status;
  eval_doc doc;
  lonejson_spooled array_spool;
  int array_spool_initialized;
  unsigned char pending_payload_prefix[32];
  size_t pending_payload_prefix_len;
} query_stream_state;

typedef struct source_reader_adapter {
  lql_read_fn read;
  void *user;
  int error_code;
  int prefix_root_array;
  lql_uint64 total_read;
  unsigned char prefix[LQL_SOURCE_PREFIX_CAP];
  size_t prefix_len;
  size_t prefix_offset;
  int prefix_eof;
} source_reader_adapter;

typedef struct eval_pread_range_reader {
  int fd;
  lql_uint64 offset;
  lql_uint64 remaining;
} eval_pread_range_reader;

typedef struct source_spooled_match_state {
  lql *receiver;
  const lql_selector *selector;
  lql_uint64 offset_base;
  lql_uint64 index_base;
  lql_query_options options;
  unsigned int limit_flags;
  lql_query_match_fn on_match;
  void *user;
  lql_query_result result;
  lql_status callback_status;
  int capture_matched;
  eval_doc doc;
} source_spooled_match_state;

typedef struct source_candidate_run_state {
  lql *receiver;
  lql_allocator *allocator;
  const lql_selector *selector;
  const lql_projection *projection;
  const lql_mutation_plan *mutation_plan;
  lql_query_options options;
  unsigned int limit_flags;
  int matches_only;
  lql_query_result result;
  eval_doc doc;
  int *applied;
  int inline_applied[16];
  void *applied_alloc;
  size_t active_item;
  int active_item_set;
  int current_matched;
  int current_matched_known;
  int current_root_object;
  int current_output_enabled;
  int transform_failed;
  int staged_output;
  FILE *out;
  lql_status callback_status;
  lql_error transform_error;
} source_candidate_run_state;

static int source_candidate_run_path_segment_is_array(
    const lonejson_candidate_run_event *event, size_t index) {
  return lonejson_candidate_run_path_segment_is_array(event, index);
}

static int source_candidate_run_segment_matches(
    unsigned char expected_kind, const char *expected, size_t expected_len,
    const lonejson_path_segment *actual, int actual_is_array) {
  switch (expected_kind) {
  case LQL_MUTATION_PATH_OBJECT_WILDCARD:
    return !actual_is_array;
  case LQL_MUTATION_PATH_ARRAY_WILDCARD:
    return actual_is_array;
  case LQL_MUTATION_PATH_RECURSIVE:
    return 1;
  default:
    return expected_len == actual->len &&
           memcmp(expected, actual->data, actual->len) == 0;
  }
}

static int source_candidate_run_path_matches_from(
    const lonejson_candidate_run_event *event,
    const lql_mutation_path *item_path, size_t item_index,
    const lonejson_value_path *path, size_t path_index) {
  size_t i;
  while (item_index < item_path->segment_count) {
    if (item_path->segment_kinds[item_index] == LQL_MUTATION_PATH_ELLIPSIS) {
      if (item_index + 1u == item_path->segment_count) {
        return 1;
      }
      for (i = path_index; i <= path->segment_count; ++i) {
        if (source_candidate_run_path_matches_from(event, item_path,
                                                   item_index + 1u, path, i)) {
          return 1;
        }
      }
      return 0;
    }
    if (path_index >= path->segment_count ||
        !source_candidate_run_segment_matches(
            item_path->segment_kinds[item_index],
            item_path->segments[item_index],
            item_path->segment_lens[item_index], &path->segments[path_index],
            source_candidate_run_path_segment_is_array(event, path_index))) {
      return 0;
    }
    ++item_index;
    ++path_index;
  }
  return path_index == path->segment_count;
}

static int source_candidate_run_value_is_array_element(
    const lonejson_candidate_run_event *event) {
  return event->relationship == LONEJSON_CANDIDATE_RUN_EVENT_ARRAY_ELEMENT;
}

static int
source_candidate_run_find_item(source_candidate_run_state *state,
                               const lonejson_candidate_run_event *event,
                               const lonejson_value_path *path, size_t *out) {
  size_t i;
  size_t index;
  size_t start;
  size_t end;
  if (state == NULL || state->mutation_plan == NULL || path == NULL) {
    return 0;
  }
  if (!state->mutation_plan->variable_depth_paths &&
      state->mutation_plan->value_depth_indexes != NULL &&
      state->mutation_plan->value_depth_offsets != NULL &&
      path->segment_count <= state->mutation_plan->max_segment_count) {
    start = state->mutation_plan->value_depth_offsets[path->segment_count];
    end = state->mutation_plan->value_depth_offsets[path->segment_count + 1u];
    for (i = start; i < end; ++i) {
      index = state->mutation_plan->value_depth_indexes[i];
      if (source_candidate_run_path_matches_from(
              event, &state->mutation_plan->items[index].path, 0u, path, 0u)) {
        *out = index;
        return 1;
      }
    }
    return 0;
  }
  for (i = 0u; i < state->mutation_plan->count; ++i) {
    if (source_candidate_run_path_matches_from(
            event, &state->mutation_plan->items[i].path, 0u, path, 0u)) {
      *out = i;
      return 1;
    }
  }
  return 0;
}

static int source_candidate_run_path_is_parent_of_item(
    const lonejson_candidate_run_event *event, const lql_mutation_item *item,
    const lonejson_value_path *path) {
  size_t i;
  if (item == NULL || path == NULL ||
      item->path.segment_count <= path->segment_count) {
    return 0;
  }
  for (i = 0u; i < path->segment_count; ++i) {
    if (!source_candidate_run_segment_matches(
            item->path.segment_kinds[i], item->path.segments[i],
            item->path.segment_lens[i], &path->segments[i],
            source_candidate_run_path_segment_is_array(event, i))) {
      return 0;
    }
  }
  return 1;
}

static int
source_candidate_run_item_child_key_available(const lql_mutation_item *item,
                                              size_t depth) {
  if (item == NULL || depth >= item->path.segment_count) {
    return 0;
  }
  return item->path.segment_kinds[depth] == LQL_MUTATION_PATH_LITERAL;
}

static lonejson_status source_candidate_run_write_item_value(
    source_candidate_run_state *state, const lql_mutation_item *item,
    lonejson_writer *writer, lonejson_error *error) {
  (void)state;
  return lql_mutation_write_item_value(writer, item, error);
}

static lonejson_status source_candidate_run_write_subtree(
    source_candidate_run_state *state, const lql_mutation_item *anchor,
    size_t depth, lonejson_writer *writer, lonejson_error *error) {
  const lql_mutation_item *item;
  size_t i;
  size_t item_index;
  if (state == NULL || state->mutation_plan == NULL || anchor == NULL) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  for (i = 0u; i < state->mutation_plan->create_count; ++i) {
    item_index = state->mutation_plan->create_indexes[i];
    item = &state->mutation_plan->items[item_index];
    if (state->applied[item_index] || item->path.segment_count <= depth ||
        item->path.segment_count < anchor->path.segment_count ||
        !source_candidate_run_item_child_key_available(item, depth) ||
        !source_candidate_run_item_child_key_available(anchor, depth) ||
        item->path.segment_lens[depth] != anchor->path.segment_lens[depth] ||
        memcmp(item->path.segments[depth], anchor->path.segments[depth],
               item->path.segment_lens[depth]) != 0) {
      continue;
    }
    if (lonejson_writer_key(writer, item->path.segments[depth],
                            item->path.segment_lens[depth],
                            error) != LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    if (item->path.segment_count == depth + 1u) {
      if (source_candidate_run_write_item_value(state, item, writer, error) !=
          LONEJSON_STATUS_OK) {
        return LONEJSON_STATUS_CALLBACK_FAILED;
      }
      state->applied[item_index] = 1;
    } else {
      if (lonejson_writer_begin_object(writer, error) != LONEJSON_STATUS_OK ||
          source_candidate_run_write_subtree(state, item, depth + 1u, writer,
                                             error) != LONEJSON_STATUS_OK ||
          lonejson_writer_end_object(writer, error) != LONEJSON_STATUS_OK) {
        return LONEJSON_STATUS_CALLBACK_FAILED;
      }
    }
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status source_candidate_run_insert_missing(
    void *user, const lonejson_candidate_run_event *event,
    lonejson_writer *writer, lonejson_error *error) {
  source_candidate_run_state *state;
  const lql_mutation_item *item;
  size_t i;
  size_t item_index;
  state = (source_candidate_run_state *)user;
  if (state == NULL || state->mutation_plan == NULL || event == NULL ||
      event->path == NULL ||
      event->insert_phase != LONEJSON_CANDIDATE_RUN_INSERT_OBJECT_END ||
      !state->current_output_enabled) {
    return LONEJSON_STATUS_OK;
  }
  if (!state->current_matched) {
    return LONEJSON_STATUS_OK;
  }
  for (i = 0u; i < state->mutation_plan->create_count; ++i) {
    item_index = state->mutation_plan->create_indexes[i];
    item = &state->mutation_plan->items[item_index];
    if (state->applied[item_index] ||
        !source_candidate_run_path_is_parent_of_item(event, item,
                                                     event->path) ||
        !source_candidate_run_item_child_key_available(
            item, event->path->segment_count)) {
      continue;
    }
    if (source_candidate_run_write_subtree(state, item,
                                           event->path->segment_count, writer,
                                           error) != LONEJSON_STATUS_OK) {
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
  }
  return LONEJSON_STATUS_OK;
}

static int source_candidate_run_parse_number(const char *data, size_t len,
                                             double *out) {
  char buf[128];
  char *end;
  double value;
  if (data == NULL || out == NULL || len == 0u || len >= sizeof(buf)) {
    return 0;
  }
  memcpy(buf, data, len);
  buf[len] = '\0';
  value = strtod(buf, &end);
  if (end == buf || *end != '\0') {
    return 0;
  }
  *out = value;
  return 1;
}

static lonejson_status
source_candidate_run_replace(void *user,
                             const lonejson_candidate_run_event *event,
                             lonejson_writer *writer, lonejson_error *error) {
  source_candidate_run_state *state;
  const lql_mutation_item *item;
  double existing;
  double next_value;
  char number_buf[64];
  state = (source_candidate_run_state *)user;
  if (state == NULL || event == NULL || writer == NULL ||
      !state->active_item_set || state->mutation_plan == NULL ||
      state->active_item >= state->mutation_plan->count) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  item = &state->mutation_plan->items[state->active_item];
  if (item->kind == LQL_MUTATION_REMOVE) {
    state->applied[state->active_item] = 1;
    return lonejson_writer_null(writer, error);
  }
  if (item->kind == LQL_MUTATION_INCREMENT) {
    if (event->old_value == NULL ||
        event->old_value->value_type != LONEJSON_VALUE_NUMBER ||
        !source_candidate_run_parse_number(event->old_value->data,
                                           event->old_value->len, &existing)) {
      if (state->staged_output) {
        state->transform_failed = 1;
        lql_set_error(&state->transform_error, LQL_STATUS_JSON_ERROR,
                      "mutation increment target not numeric");
        if (event->old_value != NULL) {
          switch (event->old_value->value_type) {
          case LONEJSON_VALUE_STRING:
            return lonejson_writer_string(writer, event->old_value->data,
                                          event->old_value->len, error);
          case LONEJSON_VALUE_NUMBER:
            return lonejson_writer_number_text(writer, event->old_value->data,
                                               event->old_value->len, error);
          case LONEJSON_VALUE_BOOL:
            return lonejson_writer_bool(writer, event->old_value->boolean_value,
                                        error);
          case LONEJSON_VALUE_NULL:
            return lonejson_writer_null(writer, error);
          default:
            break;
          }
        }
      }
      if (error != NULL) {
        error->code = LONEJSON_STATUS_CALLBACK_FAILED;
        strcpy(error->message, "mutation increment target not numeric");
      }
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    next_value = existing + item->delta;
    sprintf(number_buf, "%.17g", next_value);
    state->applied[state->active_item] = 1;
    return lonejson_writer_number_text(writer, number_buf, strlen(number_buf),
                                       error);
  }
  state->applied[state->active_item] = 1;
  return source_candidate_run_write_item_value(state, item, writer, error);
}

static lonejson_candidate_run_action
source_candidate_run_decide(void *user,
                            const lonejson_candidate_run_event *event,
                            lonejson_error *error) {
  source_candidate_run_state *state;
  const lql_mutation_item *item;
  size_t index;
  int array_element;
  (void)error;
  state = (source_candidate_run_state *)user;
  if (state == NULL || event == NULL || event->path == NULL) {
    return LONEJSON_CANDIDATE_RUN_ERROR;
  }
  state->active_item_set = 0;
  if (event->path->segment_count == 0u) {
    if (state->staged_output) {
      if (!state->current_matched_known) {
        state->current_matched = 1;
      }
      state->current_output_enabled =
          state->current_matched || !state->matches_only;
      if (event->value_type == LONEJSON_VALUE_OBJECT) {
        state->current_root_object = 1;
      }
    } else {
      if (state->selector != NULL) {
        state->current_matched = eval_doc_matches(state->selector, &state->doc);
        state->current_matched_known = 1;
      } else if (!state->current_matched_known) {
        state->current_matched = 1;
        state->current_matched_known = 1;
      }
      state->current_output_enabled =
          state->current_matched || !state->matches_only;
      if (event->value_type == LONEJSON_VALUE_OBJECT) {
        state->current_root_object = 1;
      }
    }
    if (!state->current_output_enabled) {
      return LONEJSON_CANDIDATE_RUN_DROP;
    }
    return LONEJSON_CANDIDATE_RUN_KEEP;
  }
  if (state->staged_output && state->matches_only &&
      state->current_matched_known && !state->current_matched) {
    return LONEJSON_CANDIDATE_RUN_DROP;
  }
  if (!state->current_output_enabled || !state->current_matched ||
      state->mutation_plan == NULL || !state->current_root_object) {
    return LONEJSON_CANDIDATE_RUN_KEEP;
  }
  if (!source_candidate_run_find_item(state, event, event->path, &index)) {
    return LONEJSON_CANDIDATE_RUN_KEEP;
  }
  item = &state->mutation_plan->items[index];
  state->active_item = index;
  state->active_item_set = 1;
  if (item->kind == LQL_MUTATION_REMOVE) {
    array_element = source_candidate_run_value_is_array_element(event);
    state->applied[index] = 1;
    return array_element ? LONEJSON_CANDIDATE_RUN_REPLACE
                         : LONEJSON_CANDIDATE_RUN_DROP;
  }
  return LONEJSON_CANDIDATE_RUN_REPLACE;
}

#if defined(LONEJSON_HAS_CANDIDATE_RUN_STAGED)
static lonejson_candidate_run_transition
source_candidate_run_transition(void *user,
                                const lonejson_candidate_info *candidate,
                                int finalizing, lonejson_error *error) {
  source_candidate_run_state *state = (source_candidate_run_state *)user;
  int matched;
  (void)candidate;

  if (state == NULL || !state->staged_output) {
    return LONEJSON_CANDIDATE_RUN_TRANSITION_ACCEPT;
  }
  if (!state->current_matched_known && state->selector != NULL &&
      state->selector->match_sticky_once_true && state->doc.candidate_matched) {
    state->current_matched = 1;
    state->current_matched_known = 1;
    state->current_output_enabled = 1;
  }
  if (!state->current_matched_known && state->doc.root_kind == '{') {
    if (state->doc.fast_exact_selector != NULL && state->doc.fast_exact_miss) {
      state->current_matched = 0;
      state->current_matched_known = 1;
    }
    if (state->current_matched_known) {
      state->current_output_enabled =
          state->current_matched || !state->matches_only;
    }
  }
  if (!state->current_matched_known && !finalizing) {
    return LONEJSON_CANDIDATE_RUN_TRANSITION_UNKNOWN;
  }
  if (finalizing && !state->current_matched_known) {
    matched = eval_doc_matches(state->selector, &state->doc);
    state->current_matched = matched;
    state->current_matched_known = 1;
    state->current_output_enabled = matched || !state->matches_only;
  }
  if (!finalizing) {
    if (!state->current_matched_known) {
      return LONEJSON_CANDIDATE_RUN_TRANSITION_UNKNOWN;
    }
    return state->current_matched || !state->matches_only
               ? LONEJSON_CANDIDATE_RUN_TRANSITION_ACCEPT
               : LONEJSON_CANDIDATE_RUN_TRANSITION_REJECT;
  }
  if (state->current_matched && state->transform_failed) {
    state->callback_status = state->transform_error.code;
    if (error != NULL) {
      error->code = LONEJSON_STATUS_CALLBACK_FAILED;
      strcpy(error->message, state->transform_error.message);
    }
    return LONEJSON_CANDIDATE_RUN_TRANSITION_ERROR;
  }
  return state->current_matched || !state->matches_only
             ? LONEJSON_CANDIDATE_RUN_TRANSITION_ACCEPT
             : LONEJSON_CANDIDATE_RUN_TRANSITION_REJECT;
}
#endif

static int source_candidate_run_has_increment(const lql_mutation_plan *plan) {
  size_t i;
  if (plan == NULL) {
    return 0;
  }
  for (i = 0u; i < plan->count; ++i) {
    if (plan->items[i].kind == LQL_MUTATION_INCREMENT) {
      return 1;
    }
  }
  return 0;
}

static lonejson_candidate_run_old_scalar_mode
source_candidate_run_old_scalar(void *user,
                                const lonejson_candidate_run_event *event,
                                lonejson_error *error) {
  source_candidate_run_state *state;
  size_t index;
  (void)error;
  state = (source_candidate_run_state *)user;
  if (state == NULL || event == NULL || event->path == NULL ||
      state->mutation_plan == NULL ||
      (event->value_type != LONEJSON_VALUE_NUMBER &&
       event->value_type != LONEJSON_VALUE_STRING)) {
    return LONEJSON_CANDIDATE_RUN_OLD_SCALAR_NONE;
  }
  if (!source_candidate_run_find_item(state, event, event->path, &index)) {
    return LONEJSON_CANDIDATE_RUN_OLD_SCALAR_NONE;
  }
  return state->mutation_plan->items[index].kind == LQL_MUTATION_INCREMENT
             ? LONEJSON_CANDIDATE_RUN_OLD_SCALAR_COMPLETE
             : LONEJSON_CANDIDATE_RUN_OLD_SCALAR_NONE;
}

static unsigned int query_limit_flags(const lql_query_options *options) {
  unsigned int flags;
  flags = 0u;
  if (options == NULL) {
    return flags;
  }
  if (options->max_matches != 0u) {
    flags |= LQL_QUERY_LIMIT_MATCHES;
  }
  if (options->max_candidates != 0u) {
    flags |= LQL_QUERY_LIMIT_CANDIDATES;
  }
  if (options->max_bytes_read != 0u) {
    flags |= LQL_QUERY_LIMIT_BYTES;
  }
  return flags;
}

static int eval_file_write_unlocked(FILE *out, const void *data, size_t len) {
  if (len == 0u) {
    return 1;
  }
#if defined(__linux__)
  return fwrite_unlocked(data, 1u, len, out) == len;
#else
  return fwrite(data, 1u, len, out) == len;
#endif
}

static lonejson_read_result
source_reader_read_plain(void *user, unsigned char *buffer, size_t capacity) {
  source_reader_adapter *adapter;
  lql_read_result lql_result;
  lonejson_read_result result;

  result = lonejson_default_read_result();
  adapter = (source_reader_adapter *)user;
  lql_result = adapter->read(adapter->user, buffer, capacity);
  if (lql_result.bytes_read > capacity) {
    adapter->error_code = 1;
    result.error_code = 1;
    return result;
  }
  if (lql_result.error_code != 0) {
    adapter->error_code = lql_result.error_code;
    result.error_code = lql_result.error_code;
    return result;
  }
  result.bytes_read = lql_result.bytes_read;
  adapter->total_read += (lql_uint64)lql_result.bytes_read;
  result.eof = lql_result.eof;
  return result;
}

static lonejson_read_result
source_reader_read(void *user, unsigned char *buffer, size_t capacity) {
  source_reader_adapter *adapter;
  lonejson_read_result result;
  size_t prefix_available;
  size_t copy_len;

  result = lonejson_default_read_result();
  adapter = (source_reader_adapter *)user;
  if (adapter->prefix_offset < adapter->prefix_len) {
    prefix_available = adapter->prefix_len - adapter->prefix_offset;
    copy_len = prefix_available < capacity ? prefix_available : capacity;
    if (copy_len != 0u) {
      memcpy(buffer, adapter->prefix + adapter->prefix_offset, copy_len);
      adapter->prefix_offset += copy_len;
      result.bytes_read = copy_len;
      if (adapter->prefix_offset == adapter->prefix_len &&
          !adapter->prefix_eof && copy_len < capacity) {
        lonejson_read_result extra = source_reader_read_plain(
            user, buffer + copy_len, capacity - copy_len);
        if (extra.error_code != 0 && extra.bytes_read == 0u) {
          result.error_code = extra.error_code;
        } else {
          result.bytes_read += extra.bytes_read;
          result.eof = extra.eof;
          result.error_code = extra.error_code;
        }
      } else if (adapter->prefix_offset == adapter->prefix_len &&
                 adapter->prefix_eof) {
        result.eof = 1;
      }
      return result;
    }
  }
  if (adapter->prefix_eof) {
    result.eof = 1;
    return result;
  }
  return source_reader_read_plain(user, buffer, capacity);
}

static int source_reader_prefix_capture(source_reader_adapter *adapter) {
  lql_read_result lql_result;
  size_t i;
  unsigned char ch;

  adapter->prefix_len = 0u;
  adapter->prefix_offset = 0u;
  adapter->prefix_eof = 0;
  adapter->prefix_root_array = 0;
  lql_result =
      adapter->read(adapter->user, adapter->prefix, sizeof(adapter->prefix));
  if (lql_result.bytes_read > sizeof(adapter->prefix)) {
    adapter->error_code = 1;
    return 0;
  }
  if (lql_result.error_code != 0) {
    adapter->error_code = lql_result.error_code;
    return 0;
  }
  adapter->prefix_len = lql_result.bytes_read;
  adapter->prefix_eof = lql_result.eof;
  adapter->total_read += (lql_uint64)lql_result.bytes_read;
  if (adapter->prefix_len == 0u) {
    return 1;
  }
  for (i = 0u; i < adapter->prefix_len; ++i) {
    ch = adapter->prefix[i];
    if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
      continue;
    }
    adapter->prefix_root_array = ch == '[';
    break;
  }
  return 1;
}

static int file_next_nonspace_is_root_array(FILE *file, int *out_is_array) {
  off_t pos;
  int ch;

  if (file == NULL || out_is_array == NULL) {
    return 0;
  }
  *out_is_array = 0;
  pos = ftello(file);
  if (pos < (off_t)0) {
    return 0;
  }
  do {
    ch = fgetc(file);
  } while (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t');
  if (ch == '[') {
    *out_is_array = 1;
  }
  return fseeko(file, pos, SEEK_SET) == 0;
}

static void query_finish_file_bytes(lql_query_result *result, FILE *file) {
  off_t pos;
  if (result == NULL || file == NULL || result->stopped_early) {
    return;
  }
  pos = ftello(file);
  if (pos >= (off_t)0 && (off_t)((lql_uint64)pos) == pos) {
    result->bytes_read = (lql_uint64)pos;
  }
}

static void
query_finish_source_bytes_with_base(lql_query_result *result,
                                    const source_reader_adapter *adapter,
                                    lql_uint64 offset_base) {
  if (result == NULL || adapter == NULL || result->stopped_early) {
    return;
  }
  result->bytes_read = offset_base + adapter->total_read;
}

static lonejson_read_result eval_pread_range(void *user, unsigned char *buffer,
                                             size_t capacity) {
  eval_pread_range_reader *reader;
  lonejson_read_result result;
  size_t want;
  ssize_t got;
  off_t pos;

  result = lonejson_default_read_result();
  reader = (eval_pread_range_reader *)user;
  if (reader->remaining == 0u) {
    result.eof = 1;
    return result;
  }
  want = reader->remaining > (lql_uint64)capacity ? capacity
                                                  : (size_t)reader->remaining;
  pos = (off_t)reader->offset;
  if (pos < (off_t)0 || (lql_uint64)pos != reader->offset) {
    result.error_code = 1;
    return result;
  }
  got = pread(reader->fd, buffer, want, pos);
  if (got < (ssize_t)0) {
    result.error_code = 1;
    return result;
  }
  result.bytes_read = (size_t)got;
  reader->offset += (lql_uint64)result.bytes_read;
  reader->remaining -= (lql_uint64)result.bytes_read;
  if ((size_t)got != want) {
    result.error_code = 1;
  }
  if (reader->remaining == 0u) {
    result.eof = 1;
  }
  return result;
}

static lql_read_result eval_pread_range_lql(void *user, unsigned char *buffer,
                                            size_t capacity) {
  lonejson_read_result source;
  lql_read_result result;
  source = eval_pread_range(user, buffer, capacity);
  memset(&result, 0, sizeof(result));
  result.bytes_read = source.bytes_read;
  result.eof = source.eof;
  result.error_code = source.error_code;
  return result;
}

static void query_stop(query_stream_state *state,
                       lql_query_stop_reason reason) {
  state->result.stopped_early = 1;
  state->result.stop_reason = reason;
}

static int query_result_stop_if_limited(lql_query_result *result,
                                        const lql_query_options *options,
                                        unsigned int limit_flags) {
  if (limit_flags == 0u) {
    return 0;
  }
  if ((limit_flags & LQL_QUERY_LIMIT_MATCHES) != 0u &&
      result->candidates_matched >= options->max_matches) {
    result->stopped_early = 1;
    result->stop_reason = LQL_QUERY_STOP_MATCH_LIMIT;
    return 1;
  }
  if ((limit_flags & LQL_QUERY_LIMIT_CANDIDATES) != 0u &&
      result->candidates_seen >= options->max_candidates) {
    result->stopped_early = 1;
    result->stop_reason = LQL_QUERY_STOP_CANDIDATE_LIMIT;
    return 1;
  }
  if ((limit_flags & LQL_QUERY_LIMIT_BYTES) != 0u &&
      result->bytes_read >= options->max_bytes_read) {
    result->stopped_early = 1;
    result->stop_reason = LQL_QUERY_STOP_BYTE_LIMIT;
    return 1;
  }
  return 0;
}

static void query_stream_state_cleanup_capture(query_stream_state *state) {
  if (state != NULL && state->array_spool_initialized) {
    state->array_spool.cleanup(&state->array_spool);
    state->array_spool_initialized = 0;
  }
}

static lonejson_candidate_callback_result
on_candidate_begin(void *user, const lonejson_candidate_info *candidate,
                   lonejson_error *error) {
  query_stream_state *state = (query_stream_state *)user;
  (void)candidate;
  (void)error;
  reset_doc(&state->doc);
  if (state->array_spool_initialized) {
    state->array_spool.reset(&state->array_spool);
  }
  state->pending_payload_prefix_len = 0u;
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lonejson_candidate_callback_result
on_candidate_end(void *user, const lonejson_candidate_info *candidate,
                 lonejson_error *error) {
  query_stream_state *state = (query_stream_state *)user;
  lql_query_decision decision;
  lql_query_match match;
  lql_status st;
  int matched;

  if (state->doc.root_kind == '[') {
    return reject_root_array_candidate(error);
  }
  matched = eval_doc_matches(state->selector, &state->doc);
  if (matched) {
    state->result.candidates_matched++;
  }
  state->result.candidates_seen++;
  state->result.bytes_read = state->offset_base +
                             (lql_uint64)candidate->stream_offset +
                             (lql_uint64)candidate->byte_size;
  if (state->on_match != NULL) {
    if (matched) {
      memset(&match, 0, sizeof(match));
      match.decision.matched = 1;
      match.decision.index = state->index_base + (lql_uint64)candidate->index;
      match.decision.offset =
          state->offset_base + (lql_uint64)candidate->stream_offset;
      match.decision.size = (lql_uint64)candidate->byte_size;
      match.payload.kind = LQL_PAYLOAD_SEEKABLE_RANGE;
      match.payload.index = match.decision.index;
      match.payload.offset = match.decision.offset;
      match.payload.size = match.decision.size;
      match.payload.source = state->file;
      st = state->on_match(state->user, &match);
      if (st == LQL_STATUS_STOP) {
        query_stop(state, LQL_QUERY_STOP_CALLBACK);
        return LONEJSON_CANDIDATE_STOP;
      }
      if (st != LQL_STATUS_OK) {
        state->callback_status = st;
        return LONEJSON_CANDIDATE_ERROR;
      }
    }
    if (query_result_stop_if_limited(&state->result, &state->options,
                                     state->limit_flags)) {
      return LONEJSON_CANDIDATE_STOP;
    }
    return LONEJSON_CANDIDATE_CONTINUE;
  }
  decision.matched = matched;
  decision.index = state->index_base + (lql_uint64)candidate->index;
  decision.offset = state->offset_base + (lql_uint64)candidate->stream_offset;
  decision.size = (lql_uint64)candidate->byte_size;
  st = state->on_decision(state->user, &decision);
  if (st == LQL_STATUS_STOP) {
    query_stop(state, LQL_QUERY_STOP_CALLBACK);
    return LONEJSON_CANDIDATE_STOP;
  }
  if (st != LQL_STATUS_OK) {
    state->callback_status = st;
    return LONEJSON_CANDIDATE_ERROR;
  }
  if (query_result_stop_if_limited(&state->result, &state->options,
                                   state->limit_flags)) {
    return LONEJSON_CANDIDATE_STOP;
  }
  return LONEJSON_CANDIDATE_CONTINUE;
}

static void
configure_query_candidate_callbacks(lonejson_candidate_stream_options *options,
                                    query_stream_state *state) {
  if (options == NULL || state == NULL) {
    return;
  }
  options->candidate_begin = on_candidate_begin;
  options->candidate_end = on_candidate_end;
  options->candidate_user = state;
}

static lonejson_status file_sink_unlocked(void *user, const void *data,
                                          size_t len, lonejson_error *error) {
  FILE *out;
  (void)error;
  out = (FILE *)user;
  if (!eval_file_write_unlocked(out, data, len)) {
    return LONEJSON_STATUS_IO_ERROR;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_candidate_callback_result
on_source_spooled_candidate_begin(void *user,
                                  const lonejson_candidate_info *candidate,
                                  lonejson_error *error) {
  source_spooled_match_state *state;
  (void)candidate;
  (void)error;
  state = (source_spooled_match_state *)user;
  reset_doc(&state->doc);
  state->capture_matched = 0;
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lonejson_candidate_capture_decision
on_source_spooled_capture_decision(void *user,
                                   const lonejson_candidate_info *candidate,
                                   lonejson_error *error) {
  source_spooled_match_state *state;
  (void)candidate;
  (void)error;
  state = (source_spooled_match_state *)user;
  state->capture_matched = eval_doc_matches(state->selector, &state->doc);
  if (state->capture_matched) {
    return LONEJSON_CANDIDATE_RETAIN;
  }
  return LONEJSON_CANDIDATE_DISCARD;
}

static lonejson_candidate_callback_result
on_source_spooled_candidate_end(void *user,
                                const lonejson_candidate_info *candidate,
                                lonejson_error *error) {
  source_spooled_match_state *state;
  lql_query_match match;
  lql_status st;
  int matched;

  state = (source_spooled_match_state *)user;
  if (state->doc.root_kind == '[') {
    return reject_root_array_candidate(error);
  }
  matched = state->capture_matched;
  state->result.candidates_seen++;
  state->result.bytes_read = state->offset_base +
                             (lql_uint64)candidate->stream_offset +
                             (lql_uint64)candidate->byte_size;
  if (matched) {
    if (candidate->payload_spool == NULL) {
      return LONEJSON_CANDIDATE_ERROR;
    }
    memset(&match, 0, sizeof(match));
    match.decision.matched = 1;
    match.decision.index = state->index_base + (lql_uint64)candidate->index;
    match.decision.offset =
        state->offset_base + (lql_uint64)candidate->stream_offset;
    match.decision.size = (lql_uint64)candidate->byte_size;
    match.payload.kind = LQL_PAYLOAD_SPOOLED;
    match.payload.index = match.decision.index;
    match.payload.offset = match.decision.offset;
    match.payload.size = match.decision.size;
    match.payload.spooled = candidate->payload_spool;
    state->result.candidates_matched++;
    st = state->on_match(state->user, &match);
    if (st == LQL_STATUS_STOP) {
      state->result.stopped_early = 1;
      state->result.stop_reason = LQL_QUERY_STOP_CALLBACK;
      return LONEJSON_CANDIDATE_STOP;
    }
    if (st != LQL_STATUS_OK) {
      state->callback_status = st;
      return LONEJSON_CANDIDATE_ERROR;
    }
  }
  if (query_result_stop_if_limited(&state->result, &state->options,
                                   state->limit_flags)) {
    return LONEJSON_CANDIDATE_STOP;
  }
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lql_status eval_selector_buffer(lql *self, const lql_selector *selector,
                                       const char *json, size_t json_len,
                                       int *out_matched, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_status st;
  int runtime_pooled;
  eval_doc doc;

  if (!init_doc(&doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime_pooled = 0;
  runtime = lql_lonejson_acquire(self, &runtime_pooled, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&doc);
    return LQL_STATUS_JSON_ERROR;
  }
  init_eval_visitor(&visitor);
  configure_eval_visitor_for_doc(&visitor, &doc);
  st = lonejson_visit_path_value_buffer(runtime, json, json_len, &visitor, &doc,
                                        &lj_error);
  if (st != LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&doc);
    lql_lonejson_release(self, runtime, runtime_pooled);
    return LQL_STATUS_JSON_ERROR;
  }
  *out_matched = eval_doc_matches(selector, &doc);
  destroy_doc(&doc);
  lql_lonejson_release(self, runtime, runtime_pooled);
  return LQL_STATUS_OK;
}

static lql_status matches_json_method(lql *self, const lql_selector *selector,
                                      const char *json, size_t json_len,
                                      int *out_matched, lql_error *error) {
  if (out_matched == NULL || json == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "json and out_matched are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (selector == NULL || selector->kind == LQL_SELECTOR_KIND_ALL) {
    *out_matched = 1;
    return LQL_STATUS_OK;
  }
  return eval_selector_buffer(self, selector, json, json_len, out_matched,
                              error);
}

static lql_status
query_file_decisions_method(lql *self, const lql_selector *selector, FILE *file,
                            lql_query_decision_fn on_decision, void *user,
                            lql_query_result *out_result, lql_error *error) {
  return self->query_file_decisions_with_options(
      self, selector, file, NULL, on_decision, user, out_result, error);
}

static lql_status query_file_decisions_with_options_method(
    lql *self, const lql_selector *selector, FILE *file,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error) {
  if (file == NULL || on_decision == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "file and on_decision are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return execute_query_file_decisions(self, selector, file, options,
                                      on_decision, user, out_result, error);
}

static lql_status
query_source_decisions_method(lql *self, const lql_selector *selector,
                              lql_read_fn read, void *read_user,
                              lql_query_decision_fn on_decision, void *user,
                              lql_query_result *out_result, lql_error *error) {
  return self->query_source_decisions_with_options(self, selector, read,
                                                   read_user, NULL, on_decision,
                                                   user, out_result, error);
}

static lql_status query_source_decisions_with_options_method(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error) {
  if (read == NULL || on_decision == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "read and on_decision are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return execute_query_source_decisions(self, selector, read, read_user,
                                        options, on_decision, user, out_result,
                                        error);
}

static lql_status query_source_spooled_matches_method(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_query_match_fn on_match, void *user, lql_query_result *out_result,
    lql_error *error) {
  return self->query_source_spooled_matches_with_options(
      self, selector, read, read_user, NULL, on_match, user, out_result, error);
}

static lql_status query_source_spooled_matches_with_options_method(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error) {
  if (read == NULL || on_match == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "read and on_match are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return execute_query_source_spooled_matches(self, selector, read, read_user,
                                              options, on_match, user,
                                              out_result, error);
}

static lql_status
query_file_matches_method(lql *self, const lql_selector *selector, FILE *file,
                          lql_query_match_fn on_match, void *user,
                          lql_query_result *out_result, lql_error *error) {
  return self->query_file_matches_with_options(
      self, selector, file, NULL, on_match, user, out_result, error);
}

static lql_status query_file_matches_with_options_method(
    lql *self, const lql_selector *selector, FILE *file,
    const lql_query_options *options, lql_query_match_fn on_match, void *user,
    lql_query_result *out_result, lql_error *error) {
  if (file == NULL || on_match == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "file and on_match are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return execute_query_file_matches(self, selector, file, options, on_match,
                                    user, out_result, error);
}

static lql_status payload_write_json_method(lql *self,
                                            const lql_payload *payload,
                                            FILE *out, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  int runtime_pooled;
  if (payload == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "payload and output file are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (payload->kind == LQL_PAYLOAD_SPOOLED && payload->spooled != NULL) {
    spooled_source_reader reader;
    lonejson_value_rewrite_options rewrite_options;

    runtime_pooled = 0;
    memset(&lj_error, 0, sizeof(lj_error));
    memset(&rewrite_options, 0, sizeof(rewrite_options));
    runtime = lql_lonejson_acquire(self, &runtime_pooled, &lj_error);
    if (runtime == NULL) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
      return LQL_STATUS_JSON_ERROR;
    }
    reader.cursor = *(const lonejson_spooled *)payload->spooled;
    reader.cursor.read_offset = 0u;
    flockfile(out);
    if (lonejson_value_rewrite_reader(runtime, spooled_lonejson_read, &reader,
                                      file_sink_unlocked, out, &rewrite_options,
                                      &lj_error) == LONEJSON_STATUS_OK) {
      funlockfile(out);
      lql_lonejson_release(self, runtime, runtime_pooled);
      return LQL_STATUS_OK;
    }
    funlockfile(out);
    lql_lonejson_release(self, runtime, runtime_pooled);
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  if (payload->kind == LQL_PAYLOAD_SEEKABLE_RANGE && payload->source != NULL) {
    eval_pread_range_reader reader;
    lonejson_value_rewrite_options rewrite_options;
    int fd;

    fd = fileno(payload->source);
    if (fd < 0) {
      lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                    "seekable payload has no file descriptor");
      return LQL_STATUS_UNSUPPORTED;
    }
    runtime_pooled = 0;
    memset(&lj_error, 0, sizeof(lj_error));
    memset(&rewrite_options, 0, sizeof(rewrite_options));
    runtime = lql_lonejson_acquire(self, &runtime_pooled, &lj_error);
    if (runtime == NULL) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
      return LQL_STATUS_JSON_ERROR;
    }
    reader.fd = fd;
    reader.offset = payload->offset;
    reader.remaining = payload->size;
    flockfile(out);
    if (lonejson_value_rewrite_reader(runtime, eval_pread_range, &reader,
                                      file_sink_unlocked, out, &rewrite_options,
                                      &lj_error) == LONEJSON_STATUS_OK) {
      funlockfile(out);
      lql_lonejson_release(self, runtime, runtime_pooled);
      return LQL_STATUS_OK;
    }
    funlockfile(out);
    lql_lonejson_release(self, runtime, runtime_pooled);
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  return self->payload_write_json_sink(self, payload, payload_file_write, out,
                                       error);
}

static lql_status payload_write_json_sink_method(lql *self,
                                                 const lql_payload *payload,
                                                 lql_write_fn write,
                                                 void *write_user,
                                                 lql_error *error) {
  lql_payload_sink_adapter adapter;
  off_t current;
  int fd;
  lql_status copy_status;
  (void)self;
  if (payload == NULL || write == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "payload and write callback are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (payload->kind != LQL_PAYLOAD_SEEKABLE_RANGE || payload->source == NULL) {
    if (payload->kind == LQL_PAYLOAD_SPOOLED && payload->spooled != NULL) {
      lonejson_error lj_error;
      memset(&adapter, 0, sizeof(adapter));
      adapter.write = write;
      adapter.user = write_user;
      if (lonejson_spooled_write_to_sink(
              (const lonejson_spooled *)payload->spooled, payload_lql_sink,
              &adapter, &lj_error) == LONEJSON_STATUS_OK) {
        return LQL_STATUS_OK;
      }
      if (adapter.status != LQL_STATUS_OK) {
        lql_set_error(error, adapter.status, "payload sink write failed");
        return adapter.status;
      }
      lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
      return LQL_STATUS_JSON_ERROR;
    }
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "payload is not a seekable source range");
    return LQL_STATUS_UNSUPPORTED;
  }
  fd = fileno(payload->source);
  if (fd >= 0) {
    copy_status = copy_fd_range_to_sink(fd, payload->offset, payload->size,
                                        write, write_user);
    if (copy_status == LQL_STATUS_OK) {
      return LQL_STATUS_OK;
    }
    if (copy_status == LQL_STATUS_JSON_ERROR) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "failed to write seekable payload range");
    } else {
      lql_set_error(error, copy_status, "payload sink write failed");
    }
    return copy_status;
  }
  current = ftello(payload->source);
  if (current < (off_t)0) {
    lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                  "failed to record source position");
    return LQL_STATUS_UNSUPPORTED;
  }
  if (!payload_seek_u64(payload->source, payload->offset)) {
    if (fseeko(payload->source, current, SEEK_SET) != 0) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "failed to write seekable payload range");
      return LQL_STATUS_JSON_ERROR;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to write seekable payload range");
    return LQL_STATUS_JSON_ERROR;
  }
  copy_status =
      copy_range_to_sink(payload->source, payload->size, write, write_user);
  if (fseeko(payload->source, current, SEEK_SET) != 0) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to write seekable payload range");
    return LQL_STATUS_JSON_ERROR;
  }
  if (copy_status != LQL_STATUS_OK) {
    if (copy_status == LQL_STATUS_JSON_ERROR) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "failed to write seekable payload range");
    } else {
      lql_set_error(error, copy_status, "payload sink write failed");
    }
    return copy_status;
  }
  return LQL_STATUS_OK;
}

static lql_status payload_project_json_method(lql *self,
                                              const lql_payload *payload,
                                              const lql_projection *projection,
                                              FILE *out, int *out_found,
                                              lql_error *error) {
  off_t current;
  lql_status st;
  if (out_found != NULL) {
    *out_found = 0;
  }
  if (payload == NULL || projection == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "payload, projection, and output file are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (payload->kind == LQL_PAYLOAD_SEEKABLE_RANGE && payload->source != NULL) {
    current = ftello(payload->source);
    if (current < (off_t)0) {
      lql_set_error(error, LQL_STATUS_UNSUPPORTED,
                    "failed to record source position");
      return LQL_STATUS_UNSUPPORTED;
    }
    st = self->project_file_range(self, projection, payload->source,
                                  payload->offset, payload->size, out,
                                  out_found, error);
    if (fseeko(payload->source, current, SEEK_SET) != 0) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "failed to project seekable payload range");
      return LQL_STATUS_JSON_ERROR;
    }
    return st;
  }
  if (payload->kind == LQL_PAYLOAD_SPOOLED && payload->spooled != NULL) {
    spooled_source_reader reader;
    reader.cursor = *(const lonejson_spooled *)payload->spooled;
    reader.cursor.read_offset = 0u;
    return self->project_source(self, projection, spooled_source_read, &reader,
                                out, out_found, error);
  }
  lql_set_error(error, LQL_STATUS_UNSUPPORTED, "payload cannot be projected");
  return LQL_STATUS_UNSUPPORTED;
}

static lql_status execute_query_file_range_candidate_run(
    lql *self, const lql_selector *selector, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, int compact, const lql_projection *projection,
    const lql_mutation_plan *mutation_plan, int matches_only,
    const lql_query_options *query_options, lql_query_result *out_result,
    lql_error *error) {
  eval_pread_range_reader reader;
  lql_query_result result;
  lql_status status;
  int fd;

  fd = fileno(file);
  if (fd < 0) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to access input descriptor");
    return LQL_STATUS_JSON_ERROR;
  }
  reader.fd = fd;
  reader.offset = offset;
  reader.remaining = size;
  memset(&result, 0, sizeof(result));
  status = execute_query_source_candidate_run(
      self, selector, eval_pread_range_lql, &reader, out, compact, projection,
      mutation_plan, matches_only, query_options, &result, error);
  if (result.bytes_read <= (lql_uint64)-1 - offset) {
    result.bytes_read += offset;
  } else {
    result.bytes_read = (lql_uint64)-1;
  }
  if (out_result != NULL) {
    *out_result = result;
  }
  return status;
}

static lql_status mutate_file_range_candidates_method(
    lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
    FILE *file, lql_uint64 offset, lql_uint64 size, FILE *out, int compact,
    int matches_only, lql_query_result *out_result, lql_error *error) {
  if (plan == NULL || file == NULL || out == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "plan, file, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return execute_query_file_range_candidate_run(
      self, selector, file, offset, size, out, compact, NULL, plan,
      matches_only, NULL, out_result, error);
}

static lql_status mutate_file_range_candidates_with_options_method(
    lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
    FILE *file, lql_uint64 offset, lql_uint64 size, FILE *out, int compact,
    int matches_only, const lql_query_options *query_options,
    lql_query_result *out_result, lql_error *error) {
  if (plan == NULL || file == NULL || out == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "plan, file, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return execute_query_file_range_candidate_run(
      self, selector, file, offset, size, out, compact, NULL, plan,
      matches_only, query_options, out_result, error);
}

static lql_status mutate_file_range_projected_candidates_method(
    lql *self, const lql_selector *selector, const lql_projection *projection,
    const lql_mutation_plan *plan, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, int compact, int matches_only,
    lql_query_result *out_result, lql_error *error) {
  if (projection == NULL || plan == NULL || file == NULL || out == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection, plan, file, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return execute_query_file_range_candidate_run(
      self, selector, file, offset, size, out, compact, projection, plan,
      matches_only, NULL, out_result, error);
}

static lql_status mutate_file_range_projected_candidates_with_options_method(
    lql *self, const lql_selector *selector, const lql_projection *projection,
    const lql_mutation_plan *plan, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, int compact, int matches_only,
    const lql_query_options *query_options, lql_query_result *out_result,
    lql_error *error) {
  if (projection == NULL || plan == NULL || file == NULL || out == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection, plan, file, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return execute_query_file_range_candidate_run(
      self, selector, file, offset, size, out, compact, projection, plan,
      matches_only, query_options, out_result, error);
}

static lql_status mutate_source_candidates_method(
    lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
    lql_read_fn read, void *read_user, FILE *out, int compact, int matches_only,
    lql_query_result *out_result, lql_error *error) {
  if (plan == NULL || read == NULL || out == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "plan, read, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return execute_query_source_candidate_run(
      self, selector, read, read_user, out, compact, NULL, plan, matches_only,
      NULL, out_result, error);
}

static lql_status mutate_source_candidates_with_options_method(
    lql *self, const lql_selector *selector, const lql_mutation_plan *plan,
    lql_read_fn read, void *read_user, FILE *out, int compact, int matches_only,
    const lql_query_options *query_options, lql_query_result *out_result,
    lql_error *error) {
  if (plan == NULL || read == NULL || out == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "plan, read, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return execute_query_source_candidate_run(
      self, selector, read, read_user, out, compact, NULL, plan, matches_only,
      query_options, out_result, error);
}

static lql_status mutate_source_projected_candidates_method(
    lql *self, const lql_selector *selector, const lql_projection *projection,
    const lql_mutation_plan *plan, lql_read_fn read, void *read_user, FILE *out,
    int compact, int matches_only, lql_query_result *out_result,
    lql_error *error) {
  if (projection == NULL || plan == NULL || read == NULL || out == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection, plan, read, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return execute_query_source_candidate_run(
      self, selector, read, read_user, out, compact, projection, plan,
      matches_only, NULL, out_result, error);
}

static lql_status mutate_source_projected_candidates_with_options_method(
    lql *self, const lql_selector *selector, const lql_projection *projection,
    const lql_mutation_plan *plan, lql_read_fn read, void *read_user, FILE *out,
    int compact, int matches_only, const lql_query_options *query_options,
    lql_query_result *out_result, lql_error *error) {
  if (projection == NULL || plan == NULL || read == NULL || out == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "projection, plan, read, and out are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return execute_query_source_candidate_run(
      self, selector, read, read_user, out, compact, projection, plan,
      matches_only, query_options, out_result, error);
}

static lql_status
execute_query_file_decisions(lql *self, const lql_selector *selector,
                             FILE *file, const lql_query_options *query_options,
                             lql_query_decision_fn on_decision, void *user,
                             lql_query_result *out_result, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  int runtime_pooled;
  int root_array;
  query_stream_state state;

  if (!file_next_nonspace_is_root_array(file, &root_array)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, "failed to inspect input");
    return LQL_STATUS_JSON_ERROR;
  }
  if (root_array) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, LQL_ROOT_ARRAY_NDJSON_ERROR);
    return LQL_STATUS_JSON_ERROR;
  }
  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.file = file;
  state.offset_base = 0u;
  state.index_base = 0u;
  state.selector = selector;
  state.on_decision = on_decision;
  state.user = user;
  state.callback_status = LQL_STATUS_OK;
  if (query_options != NULL) {
    state.options = *query_options;
  }
  state.limit_flags = query_limit_flags(&state.options);
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime_pooled = 0;
  runtime = lql_lonejson_acquire(self, &runtime_pooled, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  options = lonejson_default_candidate_stream_options();
  init_eval_visitor(&visitor);
  configure_eval_visitor_for_doc(&visitor, &state.doc);
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.framing = LONEJSON_CANDIDATE_FRAMING_NDJSON;
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_NONE;
  configure_query_candidate_callbacks(&options, &state);
  st = lonejson_visit_candidates_filep(runtime, file, &options, &lj_error);
  if (st != LONEJSON_STATUS_OK) {
    destroy_doc(&state.doc);
    lql_lonejson_release(self, runtime, runtime_pooled);
    if (out_result != NULL) {
      *out_result = state.result;
    }
    if (state.callback_status != LQL_STATUS_OK) {
      lql_set_error(error, state.callback_status,
                    "query decision callback failed");
      return state.callback_status;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  query_finish_file_bytes(&state.result, file);
  destroy_doc(&state.doc);
  lql_lonejson_release(self, runtime, runtime_pooled);
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}

static lql_status
execute_query_file_matches(lql *self, const lql_selector *selector, FILE *file,
                           const lql_query_options *query_options,
                           lql_query_match_fn on_match, void *user,
                           lql_query_result *out_result, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  int runtime_pooled;
  int root_array;
  query_stream_state state;

  if (!file_next_nonspace_is_root_array(file, &root_array)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, "failed to inspect input");
    return LQL_STATUS_JSON_ERROR;
  }
  if (root_array) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, LQL_ROOT_ARRAY_NDJSON_ERROR);
    return LQL_STATUS_JSON_ERROR;
  }
  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.file = file;
  state.offset_base = 0u;
  state.index_base = 0u;
  state.selector = selector;
  state.on_match = on_match;
  state.user = user;
  state.callback_status = LQL_STATUS_OK;
  if (query_options != NULL) {
    state.options = *query_options;
  }
  state.limit_flags = query_limit_flags(&state.options);
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime_pooled = 0;
  runtime = lql_lonejson_acquire(self, &runtime_pooled, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  options = lonejson_default_candidate_stream_options();
  init_eval_visitor(&visitor);
  configure_eval_visitor_for_doc(&visitor, &state.doc);
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.framing = LONEJSON_CANDIDATE_FRAMING_NDJSON;
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_NONE;
  configure_query_candidate_callbacks(&options, &state);
  st = lonejson_visit_candidates_filep(runtime, file, &options, &lj_error);
  if (st != LONEJSON_STATUS_OK) {
    destroy_doc(&state.doc);
    lql_lonejson_release(self, runtime, runtime_pooled);
    if (out_result != NULL) {
      *out_result = state.result;
    }
    if (state.callback_status != LQL_STATUS_OK) {
      lql_set_error(error, state.callback_status,
                    "query match callback failed");
      return state.callback_status;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  query_finish_file_bytes(&state.result, file);
  destroy_doc(&state.doc);
  lql_lonejson_release(self, runtime, runtime_pooled);
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}

static lql_status execute_query_source_decisions(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *query_options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error) {
  return execute_query_source_decisions_with_base(
      self, selector, read, read_user, 0u, 0u, query_options, on_decision, user,
      out_result, error);
}

static lql_status execute_query_source_decisions_with_base(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_uint64 offset_base, lql_uint64 index_base,
    const lql_query_options *query_options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  int runtime_pooled;
  query_stream_state state;
  source_reader_adapter adapter;

  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.selector = selector;
  state.offset_base = offset_base;
  state.index_base = index_base;
  state.on_decision = on_decision;
  state.user = user;
  state.callback_status = LQL_STATUS_OK;
  if (query_options != NULL) {
    state.options = *query_options;
  }
  state.limit_flags = query_limit_flags(&state.options);
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime_pooled = 0;
  runtime = lql_lonejson_acquire(self, &runtime_pooled, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  state.capture_runtime = runtime;
  memset(&adapter, 0, sizeof(adapter));
  adapter.read = read;
  adapter.user = read_user;
  if (!source_reader_prefix_capture(&adapter)) {
    destroy_doc(&state.doc);
    lql_lonejson_release(self, runtime, runtime_pooled);
    if (out_result != NULL) {
      *out_result = state.result;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, "query source reader failed");
    return LQL_STATUS_JSON_ERROR;
  }
  if (adapter.prefix_root_array) {
    destroy_doc(&state.doc);
    lql_lonejson_release(self, runtime, runtime_pooled);
    if (out_result != NULL) {
      *out_result = state.result;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, LQL_ROOT_ARRAY_NDJSON_ERROR);
    return LQL_STATUS_JSON_ERROR;
  }
  options = lonejson_default_candidate_stream_options();
  init_eval_visitor(&visitor);
  configure_eval_visitor_for_doc(&visitor, &state.doc);
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.framing = LONEJSON_CANDIDATE_FRAMING_NDJSON;
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_NONE;
  configure_query_candidate_callbacks(&options, &state);
  st = lonejson_visit_candidates_reader(runtime, source_reader_read, &adapter,
                                        &options, &lj_error);
  if (st == LONEJSON_STATUS_OK || adapter.error_code != 0 ||
      state.callback_status != LQL_STATUS_OK) {
    query_finish_source_bytes_with_base(&state.result, &adapter, offset_base);
  }
  if (st != LONEJSON_STATUS_OK) {
    query_stream_state_cleanup_capture(&state);
    destroy_doc(&state.doc);
    lql_lonejson_release(self, runtime, runtime_pooled);
    if (out_result != NULL) {
      *out_result = state.result;
    }
    if (state.callback_status != LQL_STATUS_OK) {
      lql_set_error(error, state.callback_status,
                    state.callback_status == LQL_STATUS_JSON_ERROR
                        ? "query source payload capture failed"
                        : "query decision callback failed");
      return state.callback_status;
    }
    if (adapter.error_code != 0) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR, "query source reader failed");
      return LQL_STATUS_JSON_ERROR;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  query_stream_state_cleanup_capture(&state);
  destroy_doc(&state.doc);
  lql_lonejson_release(self, runtime, runtime_pooled);
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}

static lql_status execute_query_source_spooled_matches(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *query_options, lql_query_match_fn on_match,
    void *user, lql_query_result *out_result, lql_error *error) {
  return execute_query_source_spooled_matches_with_base(
      self, selector, read, read_user, 0u, 0u, query_options, on_match, user,
      out_result, error);
}

static lql_status execute_query_source_spooled_matches_with_base(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    lql_uint64 offset_base, lql_uint64 index_base,
    const lql_query_options *query_options, lql_query_match_fn on_match,
    void *user, lql_query_result *out_result, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  int runtime_pooled;
  int runtime_can_pool;
  source_spooled_match_state state;
  source_reader_adapter adapter;

  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.selector = selector;
  state.offset_base = offset_base;
  state.index_base = index_base;
  state.on_match = on_match;
  state.user = user;
  state.callback_status = LQL_STATUS_OK;
  if (query_options != NULL) {
    state.options = *query_options;
  }
  state.limit_flags = query_limit_flags(&state.options);
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime_pooled = 0;
  runtime_can_pool = lql_lonejson_default_runtime_pool_allowed(self);
  runtime = runtime_can_pool
                ? lql_lonejson_acquire(self, &runtime_pooled, &lj_error)
                : lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  memset(&adapter, 0, sizeof(adapter));
  adapter.read = read;
  adapter.user = read_user;
  if (!source_reader_prefix_capture(&adapter)) {
    destroy_doc(&state.doc);
    lql_lonejson_release(self, runtime, runtime_pooled);
    if (out_result != NULL) {
      *out_result = state.result;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, "query source reader failed");
    return LQL_STATUS_JSON_ERROR;
  }
  if (adapter.prefix_root_array) {
    destroy_doc(&state.doc);
    lql_lonejson_release(self, runtime, runtime_pooled);
    if (out_result != NULL) {
      *out_result = state.result;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, LQL_ROOT_ARRAY_NDJSON_ERROR);
    return LQL_STATUS_JSON_ERROR;
  }
  options = lonejson_default_candidate_stream_options();
  init_eval_visitor(&visitor);
  configure_eval_visitor_for_doc(&visitor, &state.doc);
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.framing = LONEJSON_CANDIDATE_FRAMING_NDJSON;
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_GATED_SPOOLED;
  options.candidate_begin = on_source_spooled_candidate_begin;
  options.candidate_end = on_source_spooled_candidate_end;
  options.candidate_user = &state;
  options.capture_decision = on_source_spooled_capture_decision;
  options.capture_decision_user = &state;
  st = lonejson_visit_candidates_reader(runtime, source_reader_read, &adapter,
                                        &options, &lj_error);
  if (st == LONEJSON_STATUS_OK || adapter.error_code != 0 ||
      state.callback_status != LQL_STATUS_OK) {
    query_finish_source_bytes_with_base(&state.result, &adapter, offset_base);
  }
  if (st != LONEJSON_STATUS_OK) {
    destroy_doc(&state.doc);
    lql_lonejson_release(self, runtime, runtime_pooled);
    if (out_result != NULL) {
      *out_result = state.result;
    }
    if (state.callback_status != LQL_STATUS_OK) {
      lql_set_error(error, state.callback_status,
                    "query match callback failed");
      return state.callback_status;
    }
    if (adapter.error_code != 0) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR, "query source reader failed");
      return LQL_STATUS_JSON_ERROR;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  destroy_doc(&state.doc);
  lql_lonejson_release(self, runtime, runtime_pooled);
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}

static void
source_candidate_run_clear_applied(source_candidate_run_state *state) {
  if (state != NULL && state->applied != NULL && state->mutation_plan != NULL) {
    memset(state->applied, 0,
           sizeof(state->applied[0]) * state->mutation_plan->count);
  }
}

static lonejson_candidate_callback_result
source_candidate_run_candidate_begin(void *user,
                                     const lonejson_candidate_info *candidate,
                                     lonejson_error *error) {
  source_candidate_run_state *state;
  (void)candidate;
  (void)error;
  state = (source_candidate_run_state *)user;
  reset_doc(&state->doc);
  source_candidate_run_clear_applied(state);
  state->active_item = 0u;
  state->active_item_set = 0;
  state->current_matched = state->selector == NULL;
  state->current_matched_known = state->selector == NULL;
  state->current_root_object = 0;
  state->current_output_enabled =
      state->selector == NULL || !state->matches_only;
  state->transform_failed = 0;
  lql_error_init(&state->transform_error);
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lonejson_candidate_callback_result
source_candidate_run_candidate_end(void *user,
                                   const lonejson_candidate_info *candidate,
                                   lonejson_error *error) {
  source_candidate_run_state *state;
  int matched;
  state = (source_candidate_run_state *)user;
  if (state->doc.root_kind == '[') {
    return reject_root_array_candidate(error);
  }
  matched = state->current_matched_known
                ? state->current_matched
                : eval_doc_matches(state->selector, &state->doc);
  state->current_matched = matched;
  state->current_matched_known = 1;
  ++state->result.candidates_seen;
  state->result.bytes_read =
      (lql_uint64)(candidate->stream_offset + candidate->byte_size);
  if (matched) {
    ++state->result.candidates_matched;
  }
  if (query_result_stop_if_limited(&state->result, &state->options,
                                   state->limit_flags)) {
    return LONEJSON_CANDIDATE_STOP;
  }
  return LONEJSON_CANDIDATE_CONTINUE;
}

static int
source_candidate_run_alloc_applied(source_candidate_run_state *state) {
  size_t count;
  if (state == NULL || state->mutation_plan == NULL) {
    return 1;
  }
  count = state->mutation_plan->count;
  if (count <=
      sizeof(state->inline_applied) / sizeof(state->inline_applied[0])) {
    state->applied = state->inline_applied;
    state->applied_alloc = NULL;
    memset(state->applied, 0, sizeof(state->applied[0]) * count);
    return 1;
  }
  state->applied = (int *)state->allocator->calloc(state->allocator, count,
                                                   sizeof(state->applied[0]));
  state->applied_alloc = state->applied;
  return state->applied != NULL;
}

static void source_candidate_run_cleanup(source_candidate_run_state *state) {
  if (state == NULL) {
    return;
  }
  if (state->applied_alloc != NULL) {
    state->allocator->destroy(state->allocator, state->applied_alloc);
  }
  state->applied = NULL;
  state->applied_alloc = NULL;
}

static int
source_candidate_run_projection_segment_count(const lql_projection *projection,
                                              size_t *out) {
  size_t i;
  size_t count;
  count = 0u;
  if (projection == NULL) {
    *out = 0u;
    return 1;
  }
  for (i = 0u; i < projection->path_count; ++i) {
    if (projection->paths[i].segment_count > ((size_t)-1) - count) {
      return 0;
    }
    count += projection->paths[i].segment_count;
  }
  *out = count;
  return 1;
}

static int source_candidate_run_build_projection_paths(
    lql_allocator *allocator, const lql_projection *projection,
    lonejson_candidate_run_projection_path **out_paths,
    lonejson_candidate_run_projection_segment **out_segments) {
  lonejson_candidate_run_projection_path *paths;
  lonejson_candidate_run_projection_segment *segments;
  size_t segment_count;
  size_t i;
  size_t j;
  size_t cursor;
  if (out_paths != NULL) {
    *out_paths = NULL;
  }
  if (out_segments != NULL) {
    *out_segments = NULL;
  }
  if (projection == NULL || projection->path_count == 0u) {
    return 1;
  }
  if (!source_candidate_run_projection_segment_count(projection,
                                                     &segment_count)) {
    return 0;
  }
  paths = (lonejson_candidate_run_projection_path *)allocator->calloc(
      allocator, projection->path_count, sizeof(paths[0]));
  if (paths == NULL) {
    return 0;
  }
  segments = (lonejson_candidate_run_projection_segment *)allocator->calloc(
      allocator, segment_count == 0u ? 1u : segment_count, sizeof(segments[0]));
  if (segments == NULL) {
    allocator->destroy(allocator, paths);
    return 0;
  }
  cursor = 0u;
  for (i = 0u; i < projection->path_count; ++i) {
    paths[i].segments = &segments[cursor];
    paths[i].segment_count = projection->paths[i].segment_count;
    for (j = 0u; j < projection->paths[i].segment_count; ++j) {
      segments[cursor].kind =
          projection->paths[i].segment_is_array_index[j]
              ? LONEJSON_CANDIDATE_RUN_PROJECT_ARRAY_INDEX
              : LONEJSON_CANDIDATE_RUN_PROJECT_OBJECT_MEMBER;
      segments[cursor].key = projection->paths[i].segments[j];
      segments[cursor].key_len = projection->paths[i].segment_lens[j];
      segments[cursor].index =
          (lonejson_uint64)projection->paths[i].array_indexes[j];
      ++cursor;
    }
  }
  *out_paths = paths;
  *out_segments = segments;
  return 1;
}

static void source_candidate_run_free_projection_paths(
    lql_allocator *allocator, lonejson_candidate_run_projection_path *paths,
    lonejson_candidate_run_projection_segment *segments) {
  if (allocator == NULL) {
    return;
  }
  allocator->destroy(allocator, segments);
  allocator->destroy(allocator, paths);
}

static lql_status execute_query_source_candidate_run(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    FILE *out, int compact, const lql_projection *projection,
    const lql_mutation_plan *mutation_plan, int matches_only,
    const lql_query_options *query_options, lql_query_result *out_result,
    lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor observer;
  lonejson_candidate_run_options options;
  lonejson_candidate_run_projection_path *projection_paths;
  lonejson_candidate_run_projection_segment *projection_segments;
  lonejson_status st;
  source_candidate_run_state state;
  source_reader_adapter adapter;
  lql_allocator *allocator;
  int runtime_pooled;
  (void)compact;

  if (read == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "read and output file are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  allocator = lql_allocator_from_receiver(self);
  if (allocator == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "lql receiver allocator required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&state, 0, sizeof(state));
  memset(&adapter, 0, sizeof(adapter));
  projection_paths = NULL;
  projection_segments = NULL;
  state.receiver = self;
  state.allocator = allocator;
  state.selector = selector;
  state.projection = projection;
  state.mutation_plan = mutation_plan;
  state.matches_only = matches_only;
  state.callback_status = LQL_STATUS_OK;
  state.out = out;
  if (query_options != NULL) {
    state.options = *query_options;
  }
  state.limit_flags = query_limit_flags(&state.options);
  adapter.read = read;
  adapter.user = read_user;
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  if (!source_candidate_run_alloc_applied(&state)) {
    destroy_doc(&state.doc);
    return LQL_STATUS_NO_MEMORY;
  }
  if (!source_candidate_run_build_projection_paths(
          allocator, projection, &projection_paths, &projection_segments)) {
    source_candidate_run_cleanup(&state);
    destroy_doc(&state.doc);
    return LQL_STATUS_NO_MEMORY;
  }
  if (!source_reader_prefix_capture(&adapter)) {
    if (out_result != NULL) {
      *out_result = state.result;
    }
    source_candidate_run_free_projection_paths(allocator, projection_paths,
                                               projection_segments);
    source_candidate_run_cleanup(&state);
    destroy_doc(&state.doc);
    lql_set_error(error, LQL_STATUS_JSON_ERROR, "source read failed");
    return LQL_STATUS_JSON_ERROR;
  }
  if (adapter.prefix_root_array) {
    if (out_result != NULL) {
      *out_result = state.result;
    }
    source_candidate_run_free_projection_paths(allocator, projection_paths,
                                               projection_segments);
    source_candidate_run_cleanup(&state);
    destroy_doc(&state.doc);
    lql_set_error(error, LQL_STATUS_JSON_ERROR, LQL_ROOT_ARRAY_NDJSON_ERROR);
    return LQL_STATUS_JSON_ERROR;
  }
  runtime_pooled = 0;
  runtime = lql_lonejson_acquire(self, &runtime_pooled, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    source_candidate_run_free_projection_paths(allocator, projection_paths,
                                               projection_segments);
    source_candidate_run_cleanup(&state);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  init_eval_visitor(&observer);
  configure_eval_visitor_for_doc(&observer, &state.doc);
  memset(&options, 0, sizeof(options));
  options.framing = LONEJSON_CANDIDATE_FRAMING_NDJSON;
  options.output_framing = LONEJSON_CANDIDATE_RUN_NDJSON;
  /* Selector truth gates command replay. No source candidate is reparsed. */
#if defined(LONEJSON_HAS_CANDIDATE_RUN_STAGED)
  state.staged_output = selector != NULL;
#endif
  options.old_scalar_mode = source_candidate_run_has_increment(mutation_plan)
                                ? LONEJSON_CANDIDATE_RUN_OLD_SCALAR_COMPLETE
                                : LONEJSON_CANDIDATE_RUN_OLD_SCALAR_NONE;
  options.sink = file_sink_unlocked;
  options.sink_user = out;
  options.observer = &observer;
  options.observer_user = &state.doc;
  options.transform = source_candidate_run_decide;
  options.replace = source_candidate_run_replace;
  options.insert = source_candidate_run_insert_missing;
  options.insert_phase_mask = LONEJSON_CANDIDATE_RUN_INSERT_PHASE_BIT(
      LONEJSON_CANDIDATE_RUN_INSERT_OBJECT_END);
  options.projection_paths = projection_paths;
  options.projection_path_count =
      projection == NULL ? 0u : projection->path_count;
  options.transform_user = &state;
  options.candidate_begin = source_candidate_run_candidate_begin;
  options.candidate_end = source_candidate_run_candidate_end;
  options.candidate_user = &state;
  options.old_scalar = source_candidate_run_old_scalar;
  options.old_scalar_user = &state;
#if defined(LONEJSON_HAS_CANDIDATE_RUN_STAGED)
  if (state.staged_output) {
    options.output_transition = source_candidate_run_transition;
    options.output_transition_user = &state;
  }
#endif
  flockfile(out);
  st = lonejson_candidate_run_reader(runtime, source_reader_read, &adapter,
                                     &options, &lj_error);
  funlockfile(out);
  if (st == LONEJSON_STATUS_OK || adapter.error_code != 0 ||
      state.callback_status != LQL_STATUS_OK) {
    query_finish_source_bytes_with_base(&state.result, &adapter, 0u);
  }
  if (out_result != NULL) {
    *out_result = state.result;
  }
  lql_lonejson_release(self, runtime, runtime_pooled);
  source_candidate_run_free_projection_paths(allocator, projection_paths,
                                             projection_segments);
  source_candidate_run_cleanup(&state);
  destroy_doc(&state.doc);
  if (st != LONEJSON_STATUS_OK) {
    if (state.callback_status != LQL_STATUS_OK) {
      lql_set_error(error, state.callback_status,
                    state.transform_error.message);
      return state.callback_status;
    }
    if (adapter.error_code != 0) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR, "source read failed");
      return LQL_STATUS_JSON_ERROR;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  return LQL_STATUS_OK;
}

LQL_INTERNAL_SYMBOL void lql_eval_methods_install(lql *ctx) {
  ctx->matches_json = matches_json_method;
  ctx->query_file_decisions = query_file_decisions_method;
  ctx->query_file_decisions_with_options =
      query_file_decisions_with_options_method;
  ctx->query_source_decisions = query_source_decisions_method;
  ctx->query_source_decisions_with_options =
      query_source_decisions_with_options_method;
  ctx->query_file_matches = query_file_matches_method;
  ctx->query_file_matches_with_options = query_file_matches_with_options_method;
  ctx->query_source_spooled_matches = query_source_spooled_matches_method;
  ctx->query_source_spooled_matches_with_options =
      query_source_spooled_matches_with_options_method;
  ctx->payload_write_json = payload_write_json_method;
  ctx->payload_write_json_sink = payload_write_json_sink_method;
  ctx->payload_project_json = payload_project_json_method;
  ctx->mutate_file_range_candidates = mutate_file_range_candidates_method;
  ctx->mutate_file_range_candidates_with_options =
      mutate_file_range_candidates_with_options_method;
  ctx->mutate_file_range_projected_candidates =
      mutate_file_range_projected_candidates_method;
  ctx->mutate_file_range_projected_candidates_with_options =
      mutate_file_range_projected_candidates_with_options_method;
  ctx->mutate_source_candidates = mutate_source_candidates_method;
  ctx->mutate_source_candidates_with_options =
      mutate_source_candidates_with_options_method;
  ctx->mutate_source_projected_candidates =
      mutate_source_projected_candidates_method;
  ctx->mutate_source_projected_candidates_with_options =
      mutate_source_projected_candidates_with_options_method;
}
