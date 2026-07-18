#ifndef LQL_TEMPORAL_INTERNAL_H
#define LQL_TEMPORAL_INTERNAL_H

#include <stddef.h>
#include <time.h>

#if defined(__GNUC__)
#define LQL_TEMPORAL_INTERNAL_SYMBOL __attribute__((visibility("hidden")))
#else
#define LQL_TEMPORAL_INTERNAL_SYMBOL
#endif

__extension__ typedef signed long long lql_int64;

#define LQL_INT64_MAX_VALUE __extension__ 9223372036854775807LL
#define LQL_INT64_MIN_VALUE (-LQL_INT64_MAX_VALUE - __extension__ 1LL)

typedef struct lql_temporal {
  lql_int64 seconds;
  int nanoseconds;
  int year;
  int month;
  int day;
  int date_only;
} lql_temporal;

LQL_TEMPORAL_INTERNAL_SYMBOL int lql_parse_temporal_literal(const char *raw,
                                                            lql_temporal *out);
LQL_TEMPORAL_INTERNAL_SYMBOL int
lql_temporal_compare(const lql_temporal *left, const lql_temporal *right);
LQL_TEMPORAL_INTERNAL_SYMBOL int lql_temporal_equal(const lql_temporal *left,
                                                    const lql_temporal *right);
LQL_TEMPORAL_INTERNAL_SYMBOL int
lql_temporal_format_rfc3339_nano(const lql_temporal *value, char *buf,
                                 size_t buf_len);
LQL_TEMPORAL_INTERNAL_SYMBOL int lql_temporal_now(lql_temporal *out);
LQL_TEMPORAL_INTERNAL_SYMBOL int lql_temporal_today(lql_temporal *out);
LQL_TEMPORAL_INTERNAL_SYMBOL int lql_temporal_yesterday(lql_temporal *out);
LQL_TEMPORAL_INTERNAL_SYMBOL int
lql_temporal_from_time_t(time_t value, int date_only, lql_temporal *out);
LQL_TEMPORAL_INTERNAL_SYMBOL int
lql_temporal_from_seconds(lql_int64 seconds, int date_only, lql_temporal *out);

#endif
