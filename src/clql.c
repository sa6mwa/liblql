#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "lql_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct projection_args {
  char **items;
  size_t count;
} projection_args;

typedef struct output_ranges {
  lql *ctx;
  FILE *source;
  FILE *out;
  const lql_projection *projection;
  const lql_mutation_plan *mutation_plan;
  int compact;
  int matches_only;
  lql_uint64 matched;
  lql_error callback_error;
} output_ranges;

static lql *clql_ctx = NULL;

static int is_regular_file_path(const char *path);

static lql_read_result clql_file_read(void *user, unsigned char *buffer,
                                      size_t capacity) {
  FILE *file;
  lql_read_result result;
  memset(&result, 0, sizeof(result));
  file = (FILE *)user;
  if (file == NULL || buffer == NULL) {
    result.error_code = 1;
    return result;
  }
  result.bytes_read = fread(buffer, 1u, capacity, file);
  if (result.bytes_read == 0u) {
    if (ferror(file)) {
      result.error_code = 1;
    } else {
      result.eof = 1;
    }
  }
  return result;
}

static int seek_u64(FILE *file, lql_uint64 offset) {
  off_t seek_offset;
  seek_offset = (off_t)offset;
  if (seek_offset < (off_t)0 || (lql_uint64)seek_offset != offset) {
    return 0;
  }
  return fseeko(file, seek_offset, SEEK_SET) == 0;
}

static int copy_range(FILE *in, FILE *out, lql_uint64 size) {
  char buf[8192];
  size_t want;
  size_t got;
  while (size != 0u) {
    want = size > (lql_uint64)sizeof(buf) ? sizeof(buf) : (size_t)size;
    got = fread(buf, 1u, want, in);
    if (got == 0u) {
      return 0;
    }
    if (fwrite(buf, 1u, got, out) != got) {
      return 0;
    }
    size -= (lql_uint64)got;
  }
  return 1;
}

static int range_root_kind(FILE *file, lql_uint64 offset, lql_uint64 size,
                           char *out_kind) {
  int ch;
  if (file == NULL || out_kind == NULL || !seek_u64(file, offset)) {
    return 0;
  }
  *out_kind = '\0';
  while (size != 0u) {
    ch = fgetc(file);
    if (ch == EOF) {
      return 0;
    }
    --size;
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      continue;
    }
    *out_kind = (char)ch;
    return 1;
  }
  return 0;
}

static int file_size_u64(FILE *file, lql_uint64 *out) {
  off_t end;
  if (file == NULL || out == NULL) {
    return 0;
  }
  if (fseeko(file, (off_t)0, SEEK_END) != 0) {
    return 0;
  }
  end = ftello(file);
  if (end < (off_t)0) {
    return 0;
  }
  *out = (lql_uint64)end;
  return (off_t)(*out) == end;
}

static lql_status
project_then_maybe_mutate_range(lql *ctx, const lql_projection *projection,
                                const lql_mutation_plan *mutation_plan,
                                int matched, FILE *source, lql_uint64 offset,
                                lql_uint64 size, FILE *out, int *out_projected,
                                lql_error *error) {
  FILE *projected_file;
  lql_uint64 projected_size;
  lql_status st;

  if (out_projected != NULL) {
    *out_projected = 0;
  }
  projected_file = tmpfile();
  if (projected_file == NULL) {
    lql_error_init(error);
    if (error != NULL) {
      error->code = LQL_STATUS_JSON_ERROR;
      strcpy(error->message, "failed to create projection temp file");
    }
    return LQL_STATUS_JSON_ERROR;
  }
  st = ctx->project_file_range(ctx, projection, source, offset, size,
                               projected_file, out_projected, error);
  if (st == LQL_STATUS_OK && out_projected != NULL && *out_projected) {
    if (!file_size_u64(projected_file, &projected_size)) {
      st = LQL_STATUS_JSON_ERROR;
      if (error != NULL) {
        error->code = st;
        strcpy(error->message, "failed to size projection temp file");
      }
    } else if (matched && mutation_plan != NULL) {
      st = ctx->mutate_file_range_paths(ctx, mutation_plan, projected_file, 0u,
                                        projected_size, out, error);
    } else if (!seek_u64(projected_file, 0u) ||
               !copy_range(projected_file, out, projected_size)) {
      st = LQL_STATUS_JSON_ERROR;
      if (error != NULL) {
        error->code = st;
        strcpy(error->message, "failed to write projected candidate");
      }
    }
  }
  fclose(projected_file);
  return st;
}

static void destroy_projection_args(projection_args *args) {
  if (args == NULL) {
    return;
  }
  lql_receiver_destroy(clql_ctx, args->items);
  args->items = NULL;
  args->count = 0u;
}

static int add_projection_arg(projection_args *args, const char *path) {
  char **next;
  if (args == NULL || path == NULL) {
    return 0;
  }
  next = (char **)lql_receiver_realloc(
      clql_ctx, args->items, sizeof(args->items[0]) * (args->count + 1u));
  if (next == NULL) {
    return 0;
  }
  args->items = next;
  args->items[args->count++] = (char *)path;
  return 1;
}

static void destroy_clql_ctx(void) {
  if (clql_ctx != NULL) {
    clql_ctx->destroy(clql_ctx);
    clql_ctx = NULL;
  }
}

static int parse_bool_text(const char *text, int *out) {
  if (text == NULL || out == NULL) {
    return 0;
  }
  if (strcmp(text, "true") == 0 || strcmp(text, "True") == 0 ||
      strcmp(text, "TRUE") == 0 || strcmp(text, "t") == 0 ||
      strcmp(text, "T") == 0 || strcmp(text, "1") == 0) {
    *out = 1;
    return 1;
  }
  if (strcmp(text, "false") == 0 || strcmp(text, "False") == 0 ||
      strcmp(text, "FALSE") == 0 || strcmp(text, "f") == 0 ||
      strcmp(text, "F") == 0 || strcmp(text, "0") == 0) {
    *out = 0;
    return 1;
  }
  return 0;
}

static int parse_long_bool_option(const char *arg, const char *name,
                                  int *target, int *matched) {
  size_t len;
  if (arg == NULL || name == NULL || target == NULL || matched == NULL) {
    return 0;
  }
  *matched = 0;
  len = strlen(name);
  if (strncmp(arg, name, len) != 0 || arg[len] != '=') {
    return 1;
  }
  *matched = 1;
  return parse_bool_text(arg + len + 1u, target);
}

static int parse_short_bool_value(const char *arg, size_t pos, int *target,
                                  const char **error_message) {
  if (arg[pos + 1u] != '=') {
    return 0;
  }
  if (!parse_bool_text(arg + pos + 2u, target)) {
    *error_message = "invalid boolean value for short option";
    return -1;
  }
  return 1;
}

static int
parse_short_option_cluster(char **argv, int argc, int *index, int *or_mode,
                           int *matches_only, int *compact, int *inline_mode,
                           int *enable_file_mutations, projection_args *fields,
                           projection_args *mutations, int *show_help,
                           int *show_version, const char **error_message) {
  const char *arg;
  size_t pos;
  int bool_status;

  arg = argv[*index];
  if (arg == NULL || arg[0] != '-' || arg[1] == '-' || arg[1] == '\0' ||
      arg[2] == '\0') {
    return 0;
  }
  pos = 1u;
  while (arg[pos] != '\0') {
    switch (arg[pos]) {
    case 'O':
      bool_status = parse_short_bool_value(arg, pos, or_mode, error_message);
      if (bool_status != 0) {
        return bool_status > 0 ? 1 : -1;
      }
      *or_mode = 1;
      ++pos;
      break;
    case 'M':
      bool_status =
          parse_short_bool_value(arg, pos, matches_only, error_message);
      if (bool_status != 0) {
        return bool_status > 0 ? 1 : -1;
      }
      *matches_only = 1;
      ++pos;
      break;
    case 'c':
      bool_status = parse_short_bool_value(arg, pos, compact, error_message);
      if (bool_status != 0) {
        return bool_status > 0 ? 1 : -1;
      }
      *compact = 1;
      ++pos;
      break;
    case 'i':
    case 'w':
      bool_status =
          parse_short_bool_value(arg, pos, inline_mode, error_message);
      if (bool_status != 0) {
        return bool_status > 0 ? 1 : -1;
      }
      *inline_mode = 1;
      ++pos;
      break;
    case 'F':
      bool_status = parse_short_bool_value(arg, pos, enable_file_mutations,
                                           error_message);
      if (bool_status != 0) {
        return bool_status > 0 ? 1 : -1;
      }
      *enable_file_mutations = 1;
      ++pos;
      break;
    case 'h':
      *show_help = 1;
      ++pos;
      break;
    case 'v':
      *show_version = 1;
      ++pos;
      break;
    case 'f': {
      const char *value;
      if (arg[pos + 1u] == '=') {
        value = arg + pos + 2u;
      } else if (arg[pos + 1u] != '\0') {
        value = arg + pos + 1u;
      } else if (*index + 1 >= argc) {
        *error_message = "field option requires an argument";
        return -1;
      } else {
        value = argv[++(*index)];
      }
      if (!add_projection_arg(fields, value)) {
        *error_message = "failed to record field path";
        return -1;
      }
      return 1;
    }
    case 'm': {
      const char *value;
      if (arg[pos + 1u] == '=') {
        value = arg + pos + 2u;
      } else if (arg[pos + 1u] != '\0') {
        value = arg + pos + 1u;
      } else if (*index + 1 >= argc) {
        *error_message = "mutation option requires an argument";
        return -1;
      } else {
        value = argv[++(*index)];
      }
      if (!add_projection_arg(mutations, value)) {
        *error_message = "failed to record mutation expression";
        return -1;
      }
      return 1;
    }
    default:
      return 0;
    }
  }
  return 1;
}

static char *join_selector_args(const projection_args *args, size_t skip_index,
                                int has_skip) {
  size_t i;
  size_t count;
  size_t total;
  char *out;
  char *cursor;

  count = 0u;
  total = 1u;
  for (i = 0u; i < args->count; ++i) {
    if (has_skip && i == skip_index) {
      continue;
    }
    total += strlen(args->items[i]);
    if (count != 0u) {
      ++total;
    }
    ++count;
  }
  if (count == 0u) {
    return lql_receiver_strdup(clql_ctx, "");
  }
  out = (char *)lql_receiver_alloc(clql_ctx, total);
  if (out == NULL) {
    return NULL;
  }
  cursor = out;
  count = 0u;
  for (i = 0u; i < args->count; ++i) {
    size_t len;
    if (has_skip && i == skip_index) {
      continue;
    }
    if (count != 0u) {
      *cursor++ = ',';
    }
    len = strlen(args->items[i]);
    memcpy(cursor, args->items[i], len);
    cursor += len;
    ++count;
  }
  *cursor = '\0';
  return out;
}

static char *join_selector_args_excluding_inputs(const projection_args *args) {
  size_t i;
  size_t count;
  size_t total;
  char *out;
  char *cursor;

  count = 0u;
  total = 1u;
  for (i = 0u; i < args->count; ++i) {
    if (strcmp(args->items[i], "-") == 0 ||
        is_regular_file_path(args->items[i])) {
      continue;
    }
    total += strlen(args->items[i]);
    if (count != 0u) {
      ++total;
    }
    ++count;
  }
  if (count == 0u) {
    return lql_receiver_strdup(clql_ctx, "");
  }
  out = (char *)lql_receiver_alloc(clql_ctx, total);
  if (out == NULL) {
    return NULL;
  }
  cursor = out;
  count = 0u;
  for (i = 0u; i < args->count; ++i) {
    size_t len;
    if (strcmp(args->items[i], "-") == 0 ||
        is_regular_file_path(args->items[i])) {
      continue;
    }
    if (count != 0u) {
      *cursor++ = ',';
    }
    len = strlen(args->items[i]);
    memcpy(cursor, args->items[i], len);
    cursor += len;
    ++count;
  }
  *cursor = '\0';
  return out;
}

static void collect_input_args(const projection_args *args,
                               projection_args *inputs) {
  size_t i;
  for (i = 0u; i < args->count; ++i) {
    if (strcmp(args->items[i], "-") == 0 ||
        is_regular_file_path(args->items[i])) {
      (void)add_projection_arg(inputs, args->items[i]);
    }
  }
}

static lql_status output_match_range(void *user,
                                     const lql_query_decision *decision) {
  output_ranges *ranges;
  ranges = (output_ranges *)user;
  if (!decision->matched && ranges->mutation_plan == NULL) {
    return LQL_STATUS_OK;
  }
  if (ranges->mutation_plan != NULL) {
    if (!decision->matched && ranges->matches_only) {
      return LQL_STATUS_OK;
    }
    if (ranges->projection != NULL) {
      int projected;
      if (project_then_maybe_mutate_range(
              ranges->ctx, ranges->projection, ranges->mutation_plan,
              decision->matched, ranges->source, decision->offset,
              decision->size, ranges->out, &projected,
              &ranges->callback_error) != LQL_STATUS_OK) {
        return LQL_STATUS_UNSUPPORTED;
      }
      if (!projected) {
        return LQL_STATUS_OK;
      }
    } else if (decision->matched) {
      char root_kind;
      if (!range_root_kind(ranges->source, decision->offset, decision->size,
                           &root_kind)) {
        return LQL_STATUS_JSON_ERROR;
      }
      if (root_kind == '[') {
        lql_query_result nested_result;
        memset(&nested_result, 0, sizeof(nested_result));
        if (ranges->ctx->mutate_file_range_candidates(
                ranges->ctx, NULL, ranges->mutation_plan, ranges->source,
                decision->offset, decision->size, ranges->out, ranges->compact,
                0, &nested_result, &ranges->callback_error) != LQL_STATUS_OK) {
          return LQL_STATUS_UNSUPPORTED;
        }
        ranges->matched += nested_result.candidates_matched;
        return LQL_STATUS_OK;
      } else if (root_kind != '{' && ranges->compact) {
        if (ranges->ctx->compact_file_range(
                ranges->ctx, ranges->source, decision->offset, decision->size,
                ranges->out, NULL) != LQL_STATUS_OK) {
          return LQL_STATUS_JSON_ERROR;
        }
      } else if (root_kind != '{') {
        if (!seek_u64(ranges->source, decision->offset) ||
            !copy_range(ranges->source, ranges->out, decision->size)) {
          return LQL_STATUS_JSON_ERROR;
        }
      } else if (ranges->ctx->mutate_file_range_paths(
                     ranges->ctx, ranges->mutation_plan, ranges->source,
                     decision->offset, decision->size, ranges->out,
                     &ranges->callback_error) != LQL_STATUS_OK) {
        return LQL_STATUS_UNSUPPORTED;
      }
    } else if (ranges->compact) {
      if (ranges->ctx->compact_file_range(ranges->ctx, ranges->source,
                                          decision->offset, decision->size,
                                          ranges->out, NULL) != LQL_STATUS_OK) {
        return LQL_STATUS_JSON_ERROR;
      }
    } else if (!seek_u64(ranges->source, decision->offset) ||
               !copy_range(ranges->source, ranges->out, decision->size)) {
      return LQL_STATUS_JSON_ERROR;
    }
  } else if (ranges->projection != NULL) {
    int projected;
    if (ranges->ctx->project_file_range(
            ranges->ctx, ranges->projection, ranges->source, decision->offset,
            decision->size, ranges->out, &projected, NULL) != LQL_STATUS_OK) {
      return LQL_STATUS_JSON_ERROR;
    }
    if (!projected) {
      return LQL_STATUS_OK;
    }
  } else {
    if (ranges->compact) {
      if (ranges->ctx->compact_file_range(ranges->ctx, ranges->source,
                                          decision->offset, decision->size,
                                          ranges->out, NULL) != LQL_STATUS_OK) {
        return LQL_STATUS_JSON_ERROR;
      }
    } else {
      if (!seek_u64(ranges->source, decision->offset) ||
          !copy_range(ranges->source, ranges->out, decision->size)) {
        return LQL_STATUS_JSON_ERROR;
      }
    }
  }
  if (fputc('\n', ranges->out) == EOF) {
    return LQL_STATUS_JSON_ERROR;
  }
  ++ranges->matched;
  return LQL_STATUS_OK;
}

static lql_status output_payload_match(void *user,
                                       const lql_query_match *match) {
  output_ranges *ranges;
  int projected;
  ranges = (output_ranges *)user;
  if (match == NULL || ranges == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (ranges->projection != NULL) {
    projected = 0;
    if (ranges->ctx->payload_project_json(
            ranges->ctx, &match->payload, ranges->projection, ranges->out,
            &projected, &ranges->callback_error) != LQL_STATUS_OK) {
      return ranges->callback_error.code;
    }
    if (!projected) {
      return LQL_STATUS_OK;
    }
  } else if (ranges->ctx->payload_write_json(
                 ranges->ctx, &match->payload, ranges->out,
                 &ranges->callback_error) != LQL_STATUS_OK) {
    return ranges->callback_error.code;
  }
  if (fputc('\n', ranges->out) == EOF) {
    return LQL_STATUS_JSON_ERROR;
  }
  ++ranges->matched;
  return LQL_STATUS_OK;
}

static void close_input_path(FILE *file) {
  if (file != NULL && file != stdin) {
    fclose(file);
  }
}

static int is_regular_file_path(const char *path) {
  struct stat st;
  if (path == NULL || strcmp(path, "-") == 0) {
    return 0;
  }
  if (stat(path, &st) != 0) {
    return 0;
  }
  return S_ISREG(st.st_mode) != 0;
}

static int create_inline_temp(const char *path, char **out_path,
                              FILE **out_file) {
  char *template_path;
  size_t len;
  int fd;
  FILE *file;
  struct stat st;
  len = strlen(path);
  template_path =
      (char *)lql_receiver_alloc(clql_ctx,
                                 len + strlen(".lql-XXXXXX") + 1u);
  if (template_path == NULL) {
    return 0;
  }
  memcpy(template_path, path, len);
  memcpy(template_path + len, ".lql-XXXXXX", strlen(".lql-XXXXXX") + 1u);
  fd = mkstemp(template_path);
  if (fd < 0) {
    lql_receiver_destroy(clql_ctx, template_path);
    return 0;
  }
  if (stat(path, &st) == 0) {
    (void)fchmod(fd, st.st_mode);
  }
  file = fdopen(fd, "wb");
  if (file == NULL) {
    close(fd);
    unlink(template_path);
    lql_receiver_destroy(clql_ctx, template_path);
    return 0;
  }
  *out_path = template_path;
  *out_file = file;
  return 1;
}

static void usage(FILE *out) {
  fprintf(out, "clql - query, project, and mutate JSON with LQL selectors\n");
  fprintf(out, "\n");
  fprintf(out, "Usage:\n");
  fprintf(out, "  clql [options] <selector> [file]\n");
  fprintf(out, "  clql [options] <selector> < input.json\n");
  fprintf(out, "  clql --help\n");
  fprintf(out, "  clql --version\n");
  fprintf(out, "\n");
  fprintf(out, "Input:\n");
  fprintf(out, "  file                       read JSON from file; use - for stdin\n");
  fprintf(out, "  stdin                      used when no file is provided\n");
  fprintf(out, "\n");
  fprintf(out, "Selection:\n");
  fprintf(out, "  -O, --or[=bool]            combine selector terms with OR\n");
  fprintf(out, "  -M, --matches-only[=bool]  emit only matched candidates\n");
  fprintf(out, "  -c, --compact[=bool]       emit compact JSON\n");
  fprintf(out, "\n");
  fprintf(out, "Projection:\n");
  fprintf(out, "  -f, --field <path>         project a field from each match; repeatable\n");
  fprintf(out, "      --field=<path>         same as --field <path>\n");
  fprintf(out, "\n");
  fprintf(out, "Mutation:\n");
  fprintf(out, "  -m, --mutate <expr>        apply a mutation expression; repeatable\n");
  fprintf(out, "      --mutate=<expr>        same as --mutate <expr>\n");
  fprintf(out, "  -i, --inline[=bool]        rewrite the input file in place\n");
  fprintf(out, "  -w, --write[=bool]         alias for --inline\n");
  fprintf(out, "  -F, --enable-file-mutations[=bool]\n");
  fprintf(out, "                             allow file-backed mutation values\n");
  fprintf(out, "\n");
  fprintf(out, "Commands:\n");
  fprintf(out, "  -h, --help                 show this help\n");
  fprintf(out, "  -v, --version              show the clql version\n");
  fprintf(out, "\n");
  fprintf(out, "Selector examples (shorthand):\n");
  fprintf(out, "  clql '/status=\"open\"' data.json\n");
  fprintf(out, "  clql '/status!=closed' data.json\n");
  fprintf(out, "  clql '/progress>=50' data.json\n");
  fprintf(out, "  clql '/timestamp>=\"2025-01-01T00:00:00Z\"' data.json\n");
  fprintf(out, "  clql '/devices/0/status=\"online\"' data.json\n");
  fprintf(out, "  clql '/labels/*=\"production\"' data.json\n");
  fprintf(out, "  clql '/items[]/sku=\"ABC-123\"' data.json\n");
  fprintf(out, "  clql '/items/**/sku=\"ABC-123\"' data.json\n");
  fprintf(out, "  clql '/items/.../sku=\"ABC-123\"' data.json\n");
  fprintf(out, "\n");
  fprintf(out, "Selector examples (full LQL):\n");
  fprintf(out, "  clql 'eq{field=/status,value=open}' data.json\n");
  fprintf(out, "  clql 'contains{field=/msg,value=timeout,ic=t}' data.json\n");
  fprintf(out, "  clql 'contains{field=/msg,any=timeout|degraded}' data.json\n");
  fprintf(out, "  clql 'icontains{field=/msg,value=timeout}' data.json\n");
  fprintf(out, "  clql 'icontains{field=/service,a=AUTH|EDGE}' data.json\n");
  fprintf(out, "  clql 'iprefix{field=/service,value=auth}' data.json\n");
  fprintf(out,
          "  clql 'date{field=/timestamp,after=2025-01-01,before=2025-02-01}' "
          "data.json\n");
  fprintf(out, "  clql 'date{f=/timestamp,since=yesterday}' data.json\n");
  fprintf(out,
          "  clql "
          "'and.eq{field=/status,value=open},and.range{field=/progress,gte=50}' "
          "data.json\n");
  fprintf(out,
          "  clql "
          "'or.eq{field=/region,value=us},or.eq{field=/region,value=eu}' "
          "data.json\n");
  fprintf(out, "  clql 'not.eq{field=/state,value=disabled}' data.json\n");
  fprintf(out, "  clql 'exists{/metadata/etag}' data.json\n");
  fprintf(out, "\n");
  fprintf(out, "Projection and mutation examples:\n");
  fprintf(out, "  clql -c -f /owner/name '/priority>=3' < data.json\n");
  fprintf(out, "  clql -m '/status=\"closed\"' -i '/id=\"42\"' data.json\n");
  fprintf(out, "  clql -O '/status=\"open\"' '/status=\"queued\"' data.json\n");
  fprintf(out, "\n");
  fprintf(out, "Notes:\n");
  fprintf(out, "  With -m, selectors choose which objects are mutated. Add -M to output\n");
  fprintf(out, "  only selector matches. contains/icontains accept value=... or\n");
  fprintf(out, "  any=/a=... pipe-delimited lists. range accepts numeric or datetime\n");
  fprintf(out, "  literals. date supports value, after, before, gt, gte, lt, and lte;\n");
  fprintf(out, "  only date{...,since=...} supports relative macros such as now, today,\n");
  fprintf(out, "  and yesterday.\n");
}

int main(int argc, char **argv) {
  lql_selector *selector;
  lql_error error;
  const char *selector_expr;
  const char *input_path;
  char *selector_expr_owned;
  FILE *input;
  FILE *range_source;
  int matches_only;
  int or_mode;
  int compact;
  int inline_mode;
  int enable_file_mutations;
  int end_options;
  int show_help;
  int show_version;
  int i;
  projection_args fields;
  projection_args mutations;
  projection_args positionals;
  projection_args input_paths;
  lql_projection *projection;
  lql_mutation_plan *mutation_plan;
  lql_mutation_parse_options mutation_options;
  lql_status st;
  output_ranges ranges;
  lql_query_result result;
  char *inline_tmp_path;
  FILE *inline_out;

  memset(&fields, 0, sizeof(fields));
  memset(&mutations, 0, sizeof(mutations));
  memset(&positionals, 0, sizeof(positionals));
  memset(&input_paths, 0, sizeof(input_paths));
  projection = NULL;
  mutation_plan = NULL;
  or_mode = 0;
  matches_only = 0;
  compact = 0;
  inline_mode = 0;
  enable_file_mutations = 0;
  end_options = 0;
  show_help = 0;
  show_version = 0;
  inline_tmp_path = NULL;
  inline_out = NULL;
  selector_expr = NULL;
  selector_expr_owned = NULL;
  input_path = NULL;
  lql_error_init(&error);
  st = lql_new(&clql_ctx, &error);
  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "clql: %s\n", error.message);
    return 1;
  }
  (void)atexit(destroy_clql_ctx);
  for (i = 1; i < argc; ++i) {
    if (end_options) {
      if (!add_projection_arg(&positionals, argv[i])) {
        fprintf(stderr, "clql: failed to record positional argument\n");
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--") == 0) {
      end_options = 1;
      continue;
    }
    if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
      printf("clql %s\n", clql_ctx->version(clql_ctx));
      destroy_projection_args(&fields);
      destroy_projection_args(&mutations);
      destroy_projection_args(&positionals);
      return 0;
    }
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(stdout);
      destroy_projection_args(&fields);
      destroy_projection_args(&mutations);
      destroy_projection_args(&positionals);
      return 0;
    }
    {
      const char *cluster_error;
      int cluster_status;
      cluster_error = NULL;
      cluster_status = parse_short_option_cluster(
          argv, argc, &i, &or_mode, &matches_only, &compact, &inline_mode,
          &enable_file_mutations, &fields, &mutations, &show_help,
          &show_version, &cluster_error);
      if (cluster_status < 0) {
        fprintf(stderr, "clql: %s\n", cluster_error);
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
      if (cluster_status > 0) {
        if (show_help) {
          usage(stdout);
          destroy_projection_args(&fields);
          destroy_projection_args(&mutations);
          destroy_projection_args(&positionals);
          return 0;
        }
        if (show_version) {
          printf("clql %s\n", clql_ctx->version(clql_ctx));
          destroy_projection_args(&fields);
          destroy_projection_args(&mutations);
          destroy_projection_args(&positionals);
          return 0;
        }
        continue;
      }
    }
    if (strcmp(argv[i], "--or") == 0 || strcmp(argv[i], "-O") == 0) {
      or_mode = 1;
    } else if (strncmp(argv[i], "--or=", 5u) == 0) {
      int matched;
      if (!parse_long_bool_option(argv[i], "--or", &or_mode, &matched)) {
        fprintf(stderr, "clql: invalid boolean value for --or\n");
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
    } else if (strcmp(argv[i], "--matches-only") == 0 ||
               strcmp(argv[i], "-M") == 0) {
      matches_only = 1;
    } else if (strncmp(argv[i], "--matches-only=", 15u) == 0) {
      int matched;
      if (!parse_long_bool_option(argv[i], "--matches-only", &matches_only,
                                  &matched)) {
        fprintf(stderr, "clql: invalid boolean value for --matches-only\n");
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
    } else if (strcmp(argv[i], "--compact") == 0 ||
               strcmp(argv[i], "-c") == 0) {
      compact = 1;
    } else if (strncmp(argv[i], "--compact=", 10u) == 0) {
      int matched;
      if (!parse_long_bool_option(argv[i], "--compact", &compact, &matched)) {
        fprintf(stderr, "clql: invalid boolean value for --compact\n");
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
    } else if (strcmp(argv[i], "--inline") == 0 || strcmp(argv[i], "-i") == 0 ||
               strcmp(argv[i], "--write") == 0 || strcmp(argv[i], "-w") == 0) {
      inline_mode = 1;
    } else if (strncmp(argv[i], "--inline=", 9u) == 0) {
      int matched;
      if (!parse_long_bool_option(argv[i], "--inline", &inline_mode,
                                  &matched)) {
        fprintf(stderr, "clql: invalid boolean value for --inline\n");
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
    } else if (strncmp(argv[i], "--write=", 8u) == 0) {
      int matched;
      if (!parse_long_bool_option(argv[i], "--write", &inline_mode, &matched)) {
        fprintf(stderr, "clql: invalid boolean value for --write\n");
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
    } else if (strcmp(argv[i], "--enable-file-mutations") == 0 ||
               strcmp(argv[i], "-F") == 0) {
      enable_file_mutations = 1;
    } else if (strncmp(argv[i], "--enable-file-mutations=", 24u) == 0) {
      int matched;
      if (!parse_long_bool_option(argv[i], "--enable-file-mutations",
                                  &enable_file_mutations, &matched)) {
        fprintf(stderr,
                "clql: invalid boolean value for --enable-file-mutations\n");
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
    } else if (strcmp(argv[i], "--mutate") == 0 || strcmp(argv[i], "-m") == 0) {
      if (i + 1 >= argc || !add_projection_arg(&mutations, argv[++i])) {
        fprintf(stderr, "clql: failed to record mutation expression\n");
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
    } else if (strncmp(argv[i], "--mutate=", 9u) == 0) {
      if (!add_projection_arg(&mutations, argv[i] + 9u)) {
        fprintf(stderr, "clql: failed to record mutation expression\n");
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
    } else if (strcmp(argv[i], "--field") == 0 || strcmp(argv[i], "-f") == 0) {
      if (i + 1 >= argc || !add_projection_arg(&fields, argv[++i])) {
        fprintf(stderr, "clql: failed to record field path\n");
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
    } else if (strncmp(argv[i], "--field=", 8u) == 0) {
      if (!add_projection_arg(&fields, argv[i] + 8u)) {
        fprintf(stderr, "clql: failed to record field path\n");
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
    } else if (strcmp(argv[i], "-") == 0) {
      if (!add_projection_arg(&positionals, argv[i])) {
        fprintf(stderr, "clql: failed to record positional argument\n");
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "clql: unknown option %s\n", argv[i]);
      usage(stderr);
      destroy_projection_args(&fields);
      destroy_projection_args(&mutations);
      destroy_projection_args(&positionals);
      return 2;
    } else {
      if (!add_projection_arg(&positionals, argv[i])) {
        fprintf(stderr, "clql: failed to record positional argument\n");
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&positionals);
        return 2;
      }
    }
  }
  if (mutations.count != 0u) {
    collect_input_args(&positionals, &input_paths);
    if (input_paths.count != 0u) {
      input_path = input_paths.items[0];
    }
    selector_expr_owned = join_selector_args_excluding_inputs(&positionals);
  } else {
    size_t input_index;
    int has_input;
    has_input = 0;
    input_index = positionals.count;
    if (positionals.count != 0u) {
      input_index = positionals.count - 1u;
      if (strcmp(positionals.items[input_index], "-") == 0 ||
          is_regular_file_path(positionals.items[input_index])) {
        input_path = positionals.items[input_index];
        has_input = 1;
      }
    }
    selector_expr_owned =
        join_selector_args(&positionals, input_index, has_input);
  }
  if (selector_expr_owned == NULL) {
    fprintf(stderr, "clql: failed to build selector expression\n");
    destroy_projection_args(&fields);
    destroy_projection_args(&mutations);
    destroy_projection_args(&positionals);
    destroy_projection_args(&input_paths);
    return 2;
  }
  selector_expr = selector_expr_owned;
  if (argc == 1) {
    usage(stderr);
    lql_receiver_destroy(clql_ctx, selector_expr_owned);
    destroy_projection_args(&fields);
    destroy_projection_args(&mutations);
    destroy_projection_args(&positionals);
    destroy_projection_args(&input_paths);
    return 2;
  }
  destroy_projection_args(&positionals);
  if (fields.count != 0u) {
    st = clql_ctx->projection_parse(clql_ctx, (const char *const *)fields.items,
                                    fields.count, &projection, &error);
    if (st != LQL_STATUS_OK) {
      fprintf(stderr, "clql: %s\n", error.message);
      lql_receiver_destroy(clql_ctx, selector_expr_owned);
      destroy_projection_args(&fields);
      destroy_projection_args(&mutations);
      destroy_projection_args(&input_paths);
      return 2;
    }
  }
  if (mutations.count != 0u) {
    memset(&mutation_options, 0, sizeof(mutation_options));
    mutation_options.enable_file_values = enable_file_mutations;
    mutation_options.file_value_base_dir = ".";
    st = clql_ctx->mutation_plan_parse_with_options(
        clql_ctx, (const char *const *)mutations.items, mutations.count,
        &mutation_options, &mutation_plan, &error);
    if (st != LQL_STATUS_OK) {
      fprintf(stderr, "clql: %s\n", error.message);
      lql_receiver_destroy(clql_ctx, selector_expr_owned);
      clql_ctx->projection_destroy(clql_ctx, projection);
      destroy_projection_args(&fields);
      destroy_projection_args(&mutations);
      destroy_projection_args(&input_paths);
      return 2;
    }
  }
  st = or_mode ? clql_ctx->selector_parse_or(clql_ctx, selector_expr, &selector,
                                             &error)
               : clql_ctx->selector_parse(clql_ctx, selector_expr, &selector,
                                          &error);
  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "clql: %s\n", error.message);
    lql_receiver_destroy(clql_ctx, selector_expr_owned);
    clql_ctx->mutation_plan_destroy(clql_ctx, mutation_plan);
    clql_ctx->projection_destroy(clql_ctx, projection);
    destroy_projection_args(&fields);
    destroy_projection_args(&mutations);
    destroy_projection_args(&input_paths);
    return 2;
  }
  if (inline_mode && mutation_plan == NULL) {
    fprintf(stderr, "clql: inline mode requires mutation expressions\n");
    lql_receiver_destroy(clql_ctx, selector_expr_owned);
    clql_ctx->selector_destroy(clql_ctx, selector);
    clql_ctx->mutation_plan_destroy(clql_ctx, mutation_plan);
    clql_ctx->projection_destroy(clql_ctx, projection);
    destroy_projection_args(&fields);
    destroy_projection_args(&mutations);
    destroy_projection_args(&input_paths);
    return 2;
  }
  if (inline_mode && input_paths.count == 0u) {
    fprintf(stderr, "clql: inline mode requires a file path\n");
    lql_receiver_destroy(clql_ctx, selector_expr_owned);
    clql_ctx->selector_destroy(clql_ctx, selector);
    clql_ctx->mutation_plan_destroy(clql_ctx, mutation_plan);
    clql_ctx->projection_destroy(clql_ctx, projection);
    destroy_projection_args(&fields);
    destroy_projection_args(&mutations);
    destroy_projection_args(&input_paths);
    return 2;
  }
  if (inline_mode && (input_paths.count != 1u || input_path == NULL ||
                      strcmp(input_path, "-") == 0)) {
    fprintf(stderr, "clql: inline mode requires a single JSON file\n");
    lql_receiver_destroy(clql_ctx, selector_expr_owned);
    clql_ctx->selector_destroy(clql_ctx, selector);
    clql_ctx->mutation_plan_destroy(clql_ctx, mutation_plan);
    clql_ctx->projection_destroy(clql_ctx, projection);
    destroy_projection_args(&fields);
    destroy_projection_args(&mutations);
    destroy_projection_args(&input_paths);
    return 2;
  }
  if (mutation_plan != NULL && !inline_mode && input_paths.count > 1u) {
    for (i = 0; i < (int)input_paths.count; ++i) {
      if (strcmp(input_paths.items[i], "-") == 0) {
        memset(&result, 0, sizeof(result));
        if (projection != NULL) {
          st = clql_ctx->mutate_source_projected_candidates(
              clql_ctx, selector, projection, mutation_plan, clql_file_read,
              stdin, stdout, compact, matches_only, &result, &error);
        } else {
          st = clql_ctx->mutate_source_candidates(
              clql_ctx, selector, mutation_plan, clql_file_read, stdin, stdout,
              compact, matches_only, &result, &error);
        }
        if (st != LQL_STATUS_OK) {
          fprintf(stderr, "clql: %s\n", error.message);
          lql_receiver_destroy(clql_ctx, selector_expr_owned);
          clql_ctx->selector_destroy(clql_ctx, selector);
          clql_ctx->mutation_plan_destroy(clql_ctx, mutation_plan);
          clql_ctx->projection_destroy(clql_ctx, projection);
          destroy_projection_args(&fields);
          destroy_projection_args(&mutations);
          destroy_projection_args(&input_paths);
          return 1;
        }
        continue;
      }
      input = fopen(input_paths.items[i], "rb");
      range_source = fopen(input_paths.items[i], "rb");
      if (input == NULL || range_source == NULL) {
        fprintf(stderr, "clql: failed to open input %s\n",
                input_paths.items[i]);
        close_input_path(input);
        close_input_path(range_source);
        lql_receiver_destroy(clql_ctx, selector_expr_owned);
        clql_ctx->selector_destroy(clql_ctx, selector);
        clql_ctx->mutation_plan_destroy(clql_ctx, mutation_plan);
        clql_ctx->projection_destroy(clql_ctx, projection);
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&input_paths);
        return 1;
      }
      memset(&ranges, 0, sizeof(ranges));
      ranges.ctx = clql_ctx;
      memset(&result, 0, sizeof(result));
      ranges.source = range_source;
      ranges.out = stdout;
      ranges.projection = projection;
      ranges.mutation_plan = mutation_plan;
      ranges.compact = compact;
      ranges.matches_only = matches_only;
      lql_error_init(&ranges.callback_error);
      st = clql_ctx->query_file_decisions(clql_ctx, selector, input,
                                          output_match_range, &ranges, &result,
                                          &error);
      if (st != LQL_STATUS_OK && ranges.callback_error.code != LQL_STATUS_OK) {
        error = ranges.callback_error;
      }
      close_input_path(input);
      close_input_path(range_source);
      if (st != LQL_STATUS_OK) {
        fprintf(stderr, "clql: %s\n", error.message);
        lql_receiver_destroy(clql_ctx, selector_expr_owned);
        clql_ctx->selector_destroy(clql_ctx, selector);
        clql_ctx->mutation_plan_destroy(clql_ctx, mutation_plan);
        clql_ctx->projection_destroy(clql_ctx, projection);
        destroy_projection_args(&fields);
        destroy_projection_args(&mutations);
        destroy_projection_args(&input_paths);
        return 1;
      }
    }
    lql_receiver_destroy(clql_ctx, selector_expr_owned);
    clql_ctx->selector_destroy(clql_ctx, selector);
    clql_ctx->mutation_plan_destroy(clql_ctx, mutation_plan);
    clql_ctx->projection_destroy(clql_ctx, projection);
    destroy_projection_args(&fields);
    destroy_projection_args(&mutations);
    destroy_projection_args(&input_paths);
    return 0;
  }
  if (input_path != NULL && strcmp(input_path, "-") != 0) {
    input = fopen(input_path, "rb");
    range_source = fopen(input_path, "rb");
    if (input == NULL || range_source == NULL) {
      fprintf(stderr, "clql: failed to open input %s\n", input_path);
      lql_receiver_destroy(clql_ctx, selector_expr_owned);
      close_input_path(input);
      close_input_path(range_source);
      clql_ctx->selector_destroy(clql_ctx, selector);
      clql_ctx->mutation_plan_destroy(clql_ctx, mutation_plan);
      clql_ctx->projection_destroy(clql_ctx, projection);
      destroy_projection_args(&fields);
      destroy_projection_args(&mutations);
      destroy_projection_args(&input_paths);
      return 1;
    }
    memset(&ranges, 0, sizeof(ranges));
    ranges.ctx = clql_ctx;
    memset(&result, 0, sizeof(result));
    if (inline_mode &&
        !create_inline_temp(input_path, &inline_tmp_path, &inline_out)) {
      fprintf(stderr, "clql: failed to create inline temp file\n");
      lql_receiver_destroy(clql_ctx, selector_expr_owned);
      close_input_path(input);
      close_input_path(range_source);
      clql_ctx->selector_destroy(clql_ctx, selector);
      clql_ctx->mutation_plan_destroy(clql_ctx, mutation_plan);
      clql_ctx->projection_destroy(clql_ctx, projection);
      destroy_projection_args(&fields);
      destroy_projection_args(&mutations);
      destroy_projection_args(&input_paths);
      return 1;
    }
    ranges.source = range_source;
    ranges.out = inline_mode ? inline_out : stdout;
    ranges.projection = projection;
    ranges.mutation_plan = mutation_plan;
    ranges.compact = compact;
    ranges.matches_only = matches_only;
    lql_error_init(&ranges.callback_error);
    st = clql_ctx->query_file_decisions(clql_ctx, selector, input,
                                        output_match_range, &ranges, &result,
                                        &error);
    if (st != LQL_STATUS_OK && ranges.callback_error.code != LQL_STATUS_OK) {
      error = ranges.callback_error;
    }
    close_input_path(input);
    close_input_path(range_source);
    if (inline_mode) {
      if (fclose(inline_out) != 0 && st == LQL_STATUS_OK) {
        st = LQL_STATUS_JSON_ERROR;
        error.code = LQL_STATUS_JSON_ERROR;
        strcpy(error.message, "failed to close inline temp file");
      }
      inline_out = NULL;
      if (st == LQL_STATUS_OK && result.candidates_seen == 0u) {
        st = LQL_STATUS_JSON_ERROR;
        error.code = LQL_STATUS_JSON_ERROR;
        strcpy(error.message, "no JSON input");
      }
      if (st == LQL_STATUS_OK && rename(inline_tmp_path, input_path) != 0) {
        st = LQL_STATUS_JSON_ERROR;
        error.code = LQL_STATUS_JSON_ERROR;
        strcpy(error.message, "failed to replace inline input file");
      }
      if (st != LQL_STATUS_OK) {
        unlink(inline_tmp_path);
      }
      lql_receiver_destroy(clql_ctx, inline_tmp_path);
      inline_tmp_path = NULL;
    }
    lql_receiver_destroy(clql_ctx, selector_expr_owned);
    clql_ctx->selector_destroy(clql_ctx, selector);
    clql_ctx->mutation_plan_destroy(clql_ctx, mutation_plan);
    clql_ctx->projection_destroy(clql_ctx, projection);
    destroy_projection_args(&fields);
    destroy_projection_args(&mutations);
    destroy_projection_args(&input_paths);
    if (st != LQL_STATUS_OK) {
      fprintf(stderr, "clql: %s\n", error.message);
      return 1;
    }
    return 0;
  }
  memset(&result, 0, sizeof(result));
  if (mutation_plan != NULL) {
    if (projection != NULL) {
      st = clql_ctx->mutate_source_projected_candidates(
          clql_ctx, selector, projection, mutation_plan, clql_file_read, stdin,
          stdout, compact, matches_only, &result, &error);
    } else {
      st = clql_ctx->mutate_source_candidates(
          clql_ctx, selector, mutation_plan, clql_file_read, stdin, stdout,
          compact, matches_only, &result, &error);
    }
  } else {
    memset(&ranges, 0, sizeof(ranges));
    ranges.ctx = clql_ctx;
    ranges.out = stdout;
    ranges.projection = projection;
    ranges.compact = compact;
    lql_error_init(&ranges.callback_error);
    st = clql_ctx->query_source_spooled_matches(
        clql_ctx, selector, clql_file_read, stdin, output_payload_match,
        &ranges, &result, &error);
    if (st != LQL_STATUS_OK && ranges.callback_error.code != LQL_STATUS_OK) {
      error = ranges.callback_error;
    }
  }
  clql_ctx->selector_destroy(clql_ctx, selector);
  lql_receiver_destroy(clql_ctx, selector_expr_owned);
  clql_ctx->mutation_plan_destroy(clql_ctx, mutation_plan);
  clql_ctx->projection_destroy(clql_ctx, projection);
  destroy_projection_args(&fields);
  destroy_projection_args(&mutations);
  destroy_projection_args(&input_paths);
  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "clql: %s\n", error.message);
    return 1;
  }
  return 0;
}
