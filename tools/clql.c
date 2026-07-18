#define _POSIX_C_SOURCE 200809L

#include <lql/lql.h>

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

typedef struct clql_reader {
  FILE *file;
} clql_reader;

typedef struct clql_writer {
  FILE *file;
} clql_writer;

typedef struct clql_string_list {
  char **items;
  size_t count;
  size_t capacity;
} clql_string_list;

typedef struct clql_config {
  clql_string_list mutations;
  clql_string_list fields;
  int inline_write;
  int compact;
  int matches_only;
  int enable_file_mutations;
  int count_only;
  int or_mode;
} clql_config;

static void usage(FILE *file) {
  fputs("usage: clql [-m mutator...] [-f field...] selector... [data.json]\n",
        file);
  fputs("   or: clql selector... < data.json\n", file);
  fputs("   or: cat data.json | clql selector...\n\n", file);
  fputs("Selectors:\n", file);
  fputs("  LQL selector expressions (comma/newline separated).\n\n", file);
  fputs("Mutations:\n", file);
  fputs("  -m, --mutate expr    apply mutations to each JSON object in the "
        "input stream\n",
        file);
  fputs("  -i, --inline         write mutation output inline to a single input "
        "file\n",
        file);
  fputs("  -w, --write          alias of --inline\n", file);
  fputs("  -F, --enable-file-mutations\n", file);
  fputs("                       allow file:/textfile:/base64file: mutation "
        "values\n\n",
        file);
  fputs("Output:\n", file);
  fputs("  -f, --field /path    output only selected JSON Pointer fields "
        "(repeatable)\n",
        file);
  fputs("  -c, --compact        compact output (always compact; prettyx "
        "unsupported)\n",
        file);
  fputs("  -t, --theme theme    unsupported: clql does not include prettyx "
        "themes\n",
        file);
  fputs("  -h, --help           show help\n", file);
  fputs("  -v, --version        show version\n", file);
  fputs("  -O, --or             combine selector arguments with OR\n", file);
  fputs("  -M, --matches-only   output only selector matches (even with -m)\n",
        file);
  fputs("      --count          output only the number of selector matches\n\n",
        file);
  fputs("Selector examples (shorthand):\n", file);
  fputs("  /status=\"open\"\n", file);
  fputs("  /status!=closed\n", file);
  fputs("  /progress>=50\n", file);
  fputs("  /timestamp>=\"2025-01-01T00:00:00Z\"\n", file);
  fputs("  /devices/0/status=\"online\"\n", file);
  fputs("  /labels/*=\"production\"\n", file);
  fputs("  /items[]/sku=\"ABC-123\"\n", file);
  fputs("  /items/**/sku=\"ABC-123\"\n", file);
  fputs("  /items/.../sku=\"ABC-123\"\n\n", file);
  fputs("Selector examples (full LQL):\n", file);
  fputs("  eq{field=/status,value=open}\n", file);
  fputs("  contains{field=/msg,value=timeout,ic=t}\n", file);
  fputs("  contains{field=/msg,any=timeout|degraded}\n", file);
  fputs("  icontains{field=/msg,value=timeout}\n", file);
  fputs("  icontains{field=/service,a=AUTH|EDGE}\n", file);
  fputs("  iprefix{field=/service,value=auth}\n", file);
  fputs("  date{field=/timestamp,after=2025-01-01,before=2025-02-01}\n", file);
  fputs("  date{f=/timestamp,since=yesterday}\n", file);
  fputs(
      "  and.eq{field=/status,value=open},and.range{field=/progress,gte=50}\n",
      file);
  fputs("  or.eq{field=/region,value=us},or.eq{field=/region,value=eu}\n",
        file);
  fputs("  not.eq{field=/state,value=disabled}\n", file);
  fputs("  exists{/metadata/etag}\n\n", file);
  fputs("Invocation examples:\n", file);
  fputs("  clql -O '/status=\"open\"' '/status=\"queued\"' data.json\n", file);
  fputs("  clql --count '/status=\"open\"' data.json\n", file);
  fputs("  cat data.json | clql '/items[]/sku=\"ABC-123\"'\n\n", file);
  fputs("File-backed mutation example:\n", file);
  fputs("  printf '{}\\n' | clql -F \\\n", file);
  fputs("    -m '/filename=notes.txt' -m '/tags/kind=document' \\\n", file);
  fputs("    -m '/tags/source=local' -m 'textfile:/content=notes.txt'\n\n",
        file);
  fputs("Notes:\n", file);
  fputs(
      "  contains/icontains accept value=... or any=/a=... (pipe-delimited).\n",
      file);
  fputs("  range comparisons accept numeric or datetime literals.\n", file);
  fputs("  date supports value/after/before/gt/gte/lt/lte; aliases a=after and "
        "b=before.\n",
        file);
  fputs("  only date{...,since=...} supports relative macros (now, today, "
        "yesterday).\n",
        file);
  fputs("  omitted values for contains/icontains/prefix/iprefix act as path "
        "assertions.\n\n",
        file);
  fputs("Reads strict NDJSON from file or stdin and writes compact matching\n",
        file);
  fputs("records to stdout, one JSON value per line. Root arrays are errors.\n",
        file);
  fputs("Projection and mutation output use liblql's explicitly spooled path\n",
        file);
  fputs("and may spill the current record to a temporary file.\n", file);
}

static lql_status clql_read(void *user, unsigned char *buffer, size_t capacity,
                            size_t *out_len, lql_error *error) {
  clql_reader *reader;
  size_t amount;
  (void)error;
  reader = (clql_reader *)user;
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

static lql_status clql_write(void *user, const void *data, size_t len,
                             lql_error *error) {
  clql_writer *writer;
  (void)error;
  writer = (clql_writer *)user;
  if (writer == NULL || writer->file == NULL || (len != 0u && data == NULL)) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (len != 0u && fwrite(data, 1u, len, writer->file) != len) {
    return LQL_STATUS_IO_ERROR;
  }
  return LQL_STATUS_OK;
}

static int print_error(const char *context, lql_status status,
                       const lql_error *error) {
  fprintf(stderr, "clql: %s: %s", context, lql_status_string(status));
  if (error != NULL && error->message[0] != '\0') {
    fprintf(stderr, ": %s", error->message);
  }
  fputc('\n', stderr);
  return 1;
}

static int file_exists(const char *path) {
  struct stat st;
  return path != NULL && stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

static void string_list_destroy(clql_string_list *list) {
  size_t i;
  if (list == NULL) {
    return;
  }
  for (i = 0u; i < list->count; ++i) {
    free(list->items[i]);
  }
  free(list->items);
  memset(list, 0, sizeof(*list));
}

static void config_destroy(clql_config *cfg) {
  if (cfg == NULL) {
    return;
  }
  string_list_destroy(&cfg->mutations);
  string_list_destroy(&cfg->fields);
}

static char *copy_string(const char *value) {
  size_t len;
  char *out;
  if (value == NULL) {
    return NULL;
  }
  len = strlen(value);
  out = (char *)malloc(len + 1u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, value, len + 1u);
  return out;
}

static int string_list_push_copy(clql_string_list *list, const char *value) {
  char **next;
  char *copy;
  size_t next_capacity;
  if (list == NULL || value == NULL) {
    return 0;
  }
  if (list->count == list->capacity) {
    next_capacity = list->capacity == 0u ? 4u : list->capacity * 2u;
    next = (char **)realloc(list->items, next_capacity * sizeof(*next));
    if (next == NULL) {
      return 0;
    }
    list->items = next;
    list->capacity = next_capacity;
  }
  copy = copy_string(value);
  if (copy == NULL) {
    return 0;
  }
  list->items[list->count++] = copy;
  return 1;
}

static int take_option_value(int argc, char **argv, int *arg,
                             const char *inline_value, const char **out) {
  if (inline_value != NULL) {
    *out = inline_value;
    return 1;
  }
  if (*arg + 1 >= argc) {
    return 0;
  }
  ++*arg;
  *out = argv[*arg];
  return 1;
}

static int starts_with_option(const char *arg, const char *name,
                              const char **value) {
  size_t len;
  len = strlen(name);
  if (strncmp(arg, name, len) != 0) {
    return 0;
  }
  if (arg[len] == '\0') {
    *value = NULL;
    return 1;
  }
  if (arg[len] == '=') {
    *value = arg + len + 1u;
    return 1;
  }
  return 0;
}

static char *join_selector_list(const clql_string_list *list,
                                const char *separator) {
  size_t len;
  size_t sep_len;
  size_t i;
  char *out;
  char *cursor;
  if (list == NULL || list->count == 0u) {
    return copy_string("");
  }
  len = 1u;
  sep_len = strlen(separator);
  for (i = 0u; i < list->count; ++i) {
    len += strlen(list->items[i]);
    if (i + 1u < list->count) {
      len += sep_len;
    }
  }
  out = (char *)malloc(len);
  if (out == NULL) {
    return NULL;
  }
  cursor = out;
  for (i = 0u; i < list->count; ++i) {
    size_t part_len;
    if (i != 0u) {
      memcpy(cursor, separator, sep_len);
      cursor += sep_len;
    }
    part_len = strlen(list->items[i]);
    memcpy(cursor, list->items[i], part_len);
    cursor += part_len;
  }
  *cursor = '\0';
  return out;
}

static int parse_args(int argc, char **argv, clql_config *cfg,
                      clql_string_list *positionals) {
  int arg;
  const char *value;
  memset(cfg, 0, sizeof(*cfg));
  memset(positionals, 0, sizeof(*positionals));
  arg = 1;
  while (arg < argc) {
    const char *inline_value;
    inline_value = NULL;
    if (strcmp(argv[arg], "--") == 0) {
      ++arg;
      while (arg < argc) {
        if (!string_list_push_copy(positionals, argv[arg])) {
          fprintf(stderr, "clql: out of memory\n");
          return -1;
        }
        ++arg;
      }
      return 0;
    }
    if (strcmp(argv[arg], "-h") == 0 || strcmp(argv[arg], "--help") == 0) {
      usage(stdout);
      return 1;
    }
    if (strcmp(argv[arg], "-v") == 0 || strcmp(argv[arg], "--version") == 0) {
      puts(LQL_VERSION);
      return 1;
    }
    if (strcmp(argv[arg], "--count") == 0) {
      cfg->count_only = 1;
      ++arg;
      continue;
    }
    if (strcmp(argv[arg], "-O") == 0 || strcmp(argv[arg], "--or") == 0) {
      cfg->or_mode = 1;
      ++arg;
      continue;
    }
    if (strcmp(argv[arg], "-c") == 0 || strcmp(argv[arg], "--compact") == 0) {
      cfg->compact = 1;
      ++arg;
      continue;
    }
    if (strcmp(argv[arg], "-M") == 0 ||
        strcmp(argv[arg], "--matches-only") == 0) {
      cfg->matches_only = 1;
      ++arg;
      continue;
    }
    if (strcmp(argv[arg], "-i") == 0 || strcmp(argv[arg], "--inline") == 0 ||
        strcmp(argv[arg], "-w") == 0 || strcmp(argv[arg], "--write") == 0) {
      cfg->inline_write = 1;
      ++arg;
      continue;
    }
    if (strcmp(argv[arg], "-F") == 0 ||
        strcmp(argv[arg], "--enable-file-mutations") == 0) {
      cfg->enable_file_mutations = 1;
      ++arg;
      continue;
    }
    if (strcmp(argv[arg], "-m") == 0 || strcmp(argv[arg], "--mutate") == 0 ||
        starts_with_option(argv[arg], "--mutate", &inline_value)) {
      if (!take_option_value(argc, argv, &arg, inline_value, &value) ||
          !string_list_push_copy(&cfg->mutations, value)) {
        fprintf(stderr, "clql: --mutate requires a value\n");
        return -1;
      }
      ++arg;
      continue;
    }
    if (strcmp(argv[arg], "-f") == 0 || strcmp(argv[arg], "--field") == 0 ||
        starts_with_option(argv[arg], "--field", &inline_value)) {
      if (!take_option_value(argc, argv, &arg, inline_value, &value) ||
          !string_list_push_copy(&cfg->fields, value)) {
        fprintf(stderr, "clql: --field requires a value\n");
        return -1;
      }
      ++arg;
      continue;
    }
    if (strcmp(argv[arg], "-t") == 0 || strcmp(argv[arg], "--theme") == 0 ||
        starts_with_option(argv[arg], "--theme", &inline_value)) {
      if (!take_option_value(argc, argv, &arg, inline_value, &value)) {
        fprintf(stderr, "clql: --theme requires a value\n");
        return -1;
      }
      fprintf(stderr, "clql: --theme is unsupported; prettyx is not linked\n");
      return -1;
    }
    if (argv[arg][0] == '-') {
      fprintf(stderr, "clql: unknown flag: %s\n", argv[arg]);
      usage(stderr);
      return -1;
    }
    if (!string_list_push_copy(positionals, argv[arg])) {
      fprintf(stderr, "clql: out of memory\n");
      return -1;
    }
    ++arg;
  }
  return 0;
}

static int split_selection_args(const clql_string_list *positionals,
                                clql_string_list *selectors,
                                const char **input_path) {
  size_t i;
  *input_path = "";
  memset(selectors, 0, sizeof(*selectors));
  if (positionals->count == 0u) {
    return 1;
  }
  for (i = 0u; i < positionals->count; ++i) {
    int is_input;
    is_input = 0;
    if (i + 1u == positionals->count &&
        (strcmp(positionals->items[i], "-") == 0 ||
         file_exists(positionals->items[i]))) {
      is_input = 1;
    }
    if (is_input) {
      *input_path = positionals->items[i];
    } else if (!string_list_push_copy(selectors, positionals->items[i])) {
      fprintf(stderr, "clql: out of memory\n");
      return 0;
    }
  }
  return 1;
}

static int split_mutation_args(const clql_string_list *positionals,
                               clql_string_list *selectors,
                               clql_string_list *inputs) {
  size_t i;
  memset(selectors, 0, sizeof(*selectors));
  memset(inputs, 0, sizeof(*inputs));
  for (i = 0u; i < positionals->count; ++i) {
    if (strcmp(positionals->items[i], "-") == 0 ||
        file_exists(positionals->items[i])) {
      if (!string_list_push_copy(inputs, positionals->items[i])) {
        fprintf(stderr, "clql: out of memory\n");
        return 0;
      }
    } else if (!string_list_push_copy(selectors, positionals->items[i])) {
      fprintf(stderr, "clql: out of memory\n");
      return 0;
    }
  }
  return 1;
}

static int open_input_path(const char *path, FILE **out) {
  if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0) {
    *out = stdin;
    return 1;
  }
  *out = fopen(path, "rb");
  if (*out == NULL) {
    fprintf(stderr, "clql: unable to open input: %s\n", path);
    return 0;
  }
  return 1;
}

static int inline_lock_source(int fd) {
  struct flock lock;
  memset(&lock, 0, sizeof(lock));
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  return fcntl(fd, F_SETLKW, &lock) == 0;
}

static char *inline_temp_path(const char *path) {
  static const char template_name[] = ".clql.tmp.XXXXXX";
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
                              struct stat *out_st) {
  struct stat link_st;
  int flags;
  int fd;
  if (path == NULL || out_fd == NULL || out_st == NULL) {
    errno = EINVAL;
    return 0;
  }
  if (lstat(path, &link_st) != 0) {
    return 0;
  }
  if (S_ISLNK(link_st.st_mode)) {
    fputs("clql: inline mode does not rewrite symlink paths\n", stderr);
    errno = 0;
    return 0;
  }
  if (!S_ISREG(link_st.st_mode)) {
    fputs("clql: inline mode requires a regular file\n", stderr);
    errno = 0;
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
      fputs("clql: inline mode does not rewrite symlink paths\n", stderr);
      errno = 0;
    }
#endif
    return 0;
  }
  /*
   * Inline replacement is protected by a cooperative advisory lock held from
   * before the source metadata snapshot until after rename. POSIX path
   * replacement has no portable compare-and-swap primitive; non-cooperating
   * writers that ignore advisory locks cannot be made safe here.
   */
  if (!inline_lock_source(fd)) {
    close(fd);
    return 0;
  }
  if (fstat(fd, out_st) != 0) {
    close(fd);
    return 0;
  }
  if (!S_ISREG(out_st->st_mode) || out_st->st_dev != link_st.st_dev ||
      out_st->st_ino != link_st.st_ino) {
    close(fd);
    fputs("clql: inline input file changed during rewrite\n", stderr);
    errno = 0;
    return 0;
  }
  *out_fd = fd;
  return 1;
}

static long stat_mtime_nsec(const struct stat *st) {
#if defined(__APPLE__)
  return st->st_mtimensec;
#elif defined(__linux__)
  return st->st_mtim.tv_nsec;
#else
  (void)st;
  return 0L;
#endif
}

static long stat_ctime_nsec(const struct stat *st) {
#if defined(__APPLE__)
  return st->st_ctimensec;
#elif defined(__linux__)
  return st->st_ctim.tv_nsec;
#else
  (void)st;
  return 0L;
#endif
}

static int inline_source_path_still_matches(const char *path, int source_fd,
                                            const struct stat *st) {
  struct stat current_path;
  struct stat current_source;
  if (lstat(path, &current_path) != 0 ||
      fstat(source_fd, &current_source) != 0) {
    return 0;
  }
  /*
   * The final rename must not discard a concurrent in-place edit.  dev/ino
   * catches path replacement, while the open fd metadata catches writes,
   * truncation, chmod/chown, and other ctime-moving changes to the same inode.
   */
  return current_path.st_dev == st->st_dev &&
         current_path.st_ino == st->st_ino &&
         current_source.st_dev == st->st_dev &&
         current_source.st_ino == st->st_ino &&
         current_source.st_size == st->st_size &&
         current_source.st_mode == st->st_mode &&
         current_source.st_uid == st->st_uid &&
         current_source.st_gid == st->st_gid &&
         current_source.st_mtime == st->st_mtime &&
         stat_mtime_nsec(&current_source) == stat_mtime_nsec(st) &&
         current_source.st_ctime == st->st_ctime &&
         stat_ctime_nsec(&current_source) == stat_ctime_nsec(st);
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
  /*
   * Unprivileged users can rewrite group-writable files they do not own, but
   * cannot assign the replacement inode back to that foreign uid. Keep inline
   * rewrite usable in that POSIX case while preserving ownership whenever the
   * platform allows it.
   */
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
  if (error_code != EPERM && error_code != EACCES &&
      error_code != ENOTSUP && error_code != EOPNOTSUPP) {
    return 0;
  }
  /*
   * SELinux and other kernel-owned namespaces can be visible on ordinary
   * files while remaining non-restorable by an unprivileged rewriting process.
   * Leaving the platform to relabel the replacement is preferable to making
   * otherwise permitted inline rewrites unusable.
   */
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

static int execute_stream(lql *ctx, FILE *input, FILE *output,
                          const lql_selector *selector,
                          const lql_projection *projection,
                          const lql_mutation *mutation,
                          lql_stream_output_mode output_mode, int matched_only,
                          int count_only, lql_stream_result *aggregate) {
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  lql_status status;
  clql_reader reader;
  clql_writer writer;
  memset(&reader, 0, sizeof(reader));
  reader.file = input;
  memset(&writer, 0, sizeof(writer));
  writer.file = output;
  memset(&request, 0, sizeof(request));
  request.reader = clql_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.projection = count_only ? NULL : projection;
  request.mutation = count_only ? NULL : mutation;
  request.matched_only = matched_only;
  if (!count_only) {
    request.writer = clql_write;
    request.writer_user = &writer;
    request.output_mode = output_mode;
  }
  lql_error_init(&error);
  status = ctx->stream_execute_spooled(ctx, &request, &result, &error);
  if (status != LQL_STATUS_OK) {
    return print_error("execute stream", status, &error);
  }
  if (aggregate != NULL) {
    aggregate->records_seen += result.records_seen;
    aggregate->records_matched += result.records_matched;
  }
  return 0;
}

static int run_to_output(lql *ctx, const clql_config *cfg,
                         const clql_string_list *selectors,
                         const clql_string_list *inputs, const char *input_path,
                         FILE *input_override, FILE *output) {
  char *expr;
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
  lql_mutation_parse_options mutation_options;
  lql_stream_output_mode output_mode;
  lql_stream_result aggregate;
  lql_error error;
  lql_status status;
  size_t i;

  selector = NULL;
  projection = NULL;
  mutation = NULL;
  memset(&mutation_options, 0, sizeof(mutation_options));
  memset(&aggregate, 0, sizeof(aggregate));
  expr = join_selector_list(selectors, cfg->or_mode ? "," : "\n");
  if (expr == NULL) {
    fputs("clql: out of memory\n", stderr);
    return 1;
  }
  lql_error_init(&error);
  if (expr[0] != '\0') {
    status = cfg->or_mode ? ctx->selector_parse_or(ctx, expr, &selector, &error)
                          : ctx->selector_parse(ctx, expr, &selector, &error);
    if (status != LQL_STATUS_OK) {
      free(expr);
      return print_error("parse selector", status, &error);
    }
  }
  free(expr);
  if (cfg->fields.count != 0u) {
    status = ctx->projection_parse(ctx, (const char *const *)cfg->fields.items,
                                   cfg->fields.count, &projection, &error);
    if (status != LQL_STATUS_OK) {
      ctx->selector_destroy(ctx, selector);
      return print_error("parse projection", status, &error);
    }
  }
  if (cfg->mutations.count != 0u) {
    mutation_options.enable_file_values = cfg->enable_file_mutations;
    mutation_options.file_value_base_dir.data = ".";
    mutation_options.file_value_base_dir.len = 1u;
    status = ctx->mutation_parse_with_options(
        ctx, (const char *const *)cfg->mutations.items, cfg->mutations.count,
        &mutation_options, &mutation, &error);
    if (status != LQL_STATUS_OK) {
      ctx->projection_destroy(ctx, projection);
      ctx->selector_destroy(ctx, selector);
      return print_error("parse mutation", status, &error);
    }
  }
  if (mutation != NULL && projection != NULL) {
    output_mode = LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION;
  } else if (mutation != NULL) {
    output_mode = LQL_STREAM_OUTPUT_MUTATION;
  } else if (projection != NULL) {
    output_mode = LQL_STREAM_OUTPUT_PROJECTION;
  } else {
    output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  }
  if (inputs != NULL && inputs->count != 0u) {
    for (i = 0u; i < inputs->count; ++i) {
      FILE *input;
      if (!open_input_path(inputs->items[i], &input)) {
        status = LQL_STATUS_IO_ERROR;
        goto fail;
      }
      if (execute_stream(ctx, input, output, selector, projection, mutation,
                         output_mode, mutation != NULL ? cfg->matches_only : 1,
                         cfg->count_only, &aggregate) != 0) {
        if (input != stdin) {
          fclose(input);
        }
        status = LQL_STATUS_IO_ERROR;
        goto fail;
      }
      if (input != stdin) {
        fclose(input);
      }
    }
  } else {
    FILE *input;
    if (input_override != NULL) {
      input = input_override;
    } else {
      if (!open_input_path(input_path, &input)) {
        status = LQL_STATUS_IO_ERROR;
        goto fail;
      }
    }
    if (execute_stream(ctx, input, output, selector, projection, mutation,
                       output_mode, mutation != NULL ? cfg->matches_only : 1,
                       cfg->count_only, &aggregate) != 0) {
      if (input_override == NULL && input != stdin) {
        fclose(input);
      }
      status = LQL_STATUS_IO_ERROR;
      goto fail;
    }
    if (input_override == NULL && input != stdin) {
      fclose(input);
    }
  }
  if (mutation != NULL && aggregate.records_seen == 0u) {
    fputs("clql: no JSON input\n", stderr);
    status = LQL_STATUS_JSON_ERROR;
    goto fail;
  }
  if (cfg->count_only) {
    fprintf(output, "%lu\n", (unsigned long)aggregate.records_matched);
  }
  ctx->mutation_destroy(ctx, mutation);
  ctx->projection_destroy(ctx, projection);
  ctx->selector_destroy(ctx, selector);
  return 0;

fail:
  ctx->mutation_destroy(ctx, mutation);
  ctx->projection_destroy(ctx, projection);
  ctx->selector_destroy(ctx, selector);
  return status == LQL_STATUS_OK ? 1 : 1;
}

static int run_inline(lql *ctx, const clql_config *cfg,
                      const clql_string_list *selectors,
                      const clql_string_list *inputs) {
  FILE *source;
  FILE *tmp;
  char *tmp_path;
  struct stat st;
  int source_fd;
  int source_check_fd;
  int fd;
  int rc;
  int saved_errno;
  if (inputs->count != 1u || strcmp(inputs->items[0], "-") == 0) {
    fputs("clql: inline mode requires a single JSON file\n", stderr);
    return 2;
  }
  source_fd = -1;
  source_check_fd = -1;
  if (!inline_open_source(inputs->items[0], &source_fd, &st)) {
    if (errno != 0) {
      fprintf(stderr, "clql: unable to open input file: %s\n",
              inputs->items[0]);
    }
    return 1;
  }
  source_check_fd = dup(source_fd);
  if (source_check_fd < 0) {
    fprintf(stderr, "clql: unable to monitor inline input file: %s\n",
            strerror(errno));
    close(source_fd);
    return 1;
  }
  source = fdopen(source_fd, "rb");
  if (source == NULL) {
    fprintf(stderr, "clql: unable to open input stream: %s\n", strerror(errno));
    close(source_fd);
    close(source_check_fd);
    return 1;
  }
  tmp_path = inline_temp_path(inputs->items[0]);
  if (tmp_path == NULL) {
    fputs("clql: out of memory\n", stderr);
    fclose(source);
    close(source_check_fd);
    return 1;
  }
  fd = mkstemp(tmp_path);
  if (fd < 0) {
    fprintf(stderr, "clql: unable to create inline temp file: %s\n",
            strerror(errno));
    fclose(source);
    close(source_check_fd);
    free(tmp_path);
    return 1;
  }
  if (!preserve_inline_owner(fd, &st)) {
    fprintf(stderr, "clql: unable to set inline temp ownership: %s\n",
            strerror(errno));
    close(fd);
    unlink(tmp_path);
    fclose(source);
    close(source_check_fd);
    free(tmp_path);
    return 1;
  }
  tmp = fdopen(fd, "wb");
  if (tmp == NULL) {
    fprintf(stderr, "clql: unable to open inline temp stream: %s\n",
            strerror(errno));
    close(fd);
    unlink(tmp_path);
    fclose(source);
    close(source_check_fd);
    free(tmp_path);
    return 1;
  }
  rc = run_to_output(ctx, cfg, selectors, NULL, inputs->items[0], source, tmp);
  saved_errno = errno;
  errno = saved_errno;
  if (fflush(tmp) != 0 && rc == 0) {
    rc = 1;
  }
  if (rc == 0 && fchmod(fileno(tmp), st.st_mode & 07777) != 0) {
    fprintf(stderr, "clql: unable to set inline temp mode: %s\n",
            strerror(errno));
    rc = 1;
  }
  if (rc == 0 && !copy_inline_xattrs(source_check_fd, fileno(tmp))) {
    fprintf(stderr, "clql: unable to preserve inline file metadata: %s\n",
            strerror(errno));
    rc = 1;
  }
  if (rc == 0 && fsync(fileno(tmp)) != 0) {
    rc = 1;
  }
  if (fclose(tmp) != 0) {
    rc = 1;
  }
  if (rc == 0) {
    if (!inline_source_path_still_matches(inputs->items[0], source_check_fd,
                                          &st)) {
      fputs("clql: inline input file changed during rewrite\n", stderr);
      rc = 1;
    } else if (rename(tmp_path, inputs->items[0]) != 0) {
      fprintf(stderr, "clql: unable to replace input file: %s\n",
              inputs->items[0]);
      rc = 1;
    } else if (!sync_parent_dir(inputs->items[0])) {
      fprintf(stderr,
              "clql: inline replacement committed but parent directory sync "
              "failed: %s\n",
              strerror(errno));
      rc = 1;
    }
  }
  if (rc != 0) {
    unlink(tmp_path);
  }
  if (fclose(source) != 0 && rc == 0) {
    rc = 1;
  }
  if (close(source_check_fd) != 0 && rc == 0) {
    rc = 1;
  }
  free(tmp_path);
  return rc;
}

int main(int argc, char **argv) {
  clql_config cfg;
  clql_string_list positionals;
  clql_string_list selectors;
  clql_string_list inputs;
  const char *input_path;
  lql *ctx;
  lql_error error;
  lql_status status;
  int parsed;
  int rc;

  if (argc == 1) {
    usage(stderr);
    return 2;
  }
  parsed = parse_args(argc, argv, &cfg, &positionals);
  if (parsed != 0) {
    rc = parsed > 0 ? 0 : 2;
    config_destroy(&cfg);
    string_list_destroy(&positionals);
    return rc;
  }
  ctx = NULL;
  lql_error_init(&error);
  status = lql_new(&ctx, &error);
  if (status != LQL_STATUS_OK) {
    config_destroy(&cfg);
    string_list_destroy(&positionals);
    return print_error("create context", status, &error);
  }
  memset(&selectors, 0, sizeof(selectors));
  memset(&inputs, 0, sizeof(inputs));
  input_path = "";
  if (cfg.mutations.count != 0u) {
    if (!split_mutation_args(&positionals, &selectors, &inputs)) {
      rc = 1;
      goto done;
    }
    if (cfg.inline_write && cfg.count_only) {
      fputs("clql: inline mutation cannot be combined with --count\n", stderr);
      rc = 2;
      goto done;
    }
    if (cfg.inline_write) {
      rc = run_inline(ctx, &cfg, &selectors, &inputs);
    } else {
      rc = run_to_output(ctx, &cfg, &selectors, &inputs, "", NULL, stdout);
    }
  } else {
    if (cfg.inline_write) {
      fputs("clql: inline mode requires mutations\n", stderr);
      rc = 2;
      goto done;
    }
    if (!split_selection_args(&positionals, &selectors, &input_path)) {
      rc = 1;
      goto done;
    }
    rc = run_to_output(ctx, &cfg, &selectors, NULL, input_path, NULL, stdout);
  }
  if (rc == 0 && fflush(stdout) != 0) {
    fputs("clql: unable to flush stdout\n", stderr);
    rc = 1;
  }

done:
  string_list_destroy(&selectors);
  string_list_destroy(&inputs);
  ctx->destroy(ctx);
  config_destroy(&cfg);
  string_list_destroy(&positionals);
  return rc;
}
