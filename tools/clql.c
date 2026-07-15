#include <lql/lql.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static char *copy_range(const char *begin, size_t len) {
  char *out;
  out = (char *)malloc(len + 1u);
  if (out == NULL) {
    return NULL;
  }
  if (len != 0u) {
    memcpy(out, begin, len);
  }
  out[len] = '\0';
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

static int string_list_push_owned(clql_string_list *list, char *value) {
  char **next;
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
  list->items[list->count++] = value;
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

static char *json_quote_bytes(const unsigned char *data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  size_t i;
  size_t out_len;
  char *out;
  char *cursor;
  out_len = 2u;
  for (i = 0u; i < len; ++i) {
    unsigned char ch;
    ch = data[i];
    if (ch == '"' || ch == '\\' || ch < 0x20u) {
      out_len += ch < 0x20u ? 6u : 2u;
    } else {
      ++out_len;
    }
  }
  out = (char *)malloc(out_len + 1u);
  if (out == NULL) {
    return NULL;
  }
  cursor = out;
  *cursor++ = '"';
  for (i = 0u; i < len; ++i) {
    unsigned char ch;
    ch = data[i];
    if (ch == '"' || ch == '\\') {
      *cursor++ = '\\';
      *cursor++ = (char)ch;
    } else if (ch < 0x20u) {
      *cursor++ = '\\';
      *cursor++ = 'u';
      *cursor++ = '0';
      *cursor++ = '0';
      *cursor++ = hex[(ch >> 4) & 0x0fu];
      *cursor++ = hex[ch & 0x0fu];
    } else {
      *cursor++ = (char)ch;
    }
  }
  *cursor++ = '"';
  *cursor = '\0';
  return out;
}

static char *base64_encode(const unsigned char *data, size_t len) {
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t out_len;
  size_t i;
  size_t j;
  char *out;
  out_len = ((len + 2u) / 3u) * 4u;
  out = (char *)malloc(out_len + 1u);
  if (out == NULL) {
    return NULL;
  }
  i = 0u;
  j = 0u;
  while (i < len) {
    unsigned long octet_a;
    unsigned long octet_b;
    unsigned long octet_c;
    unsigned long triple;
    octet_a = data[i++];
    octet_b = i < len ? data[i++] : 0ul;
    octet_c = i < len ? data[i++] : 0ul;
    triple = (octet_a << 16) | (octet_b << 8) | octet_c;
    out[j++] = table[(triple >> 18) & 0x3ful];
    out[j++] = table[(triple >> 12) & 0x3ful];
    out[j++] = table[(triple >> 6) & 0x3ful];
    out[j++] = table[triple & 0x3ful];
  }
  if (len % 3u != 0u) {
    out[out_len - 1u] = '=';
    if (len % 3u == 1u) {
      out[out_len - 2u] = '=';
    }
  }
  out[out_len] = '\0';
  return out;
}

static int read_file_bytes(const char *path, unsigned char **out_data,
                           size_t *out_len) {
  FILE *file;
  long len;
  unsigned char *data;
  if (path == NULL || out_data == NULL || out_len == NULL) {
    return 0;
  }
  *out_data = NULL;
  *out_len = 0u;
  file = fopen(path, "rb");
  if (file == NULL) {
    return 0;
  }
  if (fseek(file, 0L, SEEK_END) != 0 || (len = ftell(file)) < 0L ||
      fseek(file, 0L, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }
  data = (unsigned char *)malloc((size_t)len + 1u);
  if (data == NULL) {
    fclose(file);
    return 0;
  }
  if ((size_t)len != 0u && fread(data, 1u, (size_t)len, file) != (size_t)len) {
    free(data);
    fclose(file);
    return 0;
  }
  if (ferror(file)) {
    free(data);
    fclose(file);
    return 0;
  }
  fclose(file);
  data[(size_t)len] = '\0';
  *out_data = data;
  *out_len = (size_t)len;
  return 1;
}

static int is_text_payload(const unsigned char *data, size_t len) {
  size_t i;
  for (i = 0u; i < len; ++i) {
    if (data[i] == 0u) {
      return 0;
    }
  }
  return 1;
}

static char *resolve_file_path(const char *raw) {
  const char *begin;
  const char *end;
  const char *home;
  char *path;
  size_t len;
  begin = raw;
  end = raw + strlen(raw);
  while (begin < end && (*begin == ' ' || *begin == '\t')) {
    ++begin;
  }
  while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) {
    --end;
  }
  if (end - begin >= 2 && ((*begin == '"' && end[-1] == '"') ||
                           (*begin == '\'' && end[-1] == '\''))) {
    ++begin;
    --end;
  }
  if (end - begin >= 1 && *begin == '~' &&
      (end - begin == 1 || begin[1] == '/')) {
    home = getenv("HOME");
    if (home == NULL) {
      return NULL;
    }
    len = strlen(home) + (size_t)(end - begin);
    path = (char *)malloc(len + 1u);
    if (path == NULL) {
      return NULL;
    }
    strcpy(path, home);
    if (end - begin > 1) {
      strcat(path, begin + 1);
    }
    return path;
  }
  return copy_range(begin, (size_t)(end - begin));
}

static char *expand_file_mutation(const char *expr, int enabled) {
  const char *prefix;
  size_t prefix_len;
  const char *stripped;
  const char *equal;
  char *path_part;
  char *file_path;
  unsigned char *data;
  size_t data_len;
  char *encoded;
  char *quoted;
  char *out;
  int base64;

  prefix = NULL;
  prefix_len = 0u;
  base64 = 0;
  if (strncmp(expr, "textfile:", 9u) == 0) {
    prefix = "textfile:";
    prefix_len = 9u;
  } else if (strncmp(expr, "base64file:", 11u) == 0) {
    prefix = "base64file:";
    prefix_len = 11u;
    base64 = 1;
  } else if (strncmp(expr, "file:", 5u) == 0) {
    prefix = "file:";
    prefix_len = 5u;
  }
  if (prefix == NULL) {
    return copy_string(expr);
  }
  if (!enabled) {
    fprintf(stderr, "clql: file-backed mutations are disabled\n");
    return NULL;
  }
  stripped = expr + prefix_len;
  if (strncmp(stripped, "rm:", 3u) == 0 ||
      strncmp(stripped, "remove:", 7u) == 0 ||
      strncmp(stripped, "time:", 5u) == 0) {
    fprintf(stderr, "clql: file-backed mutation cannot be combined with %s\n",
            stripped);
    return NULL;
  }
  if (strlen(stripped) >= 2u && stripped[strlen(stripped) - 1u] == '+' &&
      stripped[strlen(stripped) - 2u] == '+') {
    fprintf(stderr, "clql: file-backed mutation cannot be combined with ++\n");
    return NULL;
  }
  equal = strchr(stripped, '=');
  if (equal == NULL) {
    fprintf(stderr, "clql: file-backed mutation requires path=file\n");
    return NULL;
  }
  path_part = copy_range(stripped, (size_t)(equal - stripped));
  file_path = resolve_file_path(equal + 1);
  if (path_part == NULL || file_path == NULL) {
    free(path_part);
    free(file_path);
    fprintf(stderr, "clql: out of memory\n");
    return NULL;
  }
  if (!read_file_bytes(file_path, &data, &data_len)) {
    fprintf(stderr, "clql: unable to read mutation file: %s\n", file_path);
    free(path_part);
    free(file_path);
    return NULL;
  }
  if (prefix_len == 5u && !is_text_payload(data, data_len)) {
    base64 = 1;
  }
  if (base64) {
    encoded = base64_encode(data, data_len);
    quoted = encoded == NULL ? NULL
                             : json_quote_bytes((const unsigned char *)encoded,
                                                strlen(encoded));
    free(encoded);
  } else {
    quoted = json_quote_bytes(data, data_len);
  }
  free(data);
  free(file_path);
  if (quoted == NULL) {
    free(path_part);
    fprintf(stderr, "clql: out of memory\n");
    return NULL;
  }
  out = (char *)malloc(strlen(path_part) + 1u + strlen(quoted) + 1u);
  if (out == NULL) {
    free(path_part);
    free(quoted);
    fprintf(stderr, "clql: out of memory\n");
    return NULL;
  }
  strcpy(out, path_part);
  strcat(out, "=");
  strcat(out, quoted);
  free(path_part);
  free(quoted);
  return out;
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
      break;
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
    break;
  }
  while (arg < argc) {
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

static int prepare_mutations(const clql_config *cfg, clql_string_list *out) {
  size_t i;
  memset(out, 0, sizeof(*out));
  for (i = 0u; i < cfg->mutations.count; ++i) {
    char *expanded;
    expanded = expand_file_mutation(cfg->mutations.items[i],
                                    cfg->enable_file_mutations);
    if (expanded == NULL || !string_list_push_owned(out, expanded)) {
      free(expanded);
      string_list_destroy(out);
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
  request.projection = projection;
  request.mutation = mutation;
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
                         FILE *output) {
  char *expr;
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
  clql_string_list expanded_mutations;
  lql_stream_output_mode output_mode;
  lql_stream_result aggregate;
  lql_error error;
  lql_status status;
  size_t i;

  selector = NULL;
  projection = NULL;
  mutation = NULL;
  memset(&expanded_mutations, 0, sizeof(expanded_mutations));
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
    if (!prepare_mutations(cfg, &expanded_mutations)) {
      ctx->projection_destroy(ctx, projection);
      ctx->selector_destroy(ctx, selector);
      return 1;
    }
    status =
        ctx->mutation_parse(ctx, (const char *const *)expanded_mutations.items,
                            expanded_mutations.count, &mutation, &error);
    if (status != LQL_STATUS_OK) {
      string_list_destroy(&expanded_mutations);
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
    if (!open_input_path(input_path, &input)) {
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
  if (cfg->count_only) {
    fprintf(output, "%lu\n", (unsigned long)aggregate.records_matched);
  }
  string_list_destroy(&expanded_mutations);
  ctx->mutation_destroy(ctx, mutation);
  ctx->projection_destroy(ctx, projection);
  ctx->selector_destroy(ctx, selector);
  return 0;

fail:
  string_list_destroy(&expanded_mutations);
  ctx->mutation_destroy(ctx, mutation);
  ctx->projection_destroy(ctx, projection);
  ctx->selector_destroy(ctx, selector);
  return status == LQL_STATUS_OK ? 1 : 1;
}

static int run_inline(lql *ctx, const clql_config *cfg,
                      const clql_string_list *selectors,
                      const clql_string_list *inputs) {
  FILE *tmp;
  FILE *replacement;
  int rc;
  char buffer[8192];
  size_t amount;
  if (inputs->count != 1u || strcmp(inputs->items[0], "-") == 0) {
    fputs("clql: inline mode requires a single JSON file\n", stderr);
    return 2;
  }
  tmp = tmpfile();
  if (tmp == NULL) {
    fprintf(stderr, "clql: unable to create inline temp file\n");
    return 1;
  }
  rc = run_to_output(ctx, cfg, selectors, NULL, inputs->items[0], tmp);
  if (fflush(tmp) != 0 && rc == 0) {
    rc = 1;
  }
  if (rc != 0) {
    fclose(tmp);
    return rc;
  }
  if (fseek(tmp, 0L, SEEK_SET) != 0) {
    fclose(tmp);
    return 1;
  }
  replacement = fopen(inputs->items[0], "wb");
  if (replacement == NULL) {
    fclose(tmp);
    fprintf(stderr, "clql: unable to replace input file: %s\n",
            inputs->items[0]);
    return 1;
  }
  while ((amount = fread(buffer, 1u, sizeof(buffer), tmp)) != 0u) {
    if (fwrite(buffer, 1u, amount, replacement) != amount) {
      rc = 1;
      break;
    }
  }
  if (ferror(tmp)) {
    rc = 1;
  }
  if (fclose(replacement) != 0) {
    rc = 1;
  }
  fclose(tmp);
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
    if (cfg.inline_write) {
      rc = run_inline(ctx, &cfg, &selectors, &inputs);
    } else {
      rc = run_to_output(ctx, &cfg, &selectors, &inputs, "", stdout);
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
    rc = run_to_output(ctx, &cfg, &selectors, NULL, input_path, stdout);
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
