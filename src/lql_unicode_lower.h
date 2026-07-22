#ifndef LQL_UNICODE_LOWER_H
#define LQL_UNICODE_LOWER_H

#include <stddef.h>

/*
 * Unicode 15.0 simple lower-case mapping, matching Go 1.26's unicode.ToLower.
 * This is private scanner support; it has no locale dependency.
 */
unsigned long lql_unicode_simple_lower(unsigned long value);

/* Writes the UTF-8 simple-lowercase form and rejects malformed UTF-8. */
int lql_unicode_utf8_lower(const char *input, size_t input_len, char *output,
                           size_t output_capacity, size_t *out_len);
int lql_unicode_utf8_decode_one(const unsigned char *input, size_t input_len,
                                unsigned long *out);
size_t lql_unicode_utf8_encode(unsigned long value, unsigned char output[4]);

#endif
