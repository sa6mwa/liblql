#include "lql_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int parse_ndigits(const char **p, int n, int *out) {
  int i;
  int v;
  v = 0;
  for (i = 0; i < n; ++i) {
    if (!isdigit((unsigned char)(*p)[i])) {
      return 0;
    }
    v = (v * 10) + ((*p)[i] - '0');
  }
  *p += n;
  *out = v;
  return 1;
}

static int is_leap(int y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int valid_ymd(int y, int m, int d) {
  static const int days[] = {31, 28, 31, 30, 31, 30,
                             31, 31, 30, 31, 30, 31};
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

int lql_parse_temporal_literal(const char *raw, lql_temporal *out) {
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
  while (isspace((unsigned char)*raw)) {
    ++raw;
  }
  p = raw;
  if (!parse_ndigits(&p, 4, &y) || *p++ != '-' ||
      !parse_ndigits(&p, 2, &mo) || *p++ != '-' ||
      !parse_ndigits(&p, 2, &d) || !valid_ymd(y, mo, d)) {
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
  if (*p == '\0') {
    out->seconds = days_from_civil(y, mo, d) * (lql_int64)86400;
    out->nanoseconds = 0;
    return 1;
  }
  if (*p != 'T') {
    return 0;
  }
  ++p;
  if (!parse_ndigits(&p, 2, &h) || *p++ != ':' ||
      !parse_ndigits(&p, 2, &mi) || *p++ != ':' ||
      !parse_ndigits(&p, 2, &s) || h > 23 || mi > 59 || s > 60) {
    return 0;
  }
  if (*p == '.') {
    ++p;
    if (!isdigit((unsigned char)*p)) {
      return 0;
    }
    frac_digits = 0;
    while (isdigit((unsigned char)*p)) {
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

int lql_temporal_compare(const lql_temporal *left,
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

int lql_temporal_equal(const lql_temporal *left, const lql_temporal *right) {
  if (left->date_only || right->date_only) {
    return left->year == right->year && left->month == right->month &&
           left->day == right->day;
  }
  return lql_temporal_compare(left, right) == 0;
}
