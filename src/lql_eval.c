#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
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

#define LQL_EVAL_CONTAINS_TAIL_CAP 8192u
#define LQL_EVAL_PREFIX_CAP 8192u
#define LQL_EVAL_EXACT_CAP 8192u
#define LQL_EVAL_TEMPORAL_CAP 8192u
#define LQL_EVAL_NUMERIC_PREFIX_CAP 64u
#define LQL_EVAL_NUMERIC_SIG_CAP 32u
#define LQL_EVAL_NUMERIC_EXP_CAP 1000000L
#define LQL_SOURCE_PREFIX_CAP 4096u

typedef struct eval_doc {
  lql_allocator *allocator;
  lql_impl *impl;
  const lql_selector *selector;
  unsigned int *hits;
  size_t hits_cap;
  unsigned int *stream_misses;
  size_t stream_misses_cap;
  unsigned int *scalar_path_matches;
  size_t scalar_path_matches_cap;
  const lql_selector **scalar_path_predicates;
  size_t scalar_path_predicates_cap;
  size_t scalar_path_predicate_count;
  unsigned int scalar_path_epoch;
  unsigned int *in_matches;
  size_t in_matches_cap;
  size_t in_match_stride;
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
  char root_kind;
  int borrowed_scratch;
} eval_doc;

typedef struct lql_match_adapter {
  FILE *file;
  lql_query_match_fn on_match;
  void *user;
} lql_match_adapter;

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
static lql_status execute_query_file_range_decisions(
    lql *self, const lql_selector *selector, FILE *file, lql_uint64 offset,
    lql_uint64 size, lql_uint64 index_base,
    const lql_query_options *query_options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error);
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
static lql_status execute_query_file_range_spooled_matches(
    lql *self, const lql_selector *selector, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, int compact, const lql_projection *projection,
    const lql_mutation_plan *mutation_plan, int matches_only,
    const lql_query_options *query_options, lql_query_result *out_result,
    lql_error *error);
static lql_status execute_query_source_spooled_rewrite(
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

static lonejson_status payload_lql_sink(void *user, const void *data,
                                        size_t len, lonejson_error *error) {
  lql_payload_sink_adapter *adapter;
  (void)error;
  adapter = (lql_payload_sink_adapter *)user;
  adapter->status = adapter->write(adapter->user, data, len);
  return adapter->status == LQL_STATUS_OK ? LONEJSON_STATUS_OK
                                          : LONEJSON_STATUS_CALLBACK_FAILED;
}

static lql_status on_match_decision(void *user,
                                    const lql_query_decision *decision) {
  lql_match_adapter *adapter;
  lql_query_match match;

  if (!decision->matched) {
    return LQL_STATUS_OK;
  }
  adapter = (lql_match_adapter *)user;
  memset(&match, 0, sizeof(match));
  match.decision = *decision;
  match.payload.kind = LQL_PAYLOAD_SEEKABLE_RANGE;
  match.payload.index = decision->index;
  match.payload.offset = decision->offset;
  match.payload.size = decision->size;
  match.payload.source = adapter->file;
  return adapter->on_match(adapter->user, &match);
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
      doc->impl->eval_candidate_epoch = 1u;
    }
    doc->impl->eval_scalar_path_epoch = doc->scalar_path_epoch + 1u;
    if (doc->impl->eval_scalar_path_epoch == 0u) {
      if (doc->scalar_path_matches != NULL) {
        memset(doc->scalar_path_matches, 0,
               sizeof(*doc->scalar_path_matches) *
                   doc->scalar_path_matches_cap);
      }
      doc->impl->eval_scalar_path_epoch = 1u;
    }
    doc->impl->eval_hits = doc->hits;
    doc->impl->eval_hits_cap = doc->hits_cap;
    doc->impl->eval_stream_misses = doc->stream_misses;
    doc->impl->eval_stream_misses_cap = doc->stream_misses_cap;
    doc->impl->eval_scalar_path_matches = doc->scalar_path_matches;
    doc->impl->eval_scalar_path_matches_cap = doc->scalar_path_matches_cap;
    doc->impl->eval_scalar_path_predicates = doc->scalar_path_predicates;
    doc->impl->eval_scalar_path_predicates_cap =
        doc->scalar_path_predicates_cap;
    doc->impl->eval_in_matches = doc->in_matches;
    doc->impl->eval_in_matches_cap = doc->in_matches_cap;
    doc->impl->eval_contains_tail_buf = doc->contains_tail_buf;
    doc->impl->eval_contains_tail_cap = doc->contains_tail_cap;
    doc->impl->eval_container_types = doc->container_types;
    doc->impl->eval_container_cap = doc->container_cap;
    doc->impl->eval_scratch_in_use = 0;
  } else {
    doc->allocator->destroy(doc->allocator, doc->hits);
    doc->allocator->destroy(doc->allocator, doc->stream_misses);
    doc->allocator->destroy(doc->allocator, doc->scalar_path_matches);
    doc->allocator->destroy(doc->allocator, doc->scalar_path_predicates);
    doc->allocator->destroy(doc->allocator, doc->in_matches);
    doc->allocator->destroy(doc->allocator, doc->contains_tail_buf);
    doc->allocator->destroy(doc->allocator, doc->container_types);
  }
  memset(doc, 0, sizeof(*doc));
}

static int init_doc(eval_doc *doc, lql *self, const lql_selector *selector) {
  lql_impl *impl;
  unsigned int *next_hits;
  unsigned int *next_stream_misses;
  unsigned int *next_scalar_path_matches;
  const lql_selector **next_scalar_path_predicates;
  unsigned int *next_in_matches;
  size_t in_match_need;
  int clear_hits;
  int clear_stream_misses;
  int clear_scalar_path_matches;
  int clear_in_matches;
  memset(doc, 0, sizeof(*doc));
  doc->allocator = lql_allocator_from_receiver(self);
  impl = self == NULL ? NULL : (lql_impl *)self->impl;
  doc->impl = impl;
  doc->selector = selector;
  if (doc->allocator == NULL) {
    return 0;
  }
  if (impl != NULL && !impl->eval_scratch_in_use) {
    impl->eval_scratch_in_use = 1;
    doc->borrowed_scratch = 1;
    doc->hits = impl->eval_hits;
    doc->hits_cap = impl->eval_hits_cap;
    doc->stream_misses = impl->eval_stream_misses;
    doc->stream_misses_cap = impl->eval_stream_misses_cap;
    doc->scalar_path_matches = impl->eval_scalar_path_matches;
    doc->scalar_path_matches_cap = impl->eval_scalar_path_matches_cap;
    doc->scalar_path_predicates = impl->eval_scalar_path_predicates;
    doc->scalar_path_predicates_cap = impl->eval_scalar_path_predicates_cap;
    doc->in_matches = impl->eval_in_matches;
    doc->in_matches_cap = impl->eval_in_matches_cap;
    doc->contains_tail_buf = impl->eval_contains_tail_buf;
    doc->contains_tail_cap = impl->eval_contains_tail_cap;
    doc->container_types = impl->eval_container_types;
    doc->container_cap = impl->eval_container_cap;
    impl->eval_hits = NULL;
    impl->eval_hits_cap = 0u;
    impl->eval_stream_misses = NULL;
    impl->eval_stream_misses_cap = 0u;
    impl->eval_scalar_path_matches = NULL;
    impl->eval_scalar_path_matches_cap = 0u;
    impl->eval_scalar_path_predicates = NULL;
    impl->eval_scalar_path_predicates_cap = 0u;
    impl->eval_in_matches = NULL;
    impl->eval_in_matches_cap = 0u;
    impl->eval_contains_tail_buf = NULL;
    impl->eval_contains_tail_cap = 0u;
    impl->eval_container_types = NULL;
    impl->eval_container_cap = 0u;
  }
  if (selector != NULL && selector->hit_count != 0u) {
    clear_hits = !doc->borrowed_scratch;
    clear_stream_misses = !doc->borrowed_scratch;
    clear_scalar_path_matches = !doc->borrowed_scratch;
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
    if (doc->stream_misses_cap < selector->hit_count) {
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
    if (clear_stream_misses) {
      memset(doc->stream_misses, 0,
             sizeof(*doc->stream_misses) * doc->stream_misses_cap);
    }
    if (doc->scalar_path_matches_cap < selector->hit_count) {
      next_scalar_path_matches = (unsigned int *)doc->allocator->realloc(
          doc->allocator, doc->scalar_path_matches,
          sizeof(*doc->scalar_path_matches) * selector->hit_count);
      if (next_scalar_path_matches == NULL) {
        destroy_doc(doc);
        return 0;
      }
      doc->scalar_path_matches = next_scalar_path_matches;
      doc->scalar_path_matches_cap = selector->hit_count;
      clear_scalar_path_matches = 1;
    }
    if (clear_scalar_path_matches) {
      memset(doc->scalar_path_matches, 0,
             sizeof(*doc->scalar_path_matches) * doc->scalar_path_matches_cap);
    }
    if (doc->scalar_path_predicates_cap < selector->predicate_count) {
      next_scalar_path_predicates =
          (const lql_selector **)doc->allocator->realloc(
              doc->allocator, doc->scalar_path_predicates,
              sizeof(*doc->scalar_path_predicates) * selector->predicate_count);
      if (next_scalar_path_predicates == NULL) {
        destroy_doc(doc);
        return 0;
      }
      doc->scalar_path_predicates = next_scalar_path_predicates;
      doc->scalar_path_predicates_cap = selector->predicate_count;
    }
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
    doc->predicates = selector->predicates;
    doc->predicate_count = selector->predicate_count;
  }
  doc->candidate_epoch =
      doc->borrowed_scratch && impl != NULL && impl->eval_candidate_epoch != 0u
          ? impl->eval_candidate_epoch
          : 1u;
  doc->scalar_path_epoch = doc->borrowed_scratch && impl != NULL &&
                                   impl->eval_scalar_path_epoch != 0u
                               ? impl->eval_scalar_path_epoch
                               : 1u;
  return 1;
}

static void reset_doc(eval_doc *doc) {
  if (doc->selector != NULL && doc->selector->hit_count != 0u) {
    ++doc->candidate_epoch;
    if (doc->candidate_epoch == 0u) {
      memset(doc->hits, 0, sizeof(*doc->hits) * doc->selector->hit_count);
      memset(doc->stream_misses, 0,
             sizeof(*doc->stream_misses) * doc->selector->hit_count);
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
  doc->scalar_path_predicate_count = 0u;
  doc->scalar_len = 0u;
  doc->contains_tail_len = 0u;
  doc->contains_tail_need = 0u;
  doc->prefix_len = 0u;
  doc->prefix_need = 0u;
  doc->root_kind = '\0';
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
  depth = path->segment_count;
  if (depth >= doc->container_cap) {
    return;
  }
  doc->container_types[depth] = 0;
}

static int ascii_case_equal_prefix(const char *a, const char *b, size_t n) {
  size_t i;
  for (i = 0u; i < n; ++i) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
      return 0;
    }
  }
  return 1;
}

static int contains_case_len(const char *haystack, size_t h, const char *needle,
                             size_t n, int ignore_case);

static int contains_case_len(const char *haystack, size_t h, const char *needle,
                             size_t n, int ignore_case) {
  size_t i;
  unsigned char first;
  if (n == 0u) {
    return 1;
  }
  if (n > h) {
    return 0;
  }
  first = ignore_case ? (unsigned char)tolower((unsigned char)needle[0])
                      : (unsigned char)needle[0];
  for (i = 0u; i + n <= h; ++i) {
    if (ignore_case) {
      if ((unsigned char)tolower((unsigned char)haystack[i]) != first) {
        continue;
      }
      if (ascii_case_equal_prefix(haystack + i, needle, n)) {
        return 1;
      }
    } else if ((unsigned char)haystack[i] == first &&
               memcmp(haystack + i, needle, n) == 0) {
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
  unsigned char stack_firsts[32];
  size_t i;
  size_t j;
  size_t n;
  unsigned char hay_ch;
  unsigned char raw_ch;
  if (count == 0u) {
    return 0;
  }
  if (firsts == NULL &&
      count > sizeof(stack_firsts) / sizeof(stack_firsts[0])) {
    for (j = 0u; j < count; ++j) {
      if (contains_case_len(haystack, h, needles[j], needle_lens[j],
                            ignore_case)) {
        return 1;
      }
    }
    return 0;
  }
  if (firsts == NULL) {
    for (j = 0u; j < count; ++j) {
      if (needle_lens[j] == 0u) {
        return 1;
      }
      stack_firsts[j] =
          ignore_case ? (unsigned char)tolower((unsigned char)needles[j][0])
                      : (unsigned char)needles[j][0];
    }
    firsts = stack_firsts;
  }
  for (j = 0u; j < count; ++j) {
    if ((needle_lens == NULL && needles[j][0] == '\0') ||
        (needle_lens != NULL && needle_lens[j] == 0u)) {
      return 1;
    }
  }
  for (i = 0u; i < h; ++i) {
    raw_ch = (unsigned char)haystack[i];
    if (first_bitmap != NULL && (first_bitmap[raw_ch >> 3] &
                                 (unsigned char)(1u << (raw_ch & 7u))) == 0u) {
      continue;
    }
    hay_ch = ignore_case ? (unsigned char)tolower(raw_ch) : raw_ch;
    for (j = 0u; j < count; ++j) {
      if (hay_ch != firsts[j]) {
        continue;
      }
      n = needle_lens[j];
      if (n > h - i) {
        continue;
      }
      if (ignore_case) {
        if (ascii_case_equal_prefix(haystack + i, needles[j], n)) {
          return 1;
        }
      } else if (memcmp(haystack + i, needles[j], n) == 0) {
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

static int path_matches(const eval_doc *doc, const char *pattern,
                        const lonejson_value_path *path) {
  const char *seg;
  if (pattern == NULL || path == NULL || pattern[0] != '/') {
    return 0;
  }
  if (pattern[1] == '\0') {
    return path->segment_count == 0u;
  }
  seg = pattern + 1;
  return path_matches_from(doc, seg, path, 0u);
}

static void stream_miss_clear(eval_doc *doc, const lql_selector *selector);
static size_t selector_contains_max_needle(const lql_selector *selector);
static size_t selector_prefix_value_len(const lql_selector *selector);
static size_t selector_in_max_value_len(const lql_selector *selector);

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
  if (!selector->field_path_direct) {
    return path_matches(doc, selector->field, path);
  }
  if (path == NULL || path->segment_count != selector->field_segment_count) {
    return 0;
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

static void scalar_path_match_prepare(eval_doc *doc,
                                      const lonejson_value_path *path) {
  const lql_selector *selector;
  size_t i;
  size_t value_len;
  size_t max_needle;

  if (doc->selector == NULL || doc->selector->hit_count == 0u ||
      doc->scalar_path_matches == NULL) {
    return;
  }
  doc->scalar_path_features = 0u;
  doc->scalar_path_predicate_count = 0u;
  ++doc->scalar_path_epoch;
  if (doc->scalar_path_epoch == 0u) {
    memset(doc->scalar_path_matches, 0,
           sizeof(*doc->scalar_path_matches) * doc->selector->hit_count);
    doc->scalar_path_epoch = 1u;
  }
  for (i = 0u; i < doc->predicate_count; ++i) {
    selector = doc->predicates[i];
    if (selector_path_matches(doc, selector, path)) {
      doc->scalar_path_matches[selector->hit_index] = doc->scalar_path_epoch;
      if (doc->scalar_path_predicates != NULL &&
          doc->scalar_path_predicate_count < doc->scalar_path_predicates_cap) {
        doc->scalar_path_predicates[doc->scalar_path_predicate_count] =
            selector;
        ++doc->scalar_path_predicate_count;
      }
      if (doc->stream_misses != NULL) {
        stream_miss_clear(doc, selector);
      }
      switch (selector->kind) {
      case LQL_SELECTOR_KIND_CONTAINS:
      case LQL_SELECTOR_KIND_ICONTAINS:
        doc->scalar_path_features |= LQL_SELECTOR_FEATURE_CONTAINS;
        max_needle = selector_contains_max_needle(selector);
        if (max_needle > 1u && max_needle - 1u > doc->contains_tail_need) {
          doc->contains_tail_need = max_needle - 1u;
        }
        break;
      case LQL_SELECTOR_KIND_PREFIX:
      case LQL_SELECTOR_KIND_IPREFIX:
        doc->scalar_path_features |= LQL_SELECTOR_FEATURE_PREFIX;
        value_len = selector_prefix_value_len(selector);
        if (value_len > LQL_EVAL_PREFIX_CAP) {
          value_len = LQL_EVAL_PREFIX_CAP;
        }
        if (value_len > doc->prefix_need) {
          doc->prefix_need = value_len;
        }
        break;
      case LQL_SELECTOR_KIND_EQ:
      case LQL_SELECTOR_KIND_NE:
        if (selector->value_is_temporal) {
          doc->scalar_path_features |= LQL_SELECTOR_FEATURE_TEMPORAL;
          if (LQL_EVAL_TEMPORAL_CAP > doc->prefix_need) {
            doc->prefix_need = LQL_EVAL_TEMPORAL_CAP;
          }
        } else {
          doc->scalar_path_features |= LQL_SELECTOR_FEATURE_EXACT;
          value_len = selector_prefix_value_len(selector);
          if (value_len > LQL_EVAL_EXACT_CAP) {
            value_len = LQL_EVAL_EXACT_CAP;
          }
          if (value_len > doc->prefix_need) {
            doc->prefix_need = value_len;
          }
        }
        break;
      case LQL_SELECTOR_KIND_IN:
        doc->scalar_path_features |= LQL_SELECTOR_FEATURE_EXACT;
        value_len = selector_in_max_value_len(selector);
        if (value_len > LQL_EVAL_EXACT_CAP) {
          value_len = LQL_EVAL_EXACT_CAP;
        }
        if (value_len > doc->prefix_need) {
          doc->prefix_need = value_len;
        }
        break;
      case LQL_SELECTOR_KIND_RANGE:
        if (selector->range_is_temporal) {
          doc->scalar_path_features |= LQL_SELECTOR_FEATURE_TEMPORAL;
          if (LQL_EVAL_TEMPORAL_CAP > doc->prefix_need) {
            doc->prefix_need = LQL_EVAL_TEMPORAL_CAP;
          }
        } else {
          doc->scalar_path_features |= LQL_SELECTOR_FEATURE_NUMERIC_RANGE;
        }
        break;
      case LQL_SELECTOR_KIND_DATE:
        doc->scalar_path_features |= LQL_SELECTOR_FEATURE_TEMPORAL;
        if (LQL_EVAL_TEMPORAL_CAP > doc->prefix_need) {
          doc->prefix_need = LQL_EVAL_TEMPORAL_CAP;
        }
        break;
      case LQL_SELECTOR_KIND_EXISTS:
        doc->scalar_path_features |= LQL_SELECTOR_FEATURE_EXISTS;
        break;
      default:
        break;
      }
    }
  }
}

static void hit_mark(eval_doc *doc, const lql_selector *selector) {
  if (doc != NULL && selector != NULL && doc->hits != NULL &&
      selector->hit_index < doc->hits_cap) {
    doc->hits[selector->hit_index] = doc->candidate_epoch;
  }
}

static int hit_marked(const eval_doc *doc, const lql_selector *selector) {
  return doc != NULL && selector != NULL && doc->hits != NULL &&
         selector->hit_index < doc->hits_cap &&
         doc->hits[selector->hit_index] == doc->candidate_epoch;
}

static void stream_miss_mark(eval_doc *doc, const lql_selector *selector) {
  if (doc != NULL && selector != NULL && doc->stream_misses != NULL &&
      selector->hit_index < doc->stream_misses_cap) {
    doc->stream_misses[selector->hit_index] = doc->candidate_epoch;
  }
}

static void stream_miss_clear(eval_doc *doc, const lql_selector *selector) {
  if (doc != NULL && selector != NULL && doc->stream_misses != NULL &&
      selector->hit_index < doc->stream_misses_cap) {
    doc->stream_misses[selector->hit_index] = 0u;
  }
}

static int stream_miss_marked(const eval_doc *doc,
                              const lql_selector *selector) {
  return doc != NULL && selector != NULL && doc->stream_misses != NULL &&
         selector->hit_index < doc->stream_misses_cap &&
         doc->stream_misses[selector->hit_index] == doc->candidate_epoch;
}

static unsigned int *in_match_row(eval_doc *doc, const lql_selector *selector) {
  size_t offset;
  if (doc == NULL || selector == NULL || doc->in_matches == NULL ||
      doc->in_match_stride == 0u) {
    return NULL;
  }
  offset = selector->hit_index * doc->in_match_stride;
  if (offset + doc->in_match_stride > doc->in_matches_cap) {
    return NULL;
  }
  return doc->in_matches + offset;
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

static void observe_matched_selector(eval_doc *doc,
                                     const lql_selector *selector,
                                     const char *value, int is_number,
                                     int is_container, int is_null) {
  size_t n;
  size_t j;
  size_t value_len;
  double number;
  lql_temporal temporal;
  lql_temporal since_macro;
  const char *needle;
  int ignore_case;
  if (selector == NULL) {
    return;
  }
  if (doc->hits == NULL) {
    return;
  }
  switch (selector->kind) {
  case LQL_SELECTOR_KIND_EQ:
    if (is_container || is_null) {
      break;
    }
    if (strcmp(value, selector->value == NULL ? "" : selector->value) == 0) {
      hit_mark(doc, selector);
    } else if (selector->value_is_temporal &&
               lql_parse_temporal_literal(value, &temporal) &&
               lql_temporal_equal(&temporal, &selector->temporal_eq)) {
      hit_mark(doc, selector);
    }
    break;
  case LQL_SELECTOR_KIND_NE:
    if (is_container) {
      break;
    }
    if (is_null) {
      hit_mark(doc, selector);
      break;
    }
    if (strcmp(value, selector->value == NULL ? "" : selector->value) != 0 &&
        !(selector->value_is_temporal &&
          lql_parse_temporal_literal(value, &temporal) &&
          lql_temporal_equal(&temporal, &selector->temporal_eq))) {
      hit_mark(doc, selector);
    }
    break;
  case LQL_SELECTOR_KIND_CONTAINS:
  case LQL_SELECTOR_KIND_ICONTAINS:
    if (selector->any_count == 0u && !selector->value_set &&
        selector->value == NULL) {
      hit_mark(doc, selector);
      break;
    }
    if (is_container || is_null) {
      break;
    }
    if (selector->any_count == 0u) {
      value_len = strlen(value);
      if (contains_case_len(value, value_len,
                            selector->value == NULL ? "" : selector->value,
                            selector->value_len,
                            selector->kind == LQL_SELECTOR_KIND_ICONTAINS ||
                                selector->ignore_case)) {
        hit_mark(doc, selector);
      }
    } else {
      value_len = strlen(value);
      ignore_case = selector->kind == LQL_SELECTOR_KIND_ICONTAINS ||
                    selector->ignore_case;
      if (contains_any_case_len(value, value_len, selector->any,
                                selector->any_lens, selector->any_count,
                                ignore_case ? selector->any_ifirsts
                                            : selector->any_firsts,
                                ignore_case ? selector->any_ifirst_bitmap
                                            : selector->any_first_bitmap,
                                ignore_case)) {
        hit_mark(doc, selector);
      }
    }
    break;
  case LQL_SELECTOR_KIND_PREFIX:
  case LQL_SELECTOR_KIND_IPREFIX:
    if (!selector->value_set && selector->value == NULL) {
      hit_mark(doc, selector);
      break;
    }
    if (is_container || is_null) {
      break;
    }
    n = selector->value_len;
    if (strlen(value) >= n &&
        (selector->kind == LQL_SELECTOR_KIND_IPREFIX || selector->ignore_case
             ? ascii_case_equal_prefix(value, selector->value, n)
             : memcmp(value, selector->value, n) == 0)) {
      hit_mark(doc, selector);
    }
    break;
  case LQL_SELECTOR_KIND_RANGE:
    if (!is_container && !is_null && selector->range_is_temporal) {
      if (lql_parse_temporal_literal(value, &temporal) &&
          (!selector->has_temporal_gt ||
           lql_temporal_compare(&temporal, &selector->temporal_gt) > 0) &&
          (!selector->has_temporal_gte ||
           lql_temporal_compare(&temporal, &selector->temporal_gte) >= 0) &&
          (!selector->has_temporal_lt ||
           lql_temporal_compare(&temporal, &selector->temporal_lt) < 0) &&
          (!selector->has_temporal_lte ||
           lql_temporal_compare(&temporal, &selector->temporal_lte) <= 0)) {
        hit_mark(doc, selector);
      }
    } else if (!is_container && !is_null && is_number) {
      number = strtod(value, NULL);
      if ((!selector->has_range_gt || number > selector->range_gt) &&
          (!selector->has_range_gte || number >= selector->range_gte) &&
          (!selector->has_range_lt || number < selector->range_lt) &&
          (!selector->has_range_lte || number <= selector->range_lte)) {
        hit_mark(doc, selector);
      }
    }
    break;
  case LQL_SELECTOR_KIND_DATE:
    if (!is_container && !is_null &&
        (selector->since_macro == LQL_SINCE_NONE ||
         resolve_since_macro(selector->since_macro, &since_macro)) &&
        lql_parse_temporal_literal(value, &temporal) &&
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
      hit_mark(doc, selector);
    }
    break;
  case LQL_SELECTOR_KIND_IN:
    if (is_container || is_null) {
      break;
    }
    for (j = 0u; j < selector->any_count; ++j) {
      needle = selector->any[j];
      if (strcmp(value, needle) == 0) {
        hit_mark(doc, selector);
        break;
      }
    }
    break;
  case LQL_SELECTOR_KIND_EXISTS:
    if (!is_null) {
      hit_mark(doc, selector);
    }
    break;
  default:
    break;
  }
}

static size_t selector_contains_max_needle(const lql_selector *selector) {
  if (selector->any_count == 0u) {
    return selector->value_len;
  }
  return selector->any_max_len;
}

static size_t selector_prefix_value_len(const lql_selector *selector) {
  if (selector->value == NULL) {
    return 0u;
  }
  return selector->value_len;
}

static size_t selector_in_max_value_len(const lql_selector *selector) {
  return selector->any_max_len;
}

static void observe_contains_stream_begin(eval_doc *doc,
                                          const lql_selector *selector,
                                          const lonejson_value_path *path) {
  size_t i;
  size_t j;

  (void)selector;
  (void)path;
  if (doc->hits == NULL || doc->stream_misses == NULL) {
    return;
  }
  for (i = 0u; i < doc->scalar_path_predicate_count; ++i) {
    selector = doc->scalar_path_predicates[i];
    if ((selector->kind != LQL_SELECTOR_KIND_CONTAINS &&
         selector->kind != LQL_SELECTOR_KIND_ICONTAINS)) {
      continue;
    }
    if (selector->any_count == 0u) {
      if (!selector->value_set && selector->value == NULL) {
        hit_mark(doc, selector);
      } else if (selector->value == NULL || selector->value[0] == '\0') {
        hit_mark(doc, selector);
      }
      continue;
    }
    for (j = 0u; j < selector->any_count; ++j) {
      if ((selector->any_lens == NULL && selector->any[j][0] == '\0') ||
          (selector->any_lens != NULL && selector->any_lens[j] == 0u)) {
        hit_mark(doc, selector);
        break;
      }
    }
  }
}

static void observe_prefix_stream_begin(eval_doc *doc,
                                        const lql_selector *selector,
                                        const lonejson_value_path *path) {
  size_t i;

  (void)selector;
  (void)path;
  if (doc->hits == NULL || doc->stream_misses == NULL) {
    return;
  }
  for (i = 0u; i < doc->scalar_path_predicate_count; ++i) {
    selector = doc->scalar_path_predicates[i];
    if ((selector->kind == LQL_SELECTOR_KIND_PREFIX ||
         selector->kind == LQL_SELECTOR_KIND_IPREFIX) &&
        !selector->value_set && selector->value == NULL) {
      hit_mark(doc, selector);
    }
  }
}

static void observe_in_stream_begin(eval_doc *doc, const lql_selector *selector,
                                    const lonejson_value_path *path) {
  size_t i;
  size_t j;
  unsigned int *matches;

  (void)selector;
  (void)path;
  if (doc->in_matches == NULL || doc->in_match_stride == 0u) {
    return;
  }
  for (i = 0u; i < doc->scalar_path_predicate_count; ++i) {
    selector = doc->scalar_path_predicates[i];
    if (selector->kind != LQL_SELECTOR_KIND_IN) {
      continue;
    }
    matches = in_match_row(doc, selector);
    if (matches == NULL) {
      continue;
    }
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
  for (i = 0u; i < compare_len; ++i) {
    if (ignore_case) {
      if (tolower((unsigned char)data[i]) !=
          tolower((unsigned char)literal[offset + i])) {
        return 0;
      }
    } else if (data[i] != literal[offset + i]) {
      return 0;
    }
  }
  return 1;
}

static void observe_prefix_stream_chunk(eval_doc *doc,
                                        const lql_selector *selector,
                                        const lonejson_value_path *path,
                                        const char *data, size_t len) {
  size_t i;
  size_t offset;
  size_t value_len;
  int ignore_case;

  (void)selector;
  (void)path;
  if (doc->hits == NULL || doc->stream_misses == NULL ||
      doc->scalar_len < len) {
    return;
  }
  offset = doc->scalar_len - len;
  for (i = 0u; i < doc->scalar_path_predicate_count; ++i) {
    selector = doc->scalar_path_predicates[i];
    if ((selector->kind != LQL_SELECTOR_KIND_PREFIX &&
         selector->kind != LQL_SELECTOR_KIND_IPREFIX) ||
        hit_marked(doc, selector) || stream_miss_marked(doc, selector)) {
      continue;
    }
    value_len = selector_prefix_value_len(selector);
    if (value_len == 0u) {
      continue;
    }
    ignore_case =
        selector->kind == LQL_SELECTOR_KIND_IPREFIX || selector->ignore_case;
    if (!literal_chunk_matches(selector->value == NULL ? "" : selector->value,
                               value_len, offset, data, len, ignore_case,
                               value_len)) {
      stream_miss_mark(doc, selector);
    }
  }
}

static void observe_exact_stream_chunk(eval_doc *doc,
                                       const lql_selector *selector,
                                       const lonejson_value_path *path,
                                       const char *data, size_t len) {
  size_t i;
  size_t j;
  size_t offset;
  size_t value_len;
  const char *value;
  unsigned int *matches;

  (void)selector;
  (void)path;
  if (doc->hits == NULL || doc->stream_misses == NULL ||
      doc->scalar_len < len) {
    return;
  }
  offset = doc->scalar_len - len;
  for (i = 0u; i < doc->scalar_path_predicate_count; ++i) {
    selector = doc->scalar_path_predicates[i];
    if ((selector->kind != LQL_SELECTOR_KIND_EQ &&
         selector->kind != LQL_SELECTOR_KIND_NE &&
         selector->kind != LQL_SELECTOR_KIND_IN) ||
        selector->value_is_temporal || hit_marked(doc, selector) ||
        stream_miss_marked(doc, selector)) {
      continue;
    }
    if (selector->kind == LQL_SELECTOR_KIND_IN) {
      matches = in_match_row(doc, selector);
      if (matches == NULL) {
        continue;
      }
      for (j = 0u; j < selector->any_count; ++j) {
        if (matches[j] == doc->candidate_epoch) {
          value = selector->any[j];
          value_len = selector->any_lens == NULL ? strlen(value)
                                                 : selector->any_lens[j];
          if (!literal_chunk_matches(value, value_len, offset, data, len, 0,
                                     value_len)) {
            matches[j] = 0u;
          }
        }
      }
      continue;
    }
    value = selector->value == NULL ? "" : selector->value;
    value_len = selector->value_len;
    if (!literal_chunk_matches(value, value_len, offset, data, len, 0,
                               value_len)) {
      stream_miss_mark(doc, selector);
    }
  }
}

static void observe_scalar_exists_begin(eval_doc *doc,
                                        const lql_selector *selector,
                                        const lonejson_value_path *path) {
  size_t i;

  (void)selector;
  (void)path;
  if (doc->hits == NULL || doc->selector == NULL ||
      (doc->selector->predicate_features & LQL_SELECTOR_FEATURE_EXISTS) == 0u) {
    return;
  }
  for (i = 0u; i < doc->scalar_path_predicate_count; ++i) {
    selector = doc->scalar_path_predicates[i];
    if (selector->kind == LQL_SELECTOR_KIND_EXISTS) {
      hit_mark(doc, selector);
    }
  }
}

static void observe_prefix_stream_end(eval_doc *doc,
                                      const lql_selector *selector,
                                      const lonejson_value_path *path) {
  size_t i;
  size_t value_len;
  int ignore_case;

  (void)selector;
  (void)path;
  if (doc->hits == NULL) {
    return;
  }
  for (i = 0u; i < doc->scalar_path_predicate_count; ++i) {
    selector = doc->scalar_path_predicates[i];
    if ((selector->kind != LQL_SELECTOR_KIND_PREFIX &&
         selector->kind != LQL_SELECTOR_KIND_IPREFIX) ||
        hit_marked(doc, selector)) {
      continue;
    }
    value_len = selector_prefix_value_len(selector);
    if (doc->scalar_len < value_len || stream_miss_marked(doc, selector)) {
      continue;
    }
    if (value_len <= doc->prefix_len) {
      ignore_case =
          selector->kind == LQL_SELECTOR_KIND_IPREFIX || selector->ignore_case;
      if (ignore_case &&
          !ascii_case_equal_prefix(
              doc->prefix_buf, selector->value == NULL ? "" : selector->value,
              value_len)) {
        continue;
      }
      if (!ignore_case && memcmp(doc->prefix_buf,
                                 selector->value == NULL ? "" : selector->value,
                                 value_len) != 0) {
        continue;
      }
    }
    hit_mark(doc, selector);
  }
}

static void observe_exact_stream_end(eval_doc *doc,
                                     const lql_selector *selector,
                                     const lonejson_value_path *path) {
  size_t i;
  size_t j;
  size_t value_len;
  const char *value;
  unsigned int *matches;

  (void)selector;
  (void)path;
  if (doc->hits == NULL) {
    return;
  }
  for (i = 0u; i < doc->scalar_path_predicate_count; ++i) {
    selector = doc->scalar_path_predicates[i];
    if ((selector->kind != LQL_SELECTOR_KIND_EQ &&
         selector->kind != LQL_SELECTOR_KIND_NE &&
         selector->kind != LQL_SELECTOR_KIND_IN) ||
        hit_marked(doc, selector)) {
      continue;
    }
    if (selector->kind == LQL_SELECTOR_KIND_EQ ||
        selector->kind == LQL_SELECTOR_KIND_NE) {
      value = selector->value == NULL ? "" : selector->value;
      value_len = selector->value_len;
      if (selector->kind == LQL_SELECTOR_KIND_EQ &&
          doc->scalar_len == value_len && !stream_miss_marked(doc, selector) &&
          (value_len > doc->prefix_len ||
           memcmp(doc->prefix_buf, value, value_len) == 0)) {
        hit_mark(doc, selector);
      } else if (selector->kind == LQL_SELECTOR_KIND_NE &&
                 (doc->scalar_len != value_len ||
                  stream_miss_marked(doc, selector) ||
                  (value_len <= doc->prefix_len &&
                   memcmp(doc->prefix_buf, value, value_len) != 0))) {
        hit_mark(doc, selector);
      }
      continue;
    }
    matches = in_match_row(doc, selector);
    for (j = 0u; j < selector->any_count; ++j) {
      value = selector->any[j];
      value_len =
          selector->any_lens == NULL ? strlen(value) : selector->any_lens[j];
      if (doc->scalar_len == value_len &&
          ((matches != NULL && matches[j] == doc->candidate_epoch) ||
           (value_len <= doc->prefix_len &&
            memcmp(doc->prefix_buf, value, value_len) == 0))) {
        hit_mark(doc, selector);
        break;
      }
    }
  }
}

static void observe_temporal_stream_end(eval_doc *doc,
                                        const lql_selector *selector,
                                        const lonejson_value_path *path) {
  size_t i;
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
  for (i = 0u; i < doc->scalar_path_predicate_count; ++i) {
    selector = doc->scalar_path_predicates[i];
    if (hit_marked(doc, selector)) {
      continue;
    }
    if (selector->kind != LQL_SELECTOR_KIND_EQ &&
        selector->kind != LQL_SELECTOR_KIND_NE &&
        selector->kind != LQL_SELECTOR_KIND_RANGE &&
        selector->kind != LQL_SELECTOR_KIND_DATE) {
      continue;
    }
    if ((selector->kind == LQL_SELECTOR_KIND_EQ ||
         selector->kind == LQL_SELECTOR_KIND_NE) &&
        !selector->value_is_temporal) {
      continue;
    }
    if (selector->kind == LQL_SELECTOR_KIND_RANGE &&
        !selector->range_is_temporal) {
      continue;
    }
    switch (selector->kind) {
    case LQL_SELECTOR_KIND_EQ:
      if (parsed && lql_temporal_equal(&temporal, &selector->temporal_eq)) {
        hit_mark(doc, selector);
      }
      break;
    case LQL_SELECTOR_KIND_NE:
      if (!parsed || !lql_temporal_equal(&temporal, &selector->temporal_eq)) {
        hit_mark(doc, selector);
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
        hit_mark(doc, selector);
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
        hit_mark(doc, selector);
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
      } else if (isdigit(c)) {
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
    } else if (isdigit(c)) {
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
  size_t i;
  double number;

  (void)selector;
  (void)path;
  if (doc->hits == NULL) {
    return;
  }
  number = numeric_stream_value(doc);
  for (i = 0u; i < doc->scalar_path_predicate_count; ++i) {
    selector = doc->scalar_path_predicates[i];
    if (selector->kind != LQL_SELECTOR_KIND_RANGE ||
        selector->range_is_temporal || hit_marked(doc, selector)) {
      continue;
    }
    if ((!selector->has_range_gt || number > selector->range_gt) &&
        (!selector->has_range_gte || number >= selector->range_gte) &&
        (!selector->has_range_lt || number < selector->range_lt) &&
        (!selector->has_range_lte || number <= selector->range_lte)) {
      hit_mark(doc, selector);
    }
  }
}

static int contains_stream_boundary_scan(const char *tail, size_t tail_len,
                                         const char *data, size_t len,
                                         const char *needle, size_t needle_len,
                                         int ignore_case) {
  size_t start;
  size_t i;
  size_t tail_pos;
  size_t data_pos;
  unsigned char a;
  unsigned char b;
  int matched;

  if (tail_len == 0u || len == 0u || needle_len <= 1u) {
    return 0;
  }
  for (start = 0u; start < tail_len; ++start) {
    if (tail_len - start >= needle_len || tail_len - start + len < needle_len) {
      continue;
    }
    matched = 1;
    for (i = 0u; i < needle_len; ++i) {
      tail_pos = start + i;
      if (tail_pos < tail_len) {
        a = (unsigned char)tail[tail_pos];
      } else {
        data_pos = tail_pos - tail_len;
        a = (unsigned char)data[data_pos];
      }
      b = (unsigned char)needle[i];
      if (ignore_case) {
        a = (unsigned char)tolower(a);
        b = (unsigned char)tolower(b);
      }
      if (a != b) {
        matched = 0;
        break;
      }
    }
    if (matched) {
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

static int contains_any_stream_scan(const char *tail, size_t tail_len,
                                    const char *data, size_t len,
                                    char **needles, const size_t *needle_lens,
                                    const unsigned char *firsts,
                                    const unsigned char *first_bitmap,
                                    size_t count, int ignore_case) {
  size_t i;
  size_t needle_len;

  if (contains_any_case_len(data, len, needles, needle_lens, count, firsts,
                            first_bitmap, ignore_case)) {
    return 1;
  }
  if (tail_len == 0u || len == 0u) {
    return 0;
  }
  for (i = 0u; i < count; ++i) {
    needle_len = needle_lens == NULL ? strlen(needles[i]) : needle_lens[i];
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
  size_t i;
  size_t value_len;
  int ignore_case;

  (void)selector;
  (void)path;
  if (doc->hits == NULL) {
    return;
  }
  for (i = 0u; i < doc->scalar_path_predicate_count; ++i) {
    selector = doc->scalar_path_predicates[i];
    if ((selector->kind != LQL_SELECTOR_KIND_CONTAINS &&
         selector->kind != LQL_SELECTOR_KIND_ICONTAINS) ||
        hit_marked(doc, selector)) {
      continue;
    }
    ignore_case =
        selector->kind == LQL_SELECTOR_KIND_ICONTAINS || selector->ignore_case;
    if (selector->any_count == 0u) {
      value_len = selector->value_len;
      if (contains_stream_scan(contains_tail_data(doc), doc->contains_tail_len,
                               data, len,
                               selector->value == NULL ? "" : selector->value,
                               value_len, ignore_case)) {
        hit_mark(doc, selector);
      }
    } else if (contains_any_stream_scan(
                   contains_tail_data(doc), doc->contains_tail_len, data, len,
                   selector->any, selector->any_lens,
                   ignore_case ? selector->any_ifirsts : selector->any_firsts,
                   ignore_case ? selector->any_ifirst_bitmap
                               : selector->any_first_bitmap,
                   selector->any_count, ignore_case)) {
      hit_mark(doc, selector);
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

static void observe_value(eval_doc *doc, const lonejson_value_path *path,
                          const char *value, int is_number, int is_container,
                          int is_null) {
  size_t i;

  if (doc->selector == NULL || doc->selector->kind == LQL_SELECTOR_KIND_ALL) {
    return;
  }
  for (i = 0u; i < doc->predicate_count; ++i) {
    if (selector_path_matches(doc, doc->predicates[i], path)) {
      observe_matched_selector(doc, doc->predicates[i], value, is_number,
                               is_container, is_null);
    }
  }
}

static void observe_prepared_scalar_value(eval_doc *doc, const char *value,
                                          int is_number, int is_null) {
  const lql_selector *selector;
  size_t i;

  if (doc->selector == NULL || doc->selector->kind == LQL_SELECTOR_KIND_ALL) {
    return;
  }
  for (i = 0u; i < doc->scalar_path_predicate_count; ++i) {
    selector = doc->scalar_path_predicates[i];
    observe_matched_selector(doc, selector, value, is_number, 0, is_null);
  }
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
    return doc->hits != NULL && hit_marked(doc, selector);
  }
}

static lonejson_status on_object_begin(void *user,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  eval_doc *doc = (eval_doc *)user;
  (void)error;
  if (path->segment_count == 0u) {
    doc->root_kind = '{';
  }
  observe_value(doc, path, "", 0, 1, 0);
  return push_container(doc, path, '{');
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
  observe_value(doc, path, "", 0, 1, 0);
  return push_container(doc, path, '[');
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
  doc->prefix_buf[0] = '\0';
  numeric_stream_reset(doc);
  scalar_path_match_prepare(doc, path);
  observe_scalar_exists_begin(doc, doc->selector, path);
  doc->scalar_stream_features =
      doc->scalar_path_features &
      (LQL_SELECTOR_FEATURE_CONTAINS | LQL_SELECTOR_FEATURE_PREFIX |
       LQL_SELECTOR_FEATURE_EXACT | LQL_SELECTOR_FEATURE_TEMPORAL);
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
  doc->scalar_len += len;
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_CONTAINS) != 0u) {
    observe_contains_stream_chunk(doc, doc->selector, path, data, len);
    contains_stream_update_tail(doc, data, len);
  }
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_PREFIX) != 0u) {
    observe_prefix_stream_chunk(doc, doc->selector, path, data, len);
    prefix_stream_update(doc, data, len);
  }
  if ((doc->scalar_stream_features &
       (LQL_SELECTOR_FEATURE_EXACT | LQL_SELECTOR_FEATURE_PREFIX)) ==
      LQL_SELECTOR_FEATURE_EXACT) {
    observe_exact_stream_chunk(doc, doc->selector, path, data, len);
    prefix_stream_update(doc, data, len);
  } else if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_EXACT) != 0u) {
    observe_exact_stream_chunk(doc, doc->selector, path, data, len);
  }
  if ((doc->scalar_stream_features &
       (LQL_SELECTOR_FEATURE_TEMPORAL | LQL_SELECTOR_FEATURE_PREFIX |
        LQL_SELECTOR_FEATURE_EXACT)) == LQL_SELECTOR_FEATURE_TEMPORAL) {
    prefix_stream_update(doc, data, len);
  }
  if ((doc->scalar_stream_features &
       (LQL_SELECTOR_FEATURE_NUMERIC_RANGE | LQL_SELECTOR_FEATURE_PREFIX |
        LQL_SELECTOR_FEATURE_EXACT | LQL_SELECTOR_FEATURE_TEMPORAL)) ==
      LQL_SELECTOR_FEATURE_NUMERIC_RANGE) {
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
  doc->scalar_stream_features |=
      doc->scalar_path_features & LQL_SELECTOR_FEATURE_NUMERIC_RANGE;
  if ((doc->scalar_stream_features & LQL_SELECTOR_FEATURE_NUMERIC_RANGE) !=
          0u &&
      doc->prefix_need < LQL_EVAL_NUMERIC_PREFIX_CAP) {
    doc->prefix_need = LQL_EVAL_NUMERIC_PREFIX_CAP;
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
  observe_prepared_scalar_value(doc, value ? "true" : "false", 0, 0);
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
  observe_prepared_scalar_value(doc, "", 0, 1);
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

typedef struct query_stream_state {
  lql *receiver;
  FILE *file;
  lonejson *capture_runtime;
  lql_uint64 offset_base;
  lql_uint64 index_base;
  const lql_selector *selector;
  lql_query_options options;
  lql_query_decision_fn on_decision;
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
  lql_uint64 total_read;
  unsigned char prefix[LQL_SOURCE_PREFIX_CAP];
  size_t prefix_len;
  size_t prefix_offset;
} source_reader_adapter;

typedef struct eval_limited_file_reader {
  FILE *file;
  lql_uint64 remaining;
} eval_limited_file_reader;

typedef struct eval_pread_range_reader {
  int fd;
  lql_uint64 offset;
  lql_uint64 remaining;
} eval_pread_range_reader;

typedef struct spooled_match_state {
  lql *receiver;
  const lql_selector *selector;
  FILE *out;
  int compact;
  const lql_projection *projection;
  const lql_mutation_plan *mutation_plan;
  lql_query_options options;
  int matches_only;
  int expand_arrays;
  lonejson *compact_runtime;
  lql_query_result result;
  lql_error projection_error;
  lql_error mutation_error;
  eval_doc doc;
} spooled_match_state;

typedef struct source_spooled_match_state {
  lql *receiver;
  const lql_selector *selector;
  lql_query_options options;
  lql_query_match_fn on_match;
  void *user;
  lql_query_result result;
  lql_status callback_status;
  eval_doc doc;
} source_spooled_match_state;

static int query_limit_enabled(lql_uint64 limit) { return limit != 0u; }

static lql_query_options
query_remaining_options(const lql_query_options *options,
                        const lql_query_result *result) {
  lql_query_options remaining;
  memset(&remaining, 0, sizeof(remaining));
  if (options == NULL || result == NULL) {
    return remaining;
  }
  remaining = *options;
  if (query_limit_enabled(options->max_matches)) {
    remaining.max_matches =
        result->candidates_matched >= options->max_matches
            ? (lql_uint64)1
            : options->max_matches - result->candidates_matched;
  }
  if (query_limit_enabled(options->max_candidates)) {
    remaining.max_candidates =
        result->candidates_seen >= options->max_candidates
            ? (lql_uint64)1
            : options->max_candidates - result->candidates_seen;
  }
  if (query_limit_enabled(options->max_bytes_read)) {
    remaining.max_bytes_read = options->max_bytes_read;
  }
  return remaining;
}

static int eval_seek_u64(FILE *file, lql_uint64 offset) {
  off_t seek_offset;
  seek_offset = (off_t)offset;
  if (seek_offset < (off_t)0 || (lql_uint64)seek_offset != offset) {
    return 0;
  }
  return fseeko(file, seek_offset, SEEK_SET) == 0;
}

static int eval_file_size_u64(FILE *file, lql_uint64 *out) {
  off_t end;
  if (file == NULL || out == NULL) {
    return 0;
  }
  if (fseeko(file, (off_t)0, SEEK_END) != 0) {
    return 0;
  }
  end = ftello(file);
  if (end < (off_t)0) {
    return 0;
  }
  *out = (lql_uint64)end;
  return (off_t)(*out) == end;
}

static int eval_copy_range(FILE *in, FILE *out, lql_uint64 size) {
  char buf[8192];
  size_t want;
  size_t got;
  while (size != 0u) {
    want = size > (lql_uint64)sizeof(buf) ? sizeof(buf) : (size_t)size;
    got = fread(buf, 1u, want, in);
    if (got == 0u) {
      return 0;
    }
    if (fwrite(buf, 1u, got, out) != got) {
      return 0;
    }
    size -= (lql_uint64)got;
  }
  return 1;
}

static lonejson_read_result
source_reader_read(void *user, unsigned char *buffer, size_t capacity) {
  source_reader_adapter *adapter;
  lql_read_result lql_result;
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
      return result;
    }
  }
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

static int source_reader_prefix_capture(source_reader_adapter *adapter,
                                        int *out_capture_needed) {
  lql_read_result lql_result;
  size_t i;
  unsigned char ch;

  *out_capture_needed = 1;
  adapter->prefix_len = 0u;
  adapter->prefix_offset = 0u;
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
  adapter->total_read += (lql_uint64)lql_result.bytes_read;
  if (adapter->prefix_len == 0u) {
    return 1;
  }
  for (i = 0u; i < adapter->prefix_len; ++i) {
    ch = adapter->prefix[i];
    if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
      continue;
    }
    *out_capture_needed = ch == '[';
    return 1;
  }
  return 1;
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

static void query_finish_source_bytes(lql_query_result *result,
                                      const source_reader_adapter *adapter) {
  if (result == NULL || adapter == NULL || result->stopped_early) {
    return;
  }
  result->bytes_read = adapter->total_read;
}

static lonejson_read_result
eval_limited_file_read(void *user, unsigned char *buffer, size_t capacity) {
  eval_limited_file_reader *reader;
  lonejson_read_result result;
  size_t want;

  result = lonejson_default_read_result();
  reader = (eval_limited_file_reader *)user;
  if (reader->remaining == 0u) {
    result.eof = 1;
    return result;
  }
  want = reader->remaining > (lql_uint64)capacity ? capacity
                                                  : (size_t)reader->remaining;
  result.bytes_read = fread(buffer, 1u, want, reader->file);
  reader->remaining -= (lql_uint64)result.bytes_read;
  if (result.bytes_read != want) {
    result.error_code = 1;
  }
  if (reader->remaining == 0u) {
    result.eof = 1;
  }
  return result;
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

static lql_status eval_project_then_maybe_mutate_spooled(
    lql *self, const lql_projection *projection,
    const lql_mutation_plan *mutation_plan, int matched,
    const lonejson_spooled *spooled, FILE *out, int *out_projected,
    lql_error *error) {
  FILE *projected_file;
  lql_uint64 projected_size;
  lql_status st;
  spooled_source_reader reader;

  if (out_projected != NULL) {
    *out_projected = 0;
  }
  projected_file = tmpfile();
  if (projected_file == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to create projection temp file");
    return LQL_STATUS_JSON_ERROR;
  }
  reader.cursor = *spooled;
  reader.cursor.read_offset = 0u;
  st = self->project_source(self, projection, spooled_source_read, &reader,
                            projected_file, out_projected, error);
  if (st == LQL_STATUS_OK && out_projected != NULL && *out_projected) {
    if (!eval_file_size_u64(projected_file, &projected_size)) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "failed to size projection temp file");
      st = LQL_STATUS_JSON_ERROR;
    } else if (matched && mutation_plan != NULL) {
      st = self->mutate_file_range_paths(self, mutation_plan, projected_file,
                                         0u, projected_size, out, error);
    } else if (!eval_seek_u64(projected_file, 0u) ||
               !eval_copy_range(projected_file, out, projected_size)) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR,
                    "failed to write projected candidate");
      st = LQL_STATUS_JSON_ERROR;
    }
  }
  fclose(projected_file);
  return st;
}

static void query_stop(query_stream_state *state,
                       lql_query_stop_reason reason) {
  state->result.stopped_early = 1;
  state->result.stop_reason = reason;
}

static void query_stream_state_cleanup_capture(query_stream_state *state) {
  if (state != NULL && state->array_spool_initialized) {
    state->array_spool.cleanup(&state->array_spool);
    state->array_spool_initialized = 0;
  }
}

static lonejson_status
query_source_decision_payload_sink(void *user, const void *data, size_t len,
                                   lonejson_error *error) {
  query_stream_state *state;
  size_t copy_len;
  lonejson_status st;

  state = (query_stream_state *)user;
  if (state == NULL || len == 0u) {
    return LONEJSON_STATUS_OK;
  }
  if (state->doc.root_kind == '[') {
    if (!state->array_spool_initialized) {
      if (state->capture_runtime == NULL) {
        if (error != NULL) {
          error->code = LONEJSON_STATUS_INTERNAL_ERROR;
          strcpy(error->message, "source decision spool runtime missing");
        }
        return LONEJSON_STATUS_INTERNAL_ERROR;
      }
      lonejson_spooled_init(state->capture_runtime, &state->array_spool);
      state->array_spool_initialized = 1;
      if (state->pending_payload_prefix_len != 0u) {
        st = state->array_spool.append(
            &state->array_spool, state->pending_payload_prefix,
            state->pending_payload_prefix_len, error);
        if (st != LONEJSON_STATUS_OK) {
          state->callback_status = LQL_STATUS_JSON_ERROR;
          return st;
        }
        state->pending_payload_prefix_len = 0u;
      }
    }
    st = state->array_spool.append(&state->array_spool, data, len, error);
    if (st != LONEJSON_STATUS_OK) {
      state->callback_status = LQL_STATUS_JSON_ERROR;
    }
    return st;
  }
  if (state->doc.root_kind != '\0') {
    state->pending_payload_prefix_len = 0u;
    return LONEJSON_STATUS_OK;
  }
  copy_len =
      sizeof(state->pending_payload_prefix) - state->pending_payload_prefix_len;
  if (copy_len > len) {
    copy_len = len;
  }
  if (copy_len != 0u) {
    memcpy(state->pending_payload_prefix + state->pending_payload_prefix_len,
           data, copy_len);
    state->pending_payload_prefix_len += copy_len;
  }
  return LONEJSON_STATUS_OK;
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
  lql_query_options nested_options;
  lql_query_result nested_result;
  spooled_source_reader nested_reader;
  lql_query_decision decision;
  lql_status st;
  int matched;
  (void)error;
  if (state->doc.root_kind == '[' && state->receiver != NULL &&
      state->file != NULL) {
    nested_options = query_remaining_options(&state->options, &state->result);
    memset(&nested_result, 0, sizeof(nested_result));
    st = execute_query_file_range_decisions(
        state->receiver, state->selector, state->file,
        state->offset_base + (lql_uint64)candidate->stream_offset,
        (lql_uint64)candidate->byte_size,
        state->index_base + state->result.candidates_seen, &nested_options,
        state->on_decision, state->user, &nested_result, NULL);
    reset_doc(&state->doc);
    state->result.candidates_seen += nested_result.candidates_seen;
    state->result.candidates_matched += nested_result.candidates_matched;
    state->result.bytes_read = state->offset_base +
                               (lql_uint64)candidate->stream_offset +
                               (lql_uint64)candidate->byte_size;
    if (nested_result.stopped_early) {
      state->result.stopped_early = 1;
      state->result.stop_reason = nested_result.stop_reason;
      return LONEJSON_CANDIDATE_STOP;
    }
    if (st != LQL_STATUS_OK) {
      state->callback_status = st;
      return LONEJSON_CANDIDATE_ERROR;
    }
    if (query_limit_enabled(state->options.max_matches) &&
        state->result.candidates_matched >= state->options.max_matches) {
      query_stop(state, LQL_QUERY_STOP_MATCH_LIMIT);
      return LONEJSON_CANDIDATE_STOP;
    }
    if (query_limit_enabled(state->options.max_candidates) &&
        state->result.candidates_seen >= state->options.max_candidates) {
      query_stop(state, LQL_QUERY_STOP_CANDIDATE_LIMIT);
      return LONEJSON_CANDIDATE_STOP;
    }
    if (query_limit_enabled(state->options.max_bytes_read) &&
        state->result.bytes_read >= state->options.max_bytes_read) {
      query_stop(state, LQL_QUERY_STOP_BYTE_LIMIT);
      return LONEJSON_CANDIDATE_STOP;
    }
    return LONEJSON_CANDIDATE_CONTINUE;
  }
  if (state->doc.root_kind == '[' && state->receiver != NULL &&
      state->array_spool_initialized) {
    nested_reader.cursor = state->array_spool;
    nested_reader.cursor.read_offset = 0u;
    nested_options = query_remaining_options(&state->options, &state->result);
    memset(&nested_result, 0, sizeof(nested_result));
    st = execute_query_source_decisions_with_base(
        state->receiver, state->selector, spooled_source_read, &nested_reader,
        state->offset_base + (lql_uint64)candidate->stream_offset,
        state->index_base + state->result.candidates_seen, &nested_options,
        state->on_decision, state->user, &nested_result, NULL);
    reset_doc(&state->doc);
    state->result.candidates_seen += nested_result.candidates_seen;
    state->result.candidates_matched += nested_result.candidates_matched;
    state->result.bytes_read = state->offset_base +
                               (lql_uint64)candidate->stream_offset +
                               (lql_uint64)candidate->byte_size;
    if (nested_result.stopped_early) {
      state->result.stopped_early = 1;
      state->result.stop_reason = nested_result.stop_reason;
      return LONEJSON_CANDIDATE_STOP;
    }
    if (st != LQL_STATUS_OK) {
      state->callback_status = st;
      return LONEJSON_CANDIDATE_ERROR;
    }
    if (query_limit_enabled(state->options.max_matches) &&
        state->result.candidates_matched >= state->options.max_matches) {
      query_stop(state, LQL_QUERY_STOP_MATCH_LIMIT);
      return LONEJSON_CANDIDATE_STOP;
    }
    if (query_limit_enabled(state->options.max_candidates) &&
        state->result.candidates_seen >= state->options.max_candidates) {
      query_stop(state, LQL_QUERY_STOP_CANDIDATE_LIMIT);
      return LONEJSON_CANDIDATE_STOP;
    }
    if (query_limit_enabled(state->options.max_bytes_read) &&
        state->result.bytes_read >= state->options.max_bytes_read) {
      query_stop(state, LQL_QUERY_STOP_BYTE_LIMIT);
      return LONEJSON_CANDIDATE_STOP;
    }
    return LONEJSON_CANDIDATE_CONTINUE;
  }
  matched = state->selector == NULL ||
            state->selector->kind == LQL_SELECTOR_KIND_ALL ||
            eval_selector_tree(state->selector, &state->doc);
  if (matched) {
    state->result.candidates_matched++;
  }
  state->result.candidates_seen++;
  state->result.bytes_read = state->offset_base +
                             (lql_uint64)candidate->stream_offset +
                             (lql_uint64)candidate->byte_size;
  decision.matched = matched;
  decision.index = state->index_base + (lql_uint64)candidate->index;
  decision.offset = state->offset_base + (lql_uint64)candidate->stream_offset;
  decision.size = (lql_uint64)candidate->byte_size;
  st = state->on_decision(state->user, &decision);
  reset_doc(&state->doc);
  if (st == LQL_STATUS_STOP) {
    query_stop(state, LQL_QUERY_STOP_CALLBACK);
    return LONEJSON_CANDIDATE_STOP;
  }
  if (st != LQL_STATUS_OK) {
    state->callback_status = st;
    return LONEJSON_CANDIDATE_ERROR;
  }
  if (query_limit_enabled(state->options.max_matches) &&
      state->result.candidates_matched >= state->options.max_matches) {
    query_stop(state, LQL_QUERY_STOP_MATCH_LIMIT);
    return LONEJSON_CANDIDATE_STOP;
  }
  if (query_limit_enabled(state->options.max_candidates) &&
      state->result.candidates_seen >= state->options.max_candidates) {
    query_stop(state, LQL_QUERY_STOP_CANDIDATE_LIMIT);
    return LONEJSON_CANDIDATE_STOP;
  }
  if (query_limit_enabled(state->options.max_bytes_read) &&
      state->result.bytes_read >= state->options.max_bytes_read) {
    query_stop(state, LQL_QUERY_STOP_BYTE_LIMIT);
    return LONEJSON_CANDIDATE_STOP;
  }
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lonejson_status file_sink(void *user, const void *data, size_t len,
                                 lonejson_error *error) {
  FILE *out;
  (void)error;
  out = (FILE *)user;
  if (len != 0u && fwrite(data, 1u, len, out) != len) {
    return LONEJSON_STATUS_IO_ERROR;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status write_spooled_payload(FILE *out,
                                             const lonejson_spooled *spooled,
                                             int compact, lonejson *runtime,
                                             lonejson_error *error) {
  lonejson_writer writer;
  lonejson_status st;
  int writer_initialized;

  if (!compact) {
    return lonejson_spooled_write_to_sink(spooled, file_sink, out, error);
  }
  writer_initialized = 0;
  st = lonejson_writer_init_sink(runtime, &writer, file_sink, out, error);
  if (st == LONEJSON_STATUS_OK) {
    writer_initialized = 1;
    st = lonejson_writer_json_value_spooled(&writer, spooled, error);
  }
  if (st == LONEJSON_STATUS_OK) {
    st = lonejson_writer_finish(&writer, error);
  }
  if (writer_initialized) {
    lonejson_writer_cleanup(&writer);
  }
  return st;
}

static lonejson_read_result eval_spooled_read(void *user, unsigned char *buffer,
                                              size_t capacity) {
  return lonejson_spooled_read((lonejson_spooled *)user, buffer, capacity);
}

static lonejson_candidate_callback_result
on_spooled_candidate_begin(void *user, const lonejson_candidate_info *candidate,
                           lonejson_error *error);
static lonejson_candidate_callback_result
on_spooled_candidate_end(void *user, const lonejson_candidate_info *candidate,
                         lonejson_error *error);

static lonejson_status write_spooled_array_candidates(
    lql *self, const lql_selector *selector, const lql_projection *projection,
    const lql_mutation_plan *mutation_plan, int matches_only,
    const lonejson_spooled *spooled, FILE *out, int compact,
    lonejson *compact_runtime, lql_query_result *out_result,
    lonejson_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_spooled cursor;
  spooled_match_state state;
  lonejson_status st;

  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    *error = lj_error;
    return LONEJSON_STATUS_INTERNAL_ERROR;
  }
  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.selector = selector;
  state.out = out;
  state.compact = compact;
  state.projection = projection;
  state.mutation_plan = mutation_plan;
  state.compact_runtime = compact_runtime;
  state.matches_only = matches_only;
  state.expand_arrays = 1;
  lql_error_init(&state.projection_error);
  lql_error_init(&state.mutation_error);
  if (!init_doc(&state.doc, self, selector)) {
    lonejson_free(runtime);
    error->code = LONEJSON_STATUS_ALLOCATION_FAILED;
    strcpy(error->message, "failed to initialize nested array mutation");
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  cursor = *spooled;
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_SPOOLED;
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_spooled_candidate_begin;
  options.candidate_end = on_spooled_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_reader(runtime, eval_spooled_read, &cursor,
                                        &options, &lj_error);
  destroy_doc(&state.doc);
  lonejson_free(runtime);
  if (out_result != NULL) {
    *out_result = state.result;
  }
  if (st != LONEJSON_STATUS_OK) {
    if (state.mutation_error.code != LQL_STATUS_OK) {
      error->code = LONEJSON_STATUS_CALLBACK_FAILED;
      strncpy(error->message, state.mutation_error.message,
              sizeof(error->message) - 1u);
      error->message[sizeof(error->message) - 1u] = '\0';
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    *error = lj_error;
  }
  return st;
}

static lonejson_candidate_callback_result
on_spooled_candidate_begin(void *user, const lonejson_candidate_info *candidate,
                           lonejson_error *error) {
  spooled_match_state *state = (spooled_match_state *)user;
  (void)candidate;
  (void)error;
  reset_doc(&state->doc);
  return LONEJSON_CANDIDATE_CONTINUE;
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
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lonejson_candidate_callback_result
on_source_spooled_candidate_end(void *user,
                                const lonejson_candidate_info *candidate,
                                lonejson_error *error) {
  source_spooled_match_state *state;
  spooled_source_reader nested_reader;
  lql_query_options nested_options;
  lql_query_result nested_result;
  lql_query_match match;
  lql_status st;
  int matched;

  state = (source_spooled_match_state *)user;
  if (state->doc.root_kind == '[' && candidate->payload_spool != NULL &&
      state->receiver != NULL) {
    nested_reader.cursor = *candidate->payload_spool;
    nested_reader.cursor.read_offset = 0u;
    nested_options = query_remaining_options(&state->options, &state->result);
    memset(&nested_result, 0, sizeof(nested_result));
    st = execute_query_source_spooled_matches(
        state->receiver, state->selector, spooled_source_read, &nested_reader,
        &nested_options, state->on_match, state->user, &nested_result, NULL);
    reset_doc(&state->doc);
    state->result.candidates_seen += nested_result.candidates_seen;
    state->result.candidates_matched += nested_result.candidates_matched;
    state->result.bytes_read =
        (lql_uint64)(candidate->stream_offset + candidate->byte_size);
    if (nested_result.stopped_early) {
      state->result.stopped_early = 1;
      state->result.stop_reason = nested_result.stop_reason;
      return LONEJSON_CANDIDATE_STOP;
    }
    if (st != LQL_STATUS_OK) {
      state->callback_status = st;
      return LONEJSON_CANDIDATE_ERROR;
    }
    return LONEJSON_CANDIDATE_CONTINUE;
  }
  matched = state->selector == NULL ||
            state->selector->kind == LQL_SELECTOR_KIND_ALL ||
            eval_selector_tree(state->selector, &state->doc);
  state->result.candidates_seen++;
  state->result.bytes_read =
      (lql_uint64)(candidate->stream_offset + candidate->byte_size);
  if (matched) {
    if (candidate->payload_spool == NULL) {
      reset_doc(&state->doc);
      return LONEJSON_CANDIDATE_ERROR;
    }
    memset(&match, 0, sizeof(match));
    match.decision.matched = 1;
    match.decision.index = (lql_uint64)candidate->index;
    match.decision.offset = (lql_uint64)candidate->stream_offset;
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
      reset_doc(&state->doc);
      return LONEJSON_CANDIDATE_STOP;
    }
    if (st != LQL_STATUS_OK) {
      state->callback_status = st;
      reset_doc(&state->doc);
      return LONEJSON_CANDIDATE_ERROR;
    }
  }
  reset_doc(&state->doc);
  if (query_limit_enabled(state->options.max_matches) &&
      state->result.candidates_matched >= state->options.max_matches) {
    state->result.stopped_early = 1;
    state->result.stop_reason = LQL_QUERY_STOP_MATCH_LIMIT;
    return LONEJSON_CANDIDATE_STOP;
  }
  if (query_limit_enabled(state->options.max_candidates) &&
      state->result.candidates_seen >= state->options.max_candidates) {
    state->result.stopped_early = 1;
    state->result.stop_reason = LQL_QUERY_STOP_CANDIDATE_LIMIT;
    return LONEJSON_CANDIDATE_STOP;
  }
  if (query_limit_enabled(state->options.max_bytes_read) &&
      state->result.bytes_read >= state->options.max_bytes_read) {
    state->result.stopped_early = 1;
    state->result.stop_reason = LQL_QUERY_STOP_BYTE_LIMIT;
    return LONEJSON_CANDIDATE_STOP;
  }
  (void)error;
  return LONEJSON_CANDIDATE_CONTINUE;
}

static lonejson_candidate_callback_result
on_spooled_candidate_end(void *user, const lonejson_candidate_info *candidate,
                         lonejson_error *error) {
  spooled_match_state *state = (spooled_match_state *)user;
  lql_query_result nested_result;
  lonejson_status write_status;
  int matched;
  int projected;
  int wrote_output;
  write_status = LONEJSON_STATUS_OK;
  projected = 0;
  wrote_output = 0;
  if (state->doc.root_kind == '[' && state->expand_arrays &&
      candidate->payload_spool != NULL) {
    memset(&nested_result, 0, sizeof(nested_result));
    write_status = write_spooled_array_candidates(
        state->receiver, state->selector, state->projection,
        state->mutation_plan, state->matches_only, candidate->payload_spool,
        state->out, state->compact, state->compact_runtime, &nested_result,
        error);
    reset_doc(&state->doc);
    state->result.candidates_seen += nested_result.candidates_seen;
    state->result.candidates_matched += nested_result.candidates_matched;
    state->result.bytes_read =
        (lql_uint64)(candidate->stream_offset + candidate->byte_size);
    if (write_status != LONEJSON_STATUS_OK) {
      return LONEJSON_CANDIDATE_ERROR;
    }
    return LONEJSON_CANDIDATE_CONTINUE;
  }
  matched = state->selector == NULL ||
            state->selector->kind == LQL_SELECTOR_KIND_ALL ||
            eval_selector_tree(state->selector, &state->doc);
  if (matched || state->mutation_plan != NULL) {
    if (candidate->payload_spool == NULL) {
      reset_doc(&state->doc);
      return LONEJSON_CANDIDATE_ERROR;
    }
    if (state->mutation_plan != NULL) {
      if (!matched && state->matches_only) {
        state->result.candidates_seen++;
        state->result.bytes_read =
            (lql_uint64)(candidate->stream_offset + candidate->byte_size);
        reset_doc(&state->doc);
        return LONEJSON_CANDIDATE_CONTINUE;
      }
      if (state->projection != NULL) {
        if (eval_project_then_maybe_mutate_spooled(
                state->receiver, state->projection, state->mutation_plan,
                matched, candidate->payload_spool, state->out, &projected,
                &state->mutation_error) != LQL_STATUS_OK) {
          error->code = LONEJSON_STATUS_CALLBACK_FAILED;
          strncpy(error->message, state->mutation_error.message,
                  sizeof(error->message) - 1u);
          error->message[sizeof(error->message) - 1u] = '\0';
          reset_doc(&state->doc);
          return LONEJSON_CANDIDATE_ERROR;
        }
        if (!projected) {
          state->result.candidates_seen++;
          state->result.bytes_read =
              (lql_uint64)(candidate->stream_offset + candidate->byte_size);
          reset_doc(&state->doc);
          return LONEJSON_CANDIDATE_CONTINUE;
        }
        wrote_output = 1;
      } else if (matched) {
        if (state->doc.root_kind == '[' && state->expand_arrays) {
          memset(&nested_result, 0, sizeof(nested_result));
          write_status = write_spooled_array_candidates(
              state->receiver, state->selector, state->projection,
              state->mutation_plan, state->matches_only,
              candidate->payload_spool, state->out, state->compact,
              state->compact_runtime, &nested_result, error);
          state->result.candidates_seen += nested_result.candidates_seen;
          state->result.candidates_matched += nested_result.candidates_matched;
          if (write_status != LONEJSON_STATUS_OK) {
            reset_doc(&state->doc);
            return LONEJSON_CANDIDATE_ERROR;
          }
        } else if (state->doc.root_kind != '{') {
          write_status = write_spooled_payload(
              state->out, candidate->payload_spool, state->compact,
              state->compact_runtime, error);
          if (write_status != LONEJSON_STATUS_OK) {
            reset_doc(&state->doc);
            return LONEJSON_CANDIDATE_ERROR;
          }
          wrote_output = 1;
        } else {
          spooled_source_reader reader;
          reader.cursor = *candidate->payload_spool;
          reader.cursor.read_offset = 0u;
          if (state->receiver->mutate_source_paths(
                  state->receiver, state->mutation_plan, spooled_source_read,
                  &reader, state->out,
                  &state->mutation_error) != LQL_STATUS_OK) {
            error->code = LONEJSON_STATUS_CALLBACK_FAILED;
            strncpy(error->message, state->mutation_error.message,
                    sizeof(error->message) - 1u);
            error->message[sizeof(error->message) - 1u] = '\0';
            reset_doc(&state->doc);
            return LONEJSON_CANDIDATE_ERROR;
          }
        }
        if (state->doc.root_kind == '{') {
          wrote_output = 1;
        }
      } else {
        write_status = write_spooled_payload(
            state->out, candidate->payload_spool, state->compact,
            state->compact_runtime, error);
        if (write_status != LONEJSON_STATUS_OK) {
          reset_doc(&state->doc);
          return LONEJSON_CANDIDATE_ERROR;
        }
        wrote_output = 1;
      }
    } else if (state->projection != NULL) {
      spooled_source_reader reader;
      reader.cursor = *candidate->payload_spool;
      reader.cursor.read_offset = 0u;
      if (state->receiver->project_source(
              state->receiver, state->projection, spooled_source_read, &reader,
              state->out, &projected,
              &state->projection_error) != LQL_STATUS_OK) {
        error->code = LONEJSON_STATUS_CALLBACK_FAILED;
        strncpy(error->message, state->projection_error.message,
                sizeof(error->message) - 1u);
        error->message[sizeof(error->message) - 1u] = '\0';
        reset_doc(&state->doc);
        return LONEJSON_CANDIDATE_ERROR;
      }
      if (!projected) {
        state->result.candidates_seen++;
        state->result.bytes_read =
            (lql_uint64)(candidate->stream_offset + candidate->byte_size);
        reset_doc(&state->doc);
        return LONEJSON_CANDIDATE_CONTINUE;
      }
      wrote_output = 1;
    } else {
      write_status =
          write_spooled_payload(state->out, candidate->payload_spool,
                                state->compact, state->compact_runtime, error);
      if (write_status != LONEJSON_STATUS_OK) {
        reset_doc(&state->doc);
        return LONEJSON_CANDIDATE_ERROR;
      }
      wrote_output = 1;
    }
    if (wrote_output && fputc('\n', state->out) == EOF) {
      reset_doc(&state->doc);
      return LONEJSON_CANDIDATE_ERROR;
    }
    if (matched) {
      state->result.candidates_matched++;
    }
  }
  state->result.candidates_seen++;
  state->result.bytes_read =
      (lql_uint64)(candidate->stream_offset + candidate->byte_size);
  reset_doc(&state->doc);
  if (query_limit_enabled(state->options.max_matches) &&
      state->result.candidates_matched >= state->options.max_matches) {
    state->result.stopped_early = 1;
    state->result.stop_reason = LQL_QUERY_STOP_MATCH_LIMIT;
    return LONEJSON_CANDIDATE_STOP;
  }
  if (query_limit_enabled(state->options.max_candidates) &&
      state->result.candidates_seen >= state->options.max_candidates) {
    state->result.stopped_early = 1;
    state->result.stop_reason = LQL_QUERY_STOP_CANDIDATE_LIMIT;
    return LONEJSON_CANDIDATE_STOP;
  }
  if (query_limit_enabled(state->options.max_bytes_read) &&
      state->result.bytes_read >= state->options.max_bytes_read) {
    state->result.stopped_early = 1;
    state->result.stop_reason = LQL_QUERY_STOP_BYTE_LIMIT;
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
  int runtime_cached;
  eval_doc doc;

  if (!init_doc(&doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime_cached = 0;
  runtime = lql_lonejson_acquire(self, &runtime_cached, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&doc);
    return LQL_STATUS_JSON_ERROR;
  }
  init_eval_visitor(&visitor);
  st = lonejson_visit_path_value_buffer(runtime, json, json_len, &visitor, &doc,
                                        &lj_error);
  if (st != LONEJSON_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&doc);
    lql_lonejson_release(self, runtime, runtime_cached);
    return LQL_STATUS_JSON_ERROR;
  }
  *out_matched = selector == NULL || selector->kind == LQL_SELECTOR_KIND_ALL ||
                 eval_selector_tree(selector, &doc);
  destroy_doc(&doc);
  lql_lonejson_release(self, runtime, runtime_cached);
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
  lql_match_adapter adapter;
  lql_status st;
  if (file == NULL || on_match == NULL) {
    clear_query_result(out_result);
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "file and on_match are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  adapter.file = file;
  adapter.on_match = on_match;
  adapter.user = user;
  st = self->query_file_decisions_with_options(self, selector, file, options,
                                               on_match_decision, &adapter,
                                               out_result, error);
  if (st != LQL_STATUS_OK && error != NULL &&
      strcmp(error->message, "query decision callback failed") == 0) {
    lql_set_error(error, st, "query match callback failed");
  }
  return st;
}

static lql_status payload_write_json_method(lql *self,
                                            const lql_payload *payload,
                                            FILE *out, lql_error *error) {
  if (payload == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "payload and output file are required");
    return LQL_STATUS_INVALID_ARGUMENT;
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
  return execute_query_file_range_spooled_matches(
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
  return execute_query_file_range_spooled_matches(
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
  return execute_query_file_range_spooled_matches(
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
  return execute_query_file_range_spooled_matches(
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
  return execute_query_source_spooled_rewrite(
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
  return execute_query_source_spooled_rewrite(
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
  return execute_query_source_spooled_rewrite(
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
  return execute_query_source_spooled_rewrite(
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
  int runtime_cached;
  query_stream_state state;

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
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime_cached = 0;
  runtime = lql_lonejson_acquire(self, &runtime_cached, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_NONE;
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_candidate_begin;
  options.candidate_end = on_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_filep(runtime, file, &options, &lj_error);
  if (st != LONEJSON_STATUS_OK) {
    destroy_doc(&state.doc);
    lql_lonejson_release(self, runtime, runtime_cached);
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
  lql_lonejson_release(self, runtime, runtime_cached);
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}

static lql_status execute_query_file_range_decisions(
    lql *self, const lql_selector *selector, FILE *file, lql_uint64 offset,
    lql_uint64 size, lql_uint64 index_base,
    const lql_query_options *query_options, lql_query_decision_fn on_decision,
    void *user, lql_query_result *out_result, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  int runtime_cached;
  query_stream_state state;
  eval_pread_range_reader reader;

  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.file = file;
  state.offset_base = offset;
  state.index_base = index_base;
  state.selector = selector;
  state.on_decision = on_decision;
  state.user = user;
  state.callback_status = LQL_STATUS_OK;
  if (query_options != NULL) {
    state.options = *query_options;
  }
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime_cached = 0;
  runtime = lql_lonejson_acquire(self, &runtime_cached, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  reader.fd = fileno(file);
  if (reader.fd < 0) {
    destroy_doc(&state.doc);
    lql_lonejson_release(self, runtime, runtime_cached);
    lql_set_error(error, LQL_STATUS_JSON_ERROR,
                  "failed to access input range descriptor");
    return LQL_STATUS_JSON_ERROR;
  }
  reader.offset = offset;
  reader.remaining = size;
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_NONE;
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_candidate_begin;
  options.candidate_end = on_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_reader(runtime, eval_pread_range, &reader,
                                        &options, &lj_error);
  destroy_doc(&state.doc);
  lql_lonejson_release(self, runtime, runtime_cached);
  if (st != LONEJSON_STATUS_OK) {
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
  int runtime_cached;
  query_stream_state state;
  source_reader_adapter adapter;
  int capture_needed;

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
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime_cached = 0;
  runtime = lql_lonejson_acquire(self, &runtime_cached, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  state.capture_runtime = runtime;
  adapter.read = read;
  adapter.user = read_user;
  adapter.error_code = 0;
  adapter.total_read = 0u;
  adapter.prefix_len = 0u;
  adapter.prefix_offset = 0u;
  capture_needed = 1;
  if (!source_reader_prefix_capture(&adapter, &capture_needed)) {
    destroy_doc(&state.doc);
    lql_lonejson_release(self, runtime, runtime_cached);
    if (out_result != NULL) {
      *out_result = state.result;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, "query source reader failed");
    return LQL_STATUS_JSON_ERROR;
  }
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  if (capture_needed) {
    options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_SINK;
    options.payload_sink = query_source_decision_payload_sink;
    options.payload_sink_user = &state;
  } else {
    options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_NONE;
  }
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_candidate_begin;
  options.candidate_end = on_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_reader(runtime, source_reader_read, &adapter,
                                        &options, &lj_error);
  if (st == LONEJSON_STATUS_OK || adapter.error_code != 0 ||
      state.callback_status != LQL_STATUS_OK) {
    query_finish_source_bytes(&state.result, &adapter);
  }
  if (st != LONEJSON_STATUS_OK) {
    query_stream_state_cleanup_capture(&state);
    destroy_doc(&state.doc);
    lql_lonejson_release(self, runtime, runtime_cached);
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
  lql_lonejson_release(self, runtime, runtime_cached);
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}

static lql_status execute_query_source_spooled_matches(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    const lql_query_options *query_options, lql_query_match_fn on_match,
    void *user, lql_query_result *out_result, lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  source_spooled_match_state state;
  source_reader_adapter adapter;

  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.selector = selector;
  state.on_match = on_match;
  state.user = user;
  state.callback_status = LQL_STATUS_OK;
  if (query_options != NULL) {
    state.options = *query_options;
  }
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  adapter.read = read;
  adapter.user = read_user;
  adapter.error_code = 0;
  adapter.total_read = 0u;
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_SPOOLED;
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_source_spooled_candidate_begin;
  options.candidate_end = on_source_spooled_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_reader(runtime, source_reader_read, &adapter,
                                        &options, &lj_error);
  if (st == LONEJSON_STATUS_OK || adapter.error_code != 0 ||
      state.callback_status != LQL_STATUS_OK) {
    query_finish_source_bytes(&state.result, &adapter);
  }
  if (st != LONEJSON_STATUS_OK) {
    destroy_doc(&state.doc);
    lonejson_free(runtime);
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
  lonejson_free(runtime);
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}

static lql_status execute_query_file_range_spooled_matches(
    lql *self, const lql_selector *selector, FILE *file, lql_uint64 offset,
    lql_uint64 size, FILE *out, int compact, const lql_projection *projection,
    const lql_mutation_plan *mutation_plan, int matches_only,
    const lql_query_options *query_options, lql_query_result *out_result,
    lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  spooled_match_state state;
  eval_limited_file_reader reader;

  if (file == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "input and output files are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (!eval_seek_u64(file, offset)) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, "failed to seek input range");
    return LQL_STATUS_JSON_ERROR;
  }
  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.selector = selector;
  state.out = out;
  state.compact = compact;
  state.projection = projection;
  state.mutation_plan = mutation_plan;
  state.matches_only = matches_only;
  state.expand_arrays = 1;
  if (query_options != NULL) {
    state.options = *query_options;
  }
  lql_error_init(&state.projection_error);
  lql_error_init(&state.mutation_error);
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  if (compact) {
    state.compact_runtime = lql_lonejson_new(self, &lj_error);
    if (state.compact_runtime == NULL) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
      lonejson_free(runtime);
      destroy_doc(&state.doc);
      return LQL_STATUS_JSON_ERROR;
    }
  }
  reader.file = file;
  reader.remaining = size;
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_SPOOLED;
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_spooled_candidate_begin;
  options.candidate_end = on_spooled_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_reader(runtime, eval_limited_file_read,
                                        &reader, &options, &lj_error);
  if (state.compact_runtime != NULL) {
    lonejson_free(state.compact_runtime);
  }
  destroy_doc(&state.doc);
  lonejson_free(runtime);
  if (st != LONEJSON_STATUS_OK) {
    if (out_result != NULL) {
      *out_result = state.result;
    }
    if (state.mutation_error.code != LQL_STATUS_OK) {
      lql_set_error(error, state.mutation_error.code,
                    state.mutation_error.message);
      return state.mutation_error.code;
    }
    if (state.projection_error.code != LQL_STATUS_OK) {
      lql_set_error(error, state.projection_error.code,
                    state.projection_error.message);
      return state.projection_error.code;
    }
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    return LQL_STATUS_JSON_ERROR;
  }
  if (out_result != NULL) {
    *out_result = state.result;
  }
  return LQL_STATUS_OK;
}

static lql_status execute_query_source_spooled_rewrite(
    lql *self, const lql_selector *selector, lql_read_fn read, void *read_user,
    FILE *out, int compact, const lql_projection *projection,
    const lql_mutation_plan *mutation_plan, int matches_only,
    const lql_query_options *query_options, lql_query_result *out_result,
    lql_error *error) {
  lonejson *runtime;
  lonejson_error lj_error;
  lonejson_path_value_visitor visitor;
  lonejson_candidate_stream_options options;
  lonejson_status st;
  spooled_match_state state;
  source_reader_adapter adapter;

  if (read == NULL || out == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "read and output file are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&state, 0, sizeof(state));
  state.receiver = self;
  state.selector = selector;
  state.out = out;
  state.compact = compact;
  state.projection = projection;
  state.mutation_plan = mutation_plan;
  state.matches_only = matches_only;
  state.expand_arrays = 1;
  if (query_options != NULL) {
    state.options = *query_options;
  }
  lql_error_init(&state.projection_error);
  lql_error_init(&state.mutation_error);
  memset(&adapter, 0, sizeof(adapter));
  adapter.read = read;
  adapter.user = read_user;
  if (!init_doc(&state.doc, self, selector)) {
    return LQL_STATUS_NO_MEMORY;
  }
  runtime = lql_lonejson_new(self, &lj_error);
  if (runtime == NULL) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
    destroy_doc(&state.doc);
    return LQL_STATUS_JSON_ERROR;
  }
  if (compact) {
    state.compact_runtime = lql_lonejson_new(self, &lj_error);
    if (state.compact_runtime == NULL) {
      lql_set_error(error, LQL_STATUS_JSON_ERROR, lj_error.message);
      lonejson_free(runtime);
      destroy_doc(&state.doc);
      return LQL_STATUS_JSON_ERROR;
    }
  }
  init_eval_visitor(&visitor);
  options = lonejson_default_candidate_stream_options();
  options.capture_mode = LONEJSON_CANDIDATE_CAPTURE_SPOOLED;
  options.path_visitor = &visitor;
  options.visitor_user = &state.doc;
  options.candidate_begin = on_spooled_candidate_begin;
  options.candidate_end = on_spooled_candidate_end;
  options.candidate_user = &state;
  st = lonejson_visit_candidates_reader(runtime, source_reader_read, &adapter,
                                        &options, &lj_error);
  if (state.compact_runtime != NULL) {
    lonejson_free(state.compact_runtime);
  }
  if (out_result != NULL) {
    if (!state.result.stopped_early) {
      state.result.bytes_read = adapter.total_read;
    }
    *out_result = state.result;
  }
  destroy_doc(&state.doc);
  lonejson_free(runtime);
  if (st != LONEJSON_STATUS_OK) {
    if (state.mutation_error.code != LQL_STATUS_OK) {
      lql_set_error(error, state.mutation_error.code,
                    state.mutation_error.message);
      return state.mutation_error.code;
    }
    if (state.projection_error.code != LQL_STATUS_OK) {
      lql_set_error(error, state.projection_error.code,
                    state.projection_error.message);
      return state.projection_error.code;
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
