#include "lql_json_scan.h"
#include "lql_unicode_lower.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define LQL_JSON_INLINE static __inline__ __attribute__((always_inline))
#else
#define LQL_JSON_INLINE static
#endif

#define LQL_JSON_READ_BUFFER_SIZE 65536u
#define LQL_JSON_EMIT_BUFFER_SIZE 65536u
#define LQL_JSON_MAX_DEPTH 128u
#define LQL_JSON_FLAT_TERM_CAPACITY (sizeof(unsigned long) * CHAR_BIT)
#define LQL_JSON_NUMBER_MATCH_BYTES 128u

typedef struct lql_json_scan {
  lql_stream_reader_fn reader;
  void *reader_user;
  lql_stream_writer_fn writer;
  void *writer_user;
  unsigned char buffer[LQL_JSON_READ_BUFFER_SIZE];
  unsigned char emit_buffer[LQL_JSON_EMIT_BUFFER_SIZE];
  size_t emit_len;
  size_t offset;
  size_t length;
  size_t bytes_read;
  size_t depth;
  lql_error *error;
  int flat_eq_active;
  int flat_eq_stop_on_hit;
  unsigned long flat_eq_stop_hit_mask;
  int flat_eq_has_array_terms;
  int flat_eq_has_object_wildcards;
  int flat_eq_has_recursive_terms;
  const lql_json_flat_eq_term *flat_terms;
  size_t flat_term_count;
  const lql_json_capture_key *capture_keys;
  size_t capture_key_count;
  lql_json_capture_span *capture_spans;
  unsigned long capture_active;
  unsigned long capture_failed;
  size_t capture_pos[LQL_JSON_FLAT_TERM_CAPACITY];
  size_t capture_segment[LQL_JSON_FLAT_TERM_CAPACITY];
  unsigned long capture_path_active[LQL_JSON_MAX_DEPTH];
  unsigned long match_active;
  unsigned long match_failed;
  unsigned long match_contains;
  unsigned long match_icontains;
  unsigned char match_case_bytes[4];
  size_t match_case_len;
  size_t match_case_need;
  size_t match_pos[LQL_JSON_FLAT_TERM_CAPACITY];
  size_t match_term_segment[LQL_JSON_FLAT_TERM_CAPACITY];
  size_t key_term_segment[LQL_JSON_FLAT_TERM_CAPACITY];
  unsigned long number_range_active;
  unsigned long number_range_failed;
  char number_match[LQL_JSON_NUMBER_MATCH_BYTES];
  size_t number_match_len;
  unsigned long temporal_range_active;
  unsigned long temporal_range_failed;
  char temporal_match[LQL_JSON_NUMBER_MATCH_BYTES];
  size_t temporal_match_len;
  int match_key;
  size_t match_path_segment;
  unsigned long flat_eq_hits;
  unsigned long path_active[LQL_JSON_MAX_DEPTH];
  unsigned long recursive_active[LQL_JSON_MAX_DEPTH];
  int compact_range_active;
  int compact_range_ok;
} lql_json_scan;

static void lql_json_error(lql_json_scan *scan, const char *message) {
  if (scan->error != NULL) {
    scan->error->code = LQL_STATUS_JSON_ERROR;
    strncpy(scan->error->message, message, sizeof(scan->error->message) - 1u);
    scan->error->message[sizeof(scan->error->message) - 1u] = '\0';
  }
}

static lql_status lql_json_write(lql_json_scan *scan, const void *data,
                                 size_t len) {
  const unsigned char *bytes;
  lql_status status;
  if (scan->writer == NULL) {
    return LQL_STATUS_OK;
  }
  bytes = (const unsigned char *)data;
  while (len != 0u) {
    size_t amount;
    if (scan->emit_len == sizeof(scan->emit_buffer)) {
      status = scan->writer == NULL
                   ? LQL_STATUS_OK
                   : scan->writer(scan->writer_user, scan->emit_buffer,
                                  scan->emit_len, scan->error);
      if (status != LQL_STATUS_OK) {
        return status;
      }
      scan->emit_len = 0u;
    }
    amount = sizeof(scan->emit_buffer) - scan->emit_len;
    if (amount > len) {
      amount = len;
    }
    memcpy(scan->emit_buffer + scan->emit_len, bytes, amount);
    scan->emit_len += amount;
    bytes += amount;
    len -= amount;
  }
  return LQL_STATUS_OK;
}

static lql_status lql_json_write_byte(lql_json_scan *scan,
                                      unsigned char byte) {
  lql_status status;
  if (scan->writer == NULL) {
    return LQL_STATUS_OK;
  }
  if (scan->emit_len == sizeof(scan->emit_buffer)) {
    status =
        scan->writer(scan->writer_user, scan->emit_buffer, scan->emit_len,
                     scan->error);
    if (status != LQL_STATUS_OK) {
      return status;
    }
    scan->emit_len = 0u;
  }
  scan->emit_buffer[scan->emit_len++] = byte;
  return LQL_STATUS_OK;
}

static lql_status lql_json_flush(lql_json_scan *scan) {
  lql_status status;
  if (scan->emit_len == 0u || scan->writer == NULL) {
    scan->emit_len = 0u;
    return LQL_STATUS_OK;
  }
  status = scan->writer(scan->writer_user, scan->emit_buffer, scan->emit_len,
                        scan->error);
  if (status == LQL_STATUS_OK) {
    scan->emit_len = 0u;
  }
  return status;
}

static void lql_json_match_start(lql_json_scan *scan, unsigned long active,
                                 int key, size_t path_segment) {
  size_t i;
  scan->match_active = active;
  scan->match_failed = 0ul;
  scan->match_contains = 0ul;
  scan->match_icontains = 0ul;
  scan->match_case_len = 0u;
  scan->match_case_need = 0u;
  scan->match_key = key;
  scan->match_path_segment = path_segment;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    if ((active & (1ul << i)) != 0ul) {
      scan->match_pos[i] = 0u;
      scan->match_term_segment[i] = path_segment;
      if (!key && (scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_ICONTAINS ||
                   scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_IPREFIX))
        scan->match_icontains |= 1ul << i;
    }
  }
}

LQL_JSON_INLINE int lql_json_stop_hit_ready(const lql_json_scan *scan) {
  unsigned long mask;
  if (scan == NULL || !scan->flat_eq_stop_on_hit) {
    return 0;
  }
  mask = scan->flat_eq_stop_hit_mask;
  if (mask == 0ul) {
    return scan->flat_eq_hits != 0ul;
  }
  return (scan->flat_eq_hits & mask) == mask;
}

static void lql_json_number_range_start(lql_json_scan *scan,
                                        unsigned long active) {
  scan->number_range_active = active;
  scan->number_range_failed = 0ul;
  scan->number_match_len = 0u;
}

LQL_JSON_INLINE void lql_json_number_range_byte(lql_json_scan *scan,
                                                unsigned char value) {
  if (scan->number_range_active == 0ul) {
    return;
  }
  if (scan->number_match_len + 1u >= sizeof(scan->number_match)) {
    scan->number_range_failed |= scan->number_range_active;
    return;
  }
  scan->number_match[scan->number_match_len++] = (char)value;
}

static void lql_json_temporal_range_start(lql_json_scan *scan,
                                          unsigned long active) {
  scan->temporal_range_active = active;
  scan->temporal_range_failed = 0ul;
  scan->temporal_match_len = 0u;
}

LQL_JSON_INLINE void lql_json_temporal_range_byte(lql_json_scan *scan,
                                                  unsigned char value) {
  if (scan->temporal_range_active == 0ul) {
    return;
  }
  if (scan->temporal_match_len + 1u >= sizeof(scan->temporal_match)) {
    scan->temporal_range_failed |= scan->temporal_range_active;
    return;
  }
  scan->temporal_match[scan->temporal_match_len++] = (char)value;
}

static void lql_json_temporal_range_bytes(lql_json_scan *scan,
                                          const unsigned char *data,
                                          size_t len) {
  unsigned long active;
  size_t remaining;
  if (scan->temporal_range_active == 0ul || len == 0u) {
    return;
  }
  active = scan->temporal_range_active & ~scan->temporal_range_failed;
  if (active == 0ul) {
    return;
  }
  remaining = sizeof(scan->temporal_match) - scan->temporal_match_len;
  if (len >= remaining) {
    scan->temporal_range_failed |= active;
    return;
  }
  memcpy(scan->temporal_match + scan->temporal_match_len, data, len);
  scan->temporal_match_len += len;
}

static int lql_json_number_match_integer(const char *text, size_t len,
                                         double *out) {
  unsigned long value;
  unsigned long limit;
  size_t pos;
  int negative;
  if (text == NULL || len == 0u || out == NULL) {
    return 0;
  }
  pos = 0u;
  negative = 0;
  if (text[pos] == '-') {
    negative = 1;
    ++pos;
    if (pos == len) {
      return 0;
    }
  }
  value = 0ul;
  limit = negative ? (unsigned long)LONG_MAX + 1ul : (unsigned long)LONG_MAX;
  for (; pos < len; ++pos) {
    unsigned long digit;
    if (text[pos] < '0' || text[pos] > '9') {
      return 0;
    }
    digit = (unsigned long)(text[pos] - '0');
    if (value > (limit - digit) / 10ul) {
      return 0;
    }
    value = value * 10ul + digit;
  }
  if (negative) {
    *out = value == (unsigned long)LONG_MAX + 1ul ? (double)LONG_MIN
                                                  : -(double)value;
  } else {
    *out = (double)value;
  }
  return 1;
}

static unsigned long lql_json_number_range_complete(lql_json_scan *scan) {
  char *end;
  double value;
  unsigned long hits;
  size_t i;
  if (scan->number_range_active == 0ul ||
      scan->number_match_len >= sizeof(scan->number_match)) {
    return 0ul;
  }
  scan->number_match[scan->number_match_len] = '\0';
  if (!lql_json_number_match_integer(scan->number_match, scan->number_match_len,
                                     &value)) {
    errno = 0;
    value = strtod(scan->number_match, &end);
    if (end != scan->number_match + scan->number_match_len || errno == ERANGE) {
      return 0ul;
    }
  }
  hits = 0ul;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    const lql_json_flat_eq_term *term;
    unsigned long bit;
    bit = 1ul << i;
    if ((scan->number_range_active & bit) == 0ul ||
        (scan->number_range_failed & bit) != 0ul) {
      continue;
    }
    term = &scan->flat_terms[i];
    if (term->kind != LQL_JSON_FLAT_TERM_NUMBER_RANGE) {
      continue;
    }
    if (term->has_range_gt && value <= term->range_gt) {
      continue;
    }
    if (term->has_range_gte && value < term->range_gte) {
      continue;
    }
    if (term->has_range_lt && value >= term->range_lt) {
      continue;
    }
    if (term->has_range_lte && value > term->range_lte) {
      continue;
    }
    hits |= bit;
  }
  return hits;
}

static unsigned long lql_json_temporal_range_complete(lql_json_scan *scan) {
  lql_temporal value;
  unsigned long hits;
  size_t i;
  if (scan->temporal_range_active == 0ul ||
      scan->temporal_match_len >= sizeof(scan->temporal_match)) {
    return 0ul;
  }
  scan->temporal_match[scan->temporal_match_len] = '\0';
  if (!lql_parse_temporal_literal(scan->temporal_match, &value)) {
    return 0ul;
  }
  hits = 0ul;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    const lql_json_flat_eq_term *term;
    unsigned long bit;
    bit = 1ul << i;
    if ((scan->temporal_range_active & bit) == 0ul ||
        (scan->temporal_range_failed & bit) != 0ul) {
      continue;
    }
    term = &scan->flat_terms[i];
    if (term->kind != LQL_JSON_FLAT_TERM_TEMPORAL_RANGE) {
      continue;
    }
    if (term->has_temporal_eq &&
        !lql_temporal_equal(&value, &term->temporal_eq)) {
      continue;
    }
    if (term->has_temporal_gt &&
        lql_temporal_compare(&value, &term->temporal_gt) <= 0) {
      continue;
    }
    if (term->has_temporal_gte &&
        lql_temporal_compare(&value, &term->temporal_gte) < 0) {
      continue;
    }
    if (term->has_temporal_lt &&
        lql_temporal_compare(&value, &term->temporal_lt) >= 0) {
      continue;
    }
    if (term->has_temporal_lte &&
        lql_temporal_compare(&value, &term->temporal_lte) > 0) {
      continue;
    }
    hits |= bit;
  }
  return hits;
}

static void lql_json_capture_start(lql_json_scan *scan, unsigned long active,
                                   size_t segment) {
  size_t i;
  scan->capture_active = active;
  scan->capture_failed = 0ul;
  for (i = 0u; i < scan->capture_key_count; ++i) {
    if ((active & (1ul << i)) != 0ul) {
      scan->capture_pos[i] = 0u;
      scan->capture_segment[i] = segment;
    }
  }
}

static unsigned long lql_json_capture_complete(const lql_json_scan *scan) {
  unsigned long matches;
  size_t i;
  matches = 0ul;
  for (i = 0u; i < scan->capture_key_count; ++i) {
    unsigned long bit;
    bit = 1ul << i;
    if ((scan->capture_active & bit) != 0ul &&
        (scan->capture_failed & bit) == 0ul &&
        scan->capture_segment[i] < scan->capture_keys[i].segment_count &&
        scan->capture_pos[i] ==
            strlen(scan->capture_keys[i].segments[scan->capture_segment[i]]))
      matches |= bit;
  }
  return matches;
}

static int lql_json_capture_array_index(const lql_json_scan *scan, size_t key,
                                        size_t segment, size_t index) {
  const char *text;
  size_t i;
  size_t value;
  if (scan == NULL || key >= scan->capture_key_count ||
      segment >= scan->capture_keys[key].segment_count) {
    return 0;
  }
  text = scan->capture_keys[key].segments[segment];
  if (text == NULL || text[0] == '\0') {
    return 0;
  }
  value = 0u;
  for (i = 0u; text[i] != '\0'; ++i) {
    size_t digit;
    if (text[i] < '0' || text[i] > '9') {
      return 0;
    }
    digit = (size_t)(text[i] - '0');
    if (value > ((size_t)-1 - digit) / 10u) {
      return 0;
    }
    value = value * 10u + digit;
  }
  return value == index;
}

static size_t lql_json_output_offset(const lql_json_scan *scan) {
  if (scan->writer_user == NULL)
    return 0u;
  return lql_json_spool_size((const lql_json_spool *)scan->writer_user) +
         scan->emit_len;
}

static int lql_json_term_path_segment(const lql_json_flat_eq_term *term,
                                      size_t segment, const char **out,
                                      size_t *out_len) {
  const char *start;
  const char *end;
  size_t i;
  if (term == NULL || out == NULL || out_len == NULL) {
    return 0;
  }
  if (term->path == NULL || term->path_segment_count == 0u) {
    if (segment != 0u) {
      return 0;
    }
    *out = term->field;
    *out_len = term->field_len;
    return 1;
  }
  if (term->path_recursive_match != NULL &&
      segment == term->path_recursive_match_segment) {
    *out = term->path_recursive_match;
    *out_len = term->path_recursive_match_len;
    return 1;
  }
  if (segment >= term->path_segment_count || term->path[0] != '/') {
    return 0;
  }
  if (segment == 0u) {
    *out = term->field;
    *out_len = term->field_len;
    return 1;
  }
  start = term->path + 1;
  for (i = 0u; i < segment; ++i) {
    end = strchr(start, '/');
    if (end == NULL) {
      return 0;
    }
    start = end + 1;
  }
  end = strchr(start, '/');
  *out = start;
  *out_len = end == NULL ? strlen(start) : (size_t)(end - start);
  return 1;
}

static int lql_json_term_value_at(const lql_json_flat_eq_term *term,
                                  size_t path_segment) {
  if (term == NULL) {
    return 0;
  }
  return term->path_segment_count == 0u
             ? path_segment == 0u
             : term->path_segment_count == path_segment + 1u;
}

static int lql_json_term_array_index(const lql_json_flat_eq_term *term,
                                     size_t path_segment, size_t *out) {
  const char *text;
  size_t text_len;
  size_t i;
  size_t value;
  if (out == NULL || term == NULL) {
    return 0;
  }
  if (path_segment < LQL_JSON_PATH_SEGMENT_CAPACITY &&
      (term->path_array_index_cache & (1ul << path_segment)) != 0ul) {
    *out = term->path_array_index_values[path_segment];
    return 1;
  }
  if (!lql_json_term_path_segment(term, path_segment, &text, &text_len) ||
      text_len == 0u) {
    return 0;
  }
  value = 0u;
  for (i = 0u; i < text_len; ++i) {
    size_t digit;
    if (text[i] < '0' || text[i] > '9') {
      return 0;
    }
    digit = (size_t)(text[i] - '0');
    if (value > ((size_t)-1 - digit) / 10u) {
      return 0;
    }
    value = value * 10u + digit;
  }
  *out = value;
  return 1;
}

static int lql_json_term_recursive_segment(const lql_json_flat_eq_term *term,
                                           size_t *out) {
  size_t i;
  if (term == NULL || out == NULL || term->path_recursive_segments == 0ul) {
    return 0;
  }
  for (i = 0u; i < sizeof(unsigned long) * CHAR_BIT; ++i) {
    if ((term->path_recursive_segments & (1ul << i)) != 0ul) {
      *out = i;
      return 1;
    }
  }
  return 0;
}

static void lql_json_match_start_key(lql_json_scan *scan,
                                     unsigned long ordinary,
                                     unsigned long recursive,
                                     size_t path_segment) {
  size_t i;
  lql_json_match_start(scan, ordinary | recursive, 1, path_segment);
  for (i = 0u; i < scan->flat_term_count; ++i) {
    unsigned long bit;
    size_t recursive_segment;
    bit = 1ul << i;
    if ((recursive & bit) != 0ul &&
        lql_json_term_recursive_segment(&scan->flat_terms[i],
                                        &recursive_segment)) {
      scan->match_term_segment[i] = recursive_segment + 1u;
    }
  }
}

static void lql_json_match_icontains_bytes(lql_json_scan *scan,
                                           const unsigned char *data,
                                           size_t len) {
  size_t i;
  size_t offset;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    const lql_json_flat_eq_term *term;
    unsigned long bit;
    size_t pos;
    bit = 1ul << i;
    term = &scan->flat_terms[i];
    if ((scan->match_active & bit) == 0ul || scan->match_key ||
        (term->kind != LQL_JSON_FLAT_TERM_ICONTAINS &&
         term->kind != LQL_JSON_FLAT_TERM_IPREFIX))
      continue;
    pos = scan->match_pos[i];
    for (offset = 0u; offset < len; ++offset) {
      if (term->kind == LQL_JSON_FLAT_TERM_IPREFIX) {
        if (pos < term->value_len &&
            data[offset] != (unsigned char)term->value[pos])
          scan->match_failed |= bit;
        if (pos < term->value_len)
          ++pos;
        continue;
      }
      if (term->contains_failure == NULL) {
        scan->match_failed |= bit;
        break;
      }
      while (pos != 0u && data[offset] != (unsigned char)term->value[pos])
        pos = term->contains_failure[pos - 1u];
      if (data[offset] == (unsigned char)term->value[pos])
        ++pos;
      if (pos == term->value_len) {
        scan->match_contains |= bit;
        pos = term->contains_failure[pos - 1u];
      }
    }
    scan->match_pos[i] = pos;
  }
}

static void lql_json_match_icontains_ascii_span(lql_json_scan *scan,
                                                const unsigned char *data,
                                                size_t len) {
  size_t i;
  size_t offset;
  if (scan == NULL || data == NULL || len == 0u || scan->match_icontains == 0ul)
    return;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    const lql_json_flat_eq_term *term;
    unsigned long bit;
    size_t pos;
    bit = 1ul << i;
    if ((scan->match_icontains & bit) == 0ul ||
        (scan->match_active & bit) == 0ul ||
        (scan->match_failed & bit) != 0ul) {
      continue;
    }
    term = &scan->flat_terms[i];
    pos = scan->match_pos[i];
    for (offset = 0u; offset < len; ++offset) {
      unsigned char value;
      value = data[offset];
      if (value >= (unsigned char)'A' && value <= (unsigned char)'Z')
        value =
            (unsigned char)(value + ((unsigned char)'a' - (unsigned char)'A'));
      if (term->kind == LQL_JSON_FLAT_TERM_IPREFIX) {
        if (pos < term->value_len && value != (unsigned char)term->value[pos])
          scan->match_failed |= bit;
        if (pos < term->value_len)
          ++pos;
        continue;
      }
      if (term->contains_failure == NULL) {
        scan->match_failed |= bit;
        break;
      }
      while (pos != 0u && value != (unsigned char)term->value[pos])
        pos = term->contains_failure[pos - 1u];
      if (value == (unsigned char)term->value[pos])
        ++pos;
      if (pos == term->value_len) {
        scan->match_contains |= bit;
        pos = term->contains_failure[pos - 1u];
      }
    }
    scan->match_pos[i] = pos;
  }
}

static void lql_json_match_contains_ascii_span(lql_json_scan *scan,
                                               unsigned long active,
                                               const unsigned char *data,
                                               size_t len) {
  size_t i;
  size_t offset;
  if (scan == NULL || data == NULL || len == 0u || active == 0ul)
    return;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    const lql_json_flat_eq_term *term;
    unsigned long bit;
    size_t pos;
    bit = 1ul << i;
    if ((active & bit) == 0ul || (scan->match_active & bit) == 0ul ||
        (scan->match_failed & bit) != 0ul) {
      continue;
    }
    term = &scan->flat_terms[i];
    if (term->kind != LQL_JSON_FLAT_TERM_CONTAINS ||
        term->contains_failure == NULL || term->value_len == 0u) {
      scan->match_failed |= bit;
      continue;
    }
    pos = scan->match_pos[i];
    for (offset = 0u; offset < len; ++offset) {
      while (pos != 0u && data[offset] != (unsigned char)term->value[pos])
        pos = term->contains_failure[pos - 1u];
      if (data[offset] == (unsigned char)term->value[pos])
        ++pos;
      if (pos == term->value_len) {
        scan->match_contains |= bit;
        pos = term->contains_failure[pos - 1u];
      }
    }
    scan->match_pos[i] = pos;
  }
}

static void lql_json_match_icontains_byte(lql_json_scan *scan,
                                          unsigned char value) {
  unsigned long rune;
  unsigned char lower[4];
  size_t len;
  if (scan->match_case_need == 0u) {
    scan->match_case_need = value < 0x80u   ? 1u
                            : value < 0xe0u ? 2u
                            : value < 0xf0u ? 3u
                                            : 4u;
    scan->match_case_len = 0u;
  }
  scan->match_case_bytes[scan->match_case_len++] = value;
  if (scan->match_case_len != scan->match_case_need)
    return;
  if (lql_unicode_utf8_decode_one(scan->match_case_bytes, scan->match_case_len,
                                  &rune)) {
    if (rune >= (unsigned long)'A' && rune <= (unsigned long)'Z') {
      rune += (unsigned long)'a' - (unsigned long)'A';
    } else if (rune >= 0x80ul) {
      rune = lql_unicode_simple_lower(rune);
    }
    len = lql_unicode_utf8_encode(rune, lower);
    if (len != 0u)
      lql_json_match_icontains_bytes(scan, lower, len);
  }
  scan->match_case_len = 0u;
  scan->match_case_need = 0u;
}

static void lql_json_match_byte(lql_json_scan *scan, unsigned char value);

static int lql_json_match_span_term(lql_json_scan *scan,
                                    const lql_json_flat_eq_term *term,
                                    size_t term_index,
                                    const unsigned char *data, size_t len) {
  const char *target;
  size_t target_len;
  size_t pos;
  size_t remaining;
  unsigned long bit;
  bit = 1ul << term_index;
  if (term == NULL || data == NULL) {
    return 0;
  }
  if (!scan->match_key && (term->kind == LQL_JSON_FLAT_TERM_CONTAINS ||
                           term->kind == LQL_JSON_FLAT_TERM_ICONTAINS ||
                           term->kind == LQL_JSON_FLAT_TERM_IPREFIX)) {
    return 0;
  }
  if (scan->match_key) {
    if (scan->match_term_segment[term_index] == 0u) {
      target = term->field;
      target_len = term->field_len;
    } else if (!lql_json_term_path_segment(term,
                                           scan->match_term_segment[term_index],
                                           &target, &target_len)) {
      scan->match_failed |= bit;
      return 1;
    }
  } else {
    target = term->value;
    target_len = term->value_len;
  }
  pos = scan->match_pos[term_index];
  if (!scan->match_key && term->kind == LQL_JSON_FLAT_TERM_PREFIX &&
      pos == target_len) {
    return 1;
  }
  if (pos > target_len) {
    scan->match_failed |= bit;
    return 1;
  }
  remaining = target_len - pos;
  if (remaining > len) {
    remaining = len;
  }
  if (remaining != 0u && memcmp(target + pos, data, remaining) != 0) {
    scan->match_failed |= bit;
    return 1;
  }
  pos += remaining;
  if (remaining != len &&
      (scan->match_key || term->kind != LQL_JSON_FLAT_TERM_PREFIX)) {
    scan->match_failed |= bit;
    return 1;
  }
  scan->match_pos[term_index] = pos;
  return 1;
}

static void lql_json_match_span(lql_json_scan *scan, const unsigned char *data,
                                size_t len) {
  size_t i;
  unsigned long ascii_icontains;
  unsigned long ascii_contains;
  if (len == 0u) {
    return;
  }
  if (scan->match_active == 0ul && scan->capture_active == 0ul &&
      scan->temporal_range_active == 0ul) {
    return;
  }
  lql_json_temporal_range_bytes(scan, data, len);
  ascii_icontains = 0ul;
  ascii_contains = 0ul;
  if (!scan->match_key) {
    ascii_contains = scan->match_active & ~scan->match_icontains &
                     ~scan->match_failed;
    if (ascii_contains != 0ul) {
      size_t term_index;
      unsigned long contains_only;
      contains_only = 0ul;
      for (term_index = 0u; term_index < scan->flat_term_count;
           ++term_index) {
        unsigned long bit;
        bit = 1ul << term_index;
        if ((ascii_contains & bit) != 0ul &&
            scan->flat_terms[term_index].kind ==
                LQL_JSON_FLAT_TERM_CONTAINS)
          contains_only |= bit;
      }
      ascii_contains = contains_only;
      if (ascii_contains != 0ul)
        lql_json_match_contains_ascii_span(scan, ascii_contains, data, len);
    }
    ascii_icontains =
        scan->match_active & scan->match_icontains & ~scan->match_failed;
    if (ascii_icontains != 0ul) {
      lql_json_match_icontains_ascii_span(scan, data, len);
    }
  }
  if (scan->match_active != 0ul) {
    for (i = 0u; i < scan->flat_term_count; ++i) {
      unsigned long bit;
      bit = 1ul << i;
      if ((scan->match_active & bit) == 0ul ||
          (scan->match_failed & bit) != 0ul) {
        continue;
      }
      if (!scan->match_key && (scan->match_icontains & bit) != 0ul) {
        continue;
      }
      if (!scan->match_key && (ascii_contains & bit) != 0ul) {
        continue;
      }
      if (!lql_json_match_span_term(scan, &scan->flat_terms[i], i, data, len)) {
        unsigned long saved_temporal_active;
        unsigned long saved_match_icontains;
        size_t offset;
        saved_temporal_active = scan->temporal_range_active;
        saved_match_icontains = scan->match_icontains;
        scan->temporal_range_active = 0ul;
        if (ascii_icontains != 0ul) {
          scan->match_icontains = 0ul;
        }
        for (offset = 0u; offset < len && (scan->match_active & bit) != 0ul &&
                          (scan->match_failed & bit) == 0ul;
             ++offset) {
          lql_json_match_byte(scan, data[offset]);
        }
        scan->match_icontains = saved_match_icontains;
        scan->temporal_range_active = saved_temporal_active;
        return;
      }
    }
  }
  if (scan->capture_active != 0ul) {
    unsigned long saved_match_active;
    unsigned long saved_match_icontains;
    unsigned long saved_temporal_active;
    saved_match_active = scan->match_active;
    saved_match_icontains = scan->match_icontains;
    saved_temporal_active = scan->temporal_range_active;
    scan->match_active = 0ul;
    scan->match_icontains = 0ul;
    scan->temporal_range_active = 0ul;
    for (i = 0u;
         i < len && (scan->capture_active & ~scan->capture_failed) != 0ul;
         ++i) {
      lql_json_match_byte(scan, data[i]);
    }
    scan->match_active = saved_match_active;
    scan->match_icontains = saved_match_icontains;
    scan->temporal_range_active = saved_temporal_active;
  }
}

static void lql_json_match_byte(lql_json_scan *scan, unsigned char value) {
  size_t i;
  if (scan->match_active == 0ul && scan->capture_active == 0ul &&
      scan->temporal_range_active == 0ul) {
    return;
  }
  lql_json_temporal_range_byte(scan, value);
  if (scan->match_active == 0ul && scan->capture_active == 0ul) {
    return;
  }
  if (scan->match_active != 0ul) {
    for (i = 0u; i < scan->flat_term_count; ++i) {
      const lql_json_flat_eq_term *term;
      const char *target;
      size_t target_len;
      unsigned long bit;
      bit = 1ul << i;
      if ((scan->match_active & bit) == 0ul ||
          (scan->match_failed & bit) != 0ul) {
        continue;
      }
      term = &scan->flat_terms[i];
      if (!scan->match_key && (term->kind == LQL_JSON_FLAT_TERM_ICONTAINS ||
                               term->kind == LQL_JSON_FLAT_TERM_IPREFIX))
        continue;
      if (scan->match_key) {
        if (scan->match_term_segment[i] == 0u) {
          target = term->field;
          target_len = term->field_len;
        } else if (!lql_json_term_path_segment(term,
                                               scan->match_term_segment[i],
                                               &target, &target_len)) {
          scan->match_failed |= bit;
          continue;
        }
      } else {
        target = term->value;
        target_len = term->value_len;
      }
      if (!scan->match_key && term->kind == LQL_JSON_FLAT_TERM_CONTAINS) {
        size_t pos;
        if (target_len == 0u || term->contains_failure == NULL) {
          scan->match_failed |= bit;
          continue;
        }
        pos = scan->match_pos[i];
        while (pos != 0u && value != (unsigned char)target[pos]) {
          pos = term->contains_failure[pos - 1u];
        }
        if (value == (unsigned char)target[pos]) {
          ++pos;
        }
        if (pos == target_len) {
          scan->match_contains |= bit;
          pos = term->contains_failure[pos - 1u];
        }
        scan->match_pos[i] = pos;
        continue;
      }
      if (!scan->match_key && term->kind == LQL_JSON_FLAT_TERM_PREFIX &&
          scan->match_pos[i] == target_len) {
        continue;
      }
      if (scan->match_pos[i] == target_len ||
          (unsigned char)target[scan->match_pos[i]] != value) {
        scan->match_failed |= bit;
      } else {
        ++scan->match_pos[i];
      }
    }
  }
  if (scan->capture_active != 0ul) {
    for (i = 0u; i < scan->capture_key_count; ++i) {
      unsigned long bit;
      const char *target;
      size_t target_len;
      bit = 1ul << i;
      if ((scan->capture_active & bit) == 0ul ||
          (scan->capture_failed & bit) != 0ul)
        continue;
      if (scan->capture_segment[i] >= scan->capture_keys[i].segment_count) {
        scan->capture_failed |= bit;
        continue;
      }
      target = scan->capture_keys[i].segments[scan->capture_segment[i]];
      target_len = strlen(target);
      if (scan->capture_pos[i] == target_len ||
          value != (unsigned char)target[scan->capture_pos[i]])
        scan->capture_failed |= bit;
      else
        ++scan->capture_pos[i];
    }
  }
  if (scan->match_icontains != 0ul)
    lql_json_match_icontains_byte(scan, value);
}

static unsigned long lql_json_match_complete(const lql_json_scan *scan) {
  unsigned long matches;
  size_t i;
  matches = 0ul;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    const lql_json_flat_eq_term *term;
    const char *target;
    size_t target_len;
    unsigned long bit;
    bit = 1ul << i;
    if ((scan->match_active & bit) == 0ul ||
        (scan->match_failed & bit) != 0ul) {
      continue;
    }
    term = &scan->flat_terms[i];
    if (scan->match_key) {
      if (scan->match_term_segment[i] == 0u) {
        target_len = term->field_len;
      } else if (!lql_json_term_path_segment(term, scan->match_term_segment[i],
                                             &target, &target_len)) {
        continue;
      }
    } else {
      target_len = term->value_len;
    }
    if (!scan->match_key &&
        (term->kind == LQL_JSON_FLAT_TERM_CONTAINS ||
         term->kind == LQL_JSON_FLAT_TERM_ICONTAINS) &&
        (scan->match_contains & bit) != 0ul) {
      matches |= bit;
      continue;
    }
    if ((scan->match_key || term->kind != LQL_JSON_FLAT_TERM_PREFIX) &&
        scan->match_pos[i] == target_len) {
      matches |= bit;
    }
    if (!scan->match_key && term->kind == LQL_JSON_FLAT_TERM_PREFIX &&
        scan->match_pos[i] >= target_len) {
      matches |= bit;
    }
    if (!scan->match_key && term->kind == LQL_JSON_FLAT_TERM_IPREFIX &&
        scan->match_pos[i] >= target_len)
      matches |= bit;
  }
  return matches;
}

static unsigned long lql_json_match_exists(const lql_json_scan *scan,
                                           unsigned long keys) {
  unsigned long hits;
  size_t i;
  hits = 0ul;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    unsigned long bit;
    bit = 1ul << i;
    if ((keys & bit) != 0ul &&
        lql_json_term_value_at(&scan->flat_terms[i],
                               scan->key_term_segment[i]) &&
        scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_EXISTS) {
      hits |= bit;
    }
  }
  return hits;
}

static unsigned long
lql_json_match_terms_for_kind(const lql_json_scan *scan, unsigned long keys,
                              lql_json_flat_term_kind kind) {
  unsigned long active;
  size_t i;
  active = 0ul;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    unsigned long bit;
    bit = 1ul << i;
    if ((keys & bit) != 0ul &&
        lql_json_term_value_at(&scan->flat_terms[i],
                               scan->key_term_segment[i]) &&
        scan->flat_terms[i].kind == kind) {
      active |= bit;
    }
  }
  return active;
}

static unsigned long lql_json_scalar_literal_hits(const lql_json_scan *scan,
                                                  unsigned long terms,
                                                  const char *literal,
                                                  size_t literal_len) {
  unsigned long hits;
  size_t i;
  hits = 0ul;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    const lql_json_flat_eq_term *term;
    unsigned long bit;
    bit = 1ul << i;
    if ((terms & bit) == 0ul) {
      continue;
    }
    term = &scan->flat_terms[i];
    if (term->value_len == literal_len &&
        memcmp(term->value, literal, literal_len) == 0) {
      hits |= bit;
    }
  }
  return hits;
}

static unsigned long lql_json_match_descendants(const lql_json_scan *scan,
                                                unsigned long keys) {
  unsigned long active;
  size_t i;
  active = 0ul;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    const char *segment;
    size_t segment_len;
    unsigned long bit;
    bit = 1ul << i;
    if ((keys & bit) == 0ul ||
        lql_json_term_value_at(&scan->flat_terms[i],
                               scan->key_term_segment[i])) {
      continue;
    }
    if (lql_json_term_path_segment(&scan->flat_terms[i],
                                   scan->key_term_segment[i] + 1u, &segment,
                                   &segment_len)) {
      active |= bit;
    }
  }
  return active;
}

static unsigned long lql_json_match_object_terms(const lql_json_scan *scan,
                                                 unsigned long active,
                                                 size_t path_segment) {
  unsigned long matches;
  size_t i;
  matches = 0ul;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    unsigned long bit;
    bit = 1ul << i;
    if ((active & bit) != 0ul &&
        (path_segment >= sizeof(unsigned long) * CHAR_BIT ||
         ((scan->flat_terms[i].path_array_segments |
           scan->flat_terms[i].path_object_wildcards |
           scan->flat_terms[i].path_array_wildcards |
           scan->flat_terms[i].path_any_wildcards |
           scan->flat_terms[i].path_recursive_segments) &
          (1ul << path_segment)) == 0ul)) {
      matches |= bit;
    }
  }
  return matches;
}

static unsigned long lql_json_match_recursive_terms(const lql_json_scan *scan,
                                                    unsigned long active,
                                                    size_t path_segment) {
  unsigned long matches;
  size_t i;
  matches = 0ul;
  if (path_segment >= sizeof(unsigned long) * CHAR_BIT) {
    return matches;
  }
  for (i = 0u; i < scan->flat_term_count; ++i) {
    unsigned long bit;
    bit = 1ul << i;
    if ((active & bit) != 0ul && (scan->flat_terms[i].path_recursive_segments &
                                  (1ul << path_segment)) != 0ul) {
      matches |= bit;
    }
  }
  return matches;
}

static unsigned long lql_json_match_object_wildcards(const lql_json_scan *scan,
                                                     unsigned long active,
                                                     size_t path_segment) {
  unsigned long matches;
  size_t i;
  matches = 0ul;
  if (path_segment >= sizeof(unsigned long) * CHAR_BIT) {
    return matches;
  }
  for (i = 0u; i < scan->flat_term_count; ++i) {
    unsigned long bit;
    bit = 1ul << i;
    if ((active & bit) != 0ul && ((scan->flat_terms[i].path_object_wildcards |
                                   scan->flat_terms[i].path_any_wildcards) &
                                  (1ul << path_segment)) != 0ul) {
      matches |= bit;
    }
  }
  return matches;
}

static unsigned long lql_json_match_array_index(const lql_json_scan *scan,
                                                unsigned long active,
                                                size_t path_segment,
                                                size_t index) {
  unsigned long matches;
  size_t i;
  matches = 0ul;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    size_t expected;
    unsigned long bit;
    bit = 1ul << i;
    if ((active & bit) != 0ul &&
        path_segment < sizeof(unsigned long) * CHAR_BIT) {
      if (((scan->flat_terms[i].path_array_wildcards |
            scan->flat_terms[i].path_any_wildcards) &
           (1ul << path_segment)) != 0ul) {
        matches |= bit;
      } else if ((scan->flat_terms[i].path_array_segments &
                  (1ul << path_segment)) != 0ul &&
                 lql_json_term_array_index(&scan->flat_terms[i], path_segment,
                                           &expected) &&
                 expected == index) {
        matches |= bit;
      }
    }
  }
  return matches;
}

static void lql_json_match_unicode(lql_json_scan *scan, unsigned int value) {
  if (value <= 0x7fu) {
    lql_json_match_byte(scan, (unsigned char)value);
  } else if (value <= 0x7ffu) {
    lql_json_match_byte(scan, (unsigned char)(0xc0u | (value >> 6u)));
    lql_json_match_byte(scan, (unsigned char)(0x80u | (value & 0x3fu)));
  } else if (value <= 0xffffu) {
    lql_json_match_byte(scan, (unsigned char)(0xe0u | (value >> 12u)));
    lql_json_match_byte(scan, (unsigned char)(0x80u | ((value >> 6u) & 0x3fu)));
    lql_json_match_byte(scan, (unsigned char)(0x80u | (value & 0x3fu)));
  } else {
    lql_json_match_byte(scan, (unsigned char)(0xf0u | (value >> 18u)));
    lql_json_match_byte(scan,
                        (unsigned char)(0x80u | ((value >> 12u) & 0x3fu)));
    lql_json_match_byte(scan, (unsigned char)(0x80u | ((value >> 6u) & 0x3fu)));
    lql_json_match_byte(scan, (unsigned char)(0x80u | (value & 0x3fu)));
  }
}

static int lql_json_word_has_zero_byte(unsigned long value) {
  unsigned long ones;
  unsigned long high;
  ones = ~0ul / 255ul;
  high = ones * 128ul;
  return ((value - ones) & ~value & high) != 0ul;
}

static size_t lql_json_plain_ascii_span(const unsigned char *data, size_t len) {
  unsigned long word;
  unsigned long ones;
  unsigned long high;
  unsigned long control_offset;
  size_t offset;
  if (len < sizeof(word)) {
    offset = 0u;
    while (offset < len && data[offset] != (unsigned char)'"' &&
           data[offset] != (unsigned char)'\\' && data[offset] >= 0x20u &&
           data[offset] < 0x80u) {
      ++offset;
    }
    return offset;
  }
  offset = 0u;
  ones = ~0ul / 255ul;
  high = ones * 128ul;
  control_offset = ones * 96ul;
  while (len - offset >= sizeof(word)) {
    memcpy(&word, data + offset, sizeof(word));
    if ((word & high) != 0ul || (~(word + control_offset) & high) != 0ul ||
        lql_json_word_has_zero_byte(word ^ (ones * (unsigned long)'"')) ||
        lql_json_word_has_zero_byte(word ^ (ones * (unsigned long)'\\'))) {
      break;
    }
    offset += sizeof(word);
  }
  while (offset < len && data[offset] != (unsigned char)'"' &&
         data[offset] != (unsigned char)'\\' && data[offset] >= 0x20u &&
         data[offset] < 0x80u) {
    ++offset;
  }
  return offset;
}

static size_t lql_json_plain_key_span(const unsigned char *data, size_t len) {
  size_t offset;
  offset = 0u;
  while (offset < len && data[offset] != (unsigned char)'"' &&
         data[offset] != (unsigned char)'\\' && data[offset] >= 0x20u &&
         data[offset] < 0x80u) {
    ++offset;
  }
  return offset;
}

static lql_status lql_json_refill(lql_json_scan *scan) {
  size_t amount;
  lql_status status;
  if (scan->offset < scan->length) {
    return LQL_STATUS_OK;
  }
  scan->offset = 0u;
  scan->length = 0u;
  amount = 0u;
  status = scan->reader(scan->reader_user, scan->buffer, sizeof(scan->buffer),
                        &amount, scan->error);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  if (amount > sizeof(scan->buffer)) {
    lql_json_error(scan, "JSON reader exceeded its buffer capacity");
    return LQL_STATUS_JSON_ERROR;
  }
  scan->length = amount;
  scan->bytes_read += amount;
  return LQL_STATUS_OK;
}

static size_t lql_json_consumed(const lql_json_scan *scan) {
  if (scan == NULL || scan->length < scan->offset ||
      scan->bytes_read < scan->length - scan->offset) {
    return 0u;
  }
  return scan->bytes_read - (scan->length - scan->offset);
}

LQL_JSON_INLINE lql_status lql_json_peek(lql_json_scan *scan, int *out) {
  lql_status status;
  status = lql_json_refill(scan);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  *out = scan->offset == scan->length ? -1 : (int)scan->buffer[scan->offset];
  return LQL_STATUS_OK;
}

LQL_JSON_INLINE lql_status lql_json_take(lql_json_scan *scan, int *out) {
  lql_status status;
  status = lql_json_peek(scan, out);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  if (*out < 0) {
    lql_json_error(scan, "unexpected end of JSON input");
    return LQL_STATUS_JSON_ERROR;
  }
  ++scan->offset;
  return LQL_STATUS_OK;
}

LQL_JSON_INLINE lql_status lql_json_take_expected(lql_json_scan *scan,
                                                  int expected) {
  int value;
  lql_status status;
  status = lql_json_take(scan, &value);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  if (value != expected) {
    lql_json_error(scan, "invalid JSON token");
    return LQL_STATUS_JSON_ERROR;
  }
  return LQL_STATUS_OK;
}

LQL_JSON_INLINE lql_status lql_json_skip_space(lql_json_scan *scan) {
  lql_status status;
  int skipped;
  skipped = 0;
  for (;;) {
    unsigned char value;
    if (scan->offset == scan->length) {
      status = lql_json_refill(scan);
      if (status != LQL_STATUS_OK || scan->offset == scan->length) {
        return status;
      }
    }
    value = scan->buffer[scan->offset];
    if (value != (unsigned char)' ' && value != (unsigned char)'\t' &&
        value != (unsigned char)'\r' && value != (unsigned char)'\n') {
      if (skipped && scan->compact_range_active) {
        scan->compact_range_ok = 0;
      }
      return LQL_STATUS_OK;
    }
    skipped = 1;
    ++scan->offset;
  }
}

static int lql_json_hex_value(int value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

static lql_status lql_json_take_hex4(lql_json_scan *scan, unsigned int *out,
                                     unsigned char raw[4]) {
  size_t i;
  unsigned int value;
  int byte;
  int digit;
  lql_status status;
  value = 0u;
  for (i = 0u; i < 4u; ++i) {
    status = lql_json_take(scan, &byte);
    if (status != LQL_STATUS_OK) {
      return status;
    }
    digit = lql_json_hex_value(byte);
    if (digit < 0) {
      lql_json_error(scan, "invalid JSON Unicode escape");
      return LQL_STATUS_JSON_ERROR;
    }
    raw[i] = (unsigned char)byte;
    value = (value << 4u) | (unsigned int)digit;
  }
  *out = value;
  return LQL_STATUS_OK;
}

LQL_JSON_INLINE lql_status lql_json_copy_byte(lql_json_scan *scan, int value) {
  if (scan->writer == NULL) {
    return LQL_STATUS_OK;
  }
  return lql_json_write_byte(scan, (unsigned char)value);
}

LQL_JSON_INLINE lql_status lql_json_match_copy_byte(lql_json_scan *scan,
                                                    int value) {
  if ((scan->match_active & ~scan->match_failed) != 0ul ||
      (scan->capture_active & ~scan->capture_failed) != 0ul ||
      (scan->temporal_range_active & ~scan->temporal_range_failed) != 0ul) {
    lql_json_match_byte(scan, (unsigned char)value);
  }
  lql_json_number_range_byte(scan, (unsigned char)value);
  return lql_json_copy_byte(scan, value);
}

static lql_status lql_json_literal(lql_json_scan *scan, const char *literal);
static lql_status lql_json_number(lql_json_scan *scan);
static lql_status lql_json_skip_literal_fast(lql_json_scan *scan,
                                             const char *literal);
static lql_status lql_json_skip_number_fast(lql_json_scan *scan);
static lql_status lql_json_skip_value_fast(lql_json_scan *scan);

static lql_status lql_json_skip_string_fast(lql_json_scan *scan) {
  int value;
  int next;
  int remaining;
  int continuation_min;
  int continuation_max;
  const unsigned char *span;
  size_t span_len;
  unsigned int unicode;
  unsigned int low;
  unsigned char raw[4];
  lql_status status;

  status = lql_json_take_expected(scan, '"');
  if (status != LQL_STATUS_OK) {
    return status;
  }
  for (;;) {
    status = lql_json_refill(scan);
    if (status != LQL_STATUS_OK) {
      return status;
    }
    if (scan->offset == scan->length) {
      lql_json_error(scan, "unterminated JSON string");
      return LQL_STATUS_JSON_ERROR;
    }
    span = scan->buffer + scan->offset;
    span_len = lql_json_plain_ascii_span(span, scan->length - scan->offset);
    if (span_len != 0u) {
      scan->offset += span_len;
      continue;
    }
    value = (int)scan->buffer[scan->offset++];
    if (value == '"') {
      return LQL_STATUS_OK;
    }
    if (value < 0x20) {
      lql_json_error(scan, "unescaped control byte in JSON string");
      return LQL_STATUS_JSON_ERROR;
    }
    if (value == '\\') {
      status = lql_json_take(scan, &next);
      if (status != LQL_STATUS_OK) {
        lql_json_error(scan, "unterminated JSON escape");
        return status == LQL_STATUS_OK ? LQL_STATUS_JSON_ERROR : status;
      }
      if (next == '"' || next == '\\' || next == '/' || next == 'b' ||
          next == 'f' || next == 'n' || next == 'r' || next == 't') {
        continue;
      }
      if (next != 'u') {
        lql_json_error(scan, "invalid JSON escape");
        return LQL_STATUS_JSON_ERROR;
      }
      status = lql_json_take_hex4(scan, &unicode, raw);
      if (status != LQL_STATUS_OK) {
        return status;
      }
      if (unicode >= 0xdc00u && unicode <= 0xdfffu) {
        lql_json_error(scan, "unpaired low surrogate in JSON string");
        return LQL_STATUS_JSON_ERROR;
      }
      if (unicode < 0xd800u || unicode > 0xdbffu) {
        continue;
      }
      status = lql_json_take_expected(scan, '\\');
      if (status != LQL_STATUS_OK ||
          (status = lql_json_take_expected(scan, 'u')) != LQL_STATUS_OK ||
          (status = lql_json_take_hex4(scan, &low, raw)) != LQL_STATUS_OK) {
        return status;
      }
      if (low < 0xdc00u || low > 0xdfffu) {
        lql_json_error(scan, "unpaired high surrogate in JSON string");
        return LQL_STATUS_JSON_ERROR;
      }
      continue;
    }
    if (value >= 0xc2 && value <= 0xdf) {
      remaining = 1;
      continuation_min = 0x80;
      continuation_max = 0xbf;
    } else if (value >= 0xe0 && value <= 0xef) {
      remaining = 2;
      continuation_min = value == 0xe0 ? 0xa0 : 0x80;
      continuation_max = value == 0xed ? 0x9f : 0xbf;
    } else if (value >= 0xf0 && value <= 0xf4) {
      remaining = 3;
      continuation_min = value == 0xf0 ? 0x90 : 0x80;
      continuation_max = value == 0xf4 ? 0x8f : 0xbf;
    } else {
      lql_json_error(scan, "invalid UTF-8 in JSON string");
      return LQL_STATUS_JSON_ERROR;
    }
    while (remaining != 0) {
      status = lql_json_take(scan, &next);
      if (status != LQL_STATUS_OK || next < continuation_min ||
          next > continuation_max) {
        lql_json_error(scan, "invalid UTF-8 in JSON string");
        return status == LQL_STATUS_OK ? LQL_STATUS_JSON_ERROR : status;
      }
      --remaining;
      continuation_min = 0x80;
      continuation_max = 0xbf;
    }
  }
}

static lql_status lql_json_skip_object_fast(lql_json_scan *scan) {
  int value;
  lql_status status;
  if (scan->depth == LQL_JSON_MAX_DEPTH) {
    lql_json_error(scan, "JSON nesting exceeds the scanner limit");
    return LQL_STATUS_JSON_ERROR;
  }
  ++scan->depth;
  status = lql_json_take_expected(scan, '{');
  if (status != LQL_STATUS_OK ||
      (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
      (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
    return status;
  }
  if (value == '}') {
    ++scan->offset;
    --scan->depth;
    return LQL_STATUS_OK;
  }
  for (;;) {
    if (value != '"') {
      lql_json_error(scan, "JSON object key must be a string");
      return LQL_STATUS_JSON_ERROR;
    }
    status = lql_json_skip_string_fast(scan);
    if (status != LQL_STATUS_OK ||
        (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
        (status = lql_json_take_expected(scan, ':')) != LQL_STATUS_OK ||
        (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
        (status = lql_json_skip_value_fast(scan)) != LQL_STATUS_OK ||
        (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
        (status = lql_json_take(scan, &value)) != LQL_STATUS_OK) {
      return status;
    }
    if (value == '}') {
      --scan->depth;
      return LQL_STATUS_OK;
    }
    if (value != ',') {
      lql_json_error(scan, "JSON object member separator is missing");
      return LQL_STATUS_JSON_ERROR;
    }
    status = lql_json_skip_space(scan);
    if (status != LQL_STATUS_OK ||
        (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
      return status;
    }
  }
}

static lql_status lql_json_skip_array_fast(lql_json_scan *scan) {
  int value;
  lql_status status;
  if (scan->depth == LQL_JSON_MAX_DEPTH) {
    lql_json_error(scan, "JSON nesting exceeds the scanner limit");
    return LQL_STATUS_JSON_ERROR;
  }
  ++scan->depth;
  status = lql_json_take_expected(scan, '[');
  if (status != LQL_STATUS_OK ||
      (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
      (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
    return status;
  }
  if (value == ']') {
    ++scan->offset;
    --scan->depth;
    return LQL_STATUS_OK;
  }
  for (;;) {
    status = lql_json_skip_value_fast(scan);
    if (status != LQL_STATUS_OK ||
        (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
        (status = lql_json_take(scan, &value)) != LQL_STATUS_OK) {
      return status;
    }
    if (value == ']') {
      --scan->depth;
      return LQL_STATUS_OK;
    }
    if (value != ',') {
      lql_json_error(scan, "JSON array separator is missing");
      return LQL_STATUS_JSON_ERROR;
    }
    status = lql_json_skip_space(scan);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
}

static lql_status lql_json_skip_value_fast(lql_json_scan *scan) {
  int value;
  lql_status status;
  status = lql_json_peek(scan, &value);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  if (value == '{') {
    return lql_json_skip_object_fast(scan);
  }
  if (value == '[') {
    return lql_json_skip_array_fast(scan);
  }
  if (value == '"') {
    return lql_json_skip_string_fast(scan);
  }
  if (value == 't') {
    return lql_json_skip_literal_fast(scan, "true");
  }
  if (value == 'f') {
    return lql_json_skip_literal_fast(scan, "false");
  }
  if (value == 'n') {
    return lql_json_skip_literal_fast(scan, "null");
  }
  return lql_json_skip_number_fast(scan);
}

static lql_status lql_json_string(lql_json_scan *scan) {
  int value;
  int next;
  int remaining;
  int continuation_min;
  int continuation_max;
  const unsigned char *span;
  size_t span_len;
  unsigned int unicode;
  unsigned int low;
  unsigned char raw[4];
  unsigned char low_raw[4];
  lql_status status;

  status = lql_json_take_expected(scan, '"');
  if (status != LQL_STATUS_OK ||
      (status = lql_json_copy_byte(scan, '"')) != LQL_STATUS_OK) {
    return status;
  }
  for (;;) {
    status = lql_json_refill(scan);
    if (status != LQL_STATUS_OK) {
      return status;
    }
    if (scan->offset == scan->length) {
      lql_json_error(scan, "unterminated JSON string");
      return LQL_STATUS_JSON_ERROR;
    }
    span = scan->buffer + scan->offset;
    span_len = lql_json_plain_ascii_span(span, scan->length - scan->offset);
    if (span_len != 0u) {
      scan->offset += span_len;
      if (scan->writer != NULL) {
        status = lql_json_write(scan, span, span_len);
        if (status != LQL_STATUS_OK) {
          return status;
        }
      }
      if ((scan->match_active & ~scan->match_failed) != 0ul ||
          (scan->capture_active & ~scan->capture_failed) != 0ul ||
          (scan->temporal_range_active & ~scan->temporal_range_failed) != 0ul) {
        lql_json_match_span(scan, span, span_len);
      }
      continue;
    }
    value = (int)scan->buffer[scan->offset++];
    if (value == '"') {
      return lql_json_copy_byte(scan, value);
    }
    if (value < 0x20) {
      lql_json_error(scan, "unescaped control byte in JSON string");
      return LQL_STATUS_JSON_ERROR;
    }
    if (value == '\\') {
      status = lql_json_copy_byte(scan, value);
      if (status != LQL_STATUS_OK) {
        return status;
      }
      status = lql_json_take(scan, &next);
      if (status != LQL_STATUS_OK) {
        lql_json_error(scan, "unterminated JSON escape");
        return status == LQL_STATUS_OK ? LQL_STATUS_JSON_ERROR : status;
      }
      if (next == '"' || next == '\\' || next == '/' || next == 'b' ||
          next == 'f' || next == 'n' || next == 'r' || next == 't') {
        if (next == 'b') {
          lql_json_match_byte(scan, '\b');
        } else if (next == 'f') {
          lql_json_match_byte(scan, '\f');
        } else if (next == 'n') {
          lql_json_match_byte(scan, '\n');
        } else if (next == 'r') {
          lql_json_match_byte(scan, '\r');
        } else if (next == 't') {
          lql_json_match_byte(scan, '\t');
        } else {
          lql_json_match_byte(scan, (unsigned char)next);
        }
        status = lql_json_copy_byte(scan, next);
        if (status != LQL_STATUS_OK) {
          return status;
        }
        continue;
      }
      if (next != 'u') {
        lql_json_error(scan, "invalid JSON escape");
        return LQL_STATUS_JSON_ERROR;
      }
      status = lql_json_copy_byte(scan, next);
      if (status != LQL_STATUS_OK ||
          (status = lql_json_take_hex4(scan, &unicode, raw)) != LQL_STATUS_OK ||
          (status = lql_json_write(scan, raw, sizeof(raw))) != LQL_STATUS_OK) {
        return status;
      }
      if (unicode >= 0xdc00u && unicode <= 0xdfffu) {
        lql_json_error(scan, "unpaired low surrogate in JSON string");
        return LQL_STATUS_JSON_ERROR;
      }
      if (unicode < 0xd800u || unicode > 0xdbffu) {
        lql_json_match_unicode(scan, unicode);
        continue;
      }
      status = lql_json_take_expected(scan, '\\');
      if (status != LQL_STATUS_OK ||
          (status = lql_json_copy_byte(scan, '\\')) != LQL_STATUS_OK ||
          (status = lql_json_take_expected(scan, 'u')) != LQL_STATUS_OK ||
          (status = lql_json_copy_byte(scan, 'u')) != LQL_STATUS_OK ||
          (status = lql_json_take_hex4(scan, &low, low_raw)) != LQL_STATUS_OK ||
          (status = lql_json_write(scan, low_raw, sizeof(low_raw))) !=
              LQL_STATUS_OK) {
        return status;
      }
      if (low < 0xdc00u || low > 0xdfffu) {
        lql_json_error(scan, "unpaired high surrogate in JSON string");
        return LQL_STATUS_JSON_ERROR;
      }
      lql_json_match_unicode(scan, 0x10000u + ((unicode - 0xd800u) << 10u) +
                                       (low - 0xdc00u));
      continue;
    }
    if (value < 0x80) {
      lql_json_match_byte(scan, (unsigned char)value);
      status = lql_json_copy_byte(scan, value);
      if (status != LQL_STATUS_OK) {
        return status;
      }
      continue;
    }
    if (value >= 0xc2 && value <= 0xdf) {
      remaining = 1;
      continuation_min = 0x80;
      continuation_max = 0xbf;
    } else if (value >= 0xe0 && value <= 0xef) {
      remaining = 2;
      continuation_min = value == 0xe0 ? 0xa0 : 0x80;
      continuation_max = value == 0xed ? 0x9f : 0xbf;
    } else if (value >= 0xf0 && value <= 0xf4) {
      remaining = 3;
      continuation_min = value == 0xf0 ? 0x90 : 0x80;
      continuation_max = value == 0xf4 ? 0x8f : 0xbf;
    } else {
      lql_json_error(scan, "invalid UTF-8 in JSON string");
      return LQL_STATUS_JSON_ERROR;
    }
    status = lql_json_copy_byte(scan, value);
    if (status != LQL_STATUS_OK) {
      return status;
    }
    lql_json_match_byte(scan, (unsigned char)value);
    while (remaining != 0) {
      status = lql_json_take(scan, &next);
      if (status != LQL_STATUS_OK || next < continuation_min ||
          next > continuation_max) {
        lql_json_error(scan, "invalid UTF-8 in JSON string");
        return status == LQL_STATUS_OK ? LQL_STATUS_JSON_ERROR : status;
      }
      status = lql_json_copy_byte(scan, next);
      if (status != LQL_STATUS_OK) {
        return status;
      }
      lql_json_match_byte(scan, (unsigned char)next);
      --remaining;
      continuation_min = 0x80;
      continuation_max = 0xbf;
    }
  }
}

static lql_status lql_json_value(lql_json_scan *scan);

static lql_status lql_json_matched_scalar_value(lql_json_scan *scan,
                                                unsigned long matches,
                                                size_t path_segment,
                                                int value) {
  lql_status status;
  if (value != 'n') {
    size_t i;
    for (i = 0u; i < scan->flat_term_count; ++i) {
      unsigned long bit;
      bit = 1ul << i;
      if ((matches & bit) != 0ul &&
          lql_json_term_value_at(&scan->flat_terms[i], path_segment) &&
          scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_EXISTS) {
        scan->flat_eq_hits |= bit;
      }
    }
  }
  if (value == '"') {
    unsigned long eq_terms;
    unsigned long temporal_terms;
    size_t i;
    eq_terms = 0ul;
    temporal_terms = 0ul;
    for (i = 0u; i < scan->flat_term_count; ++i) {
      unsigned long bit;
      bit = 1ul << i;
      if ((matches & bit) != 0ul &&
          lql_json_term_value_at(&scan->flat_terms[i], path_segment)) {
        if (scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_TEMPORAL_RANGE) {
          temporal_terms |= bit;
        } else if (scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_EQ ||
                   scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_PREFIX ||
                   scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_CONTAINS ||
                   scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_ICONTAINS ||
                   scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_IPREFIX) {
          eq_terms |= bit;
        }
      }
    }
    if (eq_terms == 0ul && temporal_terms == 0ul)
      return lql_json_string(scan);
    lql_json_match_start(scan, eq_terms, 0, 0u);
    lql_json_temporal_range_start(scan, temporal_terms);
    status = lql_json_string(scan);
    if (status == LQL_STATUS_OK) {
      scan->flat_eq_hits |= lql_json_match_complete(scan);
      scan->flat_eq_hits |= lql_json_temporal_range_complete(scan);
    }
    lql_json_temporal_range_start(scan, 0ul);
    lql_json_match_start(scan, 0ul, 0, 0u);
    return status;
  }
  if (value != '{' && value != '[') {
    lql_json_flat_term_kind scalar_kind;
    unsigned long scalar_terms;
    unsigned long range_terms;
    const char *literal;
    size_t literal_len;
    literal = NULL;
    literal_len = 0u;
    if (value == 't' || value == 'f') {
      scalar_kind = LQL_JSON_FLAT_TERM_BOOL_EQ;
      range_terms = 0ul;
      literal = value == 't' ? "true" : "false";
      literal_len = value == 't' ? 4u : 5u;
    } else if (value == 'n') {
      scalar_kind = LQL_JSON_FLAT_TERM_NULL_EQ;
      range_terms = 0ul;
      literal = "null";
      literal_len = 4u;
    } else {
      scalar_kind = LQL_JSON_FLAT_TERM_NUMBER_EQ;
      range_terms = 0ul;
      {
        size_t i;
        for (i = 0u; i < scan->flat_term_count; ++i) {
          unsigned long bit;
          bit = 1ul << i;
          if ((matches & bit) != 0ul &&
              lql_json_term_value_at(&scan->flat_terms[i], path_segment) &&
              scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_NUMBER_RANGE) {
            range_terms |= bit;
          }
        }
      }
    }
    scalar_terms = 0ul;
    {
      size_t i;
      for (i = 0u; i < scan->flat_term_count; ++i) {
        unsigned long bit;
        bit = 1ul << i;
        if ((matches & bit) != 0ul &&
            lql_json_term_value_at(&scan->flat_terms[i], path_segment) &&
            scan->flat_terms[i].kind == scalar_kind) {
          scalar_terms |= bit;
        }
      }
    }
    if (scalar_terms == 0ul && range_terms == 0ul)
      return lql_json_value(scan);
    if (literal != NULL && range_terms == 0ul) {
      status = lql_json_literal(scan, literal);
      if (status == LQL_STATUS_OK) {
        scan->flat_eq_hits |=
            lql_json_scalar_literal_hits(scan, scalar_terms, literal,
                                         literal_len);
      }
      return status;
    }
    lql_json_match_start(scan, scalar_terms, 0, 0u);
    lql_json_number_range_start(scan, range_terms);
    status = lql_json_value(scan);
    if (status == LQL_STATUS_OK) {
      scan->flat_eq_hits |= lql_json_match_complete(scan);
      scan->flat_eq_hits |= lql_json_number_range_complete(scan);
    }
    lql_json_number_range_start(scan, 0ul);
    lql_json_match_start(scan, 0ul, 0, 0u);
    return status;
  }
  return lql_json_value(scan);
}

static int lql_json_try_plain_key_match(lql_json_scan *scan,
                                        unsigned long ordinary_terms,
                                        unsigned long recursive_terms,
                                        size_t object_depth,
                                        unsigned long *out_matches) {
  const unsigned char *key;
  size_t key_len;
  size_t key_start;
  size_t key_end;
  unsigned long active;
  unsigned long matches;
  size_t i;
  if (scan == NULL || out_matches == NULL ||
      scan->offset >= scan->length ||
      scan->buffer[scan->offset] != (unsigned char)'"') {
    return 0;
  }
  key_start = scan->offset + 1u;
  key_len = lql_json_plain_key_span(scan->buffer + key_start,
                                    scan->length - key_start);
  key_end = key_start + key_len;
  if (key_end >= scan->length ||
      scan->buffer[key_end] != (unsigned char)'"') {
    return 0;
  }
  key = scan->buffer + key_start;
  active = ordinary_terms | recursive_terms;
  matches = 0ul;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    const lql_json_flat_eq_term *term;
    const char *target;
    size_t target_len;
    size_t segment;
    unsigned long bit;
    bit = 1ul << i;
    if ((active & bit) == 0ul) {
      continue;
    }
    term = &scan->flat_terms[i];
    if ((recursive_terms & bit) != 0ul) {
      size_t recursive_segment;
      if (!lql_json_term_recursive_segment(term, &recursive_segment)) {
        continue;
      }
      segment = recursive_segment + 1u;
    } else {
      segment = object_depth;
    }
    if (segment == 0u) {
      target = term->field;
      target_len = term->field_len;
    } else if (!lql_json_term_path_segment(term, segment, &target,
                                           &target_len)) {
      continue;
    }
    if (target_len == key_len &&
        (key_len == 0u || memcmp(target, key, key_len) == 0)) {
      scan->match_term_segment[i] = segment;
      matches |= bit;
    }
  }
  scan->offset = key_end + 1u;
  *out_matches = matches;
  return 1;
}

static lql_status lql_json_object(lql_json_scan *scan) {
  int value;
  unsigned long exists_terms;
  unsigned long key_matches;
  unsigned long key_active;
  unsigned long key_source;
  unsigned long key_wildcards;
  unsigned long key_captures;
  unsigned long capture_source;
  unsigned long capture_descendants;
  unsigned long capture_values;
  unsigned long recursive_terms;
  unsigned long inherited_recursive;
  unsigned long descendants;
  size_t object_depth;
  size_t capture_start;
  lql_status status;
  if (scan->depth == LQL_JSON_MAX_DEPTH) {
    lql_json_error(scan, "JSON nesting exceeds the scanner limit");
    return LQL_STATUS_JSON_ERROR;
  }
  object_depth = scan->depth;
  ++scan->depth;
  status = lql_json_take_expected(scan, '{');
  if (status != LQL_STATUS_OK ||
      (status = lql_json_copy_byte(scan, '{')) != LQL_STATUS_OK ||
      (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
      (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
    return status;
  }
  if (value == '}') {
    ++scan->offset;
    status = lql_json_copy_byte(scan, '}');
    --scan->depth;
    return status;
  }
  for (;;) {
    if (value != '"') {
      lql_json_error(scan, "JSON object key must be a string");
      return LQL_STATUS_JSON_ERROR;
    }
    if (scan->writer == NULL && lql_json_stop_hit_ready(scan) &&
        scan->capture_path_active[object_depth] == 0ul) {
      status = lql_json_skip_string_fast(scan);
      if (status != LQL_STATUS_OK ||
          (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
          (status = lql_json_take_expected(scan, ':')) != LQL_STATUS_OK ||
          (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
          (status = lql_json_skip_value_fast(scan)) != LQL_STATUS_OK ||
          (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
          (status = lql_json_take(scan, &value)) != LQL_STATUS_OK) {
        return status;
      }
      if (value == '}') {
        --scan->depth;
        return LQL_STATUS_OK;
      }
      if (value != ',') {
        lql_json_error(scan, "JSON object member separator is missing");
        return LQL_STATUS_JSON_ERROR;
      }
      status = lql_json_skip_space(scan);
      if (status != LQL_STATUS_OK ||
          (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
        return status;
      }
      continue;
    }
    key_active = 0ul;
    inherited_recursive = 0ul;
    if (scan->flat_eq_active && !lql_json_stop_hit_ready(scan)) {
      key_active = scan->path_active[object_depth];
      inherited_recursive = scan->recursive_active[object_depth];
    }
    key_source = key_active;
    recursive_terms = inherited_recursive;
    if (scan->flat_eq_has_recursive_terms) {
      recursive_terms |=
          lql_json_match_recursive_terms(scan, key_source, object_depth);
    }
    if (scan->flat_eq_has_array_terms) {
      key_active = lql_json_match_object_terms(scan, key_active, object_depth);
    }
    key_wildcards = scan->flat_eq_has_object_wildcards
                        ? lql_json_match_object_wildcards(scan, key_source,
                                                          object_depth)
                        : 0ul;
    capture_source = scan->capture_path_active[object_depth];
    if (scan->writer == NULL && capture_source == 0ul &&
        lql_json_try_plain_key_match(scan, key_active, recursive_terms,
                                     object_depth, &key_matches)) {
      status = LQL_STATUS_OK;
      key_matches |= key_wildcards;
      key_captures = 0ul;
    } else {
      lql_json_capture_start(scan, capture_source, object_depth);
      lql_json_match_start_key(scan, key_active, recursive_terms, object_depth);
      status = lql_json_string(scan);
      key_matches = lql_json_match_complete(scan) | key_wildcards;
      key_captures = lql_json_capture_complete(scan);
    }
    if (key_matches != 0ul) {
      size_t i;
      for (i = 0u; i < scan->flat_term_count; ++i) {
        unsigned long bit;
        bit = 1ul << i;
        if ((key_matches & bit) != 0ul) {
          scan->key_term_segment[i] = (key_wildcards & bit) != 0ul
                                          ? object_depth
                                          : scan->match_term_segment[i];
        }
      }
    }
    exists_terms =
        key_matches == 0ul ? 0ul : lql_json_match_exists(scan, key_matches);
    lql_json_match_start(scan, 0ul, 0, 0u);
    if (status != LQL_STATUS_OK ||
        (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
        (status = lql_json_take_expected(scan, ':')) != LQL_STATUS_OK ||
        (status = lql_json_copy_byte(scan, ':')) != LQL_STATUS_OK ||
        (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
        (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
      return status;
    }
    if (value != 'n') {
      scan->flat_eq_hits |= exists_terms;
    }
    descendants = key_matches == 0ul
                      ? 0ul
                      : lql_json_match_descendants(scan, key_matches);
    capture_values = 0ul;
    capture_descendants = 0ul;
    {
      size_t i;
      for (i = 0u; i < scan->capture_key_count; ++i) {
        unsigned long bit;
        bit = 1ul << i;
        if ((key_captures & bit) == 0ul)
          continue;
        if (scan->capture_keys[i].segment_count == object_depth + 1u)
          capture_values |= bit;
        else
          capture_descendants |= bit;
      }
    }
    capture_start = capture_values == 0ul ? 0u : lql_json_output_offset(scan);
    if (object_depth + 1u < LQL_JSON_MAX_DEPTH) {
      scan->path_active[object_depth + 1u] =
          (value == '{' || value == '[') ? descendants : 0ul;
      scan->recursive_active[object_depth + 1u] =
          (value == '{' || value == '[') ? recursive_terms : 0ul;
      scan->capture_path_active[object_depth + 1u] =
          (value == '{' || value == '[') ? capture_descendants : 0ul;
    }
    if (key_matches && value == '"') {
      unsigned long eq_terms;
      unsigned long temporal_terms;
      size_t i;
      eq_terms = 0ul;
      temporal_terms = 0ul;
      for (i = 0u; i < scan->flat_term_count; ++i) {
        unsigned long bit;
        bit = 1ul << i;
        if ((key_matches & bit) != 0ul &&
            lql_json_term_value_at(&scan->flat_terms[i],
                                   scan->key_term_segment[i])) {
          if (scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_TEMPORAL_RANGE) {
            temporal_terms |= bit;
          } else if (scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_EQ ||
                     scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_PREFIX ||
                     scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_CONTAINS ||
                     scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_ICONTAINS ||
                     scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_IPREFIX) {
            eq_terms |= bit;
          }
        }
      }
      lql_json_match_start(scan, eq_terms, 0, 0u);
      lql_json_temporal_range_start(scan, temporal_terms);
      status = lql_json_string(scan);
      if (status == LQL_STATUS_OK) {
        scan->flat_eq_hits |= lql_json_match_complete(scan);
        scan->flat_eq_hits |= lql_json_temporal_range_complete(scan);
      }
      lql_json_temporal_range_start(scan, 0ul);
      lql_json_match_start(scan, 0ul, 0, 0u);
    } else if (key_matches) {
      lql_json_flat_term_kind scalar_kind;
      unsigned long scalar_terms;
      unsigned long range_terms;
      const char *literal;
      size_t literal_len;
      range_terms = 0ul;
      literal = NULL;
      literal_len = 0u;
      if (value == 't' || value == 'f') {
        scalar_kind = LQL_JSON_FLAT_TERM_BOOL_EQ;
        literal = value == 't' ? "true" : "false";
        literal_len = value == 't' ? 4u : 5u;
      } else if (value == 'n') {
        scalar_kind = LQL_JSON_FLAT_TERM_NULL_EQ;
        literal = "null";
        literal_len = 4u;
      } else {
        scalar_kind = LQL_JSON_FLAT_TERM_NUMBER_EQ;
        range_terms = lql_json_match_terms_for_kind(
            scan, key_matches, LQL_JSON_FLAT_TERM_NUMBER_RANGE);
      }
      scalar_terms =
          lql_json_match_terms_for_kind(scan, key_matches, scalar_kind);
      if (literal != NULL && range_terms == 0ul) {
        status = lql_json_literal(scan, literal);
        if (status == LQL_STATUS_OK) {
          scan->flat_eq_hits |=
              lql_json_scalar_literal_hits(scan, scalar_terms, literal,
                                           literal_len);
        }
      } else {
        lql_json_match_start(scan, scalar_terms, 0, 0u);
        lql_json_number_range_start(scan, range_terms);
        status = lql_json_value(scan);
        if (status == LQL_STATUS_OK) {
          scan->flat_eq_hits |= lql_json_match_complete(scan);
          scan->flat_eq_hits |= lql_json_number_range_complete(scan);
        }
        lql_json_number_range_start(scan, 0ul);
        lql_json_match_start(scan, 0ul, 0, 0u);
      }
    } else if (scan->writer == NULL && key_matches == 0ul &&
               descendants == 0ul && recursive_terms == 0ul &&
               capture_values == 0ul && capture_descendants == 0ul) {
      status = lql_json_skip_value_fast(scan);
    } else {
      status = lql_json_value(scan);
    }
    if (capture_values != 0ul && status == LQL_STATUS_OK) {
      size_t i;
      size_t capture_end;
      capture_end = lql_json_output_offset(scan);
      for (i = 0u; i < scan->capture_key_count; ++i) {
        if ((capture_values & (1ul << i)) != 0ul) {
          scan->capture_spans[i].offset = capture_start;
          scan->capture_spans[i].len = capture_end - capture_start;
          scan->capture_spans[i].found = 1;
        }
      }
    }
    if (object_depth + 1u < LQL_JSON_MAX_DEPTH) {
      scan->path_active[object_depth + 1u] = 0ul;
      scan->recursive_active[object_depth + 1u] = 0ul;
      scan->capture_path_active[object_depth + 1u] = 0ul;
    }
    if (status != LQL_STATUS_OK ||
        (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
        (status = lql_json_take(scan, &value)) != LQL_STATUS_OK) {
      return status;
    }
    if (value == '}') {
      status = lql_json_copy_byte(scan, '}');
      --scan->depth;
      return status;
    }
    if (value != ',') {
      lql_json_error(scan, "JSON object member separator is missing");
      return LQL_STATUS_JSON_ERROR;
    }
    status = lql_json_copy_byte(scan, ',');
    if (status != LQL_STATUS_OK ||
        (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
        (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
      return status;
    }
  }
}

static lql_status lql_json_array(lql_json_scan *scan) {
  int value;
  unsigned long active;
  unsigned long capture_active;
  unsigned long capture_descendants;
  unsigned long capture_values;
  unsigned long element_active;
  unsigned long recursive_terms;
  size_t capture_start;
  size_t array_segment;
  size_t index;
  lql_status status;
  if (scan->depth == LQL_JSON_MAX_DEPTH) {
    lql_json_error(scan, "JSON nesting exceeds the scanner limit");
    return LQL_STATUS_JSON_ERROR;
  }
  array_segment = scan->depth;
  active = scan->path_active[array_segment];
  capture_active = scan->capture_path_active[array_segment];
  recursive_terms =
      lql_json_match_recursive_terms(scan, active, array_segment) |
      scan->recursive_active[array_segment];
  if (scan->depth + 1u < LQL_JSON_MAX_DEPTH) {
    scan->path_active[scan->depth + 1u] = 0ul;
    scan->capture_path_active[scan->depth + 1u] = 0ul;
  }
  ++scan->depth;
  status = lql_json_take_expected(scan, '[');
  if (status != LQL_STATUS_OK ||
      (status = lql_json_copy_byte(scan, '[')) != LQL_STATUS_OK ||
      (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
      (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
    return status;
  }
  if (value == ']') {
    ++scan->offset;
    status = lql_json_copy_byte(scan, ']');
    --scan->depth;
    return status;
  }
  index = 0u;
  for (;;) {
    size_t i;
    element_active =
        lql_json_match_array_index(scan, active, array_segment, index);
    scan->path_active[scan->depth] = element_active;
    scan->recursive_active[scan->depth] = recursive_terms;
    capture_values = 0ul;
    capture_descendants = 0ul;
    for (i = 0u; i < scan->capture_key_count; ++i) {
      unsigned long bit;
      bit = 1ul << i;
      if ((capture_active & bit) == 0ul ||
          !lql_json_capture_array_index(scan, i, array_segment, index))
        continue;
      if (scan->capture_keys[i].segment_count == array_segment + 1u)
        capture_values |= bit;
      else
        capture_descendants |= bit;
    }
    capture_start = capture_values == 0ul ? 0u : lql_json_output_offset(scan);
    scan->capture_path_active[scan->depth] = capture_descendants;
    if (element_active != 0ul) {
      status = lql_json_peek(scan, &value);
      if (status == LQL_STATUS_OK) {
        status = lql_json_matched_scalar_value(scan, element_active,
                                               array_segment, value);
      }
    } else {
      status = lql_json_value(scan);
    }
    if (capture_values != 0ul && status == LQL_STATUS_OK) {
      size_t capture_end;
      capture_end = lql_json_output_offset(scan);
      for (i = 0u; i < scan->capture_key_count; ++i) {
        if ((capture_values & (1ul << i)) != 0ul) {
          scan->capture_spans[i].offset = capture_start;
          scan->capture_spans[i].len = capture_end - capture_start;
          scan->capture_spans[i].found = 1;
        }
      }
    }
    scan->path_active[scan->depth] = 0ul;
    scan->recursive_active[scan->depth] = 0ul;
    scan->capture_path_active[scan->depth] = 0ul;
    if (status != LQL_STATUS_OK ||
        (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
        (status = lql_json_take(scan, &value)) != LQL_STATUS_OK) {
      return status;
    }
    if (value == ']') {
      status = lql_json_copy_byte(scan, ']');
      --scan->depth;
      return status;
    }
    if (value != ',') {
      lql_json_error(scan, "JSON array separator is missing");
      return LQL_STATUS_JSON_ERROR;
    }
    ++index;
    status = lql_json_copy_byte(scan, ',');
    if (status != LQL_STATUS_OK ||
        (status = lql_json_skip_space(scan)) != LQL_STATUS_OK) {
      return status;
    }
  }
}

static lql_status lql_json_literal(lql_json_scan *scan, const char *literal) {
  size_t i;
  lql_status status;
  for (i = 0u; literal[i] != '\0'; ++i) {
    status = lql_json_take_expected(scan, literal[i]);
    if (status != LQL_STATUS_OK || (status = lql_json_match_copy_byte(
                                        scan, literal[i])) != LQL_STATUS_OK) {
      return status;
    }
  }
  return LQL_STATUS_OK;
}

static lql_status lql_json_skip_literal_fast(lql_json_scan *scan,
                                             const char *literal) {
  size_t i;
  lql_status status;
  for (i = 0u; literal[i] != '\0'; ++i) {
    status = lql_json_take_expected(scan, literal[i]);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  return LQL_STATUS_OK;
}

static lql_status lql_json_number(lql_json_scan *scan) {
  int value;
  int have_value;
  lql_status status;
  have_value = 0;
  status = lql_json_peek(scan, &value);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  if (value == '-') {
    ++scan->offset;
    status = lql_json_match_copy_byte(scan, '-');
    if (status != LQL_STATUS_OK ||
        (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
      return status;
    }
  }
  if (value == '0') {
    ++scan->offset;
    status = lql_json_match_copy_byte(scan, value);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  } else if (value >= '1' && value <= '9') {
    do {
      ++scan->offset;
      status = lql_json_match_copy_byte(scan, value);
      if (status != LQL_STATUS_OK ||
          (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
        return status;
      }
    } while (value >= '0' && value <= '9');
    have_value = 1;
  } else {
    lql_json_error(scan, "invalid JSON number");
    return LQL_STATUS_JSON_ERROR;
  }
  if (!have_value) {
    status = lql_json_peek(scan, &value);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  if (value == '.') {
    ++scan->offset;
    status = lql_json_match_copy_byte(scan, '.');
    if (status != LQL_STATUS_OK ||
        (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK ||
        value < '0' || value > '9') {
      lql_json_error(scan, "invalid JSON fraction");
      return status == LQL_STATUS_OK ? LQL_STATUS_JSON_ERROR : status;
    }
    do {
      ++scan->offset;
      status = lql_json_match_copy_byte(scan, value);
      if (status != LQL_STATUS_OK ||
          (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
        return status;
      }
    } while (value >= '0' && value <= '9');
  }
  if (value == 'e' || value == 'E') {
    ++scan->offset;
    status = lql_json_match_copy_byte(scan, value);
    if (status != LQL_STATUS_OK ||
        (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
      return status;
    }
    if (value == '+' || value == '-') {
      ++scan->offset;
      status = lql_json_match_copy_byte(scan, value);
      if (status != LQL_STATUS_OK ||
          (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
        return status;
      }
    }
    if (value < '0' || value > '9') {
      lql_json_error(scan, "invalid JSON exponent");
      return LQL_STATUS_JSON_ERROR;
    }
    do {
      ++scan->offset;
      status = lql_json_match_copy_byte(scan, value);
      if (status != LQL_STATUS_OK ||
          (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
        return status;
      }
    } while (value >= '0' && value <= '9');
  }
  return LQL_STATUS_OK;
}

static lql_status lql_json_skip_number_fast(lql_json_scan *scan) {
  int value;
  int have_value;
  lql_status status;
  have_value = 0;
  status = lql_json_peek(scan, &value);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  if (value == '-') {
    ++scan->offset;
    status = lql_json_peek(scan, &value);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  if (value == '0') {
    ++scan->offset;
  } else if (value >= '1' && value <= '9') {
    do {
      ++scan->offset;
      status = lql_json_peek(scan, &value);
      if (status != LQL_STATUS_OK) {
        return status;
      }
    } while (value >= '0' && value <= '9');
    have_value = 1;
  } else {
    lql_json_error(scan, "invalid JSON number");
    return LQL_STATUS_JSON_ERROR;
  }
  if (!have_value) {
    status = lql_json_peek(scan, &value);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  if (value == '.') {
    ++scan->offset;
    status = lql_json_peek(scan, &value);
    if (status != LQL_STATUS_OK || value < '0' || value > '9') {
      lql_json_error(scan, "invalid JSON fraction");
      return status == LQL_STATUS_OK ? LQL_STATUS_JSON_ERROR : status;
    }
    do {
      ++scan->offset;
      status = lql_json_peek(scan, &value);
      if (status != LQL_STATUS_OK) {
        return status;
      }
    } while (value >= '0' && value <= '9');
  }
  if (value == 'e' || value == 'E') {
    ++scan->offset;
    status = lql_json_peek(scan, &value);
    if (status != LQL_STATUS_OK) {
      return status;
    }
    if (value == '+' || value == '-') {
      ++scan->offset;
      status = lql_json_peek(scan, &value);
      if (status != LQL_STATUS_OK) {
        return status;
      }
    }
    if (value < '0' || value > '9') {
      lql_json_error(scan, "invalid JSON exponent");
      return LQL_STATUS_JSON_ERROR;
    }
    do {
      ++scan->offset;
      status = lql_json_peek(scan, &value);
      if (status != LQL_STATUS_OK) {
        return status;
      }
    } while (value >= '0' && value <= '9');
  }
  return LQL_STATUS_OK;
}

static lql_status lql_json_record_separator(lql_json_scan *scan) {
  int value;
  lql_status status;
  for (;;) {
    status = lql_json_peek(scan, &value);
    if (status != LQL_STATUS_OK || (value != ' ' && value != '\t')) {
      break;
    }
    ++scan->offset;
  }
  if (status != LQL_STATUS_OK || value < 0) {
    return status;
  }
  if (value == '\n') {
    ++scan->offset;
    return LQL_STATUS_OK;
  }
  if (value == '\r') {
    ++scan->offset;
    status = lql_json_take_expected(scan, '\n');
    return status;
  }
  lql_json_error(scan, "NDJSON record separator is missing");
  return LQL_STATUS_JSON_ERROR;
}

static lql_status lql_json_value(lql_json_scan *scan) {
  int value;
  lql_status status;
  status = lql_json_peek(scan, &value);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  if (value == '{') {
    return lql_json_object(scan);
  }
  if (value == '[') {
    return lql_json_array(scan);
  }
  if (value == '"') {
    return lql_json_string(scan);
  }
  if (value == 't') {
    return lql_json_literal(scan, "true");
  }
  if (value == 'f') {
    return lql_json_literal(scan, "false");
  }
  if (value == 'n') {
    return lql_json_literal(scan, "null");
  }
  return lql_json_number(scan);
}

lql_status lql_json_normalize_ndjson(const lql_json_normalize_request *request,
                                     size_t *out_records,
                                     size_t *out_bytes_read, lql_error *error) {
  lql_json_scan scan;
  lql_status status;
  int value;
  size_t records;
  if (out_records != NULL) {
    *out_records = 0u;
  }
  if (out_bytes_read != NULL) {
    *out_bytes_read = 0u;
  }
  if (request == NULL || request->reader == NULL) {
    if (error != NULL) {
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "JSON scanner requires a reader");
    }
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&scan, 0, sizeof(scan));
  scan.reader = request->reader;
  scan.reader_user = request->reader_user;
  scan.writer = request->writer;
  scan.writer_user = request->writer_user;
  scan.error = error;
  records = 0u;
  for (;;) {
    status = lql_json_skip_space(&scan);
    if (status != LQL_STATUS_OK ||
        (status = lql_json_peek(&scan, &value)) != LQL_STATUS_OK) {
      break;
    }
    if (value < 0) {
      status = LQL_STATUS_OK;
      break;
    }
    if (value == '[') {
      lql_json_error(&scan, "root JSON arrays are not valid NDJSON records");
      status = LQL_STATUS_JSON_ERROR;
      break;
    }
    status = lql_json_value(&scan);
    if (status != LQL_STATUS_OK) {
      break;
    }
    status = lql_json_record_separator(&scan);
    if (status != LQL_STATUS_OK) {
      break;
    }
    status = lql_json_write(&scan, "\n", 1u);
    if (status == LQL_STATUS_OK) {
      status = lql_json_flush(&scan);
    }
    if (status != LQL_STATUS_OK) {
      break;
    }
    ++records;
  }
  if (out_records != NULL) {
    *out_records = records;
  }
  if (out_bytes_read != NULL) {
    *out_bytes_read = scan.bytes_read;
  }
  return status;
}

static lql_status lql_json_spool_write(void *user, const void *data, size_t len,
                                       lql_error *error) {
  return lql_json_spool_append((lql_json_spool *)user, data, len, error);
}

lql_status lql_json_scan_flat_eq_ndjson(const lql_json_flat_eq_request *request,
                                        size_t *out_records,
                                        size_t *out_bytes_read,
                                        lql_error *error) {
  lql_json_scan scan;
  lql_status status;
  int value;
  int root_is_object;
  int source_compact;
  size_t records;
  size_t record_start;
  size_t record_end;
  if (out_records != NULL) {
    *out_records = 0u;
  }
  if (out_bytes_read != NULL) {
    *out_bytes_read = 0u;
  }
  if (request == NULL || request->reader == NULL ||
      request->term_count > LQL_JSON_FLAT_TERM_CAPACITY ||
      request->capture_key_count > LQL_JSON_FLAT_TERM_CAPACITY ||
      (request->term_count != 0u && request->terms == NULL) ||
      (request->capture_key_count != 0u &&
       (request->capture_keys == NULL || request->capture_spans == NULL)) ||
      request->record == NULL || (request->capture && request->spool == NULL)) {
    if (error != NULL) {
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "flat JSON equality scanner is not configured");
    }
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(&scan, 0, sizeof(scan));
  scan.reader = request->reader;
  scan.reader_user = request->reader_user;
  scan.writer = request->capture ? lql_json_spool_write : NULL;
  scan.writer_user = request->spool;
  scan.error = error;
  scan.flat_terms = request->terms;
  scan.flat_term_count = request->term_count;
  scan.capture_keys = request->capture_keys;
  scan.capture_key_count = request->capture_key_count;
  scan.capture_spans = request->capture_spans;
  scan.flat_eq_stop_on_hit = request->stop_matching_on_hit;
  scan.flat_eq_stop_hit_mask = request->stop_hit_mask;
  for (records = 0u; records < scan.flat_term_count; ++records) {
    if (scan.flat_terms[records].path_object_wildcards != 0ul ||
        scan.flat_terms[records].path_any_wildcards != 0ul) {
      scan.flat_eq_has_object_wildcards = 1;
    }
    if (scan.flat_terms[records].path_recursive_segments != 0ul) {
      scan.flat_eq_has_recursive_terms = 1;
    }
    if (scan.flat_terms[records].path_array_segments != 0ul ||
        scan.flat_terms[records].path_object_wildcards != 0ul ||
        scan.flat_terms[records].path_array_wildcards != 0ul ||
        scan.flat_terms[records].path_any_wildcards != 0ul ||
        scan.flat_terms[records].path_recursive_segments != 0ul) {
      scan.flat_eq_has_array_terms = 1;
    }
  }
  records = 0u;
  for (;;) {
    status = lql_json_skip_space(&scan);
    if (status != LQL_STATUS_OK ||
        (status = lql_json_peek(&scan, &value)) != LQL_STATUS_OK) {
      break;
    }
    if (value < 0) {
      status = LQL_STATUS_OK;
      break;
    }
    if (request->max_records != 0u && records >= request->max_records) {
      status = LQL_STATUS_STOP;
      break;
    }
    if (value == '[') {
      lql_json_error(&scan, "root JSON arrays are not valid NDJSON records");
      status = LQL_STATUS_JSON_ERROR;
      break;
    }
    if (request->capture) {
      lql_json_spool_reset(request->spool);
    }
    record_start = lql_json_consumed(&scan);
    root_is_object = value == '{';
    scan.flat_eq_active = root_is_object;
    scan.flat_eq_hits = 0ul;
    scan.compact_range_active = 1;
    scan.compact_range_ok = 1;
    if (scan.capture_spans != NULL)
      memset(scan.capture_spans, 0,
             scan.capture_key_count * sizeof(*scan.capture_spans));
    scan.path_active[0] = scan.flat_term_count == LQL_JSON_FLAT_TERM_CAPACITY
                              ? ~0ul
                              : ((1ul << scan.flat_term_count) - 1ul);
    scan.capture_path_active[0] =
        scan.capture_key_count == LQL_JSON_FLAT_TERM_CAPACITY
            ? ~0ul
            : ((1ul << scan.capture_key_count) - 1ul);
    lql_json_match_start(&scan, 0ul, 0, 0u);
    status = lql_json_value(&scan);
    record_end = lql_json_consumed(&scan);
    source_compact = scan.compact_range_ok;
    scan.compact_range_active = 0;
    if (status != LQL_STATUS_OK) {
      break;
    }
    status = lql_json_record_separator(&scan);
    if (status != LQL_STATUS_OK) {
      break;
    }
    status = lql_json_flush(&scan);
    if (status != LQL_STATUS_OK) {
      break;
    }
    status = request->record(request->record_user, records, root_is_object,
                             root_is_object ? scan.flat_eq_hits : 0ul,
                             request->spool, record_start,
                             record_end - record_start, source_compact, error);
    ++records;
    if (status != LQL_STATUS_OK) {
      break;
    }
    if (request->max_bytes != 0u &&
        lql_json_consumed(&scan) >= request->max_bytes) {
      status = LQL_STATUS_STOP;
      break;
    }
  }
  if (out_records != NULL) {
    *out_records = records;
  }
  if (out_bytes_read != NULL) {
    *out_bytes_read = lql_json_consumed(&scan);
  }
  return status;
}
