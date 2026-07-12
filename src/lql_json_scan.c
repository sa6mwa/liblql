#include "lql_json_scan.h"

#include <limits.h>
#include <string.h>

#define LQL_JSON_READ_BUFFER_SIZE 8192u
#define LQL_JSON_EMIT_BUFFER_SIZE 8192u
#define LQL_JSON_MAX_DEPTH 128u
#define LQL_JSON_FLAT_TERM_CAPACITY (sizeof(unsigned long) * CHAR_BIT)

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
  const lql_json_flat_eq_term *flat_terms;
  size_t flat_term_count;
  unsigned long match_active;
  unsigned long match_failed;
  size_t match_pos[LQL_JSON_FLAT_TERM_CAPACITY];
  int match_key;
  unsigned long flat_eq_hits;
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
                                 int key) {
  size_t i;
  scan->match_active = active;
  scan->match_failed = 0ul;
  scan->match_key = key;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    if ((active & (1ul << i)) != 0ul) {
      scan->match_pos[i] = 0u;
    }
  }
}

static void lql_json_match_byte(lql_json_scan *scan, unsigned char value) {
  size_t i;
  if (scan->match_active == 0ul) {
    return;
  }
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
    target = scan->match_key ? term->field : term->value;
    target_len = scan->match_key ? term->field_len : term->value_len;
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

static unsigned long lql_json_match_complete(const lql_json_scan *scan) {
  unsigned long matches;
  size_t i;
  matches = 0ul;
  for (i = 0u; i < scan->flat_term_count; ++i) {
    const lql_json_flat_eq_term *term;
    size_t target_len;
    unsigned long bit;
    bit = 1ul << i;
    if ((scan->match_active & bit) == 0ul ||
        (scan->match_failed & bit) != 0ul) {
      continue;
    }
    term = &scan->flat_terms[i];
    target_len = scan->match_key ? term->field_len : term->value_len;
    if ((scan->match_key || term->kind != LQL_JSON_FLAT_TERM_PREFIX) &&
        scan->match_pos[i] == target_len) {
      matches |= bit;
    }
    if (!scan->match_key && term->kind == LQL_JSON_FLAT_TERM_PREFIX &&
        scan->match_pos[i] >= target_len) {
      matches |= bit;
    }
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
    if ((keys & bit) != 0ul && scan->flat_terms[i].kind == kind) {
      active |= bit;
    }
  }
  return active;
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

static lql_status lql_json_peek(lql_json_scan *scan, int *out) {
  lql_status status;
  status = lql_json_refill(scan);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  *out = scan->offset == scan->length ? -1 : (int)scan->buffer[scan->offset];
  return LQL_STATUS_OK;
}

static lql_status lql_json_take(lql_json_scan *scan, int *out) {
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

static lql_status lql_json_take_expected(lql_json_scan *scan, int expected) {
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

static lql_status lql_json_skip_space(lql_json_scan *scan) {
  int value;
  lql_status status;
  for (;;) {
    status = lql_json_peek(scan, &value);
    if (status != LQL_STATUS_OK ||
        (value != ' ' && value != '\t' && value != '\r' && value != '\n')) {
      return status;
    }
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

static lql_status lql_json_copy_byte(lql_json_scan *scan, int value) {
  unsigned char byte;
  byte = (unsigned char)value;
  return lql_json_write(scan, &byte, 1u);
}

static lql_status lql_json_match_copy_byte(lql_json_scan *scan, int value) {
  if ((scan->match_active & ~scan->match_failed) != 0ul) {
    lql_json_match_byte(scan, (unsigned char)value);
  }
  return lql_json_copy_byte(scan, value);
}

static lql_status lql_json_string(lql_json_scan *scan) {
  int value;
  int next;
  int remaining;
  int continuation_min;
  int continuation_max;
  const unsigned char *span;
  size_t span_len;
  size_t i;
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
      status = lql_json_write(scan, span, span_len);
      if (status != LQL_STATUS_OK) {
        return status;
      }
      if ((scan->match_active & ~scan->match_failed) != 0ul) {
        for (i = 0u;
             i < span_len && (scan->match_active & ~scan->match_failed) != 0ul;
             ++i) {
          lql_json_match_byte(scan, span[i]);
        }
      }
      continue;
    }
    status = lql_json_take(scan, &value);
    if (status != LQL_STATUS_OK) {
      lql_json_error(scan, "unterminated JSON string");
      return status == LQL_STATUS_OK ? LQL_STATUS_JSON_ERROR : status;
    }
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

static lql_status lql_json_object(lql_json_scan *scan) {
  int value;
  unsigned long exists_terms;
  unsigned long key_matches;
  int top_level;
  lql_status status;
  if (scan->depth == LQL_JSON_MAX_DEPTH) {
    lql_json_error(scan, "JSON nesting exceeds the scanner limit");
    return LQL_STATUS_JSON_ERROR;
  }
  ++scan->depth;
  top_level = scan->flat_eq_active && scan->depth == 1u;
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
    if (top_level) {
      unsigned long active;
      active = scan->flat_term_count == LQL_JSON_FLAT_TERM_CAPACITY
                   ? ~0ul
                   : ((1ul << scan->flat_term_count) - 1ul);
      lql_json_match_start(scan, active, 1);
    } else {
      lql_json_match_start(scan, 0ul, 0);
    }
    status = lql_json_string(scan);
    key_matches = top_level ? lql_json_match_complete(scan) : 0ul;
    exists_terms = lql_json_match_exists(scan, key_matches);
    lql_json_match_start(scan, 0ul, 0);
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
    if (key_matches && value == '"') {
      unsigned long eq_terms;
      size_t i;
      eq_terms = 0ul;
      for (i = 0u; i < scan->flat_term_count; ++i) {
        unsigned long bit;
        bit = 1ul << i;
        if ((key_matches & bit) != 0ul &&
            (scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_EQ ||
             scan->flat_terms[i].kind == LQL_JSON_FLAT_TERM_PREFIX)) {
          eq_terms |= bit;
        }
      }
      lql_json_match_start(scan, eq_terms, 0);
      status = lql_json_string(scan);
      if (status == LQL_STATUS_OK) {
        scan->flat_eq_hits |= lql_json_match_complete(scan);
      }
      lql_json_match_start(scan, 0ul, 0);
    } else if (key_matches) {
      lql_json_flat_term_kind scalar_kind;
      unsigned long scalar_terms;
      if (value == 't' || value == 'f') {
        scalar_kind = LQL_JSON_FLAT_TERM_BOOL_EQ;
      } else if (value == 'n') {
        scalar_kind = LQL_JSON_FLAT_TERM_NULL_EQ;
      } else {
        scalar_kind = LQL_JSON_FLAT_TERM_NUMBER_EQ;
      }
      scalar_terms =
          lql_json_match_terms_for_kind(scan, key_matches, scalar_kind);
      lql_json_match_start(scan, scalar_terms, 0);
      status = lql_json_value(scan);
      if (status == LQL_STATUS_OK) {
        scan->flat_eq_hits |= lql_json_match_complete(scan);
      }
      lql_json_match_start(scan, 0ul, 0);
    } else {
      status = lql_json_value(scan);
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
  lql_status status;
  if (scan->depth == LQL_JSON_MAX_DEPTH) {
    lql_json_error(scan, "JSON nesting exceeds the scanner limit");
    return LQL_STATUS_JSON_ERROR;
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
  for (;;) {
    status = lql_json_value(scan);
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

static lql_status lql_json_number(lql_json_scan *scan) {
  int value;
  lql_status status;
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
  } else {
    lql_json_error(scan, "invalid JSON number");
    return LQL_STATUS_JSON_ERROR;
  }
  status = lql_json_peek(scan, &value);
  if (status != LQL_STATUS_OK) {
    return status;
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
  size_t records;
  if (out_records != NULL) {
    *out_records = 0u;
  }
  if (out_bytes_read != NULL) {
    *out_bytes_read = 0u;
  }
  if (request == NULL || request->reader == NULL ||
      request->term_count > LQL_JSON_FLAT_TERM_CAPACITY ||
      (request->term_count != 0u && request->terms == NULL) ||
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
    if (request->capture) {
      lql_json_spool_reset(request->spool);
    }
    root_is_object = value == '{';
    scan.flat_eq_active = root_is_object;
    scan.flat_eq_hits = 0ul;
    lql_json_match_start(&scan, 0ul, 0);
    status = lql_json_value(&scan);
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
                             request->spool, error);
    ++records;
    if (status != LQL_STATUS_OK) {
      break;
    }
  }
  if (out_records != NULL) {
    *out_records = records;
  }
  if (out_bytes_read != NULL) {
    *out_bytes_read = scan.bytes_read;
  }
  return status;
}
