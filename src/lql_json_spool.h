#ifndef LQL_JSON_SPOOL_H
#define LQL_JSON_SPOOL_H

#include <lql/lql.h>

#define LQL_JSON_SPOOL_MEMORY_BYTES (64u * 1024u)

typedef struct lql_json_spool {
  unsigned char *memory;
  size_t memory_len;
  size_t size;
  FILE *file;
} lql_json_spool;

typedef struct lql_json_spool_reader {
  const lql_json_spool *spool;
  size_t offset;
} lql_json_spool_reader;

lql_status lql_json_spool_init(lql_json_spool *spool, lql_error *error);
void lql_json_spool_reset(lql_json_spool *spool);
void lql_json_spool_cleanup(lql_json_spool *spool);
lql_status lql_json_spool_append(lql_json_spool *spool, const void *data,
                                 size_t len, lql_error *error);
size_t lql_json_spool_size(const lql_json_spool *spool);
lql_status lql_json_spool_write_to(const lql_json_spool *spool,
                                   lql_stream_writer_fn writer,
                                   void *writer_user, lql_error *error);
lql_status lql_json_spool_write_slice(const lql_json_spool *spool,
                                      size_t offset, size_t len,
                                      lql_stream_writer_fn writer,
                                      void *writer_user, lql_error *error);
void lql_json_spool_reader_init(lql_json_spool_reader *reader,
                                const lql_json_spool *spool);
lql_status lql_json_spool_read(void *user, unsigned char *buffer,
                                size_t capacity, size_t *out_len,
                                lql_error *error);

#endif
