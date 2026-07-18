#include "lql_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int digit_value(unsigned char c) {
  return c >= (unsigned char)'0' && c <= (unsigned char)'9'
             ? (int)(c - (unsigned char)'0')
             : -1;
}

static int parse_ndigits(const char **p, int n, int *out) {
  int i;
  int v;
  int digit;
  const char *q;
  v = 0;
  q = *p;
  for (i = 0; i < n; ++i) {
    if (*q == '\0') {
      return 0;
    }
    digit = digit_value((unsigned char)*q);
    if (digit < 0) {
      return 0;
    }
    v = (v * 10) + digit;
    ++q;
  }
  *p = q;
  *out = v;
  return 1;
}

static int parse_2_at(const char *p, int *out) {
  int hi;
  int lo;
  hi = digit_value((unsigned char)p[0]);
  lo = digit_value((unsigned char)p[1]);
  if (hi < 0 || lo < 0) {
    return 0;
  }
  *out = hi * 10 + lo;
  return 1;
}

static int parse_4_at(const char *p, int *out) {
  int a;
  int b;
  int c;
  int d;
  a = digit_value((unsigned char)p[0]);
  b = digit_value((unsigned char)p[1]);
  c = digit_value((unsigned char)p[2]);
  d = digit_value((unsigned char)p[3]);
  if (a < 0 || b < 0 || c < 0 || d < 0) {
    return 0;
  }
  *out = ((a * 10 + b) * 10 + c) * 10 + d;
  return 1;
}

static int is_leap(int y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int valid_ymd(int y, int m, int d) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int max_day;
  if (m < 1 || m > 12 || d < 1) {
    return 0;
  }
  max_day = days[m - 1];
  if (m == 2 && is_leap(y)) {
    max_day = 29;
  }
  return d <= max_day;
}

static lql_int64 days_from_civil(int y, int m, int d) {
  lql_int64 era;
  unsigned yoe;
  unsigned doy;
  unsigned doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * (lql_int64)146097 + (lql_int64)doe - (lql_int64)719468;
}

static int civil_from_days(lql_int64 z, int *out_y, int *out_m, int *out_d) {
  lql_int64 era;
  unsigned doe;
  unsigned yoe;
  lql_int64 y;
  unsigned doy;
  unsigned mp;
  unsigned d;
  unsigned m;
  z += (lql_int64)719468;
  era = (z >= 0 ? z : z - (lql_int64)146096) / (lql_int64)146097;
  doe = (unsigned)(z - era * (lql_int64)146097);
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : (unsigned)-9);
  y = (lql_int64)yoe + era * (lql_int64)400;
  y += m <= 2 ? 1 : 0;
  if (y < 0 || y > 9999) {
    return 0;
  }
  *out_y = (int)y;
  *out_m = (int)m;
  *out_d = (int)d;
  return 1;
}

static int temporal_from_seconds(lql_int64 seconds, int date_only,
                                 lql_temporal *out) {
  lql_int64 days;
  lql_int64 rem;
  days = seconds / (lql_int64)86400;
  rem = seconds % (lql_int64)86400;
  if (rem < 0) {
    rem += (lql_int64)86400;
    --days;
  }
  if (!civil_from_days(days, &out->year, &out->month, &out->day)) {
    return 0;
  }
  out->seconds = date_only ? days * (lql_int64)86400 : seconds;
  out->nanoseconds = 0;
  out->date_only = date_only;
  return 1;
}

static int parse_temporal_fast(const char *raw, lql_temporal *out) {
  const char *p;
  size_t len;
  int y;
  int mo;
  int d;
  int h;
  int mi;
  int s;
  int off_h;
  int off_m;
  int off_sign;
  int offset;
  int nanos;
  int frac_digits;
  int digit;

  len = strlen(raw);
  if (len != 10u && len < 19u) {
    return 0;
  }
  if (!parse_4_at(raw, &y) || raw[4] != '-' || !parse_2_at(raw + 5, &mo) ||
      raw[7] != '-' || !parse_2_at(raw + 8, &d) || !valid_ymd(y, mo, d)) {
    return 0;
  }
  if (raw[10] == '\0') {
    out->year = y;
    out->month = mo;
    out->day = d;
    out->date_only = 1;
    out->seconds = days_from_civil(y, mo, d) * (lql_int64)86400;
    out->nanoseconds = 0;
    return 1;
  }
  if (raw[10] != 'T' || !parse_2_at(raw + 11, &h) || raw[13] != ':' ||
      !parse_2_at(raw + 14, &mi) || raw[16] != ':' ||
      !parse_2_at(raw + 17, &s) || h > 23 || mi > 59 || s > 59) {
    return 0;
  }
  p = raw + 19;
  offset = 0;
  nanos = 0;
  if (*p == '.') {
    ++p;
    digit = digit_value((unsigned char)*p);
    if (digit < 0) {
      return 0;
    }
    frac_digits = 0;
    while ((digit = digit_value((unsigned char)*p)) >= 0) {
      if (frac_digits < 9) {
        nanos = (nanos * 10) + digit;
      }
      ++p;
      ++frac_digits;
    }
    while (frac_digits < 9) {
      nanos *= 10;
      ++frac_digits;
    }
  }
  if (*p == 'Z') {
    ++p;
  } else if (*p == '+' || *p == '-') {
    off_sign = *p == '-' ? -1 : 1;
    ++p;
    if (strlen(p) < 5u || !parse_2_at(p, &off_h) || p[2] != ':' ||
        !parse_2_at(p + 3, &off_m) || off_h > 23 || off_m > 59) {
      return 0;
    }
    p += 5;
    offset = off_sign * (off_h * 3600 + off_m * 60);
  }
  if (*p != '\0') {
    return 0;
  }
  out->year = y;
  out->month = mo;
  out->day = d;
  out->date_only = 0;
  out->seconds = days_from_civil(y, mo, d) * (lql_int64)86400 +
                 (lql_int64)(h * 3600 + mi * 60 + s - offset);
  out->nanoseconds = nanos;
  return 1;
}

LQL_INTERNAL_SYMBOL int lql_parse_temporal_literal(const char *raw,
                                                   lql_temporal *out) {
  const char *p;
  int y;
  int mo;
  int d;
  int h;
  int mi;
  int s;
  int off_sign;
  int off_h;
  int off_m;
  int offset;
  int nanos;
  int frac_digits;

  if (raw == NULL || out == NULL) {
    return 0;
  }
  if (parse_temporal_fast(raw, out)) {
    return 1;
  }
  while (isspace((unsigned char)*raw)) {
    ++raw;
  }
  p = raw;
  if (!parse_ndigits(&p, 4, &y) || *p++ != '-' || !parse_ndigits(&p, 2, &mo) ||
      *p++ != '-' || !parse_ndigits(&p, 2, &d) || !valid_ymd(y, mo, d)) {
    return 0;
  }
  out->year = y;
  out->month = mo;
  out->day = d;
  out->date_only = 1;
  h = 0;
  mi = 0;
  s = 0;
  offset = 0;
  nanos = 0;
  if (*p != '\0' && isspace((unsigned char)*p)) {
    do {
      ++p;
    } while (isspace((unsigned char)*p));
    if (*p != '\0') {
      return 0;
    }
  }
  if (*p == '\0') {
    out->seconds = days_from_civil(y, mo, d) * (lql_int64)86400;
    out->nanoseconds = 0;
    return 1;
  }
  if (*p != 'T') {
    return 0;
  }
  ++p;
  if (!parse_ndigits(&p, 2, &h) || *p++ != ':' || !parse_ndigits(&p, 2, &mi) ||
      *p++ != ':' || !parse_ndigits(&p, 2, &s) || h > 23 || mi > 59 || s > 59) {
    return 0;
  }
  if (*p == '.') {
    ++p;
    if (digit_value((unsigned char)*p) < 0) {
      return 0;
    }
    frac_digits = 0;
    while (digit_value((unsigned char)*p) >= 0) {
      if (frac_digits < 9) {
        nanos = (nanos * 10) + (*p - '0');
      }
      ++p;
      ++frac_digits;
    }
    while (frac_digits < 9) {
      nanos *= 10;
      ++frac_digits;
    }
  }
  if (*p == 'Z') {
    ++p;
  } else if (*p == '+' || *p == '-') {
    off_sign = *p == '-' ? -1 : 1;
    ++p;
    if (!parse_ndigits(&p, 2, &off_h) || *p++ != ':' ||
        !parse_ndigits(&p, 2, &off_m) || off_h > 23 || off_m > 59) {
      return 0;
    }
    offset = off_sign * (off_h * 3600 + off_m * 60);
  }
  while (isspace((unsigned char)*p)) {
    ++p;
  }
  if (*p != '\0') {
    return 0;
  }
  out->date_only = 0;
  out->seconds = days_from_civil(y, mo, d) * (lql_int64)86400 +
                 (lql_int64)(h * 3600 + mi * 60 + s - offset);
  out->nanoseconds = nanos;
  return 1;
}

LQL_INTERNAL_SYMBOL int lql_temporal_compare(const lql_temporal *left,
                                             const lql_temporal *right) {
  if (left->seconds < right->seconds) {
    return -1;
  }
  if (left->seconds > right->seconds) {
    return 1;
  }
  if (left->nanoseconds < right->nanoseconds) {
    return -1;
  }
  if (left->nanoseconds > right->nanoseconds) {
    return 1;
  }
  return 0;
}

LQL_INTERNAL_SYMBOL int lql_temporal_equal(const lql_temporal *left,
                                           const lql_temporal *right) {
  if (left->date_only || right->date_only) {
    return left->year == right->year && left->month == right->month &&
           left->day == right->day;
  }
  return lql_temporal_compare(left, right) == 0;
}

LQL_INTERNAL_SYMBOL int
lql_temporal_format_rfc3339_nano(const lql_temporal *value, char *buf,
                                 size_t buf_len) {
  lql_int64 days;
  lql_int64 rem;
  int y;
  int m;
  int d;
  int h;
  int mi;
  int s;
  char frac[16];
  int frac_len;
  if (value == NULL || buf == NULL || buf_len < 31u || value->date_only ||
      value->nanoseconds < 0 || value->nanoseconds > 999999999) {
    return 0;
  }
  days = value->seconds / (lql_int64)86400;
  rem = value->seconds % (lql_int64)86400;
  if (rem < 0) {
    rem += (lql_int64)86400;
    --days;
  }
  if (!civil_from_days(days, &y, &m, &d)) {
    return 0;
  }
  h = (int)(rem / (lql_int64)3600);
  rem %= (lql_int64)3600;
  mi = (int)(rem / (lql_int64)60);
  s = (int)(rem % (lql_int64)60);
  if (value->nanoseconds == 0) {
    sprintf(buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, m, d, h, mi, s);
    return 1;
  }
  sprintf(frac, "%09d", value->nanoseconds);
  frac_len = 9;
  while (frac_len > 0 && frac[frac_len - 1] == '0') {
    --frac_len;
  }
  frac[frac_len] = '\0';
  sprintf(buf, "%04d-%02d-%02dT%02d:%02d:%02d.%sZ", y, m, d, h, mi, s, frac);
  return 1;
}

LQL_INTERNAL_SYMBOL int lql_temporal_now(lql_temporal *out) {
  time_t value;
  value = time(NULL);
  if (value == (time_t)-1) {
    return 0;
  }
  return lql_temporal_from_time_t(value, 0, out);
}

LQL_INTERNAL_SYMBOL int lql_temporal_today(lql_temporal *out) {
  time_t value;
  value = time(NULL);
  if (value == (time_t)-1) {
    return 0;
  }
  return lql_temporal_from_time_t(value, 1, out);
}

LQL_INTERNAL_SYMBOL int lql_temporal_yesterday(lql_temporal *out) {
  time_t value;
  value = time(NULL);
  if (value == (time_t)-1 || out == NULL)
    return 0;
  return temporal_from_seconds((lql_int64)value - (lql_int64)86400, 1, out);
}

LQL_INTERNAL_SYMBOL int lql_temporal_from_time_t(time_t value, int date_only,
                                                 lql_temporal *out) {
  if (out == NULL)
    return 0;
  return temporal_from_seconds((lql_int64)value, date_only, out);
}

LQL_INTERNAL_SYMBOL int
lql_temporal_from_seconds(lql_int64 seconds, int date_only, lql_temporal *out) {
  if (out == NULL)
    return 0;
  return temporal_from_seconds(seconds, date_only, out);
}
