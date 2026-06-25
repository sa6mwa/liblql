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

typedef struct match_count {
  lql_uint64 matched;
} match_count;

typedef struct output_ranges {
  FILE *source;
  FILE *out;
  const lql_projection *projection;
  const lql_mutation_plan *mutation_plan;
  int compact;
  int matches_only;
  lql_uint64 matched;
  lql_error callback_error;
} output_ranges;

static lql_status count_match(void *user, const lql_query_decision *decision) {
  match_count *count;
  count = (match_count *)user;
  if (decision->matched) {
    ++count->matched;
  }
  return LQL_STATUS_OK;
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

static void free_projection_args(projection_args *args) {
  if (args == NULL) {
    return;
  }
  free(args->items);
  args->items = NULL;
  args->count = 0u;
}

static int add_projection_arg(projection_args *args, const char *path) {
  char **next;
  if (args == NULL || path == NULL) {
    return 0;
  }
  next = (char **)realloc(args->items,
                          sizeof(args->items[0]) * (args->count + 1u));
  if (next == NULL) {
    return 0;
  }
  args->items = next;
  args->items[args->count++] = (char *)path;
  return 1;
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
    if (decision->matched) {
      if (lql_mutate_file_range_paths(ranges->mutation_plan, ranges->source,
                                      decision->offset, decision->size,
                                      ranges->out, &ranges->callback_error) !=
          LQL_STATUS_OK) {
        return LQL_STATUS_UNSUPPORTED;
      }
    } else if (ranges->compact) {
      if (lql_compact_file_range(ranges->source, decision->offset,
                                 decision->size, ranges->out,
                                 NULL) != LQL_STATUS_OK) {
        return LQL_STATUS_JSON_ERROR;
      }
    } else if (!seek_u64(ranges->source, decision->offset) ||
               !copy_range(ranges->source, ranges->out, decision->size)) {
      return LQL_STATUS_JSON_ERROR;
    }
  } else if (ranges->projection != NULL) {
    int projected;
    if (lql_project_file_range(ranges->projection, ranges->source,
                               decision->offset, decision->size, ranges->out,
                               &projected, NULL) != LQL_STATUS_OK) {
      return LQL_STATUS_JSON_ERROR;
    }
    if (!projected) {
      return LQL_STATUS_OK;
    }
  } else {
    if (ranges->compact) {
      if (lql_compact_file_range(ranges->source, decision->offset,
                                 decision->size, ranges->out,
                                 NULL) != LQL_STATUS_OK) {
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

static FILE *open_input_path(const char *path) {
  if (path == NULL || strcmp(path, "-") == 0) {
    return stdin;
  }
  return fopen(path, "rb");
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
  template_path = (char *)malloc(len + strlen(".lql-XXXXXX") + 1u);
  if (template_path == NULL) {
    return 0;
  }
  memcpy(template_path, path, len);
  memcpy(template_path + len, ".lql-XXXXXX", strlen(".lql-XXXXXX") + 1u);
  fd = mkstemp(template_path);
  if (fd < 0) {
    free(template_path);
    return 0;
  }
  if (stat(path, &st) == 0) {
    (void)fchmod(fd, st.st_mode);
  }
  file = fdopen(fd, "wb");
  if (file == NULL) {
    close(fd);
    unlink(template_path);
    free(template_path);
    return 0;
  }
  *out_path = template_path;
  *out_file = file;
  return 1;
}

static void usage(FILE *out) {
  fprintf(out, "usage: clql [--or|-O] [-c] [-i|-w] [-F] [-t theme] [-f field] "
               "[-m expr] [--matches-only|-M] selector [data.json]\n");
  fprintf(out, "       clql [--or|-O] [-c] [--matches-only|-M] selector < "
               "data.json\n");
  fprintf(out, "       clql --version\n");
}

int main(int argc, char **argv) {
  lql_selector *selector;
  lql_error error;
  const char *selector_expr;
  const char *input_path;
  FILE *input;
  FILE *range_source;
  int matches_only;
  int or_mode;
  int compact;
  int inline_mode;
  int enable_file_mutations;
  int i;
  projection_args fields;
  projection_args mutations;
  lql_projection *projection;
  lql_mutation_plan *mutation_plan;
  lql_mutation_parse_options mutation_options;
  lql_status st;
  match_count count;
  output_ranges ranges;
  lql_query_result result;
  char *inline_tmp_path;
  FILE *inline_out;

  memset(&fields, 0, sizeof(fields));
  memset(&mutations, 0, sizeof(mutations));
  projection = NULL;
  mutation_plan = NULL;
  or_mode = 0;
  matches_only = 0;
  compact = 0;
  inline_mode = 0;
  enable_file_mutations = 0;
  inline_tmp_path = NULL;
  inline_out = NULL;
  selector_expr = NULL;
  input_path = NULL;
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
      printf("clql 0.0.0\n");
      free_projection_args(&fields);
      free_projection_args(&mutations);
      return 0;
    }
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(stdout);
      free_projection_args(&fields);
      free_projection_args(&mutations);
      return 0;
    }
    if (strcmp(argv[i], "--or") == 0 || strcmp(argv[i], "-O") == 0) {
      or_mode = 1;
    } else if (strcmp(argv[i], "--matches-only") == 0 ||
               strcmp(argv[i], "-M") == 0) {
      matches_only = 1;
    } else if (strcmp(argv[i], "--compact") == 0 ||
               strcmp(argv[i], "-c") == 0) {
      compact = 1;
    } else if (strcmp(argv[i], "--inline") == 0 || strcmp(argv[i], "-i") == 0 ||
               strcmp(argv[i], "--write") == 0 || strcmp(argv[i], "-w") == 0) {
      inline_mode = 1;
    } else if (strcmp(argv[i], "--enable-file-mutations") == 0 ||
               strcmp(argv[i], "-F") == 0) {
      enable_file_mutations = 1;
    } else if (strcmp(argv[i], "--theme") == 0 || strcmp(argv[i], "-t") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "clql: theme option requires an argument\n");
        free_projection_args(&fields);
        free_projection_args(&mutations);
        return 2;
      }
      ++i;
    } else if (strncmp(argv[i], "--theme=", 8u) == 0) {
      /* Accepted for Go CLI compatibility; colorized pretty output is out of
       * scope. */
    } else if (strcmp(argv[i], "--mutate") == 0 || strcmp(argv[i], "-m") == 0) {
      if (i + 1 >= argc || !add_projection_arg(&mutations, argv[++i])) {
        fprintf(stderr, "clql: failed to record mutation expression\n");
        free_projection_args(&fields);
        free_projection_args(&mutations);
        return 2;
      }
    } else if (strncmp(argv[i], "--mutate=", 9u) == 0) {
      if (!add_projection_arg(&mutations, argv[i] + 9u)) {
        fprintf(stderr, "clql: failed to record mutation expression\n");
        free_projection_args(&fields);
        free_projection_args(&mutations);
        return 2;
      }
    } else if (strcmp(argv[i], "--field") == 0 || strcmp(argv[i], "-f") == 0) {
      if (i + 1 >= argc || !add_projection_arg(&fields, argv[++i])) {
        fprintf(stderr, "clql: failed to record field path\n");
        free_projection_args(&fields);
        free_projection_args(&mutations);
        return 2;
      }
    } else if (strncmp(argv[i], "--field=", 8u) == 0) {
      if (!add_projection_arg(&fields, argv[i] + 8u)) {
        fprintf(stderr, "clql: failed to record field path\n");
        free_projection_args(&fields);
        free_projection_args(&mutations);
        return 2;
      }
    } else if (strcmp(argv[i], "-") == 0) {
      if (selector_expr == NULL) {
        selector_expr = argv[i];
      } else if (input_path == NULL) {
        input_path = argv[i];
      } else {
        usage(stderr);
        free_projection_args(&fields);
        free_projection_args(&mutations);
        return 2;
      }
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "clql: unknown option %s\n", argv[i]);
      usage(stderr);
      free_projection_args(&fields);
      free_projection_args(&mutations);
      return 2;
    } else if (selector_expr == NULL) {
      selector_expr = argv[i];
    } else if (input_path == NULL) {
      input_path = argv[i];
    } else {
      usage(stderr);
      free_projection_args(&fields);
      free_projection_args(&mutations);
      return 2;
    }
  }
  if (mutations.count != 0u && selector_expr != NULL && input_path != NULL &&
      is_regular_file_path(selector_expr)) {
    fprintf(stderr, "clql: mutation input accepts a single JSON file\n");
    free_projection_args(&fields);
    free_projection_args(&mutations);
    return 2;
  }
  if (selector_expr != NULL && input_path == NULL &&
      strcmp(selector_expr, "-") == 0) {
    input_path = selector_expr;
    selector_expr = "";
  }
  if (selector_expr != NULL && input_path == NULL &&
      is_regular_file_path(selector_expr)) {
    input_path = selector_expr;
    selector_expr = "";
  }
  if (selector_expr == NULL) {
    selector_expr = "";
  }
  if (argc == 1) {
    usage(stderr);
    free_projection_args(&fields);
    free_projection_args(&mutations);
    return 2;
  }
  lql_error_init(&error);
  if (fields.count != 0u) {
    st = lql_projection_parse((const char *const *)fields.items, fields.count,
                              &projection, &error);
    if (st != LQL_STATUS_OK) {
      fprintf(stderr, "clql: %s\n", error.message);
      free_projection_args(&fields);
      free_projection_args(&mutations);
      return 2;
    }
  }
  if (mutations.count != 0u) {
    memset(&mutation_options, 0, sizeof(mutation_options));
    mutation_options.enable_file_values = enable_file_mutations;
    mutation_options.file_value_base_dir = ".";
    st = lql_mutation_plan_parse_with_options(
        (const char *const *)mutations.items, mutations.count,
        &mutation_options, &mutation_plan, &error);
    if (st != LQL_STATUS_OK) {
      fprintf(stderr, "clql: %s\n", error.message);
      lql_projection_free(projection);
      free_projection_args(&fields);
      free_projection_args(&mutations);
      return 2;
    }
  }
  st = or_mode ? lql_selector_parse_or(selector_expr, &selector, &error)
               : lql_selector_parse(selector_expr, &selector, &error);
  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "clql: %s\n", error.message);
    lql_mutation_plan_free(mutation_plan);
    lql_projection_free(projection);
    free_projection_args(&fields);
    free_projection_args(&mutations);
    return 2;
  }
  if (mutation_plan != NULL && fields.count != 0u) {
    fprintf(stderr,
            "clql: mutation with field projection is not implemented\n");
    lql_selector_free(selector);
    lql_mutation_plan_free(mutation_plan);
    lql_projection_free(projection);
    free_projection_args(&fields);
    free_projection_args(&mutations);
    return 2;
  }
  if (inline_mode && mutation_plan == NULL) {
    fprintf(stderr, "clql: inline mode requires mutation expressions\n");
    lql_selector_free(selector);
    lql_mutation_plan_free(mutation_plan);
    lql_projection_free(projection);
    free_projection_args(&fields);
    free_projection_args(&mutations);
    return 2;
  }
  if (inline_mode && (input_path == NULL || strcmp(input_path, "-") == 0)) {
    fprintf(stderr, "clql: inline mode requires a single JSON file\n");
    lql_selector_free(selector);
    lql_mutation_plan_free(mutation_plan);
    lql_projection_free(projection);
    free_projection_args(&fields);
    free_projection_args(&mutations);
    return 2;
  }
  if (matches_only && mutation_plan == NULL) {
    count.matched = 0u;
    memset(&result, 0, sizeof(result));
    input = open_input_path(input_path);
    if (input == NULL) {
      fprintf(stderr, "clql: failed to open input %s\n", input_path);
      lql_selector_free(selector);
      lql_mutation_plan_free(mutation_plan);
      lql_projection_free(projection);
      free_projection_args(&fields);
      free_projection_args(&mutations);
      return 1;
    }
    st = lql_query_file_decisions(selector, input, count_match, &count, &result,
                                  &error);
    close_input_path(input);
    lql_selector_free(selector);
    lql_mutation_plan_free(mutation_plan);
    lql_projection_free(projection);
    free_projection_args(&fields);
    free_projection_args(&mutations);
    if (st != LQL_STATUS_OK) {
      fprintf(stderr, "clql: %s\n", error.message);
      return 1;
    }
    return count.matched == 0u ? 1 : 0;
  }
  if (input_path != NULL && strcmp(input_path, "-") != 0) {
    input = fopen(input_path, "rb");
    range_source = fopen(input_path, "rb");
    if (input == NULL || range_source == NULL) {
      fprintf(stderr, "clql: failed to open input %s\n", input_path);
      close_input_path(input);
      close_input_path(range_source);
      lql_selector_free(selector);
      lql_mutation_plan_free(mutation_plan);
      lql_projection_free(projection);
      free_projection_args(&fields);
      free_projection_args(&mutations);
      return 1;
    }
    memset(&ranges, 0, sizeof(ranges));
    memset(&result, 0, sizeof(result));
    if (inline_mode &&
        !create_inline_temp(input_path, &inline_tmp_path, &inline_out)) {
      fprintf(stderr, "clql: failed to create inline temp file\n");
      close_input_path(input);
      close_input_path(range_source);
      lql_selector_free(selector);
      lql_mutation_plan_free(mutation_plan);
      lql_projection_free(projection);
      free_projection_args(&fields);
      free_projection_args(&mutations);
      return 1;
    }
    ranges.source = range_source;
    ranges.out = inline_mode ? inline_out : stdout;
    ranges.projection = projection;
    ranges.mutation_plan = mutation_plan;
    ranges.compact = compact;
    ranges.matches_only = matches_only;
    lql_error_init(&ranges.callback_error);
    st = lql_query_file_decisions(selector, input, output_match_range, &ranges,
                                  &result, &error);
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
      free(inline_tmp_path);
      inline_tmp_path = NULL;
    }
    lql_selector_free(selector);
    lql_mutation_plan_free(mutation_plan);
    lql_projection_free(projection);
    free_projection_args(&fields);
    free_projection_args(&mutations);
    if (st != LQL_STATUS_OK) {
      fprintf(stderr, "clql: %s\n", error.message);
      return 1;
    }
    return ranges.matched == 0u ? 1 : 0;
  }
  if (mutation_plan != NULL) {
    fprintf(stderr,
            "clql: mutation execution requires a seekable input file\n");
    lql_selector_free(selector);
    lql_mutation_plan_free(mutation_plan);
    lql_projection_free(projection);
    free_projection_args(&fields);
    free_projection_args(&mutations);
    return 2;
  }
  memset(&result, 0, sizeof(result));
  st = lql_eval_query_file_spooled_matches(selector, stdin, stdout, compact,
                                           projection, &result, &error);
  lql_selector_free(selector);
  lql_mutation_plan_free(mutation_plan);
  lql_projection_free(projection);
  free_projection_args(&fields);
  free_projection_args(&mutations);
  if (st != LQL_STATUS_OK) {
    fprintf(stderr, "clql: %s\n", error.message);
    return 1;
  }
  return result.candidates_matched == 0u ? 1 : 0;
}
