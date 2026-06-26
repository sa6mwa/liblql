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
  lua_State *lua;
  char *data;
  size_t len;
  size_t cap;
} lua_lql_buffer;

typedef struct lua_lql_client {
  lql *ctx;
} lua_lql_client;

typedef struct lua_lql_file_state {
  lql *ctx;
  FILE *source;
  FILE *out;
  int compact;
  int matches_only;
  lua_State *lua;
  int callback_ref;
  const lql_projection *projection;
  const lql_mutation_plan *mutation_plan;
  lql_error *error;
  int callback_failed;
  char callback_message[256];
} lua_lql_file_state;

typedef struct lua_lql_payload_handle {
  lql *ctx;
  int active;
  lql_payload payload;
} lua_lql_payload_handle;

typedef struct lua_lql_payload_sink {
  lua_State *lua;
  int callback_ref;
  lql_error *error;
  int callback_failed;
  char callback_message[256];
} lua_lql_payload_sink;

#define LUA_LQL_CLIENT "lql.client"

static int lua_lql_fail(lua_State *L, const lql_error *error);

static void *lua_lql_alloc(lua_State *L, void *ptr, size_t old_size,
                           size_t new_size) {
  lua_Alloc allocf;
  void *user;

  allocf = lua_getallocf(L, &user);
  return allocf(user, ptr, old_size, new_size);
}

static void lua_lql_buffer_init(lua_lql_buffer *buffer, lua_State *L) {
  memset(buffer, 0, sizeof(*buffer));
  buffer->lua = L;
}

static void lua_lql_buffer_dispose(lua_lql_buffer *buffer) {
  if (buffer->data != NULL) {
    (void)lua_lql_alloc(buffer->lua, buffer->data, buffer->cap, 0u);
    buffer->data = NULL;
  }
  buffer->len = 0u;
  buffer->cap = 0u;
}

static lua_lql_client *lua_lql_check_client(lua_State *L, int index) {
  lua_lql_client *client;

  client = (lua_lql_client *)luaL_checkudata(L, index, LUA_LQL_CLIENT);
  luaL_argcheck(L, client != NULL && client->ctx != NULL, index,
                "closed lql client");
  return client;
}

static int lua_lql_client_gc(lua_State *L) {
  lua_lql_client *client;

  client = (lua_lql_client *)luaL_checkudata(L, 1, LUA_LQL_CLIENT);
  if (client != NULL && client->ctx != NULL) {
    client->ctx->destroy(client->ctx);
    client->ctx = NULL;
  }
  return 0;
}

static int lua_lql_new_client(lua_State *L) {
  lua_lql_client *client;
  lql_error error;
  lql_status st;

  client = (lua_lql_client *)lua_newuserdatauv(L, sizeof(*client), 0);
  client->ctx = NULL;
  luaL_getmetatable(L, LUA_LQL_CLIENT);
  lua_setmetatable(L, -2);
  lql_error_init(&error);
  st = lql_new(&client->ctx, &error);
  if (st != LQL_STATUS_OK) {
    lua_pop(L, 1);
    return lua_lql_fail(L, &error);
  }
  return 1;
}

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

static void lua_lql_record_callback_error(lua_State *L,
                                          lua_lql_file_state *state) {
  const char *message;

  message = lua_tostring(L, -1);
  state->callback_failed = 1;
  strncpy(state->callback_message,
          message != NULL ? message : "callback failed",
          sizeof(state->callback_message) - 1u);
  state->callback_message[sizeof(state->callback_message) - 1u] = '\0';
  lua_lql_set_error(state->error, LQL_STATUS_INVALID_ARGUMENT,
                    state->callback_message);
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
    next = (char *)lua_lql_alloc(buffer->lua, buffer->data, buffer->cap,
                                 next_cap);
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
    fields = (const char **)lua_lql_alloc(L, NULL, 0u,
                                          sizeof(fields[0]) * count);
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

static lql_uint64 lua_lql_options_u64(lua_State *L, int index,
                                      const char *key) {
  lua_Integer value;

  value = 0;
  if (lua_istable(L, index)) {
    lua_getfield(L, index, key);
    if (!lua_isnil(L, -1)) {
      value = luaL_checkinteger(L, -1);
      if (value < 0) {
        value = 0;
      }
    }
    lua_pop(L, 1);
  }
  return (lql_uint64)value;
}

static void lua_lql_options_query(lua_State *L, int index,
                                  lql_query_options *out) {
  memset(out, 0, sizeof(*out));
  out->max_matches = lua_lql_options_u64(L, index, "max_matches");
  out->max_candidates = lua_lql_options_u64(L, index, "max_candidates");
  out->max_bytes_read = lua_lql_options_u64(L, index, "max_bytes_read");
}

static void lua_lql_push_query_result(lua_State *L,
                                      const lql_query_result *result) {
  lua_newtable(L);
  lua_pushinteger(L, (lua_Integer)result->candidates_seen);
  lua_setfield(L, -2, "candidates_seen");
  lua_pushinteger(L, (lua_Integer)result->candidates_matched);
  lua_setfield(L, -2, "candidates_matched");
  lua_pushinteger(L, (lua_Integer)result->bytes_read);
  lua_setfield(L, -2, "bytes_read");
  lua_pushboolean(L, result->stopped_early);
  lua_setfield(L, -2, "stopped_early");
  lua_pushinteger(L, (lua_Integer)result->stop_reason);
  lua_setfield(L, -2, "stop_reason");
}

static void lua_lql_push_decision(lua_State *L,
                                  const lql_query_decision *decision) {
  lua_newtable(L);
  lua_pushboolean(L, decision->matched);
  lua_setfield(L, -2, "matched");
  lua_pushinteger(L, (lua_Integer)decision->index);
  lua_setfield(L, -2, "index");
  lua_pushinteger(L, (lua_Integer)decision->offset);
  lua_setfield(L, -2, "offset");
  lua_pushinteger(L, (lua_Integer)decision->size);
  lua_setfield(L, -2, "size");
}

static lql_status lua_lql_finish_candidate(FILE *out) {
  if (fputc('\n', out) == EOF) {
    return LQL_STATUS_JSON_ERROR;
  }
  return LQL_STATUS_OK;
}

static int lua_lql_payload_json(lua_State *L) {
  lua_lql_payload_handle *handle;
  lua_lql_buffer buffer;
  lql_error error;
  lql_status st;
  FILE *out;

  handle = (lua_lql_payload_handle *)lua_touserdata(L, lua_upvalueindex(1));
  if (handle == NULL || !handle->active) {
    lql_error_init(&error);
    lua_lql_set_error(&error, LQL_STATUS_INVALID_ARGUMENT,
                      "payload handle is no longer active");
    return lua_lql_fail(L, &error);
  }
  out = tmpfile();
  lua_lql_buffer_init(&buffer, L);
  lql_error_init(&error);
  if (out == NULL) {
    lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                      "failed to create Lua payload file");
    return lua_lql_fail(L, &error);
  }
  st = handle->ctx->payload_write_json(handle->ctx, &handle->payload, out,
                                       &error);
  if (st == LQL_STATUS_OK) {
    st = lua_lql_file_to_buffer(out, &buffer);
  }
  fclose(out);
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static lql_status lua_lql_payload_write_chunk(void *user, const void *data,
                                              size_t len) {
  lua_lql_payload_sink *sink;
  lua_State *L;
  const char *message;
  int ok;

  sink = (lua_lql_payload_sink *)user;
  L = sink->lua;
  lua_rawgeti(L, LUA_REGISTRYINDEX, sink->callback_ref);
  lua_pushlstring(L, (const char *)data, len);
  ok = lua_pcall(L, 1, 0, 0);
  if (ok != LUA_OK) {
    message = lua_tostring(L, -1);
    sink->callback_failed = 1;
    strncpy(sink->callback_message,
            message != NULL ? message : "payload write callback failed",
            sizeof(sink->callback_message) - 1u);
    sink->callback_message[sizeof(sink->callback_message) - 1u] = '\0';
    lua_lql_set_error(sink->error, LQL_STATUS_INVALID_ARGUMENT,
                      sink->callback_message);
    lua_pop(L, 1);
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  return LQL_STATUS_OK;
}

static int lua_lql_payload_write_json(lua_State *L) {
  lua_lql_payload_handle *handle;
  lua_lql_payload_sink sink;
  lql_error error;
  lql_status st;

  handle = (lua_lql_payload_handle *)lua_touserdata(L, lua_upvalueindex(1));
  if (handle == NULL || !handle->active) {
    lql_error_init(&error);
    lua_lql_set_error(&error, LQL_STATUS_INVALID_ARGUMENT,
                      "payload handle is no longer active");
    return lua_lql_fail(L, &error);
  }
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lql_error_init(&error);
  memset(&sink, 0, sizeof(sink));
  sink.lua = L;
  sink.error = &error;
  lua_pushvalue(L, 1);
  sink.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  st = handle->ctx->payload_write_json_sink(handle->ctx, &handle->payload,
                                            lua_lql_payload_write_chunk, &sink,
                                            &error);
  luaL_unref(L, LUA_REGISTRYINDEX, sink.callback_ref);
  if (sink.callback_failed) {
    lua_lql_set_error(&error, LQL_STATUS_INVALID_ARGUMENT,
                      sink.callback_message);
  }
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  lua_pushboolean(L, 1);
  return 1;
}

static lql_status lua_lql_call_decision(lua_lql_file_state *state,
                                        const lql_query_decision *decision) {
  lua_State *L;
  int ok;
  int keep_going;

  L = state->lua;
  lua_rawgeti(L, LUA_REGISTRYINDEX, state->callback_ref);
  lua_lql_push_decision(L, decision);
  ok = lua_pcall(L, 1, 1, 0);
  if (ok != LUA_OK) {
    lua_lql_record_callback_error(L, state);
    lua_pop(L, 1);
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  keep_going = lua_isnoneornil(L, -1) || lua_toboolean(L, -1);
  lua_pop(L, 1);
  return keep_going ? LQL_STATUS_OK : LQL_STATUS_STOP;
}

static lql_status
lua_lql_on_query_file_decision(void *user, const lql_query_decision *decision) {
  return lua_lql_call_decision((lua_lql_file_state *)user, decision);
}

static lql_status lua_lql_on_each_match_file(void *user,
                                             const lql_query_match *match) {
  lua_lql_file_state *state;
  lua_lql_payload_handle *handle;
  lua_State *L;
  int ok;
  int keep_going;

  state = (lua_lql_file_state *)user;
  L = state->lua;
  lua_rawgeti(L, LUA_REGISTRYINDEX, state->callback_ref);
  lua_lql_push_decision(L, &match->decision);
  handle = (lua_lql_payload_handle *)lua_newuserdatauv(L, sizeof(*handle), 0);
  memset(handle, 0, sizeof(*handle));
  handle->active = 1;
  handle->ctx = state->ctx;
  handle->payload = match->payload;
  lua_pushvalue(L, -1);
  lua_pushcclosure(L, lua_lql_payload_json, 1);
  lua_setfield(L, -3, "json");
  lua_pushcclosure(L, lua_lql_payload_write_json, 1);
  lua_setfield(L, -2, "write_json");
  ok = lua_pcall(L, 1, 1, 0);
  handle->active = 0;
  if (ok != LUA_OK) {
    lua_lql_record_callback_error(L, state);
    lua_pop(L, 1);
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  keep_going = lua_isnoneornil(L, -1) || lua_toboolean(L, -1);
  lua_pop(L, 1);
  return keep_going ? LQL_STATUS_OK : LQL_STATUS_STOP;
}

static lql_status lua_lql_write_file_payload(lua_lql_file_state *state,
                                             const lql_payload *payload) {
  lql_status st;

  if (state->compact) {
    st = state->ctx->compact_file_range(state->ctx, state->source,
                                        payload->offset, payload->size,
                                        state->out, state->error);
  } else {
    st = state->ctx->payload_write_json(state->ctx, payload, state->out,
                                        state->error);
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
  st = state->ctx->project_file_range(
      state->ctx, state->projection, state->source, match->payload.offset,
      match->payload.size, state->out, &found, state->error);
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
    st = state->ctx->mutate_file_range_paths(
        state->ctx, state->mutation_plan, state->source, decision->offset,
        decision->size, state->out, state->error);
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

static lql_status lua_lql_parse_mutation_plan(lua_State *L, lql *ctx,
                                              int expr_index, int options_index,
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
    st = ctx->mutation_plan_parse_with_options(ctx, mutations, mutation_count,
                                               &options, out, error);
  } else {
    st = ctx->mutation_plan_parse(ctx, mutations, mutation_count, out, error);
  }
  if (mutations != NULL) {
    (void)lua_lql_alloc(L, (void *)mutations,
                        sizeof(mutations[0]) * mutation_count, 0u);
  }
  return st;
}

static int lua_lql_matches_json(lua_State *L) {
  lua_lql_client *client;
  const char *selector_expr;
  const char *json;
  size_t json_len;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;

  client = lua_lql_check_client(L, 1);
  selector_expr = luaL_checkstring(L, 2);
  json = luaL_checklstring(L, 3, &json_len);
  selector = NULL;
  matched = 0;
  lql_error_init(&error);
  st = client->ctx->selector_parse(client->ctx, selector_expr, &selector,
                                   &error);
  if (st == LQL_STATUS_OK) {
    st = client->ctx->matches_json(client->ctx, selector, json, json_len,
                                   &matched, &error);
  }
  client->ctx->selector_destroy(client->ctx, selector);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  lua_pushboolean(L, matched);
  return 1;
}

static int lua_lql_select_json(lua_State *L) {
  lua_lql_client *client;
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

  client = lua_lql_check_client(L, 1);
  selector_expr = luaL_checkstring(L, 2);
  json = luaL_checklstring(L, 3, &json_len);
  compact = lua_lql_options_bool(L, 4, "compact");
  selector = NULL;
  matched = 0;
  lua_lql_buffer_init(&buffer, L);
  lql_error_init(&error);
  st = client->ctx->selector_parse(client->ctx, selector_expr, &selector,
                                   &error);
  if (st == LQL_STATUS_OK) {
    st = client->ctx->matches_json(client->ctx, selector, json, json_len,
                                   &matched, &error);
  }
  if (st == LQL_STATUS_OK && matched) {
    if (compact) {
      out = tmpfile();
      if (out == NULL) {
        lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                          "failed to create Lua output file");
        st = LQL_STATUS_JSON_ERROR;
      } else {
        st =
            client->ctx->compact_json(client->ctx, json, json_len, out, &error);
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
  client->ctx->selector_destroy(client->ctx, selector);
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static int lua_lql_project_json(lua_State *L) {
  lua_lql_client *client;
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

  client = lua_lql_check_client(L, 1);
  selector_expr = luaL_checkstring(L, 2);
  json = luaL_checklstring(L, 3, &json_len);
  fields = NULL;
  field_count = 0u;
  if (!lua_lql_fields(L, 4, &fields, &field_count)) {
    return luaL_error(L, "out of memory");
  }
  selector = NULL;
  projection = NULL;
  matched = 0;
  found = 0;
  lua_lql_buffer_init(&buffer, L);
  lql_error_init(&error);
  st = client->ctx->selector_parse(client->ctx, selector_expr, &selector,
                                   &error);
  if (st == LQL_STATUS_OK) {
    st = client->ctx->matches_json(client->ctx, selector, json, json_len,
                                   &matched, &error);
  }
  if (st == LQL_STATUS_OK && matched) {
    st = client->ctx->projection_parse(client->ctx, fields, field_count,
                                       &projection, &error);
  }
  if (st == LQL_STATUS_OK && matched) {
    out = tmpfile();
    if (out == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to create Lua output file");
      st = LQL_STATUS_JSON_ERROR;
    } else {
      st = client->ctx->project_json(client->ctx, projection, json, json_len,
                                     out, &found, &error);
      if (st == LQL_STATUS_OK && found) {
        fputc('\n', out);
        st = lua_lql_file_to_buffer(out, &buffer);
      }
      fclose(out);
    }
  }
  client->ctx->projection_destroy(client->ctx, projection);
  client->ctx->selector_destroy(client->ctx, selector);
  if (fields != NULL) {
    (void)lua_lql_alloc(L, (void *)fields, sizeof(fields[0]) * field_count,
                        0u);
  }
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static int lua_lql_mutate_json(lua_State *L) {
  lua_lql_client *client;
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

  client = lua_lql_check_client(L, 1);
  selector_expr = luaL_checkstring(L, 2);
  json = luaL_checklstring(L, 3, &json_len);
  matches_only = lua_lql_options_bool(L, 5, "matches_only");
  selector = NULL;
  plan = NULL;
  matched = 0;
  lua_lql_buffer_init(&buffer, L);
  lql_error_init(&error);
  st = client->ctx->selector_parse(client->ctx, selector_expr, &selector,
                                   &error);
  if (st == LQL_STATUS_OK) {
    st = client->ctx->matches_json(client->ctx, selector, json, json_len,
                                   &matched, &error);
  }
  if (st == LQL_STATUS_OK) {
    st = lua_lql_parse_mutation_plan(L, client->ctx, 4, 5, NULL, &plan, &error);
  }
  if (st == LQL_STATUS_OK && matched) {
    out = tmpfile();
    if (out == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to create Lua output file");
      st = LQL_STATUS_JSON_ERROR;
    } else {
      st = client->ctx->mutate_json(client->ctx, plan, json, json_len, out,
                                    &error);
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
  client->ctx->mutation_plan_destroy(client->ctx, plan);
  client->ctx->selector_destroy(client->ctx, selector);
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static int lua_lql_select_file(lua_State *L) {
  lua_lql_client *client;
  const char *selector_expr;
  const char *path;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  FILE *input;
  FILE *out;
  lua_lql_buffer buffer;
  lua_lql_file_state state;

  client = lua_lql_check_client(L, 1);
  selector_expr = luaL_checkstring(L, 2);
  path = luaL_checkstring(L, 3);
  selector = NULL;
  input = NULL;
  out = NULL;
  lua_lql_buffer_init(&buffer, L);
  memset(&state, 0, sizeof(state));
  lql_error_init(&error);
  st = client->ctx->selector_parse(client->ctx, selector_expr, &selector,
                                   &error);
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
    state.ctx = client->ctx;
    state.source = input;
    state.out = out;
    state.compact = lua_lql_options_bool(L, 4, "compact");
    state.error = &error;
    st = client->ctx->query_file_matches(client->ctx, selector, input,
                                         lua_lql_on_select_file, &state, NULL,
                                         &error);
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
  client->ctx->selector_destroy(client->ctx, selector);
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static int lua_lql_query_file(lua_State *L) {
  lua_lql_client *client;
  const char *selector_expr;
  const char *path;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  FILE *input;
  lql_query_options options;
  lql_query_result result;
  lua_lql_file_state state;

  client = lua_lql_check_client(L, 1);
  selector_expr = luaL_checkstring(L, 2);
  path = luaL_checkstring(L, 3);
  luaL_checktype(L, 4, LUA_TFUNCTION);
  selector = NULL;
  input = NULL;
  memset(&state, 0, sizeof(state));
  memset(&result, 0, sizeof(result));
  lua_lql_options_query(L, 5, &options);
  lql_error_init(&error);
  st = client->ctx->selector_parse(client->ctx, selector_expr, &selector,
                                   &error);
  if (st == LQL_STATUS_OK) {
    input = fopen(path, "rb");
    if (input == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to open Lua input file");
      st = LQL_STATUS_JSON_ERROR;
    }
  }
  if (st == LQL_STATUS_OK) {
    lua_pushvalue(L, 4);
    state.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    state.ctx = client->ctx;
    state.lua = L;
    state.error = &error;
    st = client->ctx->query_file_decisions_with_options(
        client->ctx, selector, input, &options, lua_lql_on_query_file_decision,
        &state, &result, &error);
    luaL_unref(L, LUA_REGISTRYINDEX, state.callback_ref);
  }
  if (state.callback_failed) {
    lua_lql_set_error(&error, LQL_STATUS_INVALID_ARGUMENT,
                      state.callback_message);
  }
  if (input != NULL) {
    fclose(input);
  }
  client->ctx->selector_destroy(client->ctx, selector);
  if (st != LQL_STATUS_OK && st != LQL_STATUS_STOP) {
    return lua_lql_fail(L, &error);
  }
  lua_lql_push_query_result(L, &result);
  return 1;
}

static int lua_lql_each_match_file(lua_State *L) {
  lua_lql_client *client;
  const char *selector_expr;
  const char *path;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  FILE *input;
  lql_query_options options;
  lql_query_result result;
  lua_lql_file_state state;

  client = lua_lql_check_client(L, 1);
  selector_expr = luaL_checkstring(L, 2);
  path = luaL_checkstring(L, 3);
  luaL_checktype(L, 4, LUA_TFUNCTION);
  selector = NULL;
  input = NULL;
  memset(&state, 0, sizeof(state));
  memset(&result, 0, sizeof(result));
  lua_lql_options_query(L, 5, &options);
  lql_error_init(&error);
  st = client->ctx->selector_parse(client->ctx, selector_expr, &selector,
                                   &error);
  if (st == LQL_STATUS_OK) {
    input = fopen(path, "rb");
    if (input == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to open Lua input file");
      st = LQL_STATUS_JSON_ERROR;
    }
  }
  if (st == LQL_STATUS_OK) {
    lua_pushvalue(L, 4);
    state.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    state.ctx = client->ctx;
    state.lua = L;
    state.error = &error;
    st = client->ctx->query_file_matches_with_options(
        client->ctx, selector, input, &options, lua_lql_on_each_match_file,
        &state, &result, &error);
    luaL_unref(L, LUA_REGISTRYINDEX, state.callback_ref);
  }
  if (state.callback_failed) {
    lua_lql_set_error(&error, LQL_STATUS_INVALID_ARGUMENT,
                      state.callback_message);
  }
  if (input != NULL) {
    fclose(input);
  }
  client->ctx->selector_destroy(client->ctx, selector);
  if (st != LQL_STATUS_OK && st != LQL_STATUS_STOP) {
    return lua_lql_fail(L, &error);
  }
  lua_lql_push_query_result(L, &result);
  return 1;
}

static int lua_lql_project_file(lua_State *L) {
  lua_lql_client *client;
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

  client = lua_lql_check_client(L, 1);
  selector_expr = luaL_checkstring(L, 2);
  path = luaL_checkstring(L, 3);
  fields = NULL;
  field_count = 0u;
  if (!lua_lql_fields(L, 4, &fields, &field_count)) {
    return luaL_error(L, "out of memory");
  }
  selector = NULL;
  projection = NULL;
  input = NULL;
  out = NULL;
  lua_lql_buffer_init(&buffer, L);
  memset(&state, 0, sizeof(state));
  lql_error_init(&error);
  st = client->ctx->selector_parse(client->ctx, selector_expr, &selector,
                                   &error);
  if (st == LQL_STATUS_OK) {
    st = client->ctx->projection_parse(client->ctx, fields, field_count,
                                       &projection, &error);
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
    state.ctx = client->ctx;
    state.source = input;
    state.out = out;
    state.projection = projection;
    state.error = &error;
    st = client->ctx->query_file_matches(client->ctx, selector, input,
                                         lua_lql_on_project_file, &state, NULL,
                                         &error);
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
  client->ctx->projection_destroy(client->ctx, projection);
  client->ctx->selector_destroy(client->ctx, selector);
  if (fields != NULL) {
    (void)lua_lql_alloc(L, (void *)fields, sizeof(fields[0]) * field_count,
                        0u);
  }
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static int lua_lql_mutate_file(lua_State *L) {
  lua_lql_client *client;
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

  client = lua_lql_check_client(L, 1);
  selector_expr = luaL_checkstring(L, 2);
  path = luaL_checkstring(L, 3);
  selector = NULL;
  plan = NULL;
  input = NULL;
  out = NULL;
  lua_lql_buffer_init(&buffer, L);
  memset(&state, 0, sizeof(state));
  lql_error_init(&error);
  st = client->ctx->selector_parse(client->ctx, selector_expr, &selector,
                                   &error);
  if (st == LQL_STATUS_OK) {
    st = lua_lql_parse_mutation_plan(L, client->ctx, 4, 5, NULL, &plan, &error);
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
    state.ctx = client->ctx;
    state.source = input;
    state.out = out;
    state.compact = lua_lql_options_bool(L, 5, "compact");
    state.matches_only = lua_lql_options_bool(L, 5, "matches_only");
    state.mutation_plan = plan;
    state.error = &error;
    st = client->ctx->query_file_decisions(client->ctx, selector, input,
                                           lua_lql_on_mutate_file, &state, NULL,
                                           &error);
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
  client->ctx->mutation_plan_destroy(client->ctx, plan);
  client->ctx->selector_destroy(client->ctx, selector);
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static const luaL_Reg lua_lql_client_methods[] = {
    {"matches_json", lua_lql_matches_json},
    {"select_json", lua_lql_select_json},
    {"select_file", lua_lql_select_file},
    {"query_file", lua_lql_query_file},
    {"each_match_file", lua_lql_each_match_file},
    {"project_json", lua_lql_project_json},
    {"project_file", lua_lql_project_file},
    {"mutate_json", lua_lql_mutate_json},
    {"mutate_file", lua_lql_mutate_file},
    {NULL, NULL}};

static const luaL_Reg lua_lql_client_meta[] = {{"__gc", lua_lql_client_gc},
                                               {NULL, NULL}};

static const luaL_Reg lua_lql_functions[] = {{"new", lua_lql_new_client},
                                             {NULL, NULL}};

int luaopen_lql_core(lua_State *L) {
  luaL_newmetatable(L, LUA_LQL_CLIENT);
  luaL_setfuncs(L, lua_lql_client_meta, 0);
  lua_newtable(L);
  luaL_setfuncs(L, lua_lql_client_methods, 0);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  luaL_newlib(L, lua_lql_functions);
  return 1;
}
