#include "lql_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct lql_number_decimal {
  const char *integer;
  size_t integer_len;
  const char *fraction;
  size_t fraction_len;
  const char *exponent;
  size_t exponent_len;
  size_t leading_skip;
  size_t coeff_len;
  long decimal_adjust;
  int exponent_negative;
  int negative;
  int zero;
} lql_number_decimal;

static int lql_number_finite(double value) {
  return value == value && value != HUGE_VAL && value != -HUGE_VAL;
}

static long lql_number_clamp_add(long left, long right) {
  if (right > 0 && left > LONG_MAX - right) {
    return LONG_MAX;
  }
  if (right < 0 && left < LONG_MIN - right) {
    return LONG_MIN;
  }
  return left + right;
}

static int lql_number_size_to_long(size_t value, long *out) {
  if (value > (size_t)LONG_MAX) {
    *out = LONG_MAX;
    return 0;
  }
  *out = (long)value;
  return 1;
}

static char lql_number_digit_at(const lql_number_decimal *number,
                                size_t index) {
  size_t raw_index;
  raw_index = number->leading_skip + index;
  if (raw_index < number->integer_len) {
    return number->integer[raw_index];
  }
  return number->fraction[raw_index - number->integer_len];
}

static int lql_number_parse_exponent(const char *text, size_t len, size_t *pos,
                                     int *out_negative, const char **digits,
                                     size_t *digits_len) {
  const char *begin;
  int negative;
  size_t count;
  if (*pos >= len || (text[*pos] != 'e' && text[*pos] != 'E')) {
    *out_negative = 0;
    *digits = text + *pos;
    *digits_len = 0u;
    return 1;
  }
  ++*pos;
  negative = 0;
  if (*pos < len && (text[*pos] == '+' || text[*pos] == '-')) {
    negative = text[*pos] == '-';
    ++*pos;
  }
  if (*pos == len || text[*pos] < '0' || text[*pos] > '9') {
    return 0;
  }
  begin = text + *pos;
  count = 0u;
  while (*pos < len && text[*pos] >= '0' && text[*pos] <= '9') {
    ++*pos;
    ++count;
  }
  while (count != 0u && *begin == '0') {
    ++begin;
    --count;
  }
  *out_negative = count == 0u ? 0 : negative;
  *digits = begin;
  *digits_len = count;
  return 1;
}

static void lql_number_ulong_text(unsigned long value, char out[32],
                                  size_t *out_len) {
  char tmp[32];
  size_t len;
  len = 0u;
  do {
    tmp[len++] = (char)('0' + value % 10ul);
    value /= 10ul;
  } while (value != 0ul);
  *out_len = len;
  while (len != 0u) {
    *out++ = tmp[--len];
  }
}

static int lql_number_compare_digits(const char *left, size_t left_len,
                                     const char *right, size_t right_len) {
  int cmp;
  if (left_len < right_len) {
    return -1;
  }
  if (left_len > right_len) {
    return 1;
  }
  cmp = left_len == 0u ? 0 : memcmp(left, right, left_len);
  return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
}

static int lql_number_plain_integer(const char *text, size_t len,
                                    const char **digits, size_t *digits_len,
                                    int *negative, int *zero) {
  size_t pos;
  if (text == NULL || len == 0u || digits == NULL || digits_len == NULL ||
      negative == NULL || zero == NULL) {
    return 0;
  }
  pos = 0u;
  *negative = 0;
  if (text[pos] == '-') {
    *negative = 1;
    ++pos;
    if (pos == len) {
      return 0;
    }
  }
  *digits = text + pos;
  if (text[pos] == '0') {
    ++pos;
    if (pos != len) {
      return 0;
    }
    *digits_len = 1u;
    *negative = 0;
    *zero = 1;
    return 1;
  }
  if (text[pos] < '1' || text[pos] > '9') {
    return 0;
  }
  do {
    ++pos;
  } while (pos < len && text[pos] >= '0' && text[pos] <= '9');
  if (pos != len) {
    return 0;
  }
  *digits_len = (size_t)((text + pos) - *digits);
  *zero = 0;
  return 1;
}

static int lql_number_compare_plain_integer(const char *left, size_t left_len,
                                            const char *right,
                                            size_t right_len, int *out) {
  const char *left_digits;
  const char *right_digits;
  size_t left_digits_len;
  size_t right_digits_len;
  int left_negative;
  int right_negative;
  int left_zero;
  int right_zero;
  int cmp;
  if (!lql_number_plain_integer(left, left_len, &left_digits, &left_digits_len,
                                &left_negative, &left_zero) ||
      !lql_number_plain_integer(right, right_len, &right_digits,
                                &right_digits_len, &right_negative,
                                &right_zero)) {
    return 0;
  }
  if (left_zero && right_zero) {
    *out = 0;
    return 1;
  }
  if (left_zero != right_zero) {
    *out = left_zero ? (right_negative ? 1 : -1)
                     : (left_negative ? -1 : 1);
    return 1;
  }
  if (left_negative != right_negative) {
    *out = left_negative ? -1 : 1;
    return 1;
  }
  cmp = lql_number_compare_digits(left_digits, left_digits_len, right_digits,
                                  right_digits_len);
  *out = left_negative ? -cmp : cmp;
  return 1;
}

static char *lql_number_add_digits(lql_allocator *allocator, const char *left,
                                   size_t left_len,
                                   const char *right, size_t right_len,
                                   int negative) {
  size_t max_len;
  size_t out_pos;
  size_t li;
  size_t ri;
  int carry;
  char *out;
  char *digits;
  max_len = left_len > right_len ? left_len : right_len;
  out = (char *)allocator->alloc(allocator, max_len + 3u);
  if (out == NULL) {
    return NULL;
  }
  digits = out + (negative ? 1u : 0u);
  out_pos = max_len + 1u;
  digits[out_pos] = '\0';
  li = left_len;
  ri = right_len;
  carry = 0;
  while (out_pos != 0u) {
    int sum;
    sum = carry;
    if (li != 0u) {
      sum += left[--li] - '0';
    }
    if (ri != 0u) {
      sum += right[--ri] - '0';
    }
    digits[--out_pos] = (char)('0' + sum % 10);
    carry = sum / 10;
  }
  while (digits[0] == '0' && digits[1] != '\0') {
    ++digits;
  }
  if (negative) {
    *--digits = '-';
  }
  if (digits != out) {
    memmove(out, digits, strlen(digits) + 1u);
  }
  return out;
}

static char *lql_number_sub_digits(lql_allocator *allocator, const char *left,
                                   size_t left_len,
                                   const char *right, size_t right_len,
                                   int negative) {
  size_t out_pos;
  size_t li;
  size_t ri;
  int borrow;
  char *out;
  char *digits;
  out = (char *)allocator->alloc(allocator, left_len + 2u);
  if (out == NULL) {
    return NULL;
  }
  digits = out + (negative ? 1u : 0u);
  digits[left_len] = '\0';
  out_pos = left_len;
  li = left_len;
  ri = right_len;
  borrow = 0;
  while (out_pos != 0u) {
    int diff;
    diff = left[--li] - '0' - borrow;
    if (ri != 0u) {
      diff -= right[--ri] - '0';
    }
    if (diff < 0) {
      diff += 10;
      borrow = 1;
    } else {
      borrow = 0;
    }
    digits[--out_pos] = (char)('0' + diff);
  }
  while (digits[0] == '0' && digits[1] != '\0') {
    ++digits;
  }
  if (digits[0] == '0' && digits[1] == '\0') {
    negative = 0;
  }
  if (negative) {
    *--digits = '-';
  }
  if (digits != out) {
    memmove(out, digits, strlen(digits) + 1u);
  }
  return out;
}

static char *lql_number_adjusted_exponent(lql_allocator *allocator,
                                          const lql_number_decimal *number) {
  char small_digits[32];
  const char *base_digits;
  const char *right_digits;
  size_t base_len;
  size_t small_len;
  unsigned long small_mag;
  int base_negative;
  int small_negative;
  long small;
  int cmp;
  small = number->decimal_adjust;
  if (small < 0) {
    small_mag = (unsigned long)(-(small + 1)) + 1ul;
    small_negative = 1;
  } else {
    small_mag = (unsigned long)small;
    small_negative = 0;
  }
  lql_number_ulong_text(small_mag, small_digits, &small_len);
  base_digits = number->exponent;
  base_len = number->exponent_len;
  base_negative = number->exponent_negative;
  if (base_len == 0u) {
    base_digits = "0";
    base_len = 1u;
    base_negative = 0;
  }
  if (small_mag == 0ul) {
    small_len = 1u;
    small_digits[0] = '0';
  }
  if (base_len == 1u && base_digits[0] == '0') {
    return lql_number_add_digits(allocator, "0", 1u, small_digits, small_len,
                                 small_negative);
  }
  if (small_mag == 0ul || base_negative == small_negative) {
    return lql_number_add_digits(allocator, base_digits, base_len, small_digits,
                                 small_len, base_negative);
  }
  right_digits = small_digits;
  cmp = lql_number_compare_digits(base_digits, base_len, right_digits, small_len);
  if (cmp == 0) {
    return lql_number_sub_digits(allocator, "0", 1u, "0", 1u, 0);
  }
  if (cmp > 0) {
    return lql_number_sub_digits(allocator, base_digits, base_len, right_digits,
                                 small_len, base_negative);
  }
  return lql_number_sub_digits(allocator, right_digits, small_len, base_digits,
                               base_len, small_negative);
}

static int lql_number_compare_signed_decimal(const char *left,
                                             const char *right) {
  int left_negative;
  int right_negative;
  const char *left_digits;
  const char *right_digits;
  size_t left_len;
  size_t right_len;
  int cmp;
  left_negative = left[0] == '-';
  right_negative = right[0] == '-';
  left_digits = left_negative ? left + 1 : left;
  right_digits = right_negative ? right + 1 : right;
  left_len = strlen(left_digits);
  right_len = strlen(right_digits);
  if (left_negative != right_negative) {
    return left_negative ? -1 : 1;
  }
  cmp = lql_number_compare_digits(left_digits, left_len, right_digits, right_len);
  return left_negative ? -cmp : cmp;
}

static int lql_number_decimal_parse(const char *text, size_t len,
                                    lql_number_decimal *out) {
  size_t pos;
  size_t total_digits;
  size_t first_nonzero;
  size_t last_nonzero;
  int exponent_negative;
  const char *exponent;
  size_t exponent_len;
  long coeff_len;
  long fraction_len;
  long trailing_zeroes;
  if (text == NULL || len == 0u || out == NULL) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  pos = 0u;
  if (text[pos] == '-') {
    out->negative = 1;
    ++pos;
    if (pos == len) {
      return 0;
    }
  }
  out->integer = text + pos;
  if (text[pos] == '0') {
    ++pos;
    out->integer_len = 1u;
    if (pos < len && text[pos] >= '0' && text[pos] <= '9') {
      return 0;
    }
  } else if (text[pos] >= '1' && text[pos] <= '9') {
    do {
      ++pos;
    } while (pos < len && text[pos] >= '0' && text[pos] <= '9');
    out->integer_len = (size_t)((text + pos) - out->integer);
  } else {
    return 0;
  }
  if (pos < len && text[pos] == '.') {
    ++pos;
    out->fraction = text + pos;
    if (pos == len || text[pos] < '0' || text[pos] > '9') {
      return 0;
    }
    do {
      ++pos;
    } while (pos < len && text[pos] >= '0' && text[pos] <= '9');
    out->fraction_len = (size_t)((text + pos) - out->fraction);
  } else {
    out->fraction = text + pos;
  }
  if (!lql_number_parse_exponent(text, len, &pos, &exponent_negative,
                                 &exponent, &exponent_len) ||
      pos != len) {
    return 0;
  }
  total_digits = out->integer_len + out->fraction_len;
  first_nonzero = 0u;
  while (first_nonzero < total_digits &&
         lql_number_digit_at(out, first_nonzero) == '0') {
    ++first_nonzero;
  }
  if (first_nonzero == total_digits) {
    out->zero = 1;
    out->negative = 0;
    out->coeff_len = 0u;
    out->exponent = text + len;
    out->exponent_len = 0u;
    out->exponent_negative = 0;
    out->decimal_adjust = 0;
    return 1;
  }
  last_nonzero = total_digits - 1u;
  while (last_nonzero > first_nonzero &&
         lql_number_digit_at(out, last_nonzero) == '0') {
    --last_nonzero;
  }
  out->leading_skip = first_nonzero;
  out->coeff_len = last_nonzero - first_nonzero + 1u;
  out->exponent = exponent;
  out->exponent_len = exponent_len;
  out->exponent_negative = exponent_negative;
  lql_number_size_to_long(out->coeff_len, &coeff_len);
  lql_number_size_to_long(out->fraction_len, &fraction_len);
  lql_number_size_to_long(total_digits - 1u - last_nonzero, &trailing_zeroes);
  out->decimal_adjust =
      lql_number_clamp_add(lql_number_clamp_add(coeff_len, -fraction_len),
                           trailing_zeroes);
  return 1;
}

static int lql_number_compare_abs(lql_allocator *allocator,
                                  const lql_number_decimal *left,
                                  const lql_number_decimal *right, int *out) {
  char *left_adjusted;
  char *right_adjusted;
  int adjusted_cmp;
  size_t i;
  size_t max_digits;
  left_adjusted = lql_number_adjusted_exponent(allocator, left);
  right_adjusted = lql_number_adjusted_exponent(allocator, right);
  if (left_adjusted == NULL || right_adjusted == NULL) {
    allocator->destroy(allocator, left_adjusted);
    allocator->destroy(allocator, right_adjusted);
    return 0;
  }
  adjusted_cmp =
      lql_number_compare_signed_decimal(left_adjusted, right_adjusted);
  allocator->destroy(allocator, left_adjusted);
  allocator->destroy(allocator, right_adjusted);
  if (adjusted_cmp < 0) {
    *out = -1;
    return 1;
  }
  if (adjusted_cmp > 0) {
    *out = 1;
    return 1;
  }
  max_digits = left->coeff_len > right->coeff_len ? left->coeff_len
                                                  : right->coeff_len;
  for (i = 0u; i < max_digits; ++i) {
    char a;
    char b;
    a = i < left->coeff_len ? lql_number_digit_at(left, i) : '0';
    b = i < right->coeff_len ? lql_number_digit_at(right, i) : '0';
    if (a < b) {
      *out = -1;
      return 1;
    }
    if (a > b) {
      *out = 1;
      return 1;
    }
  }
  *out = 0;
  return 1;
}

LQL_INTERNAL_SYMBOL int lql_number_compare_json(lql_allocator *allocator,
                                                const char *left,
                                                size_t left_len,
                                                const char *right,
                                                size_t right_len, int *out) {
  lql_number_decimal a;
  lql_number_decimal b;
  int cmp;
  if (allocator == NULL) {
    allocator = lql_allocator_default();
  }
  if (out == NULL || allocator == NULL) {
    return 0;
  }
  if (lql_number_compare_plain_integer(left, left_len, right, right_len, out)) {
    return 1;
  }
  if (!lql_number_decimal_parse(left, left_len, &a) ||
      !lql_number_decimal_parse(right, right_len, &b)) {
    return 0;
  }
  if (a.zero && b.zero) {
    *out = 0;
    return 1;
  }
  if (a.zero != b.zero) {
    *out = a.zero ? (b.negative ? 1 : -1) : (a.negative ? -1 : 1);
    return 1;
  }
  if (a.negative != b.negative) {
    *out = a.negative ? -1 : 1;
    return 1;
  }
  if (!lql_number_compare_abs(allocator, &a, &b, &cmp)) {
    return 0;
  }
  *out = a.negative ? -cmp : cmp;
  return 1;
}

LQL_INTERNAL_SYMBOL int lql_number_is_json(const char *text, size_t len) {
  lql_number_decimal number;
  return lql_number_decimal_parse(text, len, &number);
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
