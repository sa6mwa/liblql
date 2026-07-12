#include "lql_json_spool.h"

#include <stdlib.h>
#include <string.h>

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
  unsigned char buffer[8192];
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

void lql_json_spool_reader_init(lql_json_spool_reader *reader,
                                const lql_json_spool *spool) {
  if (reader == NULL) return;
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
  if (out_len != NULL) *out_len = 0u;
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
    if (amount > capacity) amount = capacity;
    if (amount != 0u) memcpy(buffer, spool->memory + reader->offset, amount);
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
