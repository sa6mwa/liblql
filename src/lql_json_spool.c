#include "lql_json_spool.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define LQL_JSON_SPOOL_IO_BUFFER_BYTES 32768u

static void lql_json_spool_error(lql_error *error, lql_status status,
                                 const char *message) {
  if (error != NULL) {
    error->code = status;
    strncpy(error->message, message, sizeof(error->message) - 1u);
    error->message[sizeof(error->message) - 1u] = '\0';
  }
}

lql_status lql_json_spool_init(lql_json_spool *spool, lql_error *error) {
  if (spool == NULL) {
    lql_json_spool_error(error, LQL_STATUS_INVALID_ARGUMENT,
                         "JSON spool is required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  memset(spool, 0, sizeof(*spool));
  spool->memory = (unsigned char *)malloc(LQL_JSON_SPOOL_MEMORY_BYTES);
  if (spool->memory == NULL) {
    lql_json_spool_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  return LQL_STATUS_OK;
}

void lql_json_spool_reset(lql_json_spool *spool) {
  if (spool == NULL) {
    return;
  }
  if (spool->file != NULL) {
    fclose(spool->file);
    spool->file = NULL;
  }
  spool->memory_len = 0u;
  spool->size = 0u;
  spool->read_cache_offset = 0u;
  spool->read_cache_len = 0u;
}

void lql_json_spool_cleanup(lql_json_spool *spool) {
  if (spool == NULL) {
    return;
  }
  lql_json_spool_reset(spool);
  free(spool->memory);
  spool->memory = NULL;
}

static lql_status lql_json_spool_spill(lql_json_spool *spool,
                                       lql_error *error) {
  FILE *file;
  file = tmpfile();
  if (file == NULL) {
    lql_json_spool_error(error, LQL_STATUS_IO_ERROR,
                         "unable to create JSON spool file");
    return LQL_STATUS_IO_ERROR;
  }
  if (spool->memory_len != 0u &&
      fwrite(spool->memory, 1u, spool->memory_len, file) != spool->memory_len) {
    fclose(file);
    lql_json_spool_error(error, LQL_STATUS_IO_ERROR,
                         "unable to write JSON spool file");
    return LQL_STATUS_IO_ERROR;
  }
  spool->file = file;
  return LQL_STATUS_OK;
}

lql_status lql_json_spool_append(lql_json_spool *spool, const void *data,
                                 size_t len, lql_error *error) {
  const unsigned char *bytes;
  lql_status status;
  if (spool == NULL || spool->memory == NULL || (data == NULL && len != 0u)) {
    lql_json_spool_error(error, LQL_STATUS_INVALID_ARGUMENT,
                         "JSON spool and data are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (len > (size_t)-1 - spool->size) {
    lql_json_spool_error(error, LQL_STATUS_NO_MEMORY,
                         "JSON spool is too large");
    return LQL_STATUS_NO_MEMORY;
  }
  bytes = (const unsigned char *)data;
  if (spool->file == NULL &&
      len <= LQL_JSON_SPOOL_MEMORY_BYTES - spool->memory_len) {
    if (len != 0u) {
      memcpy(spool->memory + spool->memory_len, bytes, len);
      spool->memory_len += len;
    }
  } else {
    if (spool->file == NULL) {
      status = lql_json_spool_spill(spool, error);
      if (status != LQL_STATUS_OK) {
        return status;
      }
    }
    spool->read_cache_len = 0u;
    if (len != 0u && fwrite(bytes, 1u, len, spool->file) != len) {
      lql_json_spool_error(error, LQL_STATUS_IO_ERROR,
                           "unable to write JSON spool file");
      return LQL_STATUS_IO_ERROR;
    }
  }
  spool->size += len;
  return LQL_STATUS_OK;
}

size_t lql_json_spool_size(const lql_json_spool *spool) {
  return spool == NULL ? 0u : spool->size;
}

lql_status lql_json_spool_write_to(const lql_json_spool *spool,
                                   lql_stream_writer_fn writer,
                                   void *writer_user, lql_error *error) {
  unsigned char buffer[LQL_JSON_SPOOL_IO_BUFFER_BYTES];
  size_t amount;
  lql_status status;
  if (spool == NULL || spool->memory == NULL || writer == NULL) {
    lql_json_spool_error(error, LQL_STATUS_INVALID_ARGUMENT,
                         "JSON spool and writer are required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (spool->file == NULL) {
    return writer(writer_user, spool->memory, spool->memory_len, error);
  }
  if (fflush(spool->file) != 0 || fseek(spool->file, 0L, SEEK_SET) != 0) {
    lql_json_spool_error(error, LQL_STATUS_IO_ERROR,
                         "unable to read JSON spool file");
    return LQL_STATUS_IO_ERROR;
  }
  while ((amount = fread(buffer, 1u, sizeof(buffer), spool->file)) != 0u) {
    status = writer(writer_user, buffer, amount, error);
    if (status != LQL_STATUS_OK) {
      return status;
    }
  }
  if (ferror(spool->file)) {
    lql_json_spool_error(error, LQL_STATUS_IO_ERROR,
                         "unable to read JSON spool file");
    return LQL_STATUS_IO_ERROR;
  }
  return LQL_STATUS_OK;
}

lql_status lql_json_spool_write_slice(const lql_json_spool *spool,
                                      size_t offset, size_t len,
                                      lql_stream_writer_fn writer,
                                      void *writer_user, lql_error *error) {
  unsigned char buffer[LQL_JSON_SPOOL_IO_BUFFER_BYTES];
  lql_status status;
  if (spool == NULL || spool->memory == NULL || writer == NULL ||
      offset > spool->size || len > spool->size - offset) {
    lql_json_spool_error(error, LQL_STATUS_INVALID_ARGUMENT,
                         "JSON spool slice arguments are invalid");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (spool->file == NULL) {
    return writer(writer_user, spool->memory + offset, len, error);
  }
  if (fflush(spool->file) != 0 || offset > (size_t)LONG_MAX ||
      fseek(spool->file, (long)offset, SEEK_SET) != 0) {
    lql_json_spool_error(error, LQL_STATUS_IO_ERROR,
                         "unable to seek JSON spool slice");
    return LQL_STATUS_IO_ERROR;
  }
  while (len != 0u) {
    size_t amount;
    amount = len > sizeof(buffer) ? sizeof(buffer) : len;
    if (fread(buffer, 1u, amount, spool->file) != amount) {
      lql_json_spool_error(error, LQL_STATUS_IO_ERROR,
                           "unable to read JSON spool slice");
      return LQL_STATUS_IO_ERROR;
    }
    status = writer(writer_user, buffer, amount, error);
    if (status != LQL_STATUS_OK)
      return status;
    len -= amount;
  }
  return LQL_STATUS_OK;
}

lql_status lql_json_spool_byte_at(const lql_json_spool *spool, size_t offset,
                                  unsigned char *out, lql_error *error) {
  lql_json_spool *mutable_spool;
  size_t amount;
  if (spool == NULL || spool->memory == NULL || out == NULL ||
      offset >= spool->size) {
    lql_json_spool_error(error, LQL_STATUS_INVALID_ARGUMENT,
                         "JSON spool byte arguments are invalid");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (spool->file == NULL) {
    *out = spool->memory[offset];
    return LQL_STATUS_OK;
  }
  mutable_spool = (lql_json_spool *)spool;
  if (offset >= mutable_spool->read_cache_offset &&
      offset < mutable_spool->read_cache_offset +
                   mutable_spool->read_cache_len) {
    *out = mutable_spool
               ->read_cache[offset - mutable_spool->read_cache_offset];
    return LQL_STATUS_OK;
  }
  amount = spool->size - offset;
  if (amount > sizeof(mutable_spool->read_cache))
    amount = sizeof(mutable_spool->read_cache);
  if (fflush(spool->file) != 0 || offset > (size_t)LONG_MAX ||
      fseek(spool->file, (long)offset, SEEK_SET) != 0 ||
      fread(mutable_spool->read_cache, 1u, amount, spool->file) != amount) {
    mutable_spool->read_cache_len = 0u;
    lql_json_spool_error(error, LQL_STATUS_IO_ERROR,
                         "unable to read JSON spool byte");
    return LQL_STATUS_IO_ERROR;
  }
  mutable_spool->read_cache_offset = offset;
  mutable_spool->read_cache_len = amount;
  *out = mutable_spool->read_cache[0];
  return LQL_STATUS_OK;
}

static int lql_json_spool_string_chunk_end(const unsigned char *data,
                                           size_t len, int *escaped,
                                           size_t *out) {
  size_t offset;
  if (data == NULL || escaped == NULL || out == NULL) {
    return 0;
  }
  offset = 0u;
  while (offset < len) {
    const unsigned char *quote;
    const unsigned char *slash;
    size_t remaining;
    if (*escaped) {
      *escaped = 0;
      ++offset;
      continue;
    }
    remaining = len - offset;
    quote = (const unsigned char *)memchr(data + offset, '"', remaining);
    slash = (const unsigned char *)memchr(data + offset, '\\', remaining);
    if (quote == NULL && slash == NULL) {
      return 0;
    }
    if (slash != NULL && (quote == NULL || slash < quote)) {
      offset = (size_t)(slash - data) + 1u;
      *escaped = 1;
      continue;
    }
    *out = (size_t)(quote - data) + 1u;
    return 1;
  }
  return 0;
}

lql_status lql_json_spool_find_string_end(const lql_json_spool *spool,
                                          size_t offset, size_t end,
                                          size_t *out, lql_error *error) {
  lql_json_spool *mutable_spool;
  unsigned char ch;
  size_t pos;
  int escaped;
  if (spool == NULL || out == NULL || offset >= end || end > spool->size) {
    lql_json_spool_error(error, LQL_STATUS_INVALID_ARGUMENT,
                         "JSON spool string scan arguments are invalid");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (lql_json_spool_byte_at(spool, offset, &ch, error) != LQL_STATUS_OK)
    return error == NULL ? LQL_STATUS_IO_ERROR : error->code;
  if (ch != (unsigned char)'"') {
    lql_json_spool_error(error, LQL_STATUS_JSON_ERROR,
                         "JSON spool string scan expected quote");
    return LQL_STATUS_JSON_ERROR;
  }
  pos = offset + 1u;
  escaped = 0;
  if (spool->file == NULL) {
    while (pos < end) {
      size_t found;
      if (lql_json_spool_string_chunk_end(spool->memory + pos, end - pos,
                                          &escaped, &found)) {
        *out = pos + found;
        return LQL_STATUS_OK;
      }
      pos = end;
    }
    lql_json_spool_error(error, LQL_STATUS_JSON_ERROR,
                         "unterminated JSON spool string");
    return LQL_STATUS_JSON_ERROR;
  }
  mutable_spool = (lql_json_spool *)spool;
  if (fflush(spool->file) != 0 || pos > (size_t)LONG_MAX ||
      fseek(spool->file, (long)pos, SEEK_SET) != 0) {
    mutable_spool->read_cache_len = 0u;
    lql_json_spool_error(error, LQL_STATUS_IO_ERROR,
                         "unable to scan JSON spool string");
    return LQL_STATUS_IO_ERROR;
  }
  while (pos < end) {
    size_t amount;
    size_t i;
    amount = end - pos;
    if (amount > sizeof(mutable_spool->read_cache))
      amount = sizeof(mutable_spool->read_cache);
    if (fread(mutable_spool->read_cache, 1u, amount, spool->file) != amount) {
      mutable_spool->read_cache_len = 0u;
      lql_json_spool_error(error, LQL_STATUS_IO_ERROR,
                           "unable to scan JSON spool string");
      return LQL_STATUS_IO_ERROR;
    }
    mutable_spool->read_cache_offset = pos;
    mutable_spool->read_cache_len = amount;
    if (lql_json_spool_string_chunk_end(mutable_spool->read_cache, amount,
                                        &escaped, &i)) {
      *out = pos + i;
      return LQL_STATUS_OK;
    }
    pos += amount;
  }
  lql_json_spool_error(error, LQL_STATUS_JSON_ERROR,
                       "unterminated JSON spool string");
  return LQL_STATUS_JSON_ERROR;
}

void lql_json_spool_reader_init(lql_json_spool_reader *reader,
                                const lql_json_spool *spool) {
  if (reader == NULL)
    return;
  reader->spool = spool;
  reader->offset = 0u;
  if (spool != NULL && spool->file != NULL) {
    fflush(spool->file);
    fseek(spool->file, 0L, SEEK_SET);
  }
}

lql_status lql_json_spool_read(void *user, unsigned char *buffer,
                               size_t capacity, size_t *out_len,
                               lql_error *error) {
  lql_json_spool_reader *reader;
  const lql_json_spool *spool;
  size_t amount;
  if (out_len != NULL)
    *out_len = 0u;
  reader = (lql_json_spool_reader *)user;
  if (reader == NULL || buffer == NULL || out_len == NULL || capacity == 0u ||
      reader->spool == NULL || reader->spool->memory == NULL) {
    lql_json_spool_error(error, LQL_STATUS_INVALID_ARGUMENT,
                         "JSON spool reader arguments are invalid");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  spool = reader->spool;
  if (spool->file == NULL) {
    amount = spool->memory_len - reader->offset;
    if (amount > capacity)
      amount = capacity;
    if (amount != 0u)
      memcpy(buffer, spool->memory + reader->offset, amount);
    reader->offset += amount;
    *out_len = amount;
    return LQL_STATUS_OK;
  }
  amount = fread(buffer, 1u, capacity, spool->file);
  reader->offset += amount;
  *out_len = amount;
  if (amount == 0u && ferror(spool->file)) {
    lql_json_spool_error(error, LQL_STATUS_IO_ERROR,
                         "unable to read JSON spool file");
    return LQL_STATUS_IO_ERROR;
  }
  return LQL_STATUS_OK;
}
