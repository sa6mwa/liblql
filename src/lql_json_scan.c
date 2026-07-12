#include "lql_json_scan.h"

#include <string.h>

#define LQL_JSON_READ_BUFFER_SIZE 8192u
#define LQL_JSON_MAX_DEPTH 128u

typedef struct lql_json_scan {
  const lql_json_normalize_request *request;
  unsigned char buffer[LQL_JSON_READ_BUFFER_SIZE];
  size_t offset;
  size_t length;
  size_t bytes_read;
  size_t depth;
  lql_error *error;
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
  if (len == 0u) {
    return LQL_STATUS_OK;
  }
  if (scan->request->writer == NULL) {
    return LQL_STATUS_OK;
  }
  return scan->request->writer(scan->request->writer_user, data, len,
                               scan->error);
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
  status = scan->request->reader(scan->request->reader_user, scan->buffer,
                                 sizeof(scan->buffer), &amount, scan->error);
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

static lql_status lql_json_string(lql_json_scan *scan) {
  int value;
  int next;
  int remaining;
  int continuation_min;
  int continuation_max;
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
      continue;
    }
    if (value < 0x80) {
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
      --remaining;
      continuation_min = 0x80;
      continuation_max = 0xbf;
    }
  }
}

static lql_status lql_json_value(lql_json_scan *scan);

static lql_status lql_json_object(lql_json_scan *scan) {
  int value;
  lql_status status;
  if (scan->depth == LQL_JSON_MAX_DEPTH) {
    lql_json_error(scan, "JSON nesting exceeds the scanner limit");
    return LQL_STATUS_JSON_ERROR;
  }
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
    status = lql_json_string(scan);
    if (status != LQL_STATUS_OK ||
        (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
        (status = lql_json_take_expected(scan, ':')) != LQL_STATUS_OK ||
        (status = lql_json_copy_byte(scan, ':')) != LQL_STATUS_OK ||
        (status = lql_json_skip_space(scan)) != LQL_STATUS_OK ||
        (status = lql_json_value(scan)) != LQL_STATUS_OK ||
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
    if (status != LQL_STATUS_OK ||
        (status = lql_json_copy_byte(scan, literal[i])) != LQL_STATUS_OK) {
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
    status = lql_json_copy_byte(scan, '-');
    if (status != LQL_STATUS_OK ||
        (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
      return status;
    }
  }
  if (value == '0') {
    ++scan->offset;
    status = lql_json_copy_byte(scan, value);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  } else if (value >= '1' && value <= '9') {
    do {
      ++scan->offset;
      status = lql_json_copy_byte(scan, value);
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
    status = lql_json_copy_byte(scan, '.');
    if (status != LQL_STATUS_OK ||
        (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK ||
        value < '0' || value > '9') {
      lql_json_error(scan, "invalid JSON fraction");
      return status == LQL_STATUS_OK ? LQL_STATUS_JSON_ERROR : status;
    }
    do {
      ++scan->offset;
      status = lql_json_copy_byte(scan, value);
      if (status != LQL_STATUS_OK ||
          (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
        return status;
      }
    } while (value >= '0' && value <= '9');
  }
  if (value == 'e' || value == 'E') {
    ++scan->offset;
    status = lql_json_copy_byte(scan, value);
    if (status != LQL_STATUS_OK ||
        (status = lql_json_peek(scan, &value)) != LQL_STATUS_OK) {
      return status;
    }
    if (value == '+' || value == '-') {
      ++scan->offset;
      status = lql_json_copy_byte(scan, value);
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
      status = lql_json_copy_byte(scan, value);
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
  scan.request = request;
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
