#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "lql_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/xattr.h>
#endif
#include <unistd.h>

typedef struct lql_file_reader {
  FILE *file;
} lql_file_reader;

typedef struct lql_file_writer {
  FILE *file;
} lql_file_writer;

static lql_status file_read(void *user, unsigned char *buffer, size_t capacity,
                            size_t *out_len, lql_error *error) {
  lql_file_reader *reader;
  size_t amount;
  (void)error;
  reader = (lql_file_reader *)user;
  if (reader == NULL || reader->file == NULL || buffer == NULL ||
      out_len == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out_len = 0u;
  if (capacity == 0u) {
    return LQL_STATUS_OK;
  }
  amount = fread(buffer, 1u, capacity, reader->file);
  if (amount == 0u && ferror(reader->file)) {
    return LQL_STATUS_IO_ERROR;
  }
  *out_len = amount;
  return LQL_STATUS_OK;
}

static lql_status file_write(void *user, const void *data, size_t len,
                             lql_error *error) {
  lql_file_writer *writer;
  (void)error;
  writer = (lql_file_writer *)user;
  if (writer == NULL || writer->file == NULL || (len != 0u && data == NULL)) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (len != 0u && fwrite(data, 1u, len, writer->file) != len) {
    return LQL_STATUS_IO_ERROR;
  }
  return LQL_STATUS_OK;
}

static int open_input_path(const char *path, FILE **out) {
  if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0) {
    *out = stdin;
    return 1;
  }
  *out = fopen(path, "rb");
  return *out != NULL;
}

static int path_is_stdio(const char *path) {
  return path == NULL || path[0] == '\0' || strcmp(path, "-") == 0;
}

static int file_request_has_multiple_output_sinks(
    const lql_file_filter_request *request) {
  unsigned int sinks;
  sinks = 0u;
  if (request->output_writer != NULL) {
    ++sinks;
  }
  if (request->output_file != NULL) {
    ++sinks;
  }
  if (request->output_path != NULL) {
    ++sinks;
  }
  return sinks > 1u;
}

static int same_open_file_identity(FILE *left, int right_fd, int *same) {
  struct stat left_st;
  struct stat right_st;
  int left_fd;
  *same = 0;
  if (left == NULL || right_fd < 0) {
    errno = EINVAL;
    return 0;
  }
  left_fd = fileno(left);
  if (left_fd < 0) {
    return 1;
  }
  if (fstat(left_fd, &left_st) != 0 || fstat(right_fd, &right_st) != 0) {
    return 0;
  }
  *same = left_st.st_dev == right_st.st_dev &&
          left_st.st_ino == right_st.st_ino;
  return 1;
}

static int open_output_path_checked(const char *path, FILE *input,
                                    FILE **out, int *same_input_output) {
  int fd;
  int same;
  struct stat output_st;
  if (same_input_output != NULL) {
    *same_input_output = 0;
  }
  if (path_is_stdio(path)) {
    *out = stdout;
    return 1;
  }
  fd = open(path, O_WRONLY | O_CREAT, 0666);
  if (fd < 0) {
    return 0;
  }
  if (!same_open_file_identity(input, fd, &same)) {
    close(fd);
    return 0;
  }
  if (same) {
    if (same_input_output != NULL) {
      *same_input_output = 1;
    }
    close(fd);
    return 0;
  }
  if (fstat(fd, &output_st) != 0) {
    close(fd);
    return 0;
  }
  if (S_ISREG(output_st.st_mode) && ftruncate(fd, 0) != 0) {
    close(fd);
    return 0;
  }
  *out = fdopen(fd, "wb");
  if (*out == NULL) {
    close(fd);
    return 0;
  }
  return 1;
}

static lql_stream_output_mode
filter_file_output_mode(const lql_file_filter_request *request) {
  if (request->output_mode != LQL_STREAM_OUTPUT_DECISION_ONLY) {
    return request->output_mode;
  }
  if (request->mutation != NULL && request->projection != NULL) {
    return LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION;
  }
  if (request->mutation != NULL) {
    return LQL_STREAM_OUTPUT_MUTATION;
  }
  if (request->projection != NULL) {
    return LQL_STREAM_OUTPUT_PROJECTION;
  }
  return LQL_STREAM_OUTPUT_SELECTED_RECORD;
}

static lql_status filter_open_stream(lql *self,
                                     const lql_file_filter_request *request,
                                     FILE *input, FILE *output,
                                     lql_stream_result *result,
                                     lql_error *error) {
  lql_file_reader reader;
  lql_file_writer writer;
  lql_stream_request stream;
  lql_status status;
  lql_stream_result local_result;
  char count_buffer[64];
  int count_len;

  memset(&reader, 0, sizeof(reader));
  reader.file = input;
  memset(&writer, 0, sizeof(writer));
  writer.file = output;
  memset(&stream, 0, sizeof(stream));
  stream.reader = file_read;
  stream.reader_user = &reader;
  stream.selector = request->selector;
  stream.projection = request->count_only ? NULL : request->projection;
  stream.mutation = request->count_only ? NULL : request->mutation;
  stream.matched_only = request->matched_only;
  if (!request->count_only) {
    if (request->output_writer != NULL) {
      stream.writer = request->output_writer;
      stream.writer_user = request->output_user;
    } else {
      stream.writer = file_write;
      stream.writer_user = &writer;
    }
    stream.output_mode = filter_file_output_mode(request);
  }
  memset(&local_result, 0, sizeof(local_result));
  status = self->stream_execute_spooled(self, &stream, &local_result, error);
  if (result != NULL) {
    *result = local_result;
  }
  if (status != LQL_STATUS_OK) {
    return status;
  }
  if (request->mutation != NULL && local_result.records_seen == 0u) {
    lql_set_error(error, LQL_STATUS_JSON_ERROR, "no JSON input");
    return LQL_STATUS_JSON_ERROR;
  }
  if (request->count_only) {
    count_len = sprintf(count_buffer, "%lu\n",
                        (unsigned long)local_result.records_matched);
    if (count_len < 0) {
      lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to format count");
      return LQL_STATUS_IO_ERROR;
    }
    if (request->output_writer != NULL) {
      return request->output_writer(request->output_user, count_buffer,
                                    (size_t)count_len, error);
    }
    if (fwrite(count_buffer, 1u, (size_t)count_len, output) !=
        (size_t)count_len) {
      lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to write count");
      return LQL_STATUS_IO_ERROR;
    }
  }
  return LQL_STATUS_OK;
}

lql_status
lql_filter_file_spooled_internal(lql *self,
                                 const lql_file_filter_request *request,
                                 lql_stream_result *result, lql_error *error) {
  FILE *input;
  FILE *output;
  int close_input;
  int close_output;
  lql_status status;

  if (result != NULL) {
    memset(result, 0, sizeof(*result));
  }
  if (self == NULL || request == NULL) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "file request required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  input = request->input_file;
  output = request->output_file;
  close_input = 0;
  close_output = 0;
  if (file_request_has_multiple_output_sinks(request)) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "file request has multiple output sinks");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (input == NULL) {
    if (!open_input_path(request->input_path, &input)) {
      lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to open input file");
      return LQL_STATUS_IO_ERROR;
    }
    close_input = input != stdin;
  }
  if (output == NULL && request->output_writer == NULL) {
    int same_input_output;
    if (!open_output_path_checked(request->output_path, input, &output,
                                  &same_input_output)) {
      if (close_input) {
        fclose(input);
      }
      if (same_input_output) {
        lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                      "input and output paths identify the same file; use "
                      "inline rewrite for in-place updates");
        return LQL_STATUS_INVALID_ARGUMENT;
      }
      lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to open output file");
      return LQL_STATUS_IO_ERROR;
    }
    close_output = output != stdout;
  }
  status = filter_open_stream(self, request, input, output, result, error);
  if (output != NULL && fflush(output) != 0 && status == LQL_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to flush output");
    status = LQL_STATUS_IO_ERROR;
  }
  if (close_output && fclose(output) != 0 && status == LQL_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to close output file");
    status = LQL_STATUS_IO_ERROR;
  }
  if (close_input && fclose(input) != 0 && status == LQL_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to close input file");
    status = LQL_STATUS_IO_ERROR;
  }
  return status;
}

static int inline_lock_source(int fd) {
  struct flock lock;
  memset(&lock, 0, sizeof(lock));
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  return fcntl(fd, F_SETLKW, &lock) == 0;
}

static char *inline_temp_path(const char *path) {
  static const char template_name[] = ".lql.tmp.XXXXXX";
  const char *slash;
  size_t dir_len;
  char *out;
  slash = strrchr(path, '/');
  if (slash == NULL) {
    dir_len = 0u;
  } else if (slash == path) {
    dir_len = 1u;
  } else {
    dir_len = (size_t)(slash - path);
  }
  out = (char *)malloc(dir_len + (dir_len == 0u ? 0u : 1u) +
                       sizeof(template_name));
  if (out == NULL) {
    return NULL;
  }
  if (dir_len == 0u) {
    memcpy(out, template_name, sizeof(template_name));
  } else if (dir_len == 1u && path[0] == '/') {
    out[0] = '/';
    memcpy(out + 1u, template_name, sizeof(template_name));
  } else {
    memcpy(out, path, dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1u, template_name, sizeof(template_name));
  }
  return out;
}

static int inline_open_source(const char *path, int *out_fd,
                              struct stat *out_st, lql_error *error) {
  struct stat link_st;
  int flags;
  int fd;
  if (path == NULL || out_fd == NULL || out_st == NULL) {
    errno = EINVAL;
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT, "inline path required");
    return 0;
  }
  if (lstat(path, &link_st) != 0) {
    lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to open input file");
    return 0;
  }
  if (S_ISLNK(link_st.st_mode)) {
    errno = 0;
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "inline mode does not rewrite symlink paths");
    return 0;
  }
  if (!S_ISREG(link_st.st_mode)) {
    errno = 0;
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "inline mode requires a regular file");
    return 0;
  }
  flags = O_RDWR;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  fd = open(path, flags);
  if (fd < 0) {
#ifdef ELOOP
    if (errno == ELOOP) {
      lql_set_error(error, LQL_STATUS_IO_ERROR,
                    "inline mode does not rewrite symlink paths");
      errno = 0;
    } else
#endif
    {
      lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to open input file");
    }
    return 0;
  }
  if (!inline_lock_source(fd)) {
    close(fd);
    lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to lock input file");
    return 0;
  }
  if (fstat(fd, out_st) != 0) {
    close(fd);
    lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to inspect input file");
    return 0;
  }
  if (!S_ISREG(out_st->st_mode) || out_st->st_dev != link_st.st_dev ||
      out_st->st_ino != link_st.st_ino) {
    close(fd);
    errno = 0;
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "inline input file changed during rewrite");
    return 0;
  }
  *out_fd = fd;
  return 1;
}

static long stat_mtime_nsec(const struct stat *st) {
#if defined(__APPLE__)
  return st->st_mtimespec.tv_nsec;
#elif defined(__linux__)
  return st->st_mtim.tv_nsec;
#else
  (void)st;
  return 0L;
#endif
}

static long stat_ctime_nsec(const struct stat *st) {
#if defined(__APPLE__)
  return st->st_ctimespec.tv_nsec;
#elif defined(__linux__)
  return st->st_ctim.tv_nsec;
#else
  (void)st;
  return 0L;
#endif
}

static int inline_stat_equals_source(const struct stat *candidate,
                                     const struct stat *source) {
  return candidate->st_dev == source->st_dev &&
         candidate->st_ino == source->st_ino &&
         candidate->st_size == source->st_size &&
         candidate->st_mode == source->st_mode &&
         candidate->st_uid == source->st_uid &&
         candidate->st_gid == source->st_gid &&
         candidate->st_mtime == source->st_mtime &&
         stat_mtime_nsec(candidate) == stat_mtime_nsec(source) &&
         candidate->st_ctime == source->st_ctime &&
         stat_ctime_nsec(candidate) == stat_ctime_nsec(source);
}

static int inline_source_path_still_matches(const char *path, int source_fd,
                                            const struct stat *st) {
  struct stat current_path;
  struct stat current_source;
  if (lstat(path, &current_path) != 0 ||
      fstat(source_fd, &current_source) != 0) {
    return 0;
  }
  return current_path.st_dev == st->st_dev &&
         current_path.st_ino == st->st_ino &&
         inline_stat_equals_source(&current_source, st);
}

static int sync_parent_dir(const char *path) {
  const char *slash;
  char *dir;
  int fd;
  int ok;
  slash = strrchr(path, '/');
  if (slash == NULL) {
    dir = strdup(".");
  } else if (slash == path) {
    dir = strdup("/");
  } else {
    dir = (char *)malloc((size_t)(slash - path) + 1u);
    if (dir != NULL) {
      memcpy(dir, path, (size_t)(slash - path));
      dir[slash - path] = '\0';
    }
  }
  if (dir == NULL) {
    return 0;
  }
  fd = open(dir, O_RDONLY);
  free(dir);
  if (fd < 0) {
    return 0;
  }
  ok = fsync(fd) == 0;
  if (close(fd) != 0) {
    ok = 0;
  }
  return ok;
}

static int preserve_inline_owner(int fd, const struct stat *st) {
  struct stat tmp_st;
  int saved_errno;
  if (fchown(fd, st->st_uid, st->st_gid) == 0) {
    return 1;
  }
  saved_errno = errno;
  if (saved_errno != EPERM && saved_errno != EINVAL) {
    errno = saved_errno;
    return 0;
  }
  if (fstat(fd, &tmp_st) != 0) {
    return 0;
  }
  if (tmp_st.st_uid != geteuid()) {
    errno = saved_errno;
    return 0;
  }
  if (fchown(fd, (uid_t)-1, st->st_gid) != 0 && errno != EPERM &&
      errno != EINVAL) {
    return 0;
  }
  return 1;
}

#if defined(__linux__) || defined(__APPLE__)
static ssize_t inline_listxattr_fd(int fd, char *names, size_t size) {
#if defined(__APPLE__)
  return flistxattr(fd, names, size, 0);
#else
  return flistxattr(fd, names, size);
#endif
}

static ssize_t inline_getxattr_fd(int fd, const char *name, void *value,
                                  size_t size) {
#if defined(__APPLE__)
  return fgetxattr(fd, name, value, size, 0, 0);
#else
  return fgetxattr(fd, name, value, size);
#endif
}

static int inline_setxattr(int fd, const char *name, const void *value,
                           size_t size) {
#if defined(__APPLE__)
  return fsetxattr(fd, name, value, size, 0, 0);
#else
  return fsetxattr(fd, name, value, size, 0);
#endif
}

static int inline_xattr_set_error_ignorable(const char *name, int error_code) {
#if defined(__linux__)
  if (name == NULL) {
    return 0;
  }
  if (error_code != EPERM && error_code != EACCES && error_code != ENOTSUP &&
      error_code != EOPNOTSUPP) {
    return 0;
  }
  return strncmp(name, "security.", 9u) == 0 ||
         strncmp(name, "system.", 7u) == 0;
#else
  (void)name;
  (void)error_code;
  return 0;
#endif
}

static int copy_inline_xattrs(int source_fd, int fd) {
  char stack_names[4096];
  char *names;
  ssize_t names_len;
  ssize_t need;
  size_t offset;
  names = stack_names;
  names_len = inline_listxattr_fd(source_fd, names, sizeof(stack_names));
  if (names_len < 0 && errno == ERANGE) {
    need = inline_listxattr_fd(source_fd, NULL, 0u);
    if (need < 0) {
      return 0;
    }
    if (need == 0) {
      return 1;
    }
    names = (char *)malloc((size_t)need);
    if (names == NULL) {
      return 0;
    }
    names_len = inline_listxattr_fd(source_fd, names, (size_t)need);
  }
  if (names_len < 0) {
    if (names != stack_names) {
      free(names);
    }
    return errno == ENOTSUP || errno == EOPNOTSUPP;
  }
  offset = 0u;
  while (offset < (size_t)names_len) {
    const char *name;
    char stack_value[4096];
    char *value;
    ssize_t value_len;
    name = names + offset;
    offset += strlen(name) + 1u;
    value = stack_value;
    value_len = inline_getxattr_fd(source_fd, name, value, sizeof(stack_value));
    if (value_len < 0 && errno == ERANGE) {
      need = inline_getxattr_fd(source_fd, name, NULL, 0u);
      if (need < 0) {
        if (names != stack_names) {
          free(names);
        }
        return 0;
      }
      value = (char *)malloc((size_t)need);
      if (value == NULL) {
        if (names != stack_names) {
          free(names);
        }
        return 0;
      }
      value_len = inline_getxattr_fd(source_fd, name, value, (size_t)need);
    }
    if (value_len < 0) {
      if (value != stack_value) {
        free(value);
      }
      if (errno == ENODATA) {
        continue;
      }
      if (names != stack_names) {
        free(names);
      }
      return 0;
    }
    if (inline_setxattr(fd, name, value, (size_t)value_len) != 0) {
      int set_errno;
      set_errno = errno;
      if (value != stack_value) {
        free(value);
      }
      if (inline_xattr_set_error_ignorable(name, set_errno)) {
        continue;
      }
      if (names != stack_names) {
        free(names);
      }
      errno = set_errno;
      return 0;
    }
    if (value != stack_value) {
      free(value);
    }
  }
  if (names != stack_names) {
    free(names);
  }
  return 1;
}
#else
static int copy_inline_xattrs(int source_fd, int fd) {
  (void)source_fd;
  (void)fd;
  return 1;
}
#endif

lql_status lql_rewrite_file_inline_spooled_internal(
    lql *self, const lql_file_filter_request *request,
    lql_stream_result *result, lql_error *error) {
  FILE *source;
  FILE *tmp;
  char *tmp_path;
  struct stat st;
  int source_fd;
  int source_check_fd;
  int fd;
  lql_status status;

  if (result != NULL) {
    memset(result, 0, sizeof(*result));
  }
  if (self == NULL || request == NULL || request->input_path == NULL ||
      request->input_path[0] == '\0' || strcmp(request->input_path, "-") == 0 ||
      request->input_file != NULL || request->output_file != NULL ||
      request->output_path != NULL || request->output_writer != NULL ||
      request->count_only) {
    lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                  "inline rewrite requires one input path");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  source_fd = -1;
  source_check_fd = -1;
  if (!inline_open_source(request->input_path, &source_fd, &st, error)) {
    return error != NULL && error->code != LQL_STATUS_OK ? error->code
                                                         : LQL_STATUS_IO_ERROR;
  }
  source_check_fd = dup(source_fd);
  if (source_check_fd < 0) {
    close(source_fd);
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "unable to monitor inline input file");
    return LQL_STATUS_IO_ERROR;
  }
  source = fdopen(source_fd, "rb");
  if (source == NULL) {
    close(source_fd);
    close(source_check_fd);
    lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to open input stream");
    return LQL_STATUS_IO_ERROR;
  }
  tmp_path = inline_temp_path(request->input_path);
  if (tmp_path == NULL) {
    fclose(source);
    close(source_check_fd);
    lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  fd = mkstemp(tmp_path);
  if (fd < 0) {
    fclose(source);
    close(source_check_fd);
    free(tmp_path);
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "unable to create inline temp file");
    return LQL_STATUS_IO_ERROR;
  }
  if (!preserve_inline_owner(fd, &st)) {
    close(fd);
    unlink(tmp_path);
    fclose(source);
    close(source_check_fd);
    free(tmp_path);
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "unable to set inline temp ownership");
    return LQL_STATUS_IO_ERROR;
  }
  tmp = fdopen(fd, "wb");
  if (tmp == NULL) {
    close(fd);
    unlink(tmp_path);
    fclose(source);
    close(source_check_fd);
    free(tmp_path);
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "unable to open inline temp stream");
    return LQL_STATUS_IO_ERROR;
  }
  status = filter_open_stream(self, request, source, tmp, result, error);
  if (fflush(tmp) != 0 && status == LQL_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to flush output");
    status = LQL_STATUS_IO_ERROR;
  }
  if (status == LQL_STATUS_OK && fchmod(fileno(tmp), st.st_mode & 07777) != 0) {
    lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to set inline temp mode");
    status = LQL_STATUS_IO_ERROR;
  }
  if (status == LQL_STATUS_OK &&
      !copy_inline_xattrs(source_check_fd, fileno(tmp))) {
    lql_set_error(error, LQL_STATUS_IO_ERROR,
                  "unable to preserve inline file metadata");
    status = LQL_STATUS_IO_ERROR;
  }
  if (status == LQL_STATUS_OK && fsync(fileno(tmp)) != 0) {
    lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to sync inline output");
    status = LQL_STATUS_IO_ERROR;
  }
  if (fclose(tmp) != 0 && status == LQL_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to close inline output");
    status = LQL_STATUS_IO_ERROR;
  }
  if (status == LQL_STATUS_OK) {
    /*
     * The completed temp file is durable before rename, preserving the
     * crash/failure atomicity expected from inline rewrite. The identity check
     * detects cooperative changes through the held advisory lock; POSIX does
     * not provide a portable atomic path-identity compare-and-swap for
     * non-cooperating concurrent replacement.
     */
    if (!inline_source_path_still_matches(request->input_path, source_check_fd,
                                          &st)) {
      lql_set_error(error, LQL_STATUS_IO_ERROR,
                    "inline input file changed during rewrite");
      status = LQL_STATUS_IO_ERROR;
    } else if (rename(tmp_path, request->input_path) != 0) {
      lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to replace input file");
      status = LQL_STATUS_IO_ERROR;
    } else if (!sync_parent_dir(request->input_path)) {
      lql_set_error(error, LQL_STATUS_IO_ERROR,
                    "inline replacement committed but parent directory sync "
                    "failed");
      status = LQL_STATUS_IO_ERROR;
    }
  }
  if (status != LQL_STATUS_OK) {
    unlink(tmp_path);
  }
  if (fclose(source) != 0 && status == LQL_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to close input file");
    status = LQL_STATUS_IO_ERROR;
  }
  if (close(source_check_fd) != 0 && status == LQL_STATUS_OK) {
    lql_set_error(error, LQL_STATUS_IO_ERROR, "unable to close inline monitor");
    status = LQL_STATUS_IO_ERROR;
  }
  free(tmp_path);
  return status;
}

lql_status lql_filter_file_spooled(lql *self,
                                   const lql_file_filter_request *request,
                                   lql_stream_result *result,
                                   lql_error *error) {
  return lql_filter_file_spooled_internal(self, request, result, error);
}

lql_status
lql_rewrite_file_inline_spooled(lql *self,
                                const lql_file_filter_request *request,
                                lql_stream_result *result, lql_error *error) {
  return lql_rewrite_file_inline_spooled_internal(self, request, result, error);
}
