#include "lql/lql.h"

#include <lauxlib.h>
#include <lua.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LUA_VERSION_NUM != 505
#error "liblql Lua bindings support Lua 5.5 only"
#endif

typedef struct lua_lql_buffer {
  char *data;
  size_t len;
  size_t cap;
} lua_lql_buffer;

typedef struct lua_lql_file_state {
  FILE *source;
  FILE *out;
  int compact;
  int matches_only;
  const lql_projection *projection;
  const lql_mutation_plan *mutation_plan;
  lql_error *error;
} lua_lql_file_state;

static void lua_lql_set_error(lql_error *error, lql_status status,
                              const char *message) {
  if (error != NULL) {
    error->code = status;
    if (message == NULL) {
      message = lql_status_string(status);
    }
    strncpy(error->message, message, sizeof(error->message) - 1u);
    error->message[sizeof(error->message) - 1u] = '\0';
  }
}

static int lua_lql_push_error(lua_State *L, const lql_error *error) {
  lua_newtable(L);
  lua_pushinteger(L, error != NULL ? (lua_Integer)error->code
                                   : (lua_Integer)LQL_STATUS_INVALID_ARGUMENT);
  lua_setfield(L, -2, "status");
  lua_pushstring(L, error != NULL ? lql_status_string(error->code)
                                  : "invalid argument");
  lua_setfield(L, -2, "status_string");
  lua_pushstring(L, error != NULL ? error->message : "invalid argument");
  lua_setfield(L, -2, "stderr");
  return 1;
}

static int lua_lql_fail(lua_State *L, const lql_error *error) {
  lua_pushnil(L);
  lua_lql_push_error(L, error);
  return 2;
}

static lql_status lua_lql_write_buffer(void *user, const void *data,
                                       size_t len) {
  lua_lql_buffer *buffer;
  char *next;
  size_t next_cap;

  buffer = (lua_lql_buffer *)user;
  if (len == 0u) {
    return LQL_STATUS_OK;
  }
  if (len > ((size_t)-1) - buffer->len - 1u) {
    return LQL_STATUS_NO_MEMORY;
  }
  if (buffer->len + len + 1u > buffer->cap) {
    next_cap = buffer->cap == 0u ? 256u : buffer->cap;
    while (next_cap < buffer->len + len + 1u) {
      if (next_cap > ((size_t)-1) / 2u) {
        return LQL_STATUS_NO_MEMORY;
      }
      next_cap *= 2u;
    }
    next = (char *)realloc(buffer->data, next_cap);
    if (next == NULL) {
      return LQL_STATUS_NO_MEMORY;
    }
    buffer->data = next;
    buffer->cap = next_cap;
  }
  memcpy(buffer->data + buffer->len, data, len);
  buffer->len += len;
  buffer->data[buffer->len] = '\0';
  return LQL_STATUS_OK;
}

static lql_status lua_lql_file_to_buffer(FILE *file, lua_lql_buffer *buffer) {
  char chunk[4096];
  size_t nread;

  if (fflush(file) != 0 || fseek(file, 0L, SEEK_SET) != 0) {
    return LQL_STATUS_JSON_ERROR;
  }
  for (;;) {
    nread = fread(chunk, 1u, sizeof(chunk), file);
    if (nread > 0u &&
        lua_lql_write_buffer(buffer, chunk, nread) != LQL_STATUS_OK) {
      return LQL_STATUS_NO_MEMORY;
    }
    if (nread < sizeof(chunk)) {
      if (ferror(file)) {
        return LQL_STATUS_JSON_ERROR;
      }
      break;
    }
  }
  return LQL_STATUS_OK;
}

static int lua_lql_fields(lua_State *L, int index, const char ***out_fields,
                          size_t *out_count) {
  const char **fields;
  size_t count;
  size_t i;

  luaL_checktype(L, index, LUA_TTABLE);
  count = (size_t)lua_rawlen(L, index);
  fields = NULL;
  if (count > 0u) {
    fields = (const char **)malloc(sizeof(fields[0]) * count);
    if (fields == NULL) {
      return 0;
    }
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(L, index, (lua_Integer)i + 1);
    fields[i] = luaL_checkstring(L, -1);
    lua_pop(L, 1);
  }
  *out_fields = fields;
  *out_count = count;
  return 1;
}

static int lua_lql_strings(lua_State *L, int index, const char ***out_values,
                           size_t *out_count) {
  return lua_lql_fields(L, index, out_values, out_count);
}

static int lua_lql_options_bool(lua_State *L, int index, const char *key) {
  int value;

  value = 0;
  if (lua_istable(L, index)) {
    lua_getfield(L, index, key);
    value = lua_toboolean(L, -1);
    lua_pop(L, 1);
  }
  return value;
}

static lql_status lua_lql_finish_candidate(FILE *out) {
  if (fputc('\n', out) == EOF) {
    return LQL_STATUS_JSON_ERROR;
  }
  return LQL_STATUS_OK;
}

static lql_status lua_lql_write_file_payload(lua_lql_file_state *state,
                                             const lql_payload *payload) {
  lql_status st;

  if (state->compact) {
    st = lql_compact_file_range(state->source, payload->offset, payload->size,
                                state->out, state->error);
  } else {
    st = lql_payload_write_json(payload, state->out, state->error);
  }
  if (st == LQL_STATUS_OK) {
    st = lua_lql_finish_candidate(state->out);
  }
  return st;
}

static lql_status lua_lql_on_select_file(void *user,
                                         const lql_query_match *match) {
  lua_lql_file_state *state;

  state = (lua_lql_file_state *)user;
  return lua_lql_write_file_payload(state, &match->payload);
}

static lql_status lua_lql_on_project_file(void *user,
                                          const lql_query_match *match) {
  lua_lql_file_state *state;
  lql_status st;
  int found;

  state = (lua_lql_file_state *)user;
  found = 0;
  st = lql_project_file_range(state->projection, state->source,
                              match->payload.offset, match->payload.size,
                              state->out, &found, state->error);
  if (st == LQL_STATUS_OK && found) {
    st = lua_lql_finish_candidate(state->out);
  }
  return st;
}

static lql_status lua_lql_on_mutate_file(void *user,
                                         const lql_query_decision *decision) {
  lua_lql_file_state *state;
  lql_payload payload;
  lql_status st;

  state = (lua_lql_file_state *)user;
  memset(&payload, 0, sizeof(payload));
  payload.kind = LQL_PAYLOAD_SEEKABLE_RANGE;
  payload.index = decision->index;
  payload.offset = decision->offset;
  payload.size = decision->size;
  payload.source = state->source;
  if (decision->matched) {
    st = lql_mutate_file_range_paths(state->mutation_plan, state->source,
                                     decision->offset, decision->size,
                                     state->out, state->error);
    if (st == LQL_STATUS_OK) {
      st = lua_lql_finish_candidate(state->out);
    }
    return st;
  }
  if (state->matches_only) {
    return LQL_STATUS_OK;
  }
  return lua_lql_write_file_payload(state, &payload);
}

static lql_status lua_lql_parse_mutation_plan(lua_State *L, int expr_index,
                                              int options_index,
                                              const char *base_dir,
                                              lql_mutation_plan **out,
                                              lql_error *error) {
  const char **mutations;
  size_t mutation_count;
  lql_status st;
  lql_mutation_parse_options options;

  mutations = NULL;
  mutation_count = 0u;
  if (!lua_lql_strings(L, expr_index, &mutations, &mutation_count)) {
    lua_lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  if (lua_lql_options_bool(L, options_index, "enable_file_mutations")) {
    memset(&options, 0, sizeof(options));
    options.enable_file_values = 1;
    options.file_value_base_dir = base_dir;
    st = lql_mutation_plan_parse_with_options(mutations, mutation_count,
                                              &options, out, error);
  } else {
    st = lql_mutation_plan_parse(mutations, mutation_count, out, error);
  }
  free(mutations);
  return st;
}

static int lua_lql_matches_json(lua_State *L) {
  const char *selector_expr;
  const char *json;
  size_t json_len;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;

  selector_expr = luaL_checkstring(L, 1);
  json = luaL_checklstring(L, 2, &json_len);
  selector = NULL;
  matched = 0;
  lql_error_init(&error);
  st = lql_selector_parse(selector_expr, &selector, &error);
  if (st == LQL_STATUS_OK) {
    st = lql_matches_json(selector, json, json_len, &matched, &error);
  }
  lql_selector_free(selector);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  lua_pushboolean(L, matched);
  return 1;
}

static int lua_lql_select_json(lua_State *L) {
  const char *selector_expr;
  const char *json;
  size_t json_len;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;
  int compact;
  FILE *out;
  lua_lql_buffer buffer;

  selector_expr = luaL_checkstring(L, 1);
  json = luaL_checklstring(L, 2, &json_len);
  compact = lua_lql_options_bool(L, 3, "compact");
  selector = NULL;
  matched = 0;
  memset(&buffer, 0, sizeof(buffer));
  lql_error_init(&error);
  st = lql_selector_parse(selector_expr, &selector, &error);
  if (st == LQL_STATUS_OK) {
    st = lql_matches_json(selector, json, json_len, &matched, &error);
  }
  if (st == LQL_STATUS_OK && matched) {
    if (compact) {
      out = tmpfile();
      if (out == NULL) {
        lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                          "failed to create Lua output file");
        st = LQL_STATUS_JSON_ERROR;
      } else {
        st = lql_compact_json(json, json_len, out, &error);
        if (st == LQL_STATUS_OK) {
          fputc('\n', out);
          st = lua_lql_file_to_buffer(out, &buffer);
        }
        fclose(out);
      }
    } else {
      st = lua_lql_write_buffer(&buffer, json, json_len);
      if (st == LQL_STATUS_OK) {
        st = lua_lql_write_buffer(&buffer, "\n", 1u);
      }
      if (st != LQL_STATUS_OK) {
        lua_lql_set_error(&error, st, lql_status_string(st));
      }
    }
  }
  lql_selector_free(selector);
  if (st != LQL_STATUS_OK) {
    free(buffer.data);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  free(buffer.data);
  return 1;
}

static int lua_lql_project_json(lua_State *L) {
  const char *selector_expr;
  const char *json;
  const char **fields;
  size_t field_count;
  size_t json_len;
  lql_selector *selector;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  int matched;
  int found;
  FILE *out;
  lua_lql_buffer buffer;

  selector_expr = luaL_checkstring(L, 1);
  json = luaL_checklstring(L, 2, &json_len);
  fields = NULL;
  field_count = 0u;
  if (!lua_lql_fields(L, 3, &fields, &field_count)) {
    return luaL_error(L, "out of memory");
  }
  selector = NULL;
  projection = NULL;
  matched = 0;
  found = 0;
  memset(&buffer, 0, sizeof(buffer));
  lql_error_init(&error);
  st = lql_selector_parse(selector_expr, &selector, &error);
  if (st == LQL_STATUS_OK) {
    st = lql_matches_json(selector, json, json_len, &matched, &error);
  }
  if (st == LQL_STATUS_OK && matched) {
    st = lql_projection_parse(fields, field_count, &projection, &error);
  }
  if (st == LQL_STATUS_OK && matched) {
    out = tmpfile();
    if (out == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to create Lua output file");
      st = LQL_STATUS_JSON_ERROR;
    } else {
      st = lql_project_json(projection, json, json_len, out, &found, &error);
      if (st == LQL_STATUS_OK && found) {
        fputc('\n', out);
        st = lua_lql_file_to_buffer(out, &buffer);
      }
      fclose(out);
    }
  }
  lql_projection_free(projection);
  lql_selector_free(selector);
  free(fields);
  if (st != LQL_STATUS_OK) {
    free(buffer.data);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  free(buffer.data);
  return 1;
}

static int lua_lql_mutate_json(lua_State *L) {
  const char *selector_expr;
  const char *json;
  size_t json_len;
  lql_selector *selector;
  lql_mutation_plan *plan;
  lql_error error;
  lql_status st;
  int matched;
  int matches_only;
  FILE *out;
  lua_lql_buffer buffer;

  selector_expr = luaL_checkstring(L, 1);
  json = luaL_checklstring(L, 2, &json_len);
  matches_only = lua_lql_options_bool(L, 4, "matches_only");
  selector = NULL;
  plan = NULL;
  matched = 0;
  memset(&buffer, 0, sizeof(buffer));
  lql_error_init(&error);
  st = lql_selector_parse(selector_expr, &selector, &error);
  if (st == LQL_STATUS_OK) {
    st = lql_matches_json(selector, json, json_len, &matched, &error);
  }
  if (st == LQL_STATUS_OK) {
    st = lua_lql_parse_mutation_plan(L, 3, 4, NULL, &plan, &error);
  }
  if (st == LQL_STATUS_OK && matched) {
    out = tmpfile();
    if (out == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to create Lua output file");
      st = LQL_STATUS_JSON_ERROR;
    } else {
      st = lql_mutate_json(plan, json, json_len, out, &error);
      if (st == LQL_STATUS_OK) {
        fputc('\n', out);
        st = lua_lql_file_to_buffer(out, &buffer);
      }
      fclose(out);
    }
  } else if (st == LQL_STATUS_OK && !matches_only) {
    st = lua_lql_write_buffer(&buffer, json, json_len);
    if (st == LQL_STATUS_OK) {
      st = lua_lql_write_buffer(&buffer, "\n", 1u);
    }
    if (st != LQL_STATUS_OK) {
      lua_lql_set_error(&error, st, lql_status_string(st));
    }
  }
  lql_mutation_plan_free(plan);
  lql_selector_free(selector);
  if (st != LQL_STATUS_OK) {
    free(buffer.data);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  free(buffer.data);
  return 1;
}

static int lua_lql_select_file(lua_State *L) {
  const char *selector_expr;
  const char *path;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  FILE *input;
  FILE *out;
  lua_lql_buffer buffer;
  lua_lql_file_state state;

  selector_expr = luaL_checkstring(L, 1);
  path = luaL_checkstring(L, 2);
  selector = NULL;
  input = NULL;
  out = NULL;
  memset(&buffer, 0, sizeof(buffer));
  memset(&state, 0, sizeof(state));
  lql_error_init(&error);
  st = lql_selector_parse(selector_expr, &selector, &error);
  if (st == LQL_STATUS_OK) {
    input = fopen(path, "rb");
    if (input == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to open Lua input file");
      st = LQL_STATUS_JSON_ERROR;
    }
  }
  if (st == LQL_STATUS_OK) {
    out = tmpfile();
    if (out == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to create Lua output file");
      st = LQL_STATUS_JSON_ERROR;
    }
  }
  if (st == LQL_STATUS_OK) {
    state.source = input;
    state.out = out;
    state.compact = lua_lql_options_bool(L, 3, "compact");
    state.error = &error;
    st = lql_query_file_matches(selector, input, lua_lql_on_select_file, &state,
                                NULL, &error);
  }
  if (st == LQL_STATUS_OK) {
    st = lua_lql_file_to_buffer(out, &buffer);
  }
  if (out != NULL) {
    fclose(out);
  }
  if (input != NULL) {
    fclose(input);
  }
  lql_selector_free(selector);
  if (st != LQL_STATUS_OK) {
    free(buffer.data);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  free(buffer.data);
  return 1;
}

static int lua_lql_project_file(lua_State *L) {
  const char *selector_expr;
  const char *path;
  const char **fields;
  size_t field_count;
  lql_selector *selector;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  FILE *input;
  FILE *out;
  lua_lql_buffer buffer;
  lua_lql_file_state state;

  selector_expr = luaL_checkstring(L, 1);
  path = luaL_checkstring(L, 2);
  fields = NULL;
  field_count = 0u;
  if (!lua_lql_fields(L, 3, &fields, &field_count)) {
    return luaL_error(L, "out of memory");
  }
  selector = NULL;
  projection = NULL;
  input = NULL;
  out = NULL;
  memset(&buffer, 0, sizeof(buffer));
  memset(&state, 0, sizeof(state));
  lql_error_init(&error);
  st = lql_selector_parse(selector_expr, &selector, &error);
  if (st == LQL_STATUS_OK) {
    st = lql_projection_parse(fields, field_count, &projection, &error);
  }
  if (st == LQL_STATUS_OK) {
    input = fopen(path, "rb");
    if (input == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to open Lua input file");
      st = LQL_STATUS_JSON_ERROR;
    }
  }
  if (st == LQL_STATUS_OK) {
    out = tmpfile();
    if (out == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to create Lua output file");
      st = LQL_STATUS_JSON_ERROR;
    }
  }
  if (st == LQL_STATUS_OK) {
    state.source = input;
    state.out = out;
    state.projection = projection;
    state.error = &error;
    st = lql_query_file_matches(selector, input, lua_lql_on_project_file,
                                &state, NULL, &error);
  }
  if (st == LQL_STATUS_OK) {
    st = lua_lql_file_to_buffer(out, &buffer);
  }
  if (out != NULL) {
    fclose(out);
  }
  if (input != NULL) {
    fclose(input);
  }
  lql_projection_free(projection);
  lql_selector_free(selector);
  free(fields);
  if (st != LQL_STATUS_OK) {
    free(buffer.data);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  free(buffer.data);
  return 1;
}

static int lua_lql_mutate_file(lua_State *L) {
  const char *selector_expr;
  const char *path;
  lql_selector *selector;
  lql_mutation_plan *plan;
  lql_error error;
  lql_status st;
  FILE *input;
  FILE *out;
  lua_lql_buffer buffer;
  lua_lql_file_state state;

  selector_expr = luaL_checkstring(L, 1);
  path = luaL_checkstring(L, 2);
  selector = NULL;
  plan = NULL;
  input = NULL;
  out = NULL;
  memset(&buffer, 0, sizeof(buffer));
  memset(&state, 0, sizeof(state));
  lql_error_init(&error);
  st = lql_selector_parse(selector_expr, &selector, &error);
  if (st == LQL_STATUS_OK) {
    st = lua_lql_parse_mutation_plan(L, 3, 4, NULL, &plan, &error);
  }
  if (st == LQL_STATUS_OK) {
    input = fopen(path, "rb");
    if (input == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to open Lua input file");
      st = LQL_STATUS_JSON_ERROR;
    }
  }
  if (st == LQL_STATUS_OK) {
    out = tmpfile();
    if (out == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to create Lua output file");
      st = LQL_STATUS_JSON_ERROR;
    }
  }
  if (st == LQL_STATUS_OK) {
    state.source = input;
    state.out = out;
    state.compact = lua_lql_options_bool(L, 4, "compact");
    state.matches_only = lua_lql_options_bool(L, 4, "matches_only");
    state.mutation_plan = plan;
    state.error = &error;
    st = lql_query_file_decisions(selector, input, lua_lql_on_mutate_file,
                                  &state, NULL, &error);
  }
  if (st == LQL_STATUS_OK) {
    st = lua_lql_file_to_buffer(out, &buffer);
  }
  if (out != NULL) {
    fclose(out);
  }
  if (input != NULL) {
    fclose(input);
  }
  lql_mutation_plan_free(plan);
  lql_selector_free(selector);
  if (st != LQL_STATUS_OK) {
    free(buffer.data);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  free(buffer.data);
  return 1;
}

static const luaL_Reg lua_lql_functions[] = {
    {"matches_json", lua_lql_matches_json},
    {"select_json", lua_lql_select_json},
    {"select_file", lua_lql_select_file},
    {"project_json", lua_lql_project_json},
    {"project_file", lua_lql_project_file},
    {"mutate_json", lua_lql_mutate_json},
    {"mutate_file", lua_lql_mutate_file},
    {NULL, NULL}};

int luaopen_lql_core(lua_State *L) {
  luaL_newlib(L, lua_lql_functions);
  return 1;
}
