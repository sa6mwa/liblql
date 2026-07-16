#include "lql_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int lql_number_finite(double value) {
  return value == value && value != HUGE_VAL && value != -HUGE_VAL;
}

static int lql_number_pow10(int exponent, long double *out) {
  long double result;
  long double factor;
  unsigned int power;

  result = 1.0L;
  factor = 10.0L;
  power = exponent < 0 ? (unsigned int)(-exponent) : (unsigned int)exponent;
  while (power != 0u) {
    if ((power & 1u) != 0u) {
      result *= factor;
      if (result > LDBL_MAX / 2.0L) {
        return 0;
      }
    }
    power >>= 1;
    if (power != 0u) {
      factor *= factor;
      if (factor > LDBL_MAX / 2.0L) {
        return 0;
      }
    }
  }
  *out = exponent < 0 ? 1.0L / result : result;
  return 1;
}

static int lql_number_parse_integer(const char *text, size_t len, double *out) {
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
  if (text[pos] == '0' && pos + 1u != len) {
    return 0;
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

LQL_INTERNAL_SYMBOL int lql_number_parse_json(const char *text, size_t len,
                                              double *out) {
  size_t pos;
  int negative;
  long double value;
  long double scale;
  int exp_negative;
  int exponent;
  long double multiplier;
  double result;

  if (text == NULL || len == 0u || out == NULL) {
    return 0;
  }
  if (lql_number_parse_integer(text, len, out)) {
    return 1;
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
  value = 0.0L;
  if (text[pos] == '0') {
    ++pos;
    if (pos < len && text[pos] >= '0' && text[pos] <= '9') {
      return 0;
    }
  } else if (text[pos] >= '1' && text[pos] <= '9') {
    do {
      if (value > (LDBL_MAX - 9.0L) / 10.0L) {
        return 0;
      }
      value = value * 10.0L + (long double)(text[pos] - '0');
      ++pos;
    } while (pos < len && text[pos] >= '0' && text[pos] <= '9');
  } else {
    return 0;
  }
  if (pos < len && text[pos] == '.') {
    ++pos;
    if (pos == len || text[pos] < '0' || text[pos] > '9') {
      return 0;
    }
    scale = 0.1L;
    do {
      value += (long double)(text[pos] - '0') * scale;
      scale *= 0.1L;
      ++pos;
    } while (pos < len && text[pos] >= '0' && text[pos] <= '9');
  }
  if (pos < len && (text[pos] == 'e' || text[pos] == 'E')) {
    ++pos;
    exp_negative = 0;
    if (pos < len && (text[pos] == '+' || text[pos] == '-')) {
      exp_negative = text[pos] == '-';
      ++pos;
    }
    if (pos == len || text[pos] < '0' || text[pos] > '9') {
      return 0;
    }
    exponent = 0;
    do {
      if (exponent > 10000) {
        return 0;
      }
      exponent = exponent * 10 + (int)(text[pos] - '0');
      ++pos;
    } while (pos < len && text[pos] >= '0' && text[pos] <= '9');
    if (exp_negative) {
      exponent = -exponent;
    }
    if (!lql_number_pow10(exponent, &multiplier)) {
      return 0;
    }
    value *= multiplier;
  }
  if (pos != len) {
    return 0;
  }
  if (negative) {
    value = -value;
  }
  result = (double)value;
  if (!lql_number_finite(result)) {
    return 0;
  }
  *out = result;
  return 1;
}

LQL_INTERNAL_SYMBOL int lql_number_format_json(double value, char *out,
                                               size_t cap) {
  int len;
  size_t i;

  if (out == NULL || cap == 0u || !lql_number_finite(value)) {
    return 0;
  }
  len = sprintf(out, "%.17g", value);
  if (len <= 0 || (size_t)len >= cap) {
    return 0;
  }
  for (i = 0u; out[i] != '\0'; ++i) {
    if (out[i] == ',') {
      out[i] = '.';
    }
  }
  return 1;
}
