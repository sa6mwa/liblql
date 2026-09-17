#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <lql/lql.h>

#include <lauxlib.h>
#include <lua.h>

#include <stdio.h>
#include <string.h>

#if LUA_VERSION_NUM != 505
#error "liblql Lua bindings support Lua 5.5 only"
#endif

#define LUA_LQL_CLIENT "lql.client"
#define LUA_LQL_SELECTOR "lql.selector"
#define LUA_LQL_SELECTOR_NODE "lql.selector_node"
#define LUA_LQL_PROJECTION "lql.projection"
#define LUA_LQL_MUTATION "lql.mutation"
#define LUA_LQL_STREAM_VALUE "lql.stream_value"
#define LUA_LQL_STREAM_WRITER "lql.stream_writer"
#define LUA_LQL_MUTATION_CALLBACK_OWNERS "lql.mutation_callback_owners"

typedef struct lua_lql_client {
  lql *ctx;
} lua_lql_client;

typedef struct lua_lql_selector {
  lql *ctx;
  lql_selector *selector;
} lua_lql_selector;

/* A cursor is a borrowed public C AST view. Its Lua uservalue retains the
 * selector userdata that owns the C selector for the cursor's full lifetime. */
typedef struct lua_lql_selector_node {
  lql *ctx;
  lql_selector_node node;
} lua_lql_selector_node;

typedef struct lua_lql_projection {
  lql *ctx;
  lql_projection *projection;
} lua_lql_projection;

/* A mutation keeps its callback options borrowed by the C handle. Persistent
 * userdata owns them through its uservalue; temporary parsed mutations retain
 * registry references only for their enclosing C call. */
typedef struct lua_lql_mutation_bridge {
  lua_State *lua;
  int callback_owner;
  int file_open_ref;
  int time_now_ref;
  lql_status callback_status;
  char callback_message[256];
} lua_lql_mutation_bridge;

typedef struct lua_lql_mutation {
  lql *ctx;
  lql_mutation *mutation;
  lua_lql_mutation_bridge *bridge;
} lua_lql_mutation;

typedef struct lua_lql_mutation_reader {
  lua_lql_mutation_bridge *bridge;
  int reader_ref;
} lua_lql_mutation_reader;

typedef struct lua_lql_string_reader {
  const char *data;
  size_t len;
  size_t offset;
} lua_lql_string_reader;

typedef struct lua_lql_buffer {
  lua_State *lua;
  char *data;
  size_t len;
  size_t cap;
} lua_lql_buffer;

typedef struct lua_lql_stream_value {
  const lql_stream_value *value;
} lua_lql_stream_value;

typedef struct lua_lql_stream_writer {
  lql_stream_writer_fn writer;
  void *writer_user;
  lql_error *error;
  int active;
} lua_lql_stream_writer;

typedef struct lua_lql_stream_bridge {
  lua_State *lua;
  int reader_ref;
  int range_ref;
  int writer_ref;
  int decision_ref;
  int value_ref;
  int cancelled_ref;
  int time_now_ref;
  int projection_ref;
  int mutation_ref;
  lql_status callback_status;
  char callback_message[256];
} lua_lql_stream_bridge;

/* Parser entry points normally borrow Lua strings only for one C call. A
 * callback made by that call may nevertheless mutate its source table and run
 * collection, so mutation parsing keeps explicit registry roots until the
 * parser has finished with every expression. */
typedef struct lua_lql_registry_refs {
  int *refs;
  size_t count;
  size_t capacity;
} lua_lql_registry_refs;

static int lua_lql_return_error(lua_State *lua, const lql_error *error,
                                lql_status fallback);

static int lua_lql_callback_error_result(lua_State *lua, int value_index,
                                         int error_index, lql_error *error,
                                         lql_status *out_status);

static void *lua_lql_alloc(lua_State *lua, void *ptr, size_t old_size,
                           size_t new_size) {
  lua_Alloc allocf;
  void *user;
  allocf = lua_getallocf(lua, &user);
  return allocf(user, ptr, old_size, new_size);
}

static int lua_lql_getfield_callback(lua_State *lua) {
  const char *field;
  field = lua_tostring(lua, 2);
  lua_getfield(lua, 1, field);
  return 1;
}

/* User tables may implement __index. Run that lookup under lua_pcall so a
 * getter exception returns a normal facade failure and every C-side owner can
 * release its roots and callback references. On success the value remains at
 * the top of the stack, matching lua_getfield. */
static int lua_lql_getfield_protected(lua_State *lua, int table_index,
                                      const char *field, lql_error *error) {
  const char *message;
  table_index = lua_absindex(lua, table_index);
  lua_pushcfunction(lua, lua_lql_getfield_callback);
  lua_pushvalue(lua, table_index);
  lua_pushstring(lua, field);
  if (lua_pcall(lua, 2, 1, 0) == LUA_OK) {
    return 1;
  }
  message = lua_tostring(lua, -1);
  if (error != NULL) {
    error->code = LQL_STATUS_CALLBACK_ERROR;
    if (message != NULL && message[0] != '\0') {
      strncpy(error->message, message, sizeof(error->message) - 1u);
      error->message[sizeof(error->message) - 1u] = '\0';
    } else {
      strcpy(error->message, "Lua table getter failed");
    }
  }
  lua_pop(lua, 1);
  return 0;
}

static void lua_lql_buffer_init(lua_lql_buffer *buffer, lua_State *lua) {
  memset(buffer, 0, sizeof(*buffer));
  buffer->lua = lua;
}

static void lua_lql_buffer_dispose(lua_lql_buffer *buffer) {
  if (buffer->data != NULL) {
    (void)lua_lql_alloc(buffer->lua, buffer->data, buffer->cap, 0u);
  }
  memset(buffer, 0, sizeof(*buffer));
}

static lql_status lua_lql_buffer_write(void *user, const void *data, size_t len,
                                       lql_error *error) {
  lua_lql_buffer *buffer;
  char *next;
  size_t cap;
  (void)error;
  buffer = (lua_lql_buffer *)user;
  if (buffer == NULL || (len != 0u && data == NULL)) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (len == 0u) {
    return LQL_STATUS_OK;
  }
  if (buffer->len > ((size_t)-1) - len) {
    return LQL_STATUS_NO_MEMORY;
  }
  if (buffer->len + len > buffer->cap) {
    cap = buffer->cap == 0u ? 256u : buffer->cap;
    while (cap < buffer->len + len) {
      if (cap > ((size_t)-1) / 2u) {
        cap = buffer->len + len;
        break;
      }
      cap *= 2u;
    }
    next = (char *)lua_lql_alloc(buffer->lua, buffer->data, buffer->cap, cap);
    if (next == NULL) {
      return LQL_STATUS_NO_MEMORY;
    }
    buffer->data = next;
    buffer->cap = cap;
  }
  memcpy(buffer->data + buffer->len, data, len);
  buffer->len += len;
  return LQL_STATUS_OK;
}

static lql_status lua_lql_string_read(void *user, unsigned char *buffer,
                                      size_t capacity, size_t *out_len,
                                      lql_error *error) {
  lua_lql_string_reader *reader;
  size_t len;
  (void)error;
  reader = (lua_lql_string_reader *)user;
  if (reader == NULL || buffer == NULL || out_len == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out_len = 0u;
  if (capacity == 0u || reader->offset >= reader->len) {
    return LQL_STATUS_OK;
  }
  len = reader->len - reader->offset;
  if (len > capacity) {
    len = capacity;
  }
  memcpy(buffer, reader->data + reader->offset, len);
  reader->offset += len;
  *out_len = len;
  return LQL_STATUS_OK;
}

static lql_status lua_lql_discard_write(void *user, const void *data,
                                        size_t len, lql_error *error) {
  (void)user;
  (void)data;
  (void)len;
  (void)error;
  return LQL_STATUS_OK;
}

static void lua_lql_bridge_fail_status(lua_lql_stream_bridge *bridge,
                                       lql_status status, const char *message) {
  if (bridge == NULL || bridge->callback_status != LQL_STATUS_OK) {
    return;
  }
  bridge->callback_status =
      status == LQL_STATUS_OK ? LQL_STATUS_CALLBACK_ERROR : status;
  if (message == NULL || message[0] == '\0') {
    strcpy(bridge->callback_message, "Lua stream callback failed");
  } else {
    strncpy(bridge->callback_message, message,
            sizeof(bridge->callback_message) - 1u);
    bridge->callback_message[sizeof(bridge->callback_message) - 1u] = '\0';
  }
}

static void lua_lql_bridge_fail(lua_lql_stream_bridge *bridge,
                                const char *message) {
  lua_lql_bridge_fail_status(bridge, LQL_STATUS_CALLBACK_ERROR, message);
}

static lql_status lua_lql_bridge_status(lua_lql_stream_bridge *bridge,
                                        lql_error *error) {
  if (bridge == NULL) {
    if (error != NULL) {
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "Lua stream callback bridge is unavailable");
    }
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (bridge->callback_status != LQL_STATUS_OK && error != NULL) {
    error->code = bridge->callback_status;
    strcpy(error->message, bridge->callback_message);
  }
  return bridge->callback_status;
}

static int lua_lql_bridge_call(lua_lql_stream_bridge *bridge, int ref,
                               int arg_count, int result_count) {
  lua_State *lua;
  const char *message;
  if (bridge == NULL) {
    return 0;
  }
  if (bridge->callback_status != LQL_STATUS_OK) {
    return 0;
  }
  if (ref == LUA_NOREF || ref == LUA_REFNIL) {
    lua_lql_bridge_fail(bridge, "Lua stream callback is unavailable");
    return 0;
  }
  lua = bridge->lua;
  lua_rawgeti(lua, LUA_REGISTRYINDEX, ref);
  lua_insert(lua, -arg_count - 1);
  if (lua_pcall(lua, arg_count, result_count, 0) == LUA_OK) {
    return 1;
  }
  message = lua_tostring(lua, -1);
  lua_lql_bridge_fail(bridge, message);
  lua_pop(lua, 1);
  return 0;
}

static lql_status lua_lql_bridge_write_ref(lua_lql_stream_bridge *bridge,
                                           int ref, const void *data,
                                           size_t len, lql_error *error) {
  lua_State *lua;
  int accepted;
  lql_status result_status;
  if (bridge == NULL || ref == LUA_NOREF || ref == LUA_REFNIL) {
    if (error != NULL) {
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "Lua writer callback is unavailable");
    }
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lua = bridge->lua;
  lua_pushlstring(lua, (const char *)data, len);
  if (!lua_lql_bridge_call(bridge, ref, 1, 2)) {
    return lua_lql_bridge_status(bridge, error);
  }
  if (lua_lql_callback_error_result(lua, -2, -1, error, &result_status)) {
    lua_pop(lua, 2);
    lua_lql_bridge_fail_status(
        bridge, result_status,
        error != NULL ? error->message : lql_status_string(result_status));
    return result_status;
  }
  accepted = !lua_isboolean(lua, -2) || lua_toboolean(lua, -2);
  lua_pop(lua, 2);
  if (!accepted) {
    lua_lql_bridge_fail(bridge, "Lua writer callback rejected output");
    if (error != NULL) {
      error->code = bridge->callback_status;
      strcpy(error->message, bridge->callback_message);
    }
    return bridge->callback_status;
  }
  return LQL_STATUS_OK;
}

static lql_status lua_lql_stream_reader(void *user, unsigned char *buffer,
                                        size_t capacity, size_t *out_len,
                                        lql_error *error) {
  lua_lql_stream_bridge *bridge;
  lua_State *lua;
  const char *chunk;
  size_t chunk_len;
  bridge = (lua_lql_stream_bridge *)user;
  if (bridge == NULL || buffer == NULL || out_len == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  *out_len = 0u;
  lua = bridge->lua;
  lua_pushinteger(lua, (lua_Integer)capacity);
  if (!lua_lql_bridge_call(bridge, bridge->reader_ref, 1, 2)) {
    return lua_lql_bridge_status(bridge, error);
  }
  {
    lql_status result_status;
    if (lua_lql_callback_error_result(lua, -2, -1, error, &result_status)) {
      lua_pop(lua, 2);
      lua_lql_bridge_fail_status(
          bridge, result_status,
          error != NULL ? error->message : lql_status_string(result_status));
      return result_status;
    }
  }
  if (lua_isnil(lua, -2)) {
    lua_pop(lua, 2);
    return LQL_STATUS_OK;
  }
  if (lua_type(lua, -2) != LUA_TSTRING) {
    lua_pop(lua, 2);
    lua_lql_bridge_fail(bridge,
                        "Lua reader callback must return string or nil");
    if (error != NULL) {
      error->code = bridge->callback_status;
      strcpy(error->message, bridge->callback_message);
    }
    return bridge->callback_status;
  }
  chunk = lua_tolstring(lua, -2, &chunk_len);
  if (chunk_len > capacity) {
    lua_pop(lua, 2);
    lua_lql_bridge_fail(
        bridge, "Lua reader callback returned more than requested bytes");
    if (error != NULL) {
      error->code = bridge->callback_status;
      strcpy(error->message, bridge->callback_message);
    }
    return bridge->callback_status;
  }
  if (chunk_len > 0u) {
    memcpy(buffer, chunk, chunk_len);
  }
  lua_pop(lua, 2);
  *out_len = chunk_len;
  return LQL_STATUS_OK;
}

static lql_status lua_lql_stream_output_writer(void *user, const void *data,
                                               size_t len, lql_error *error) {
  lua_lql_stream_bridge *bridge;
  bridge = (lua_lql_stream_bridge *)user;
  return lua_lql_bridge_write_ref(
      bridge, bridge != NULL ? bridge->writer_ref : LUA_NOREF, data, len,
      error);
}

static int lua_lql_stream_writer_write(lua_State *lua) {
  lua_lql_stream_writer *writer;
  const char *data;
  size_t len;
  lql_status status;
  writer =
      (lua_lql_stream_writer *)luaL_checkudata(lua, 1, LUA_LQL_STREAM_WRITER);
  luaL_argcheck(lua, writer != NULL && writer->active && writer->writer != NULL,
                1, "expired lql stream writer");
  data = luaL_checklstring(lua, 2, &len);
  status = writer->writer(writer->writer_user, data, len, writer->error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, writer->error, status);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

/* Lua facade methods report failures as nil, {status=..., message=...}. Lua
 * file handles conventionally report I/O failures as nil, message, errno.
 * Treat either form as a callback failure rather than as EOF or success. */
static int lua_lql_callback_error_result(lua_State *lua, int value_index,
                                         int error_index, lql_error *error,
                                         lql_status *out_status) {
  lql_status status;
  const char *message;
  value_index = lua_absindex(lua, value_index);
  error_index = lua_absindex(lua, error_index);
  if (!lua_isnil(lua, value_index) || lua_isnil(lua, error_index)) {
    return 0;
  }
  if (lua_type(lua, error_index) == LUA_TSTRING) {
    status = LQL_STATUS_IO_ERROR;
    message = lua_tostring(lua, error_index);
    if (error != NULL) {
      error->code = status;
      strncpy(error->message, message, sizeof(error->message) - 1u);
      error->message[sizeof(error->message) - 1u] = '\0';
    }
    *out_status = status;
    return 1;
  }
  if (!lua_istable(lua, error_index)) {
    status = LQL_STATUS_CALLBACK_ERROR;
    if (error != NULL) {
      error->code = status;
      strcpy(error->message, "Lua callback returned an invalid error value");
    }
    *out_status = status;
    return 1;
  }
  status = LQL_STATUS_CALLBACK_ERROR;
  lua_pushliteral(lua, "status");
  lua_rawget(lua, error_index);
  if (lua_isinteger(lua, -1) && lua_tointeger(lua, -1) != 0) {
    status = (lql_status)lua_tointeger(lua, -1);
  }
  lua_pop(lua, 1);
  message = NULL;
  lua_pushliteral(lua, "message");
  lua_rawget(lua, error_index);
  if (lua_type(lua, -1) == LUA_TSTRING) {
    message = lua_tostring(lua, -1);
  }
  if (error != NULL) {
    error->code = status;
    if (message != NULL && message[0] != '\0') {
      strncpy(error->message, message, sizeof(error->message) - 1u);
      error->message[sizeof(error->message) - 1u] = '\0';
    } else {
      strcpy(error->message, lql_status_string(status));
    }
  }
  lua_pop(lua, 1);
  *out_status = status;
  return 1;
}

static int lua_lql_push_stream_writer(lua_State *lua,
                                      lql_stream_writer_fn writer,
                                      void *writer_user, lql_error *error) {
  lua_lql_stream_writer *handle;
  handle = (lua_lql_stream_writer *)lua_newuserdatauv(lua, sizeof(*handle), 0);
  handle->writer = writer;
  handle->writer_user = writer_user;
  handle->error = error;
  handle->active = 1;
  luaL_getmetatable(lua, LUA_LQL_STREAM_WRITER);
  lua_setmetatable(lua, -2);
  return 1;
}

static lql_status lua_lql_stream_range_writer(void *user, size_t offset,
                                              size_t len,
                                              lql_stream_writer_fn writer,
                                              void *writer_user,
                                              lql_error *error) {
  lua_lql_stream_bridge *bridge;
  lua_lql_stream_writer *handle;
  int handle_ref;
  lql_status result_status;
  bridge = (lua_lql_stream_bridge *)user;
  if (bridge == NULL || bridge->range_ref == LUA_NOREF ||
      bridge->range_ref == LUA_REFNIL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lua_pushinteger(bridge->lua, (lua_Integer)offset);
  lua_pushinteger(bridge->lua, (lua_Integer)len);
  lua_lql_push_stream_writer(bridge->lua, writer, writer_user, error);
  /* Keep the temporary writer alive while lua_pcall removes its argument from
   * the stack. Lua code may retain it, but it becomes unusable on return. */
  lua_pushvalue(bridge->lua, -1);
  handle_ref = luaL_ref(bridge->lua, LUA_REGISTRYINDEX);
  if (!lua_lql_bridge_call(bridge, bridge->range_ref, 3, 2)) {
    lua_rawgeti(bridge->lua, LUA_REGISTRYINDEX, handle_ref);
    handle = (lua_lql_stream_writer *)luaL_checkudata(bridge->lua, -1,
                                                      LUA_LQL_STREAM_WRITER);
    handle->active = 0;
    lua_pop(bridge->lua, 1);
    luaL_unref(bridge->lua, LUA_REGISTRYINDEX, handle_ref);
    return lua_lql_bridge_status(bridge, error);
  }
  lua_rawgeti(bridge->lua, LUA_REGISTRYINDEX, handle_ref);
  handle = (lua_lql_stream_writer *)luaL_checkudata(bridge->lua, -1,
                                                    LUA_LQL_STREAM_WRITER);
  handle->active = 0;
  lua_pop(bridge->lua, 1);
  luaL_unref(bridge->lua, LUA_REGISTRYINDEX, handle_ref);
  if (lua_lql_callback_error_result(bridge->lua, -2, -1, error,
                                    &result_status)) {
    lua_pop(bridge->lua, 2);
    lua_lql_bridge_fail_status(
        bridge, result_status,
        error != NULL ? error->message : lql_status_string(result_status));
    return result_status;
  }
  if (lua_isboolean(bridge->lua, -2) && !lua_toboolean(bridge->lua, -2)) {
    lua_pop(bridge->lua, 2);
    lua_lql_bridge_fail(bridge, "Lua range writer callback rejected replay");
    if (error != NULL) {
      error->code = bridge->callback_status;
      strcpy(error->message, bridge->callback_message);
    }
    return bridge->callback_status;
  }
  lua_pop(bridge->lua, 2);
  return lua_lql_bridge_status(bridge, error);
}

static lql_stream_callback_result
lua_lql_stream_decision(void *user, const lql_stream_decision *decision,
                        lql_error *error) {
  lua_lql_stream_bridge *bridge;
  int continue_running;
  lql_status result_status;
  bridge = (lua_lql_stream_bridge *)user;
  if (bridge == NULL || bridge->decision_ref == LUA_NOREF ||
      bridge->decision_ref == LUA_REFNIL) {
    return LQL_STREAM_CALLBACK_CONTINUE;
  }
  lua_pushinteger(bridge->lua, (lua_Integer)decision->record_index);
  lua_pushboolean(bridge->lua, decision->matched);
  if (!lua_lql_bridge_call(bridge, bridge->decision_ref, 2, 2)) {
    (void)lua_lql_bridge_status(bridge, error);
    return LQL_STREAM_CALLBACK_ERROR;
  }
  if (lua_lql_callback_error_result(bridge->lua, -2, -1, error,
                                    &result_status)) {
    lua_pop(bridge->lua, 2);
    lua_lql_bridge_fail_status(
        bridge, result_status,
        error != NULL ? error->message : lql_status_string(result_status));
    return LQL_STREAM_CALLBACK_ERROR;
  }
  continue_running =
      !lua_isboolean(bridge->lua, -2) || lua_toboolean(bridge->lua, -2);
  lua_pop(bridge->lua, 2);
  return continue_running ? LQL_STREAM_CALLBACK_CONTINUE
                          : LQL_STREAM_CALLBACK_STOP;
}

static int lua_lql_stream_value_write_to(lua_State *lua) {
  lua_lql_stream_value *value;
  lua_lql_stream_bridge bridge;
  lql_error error;
  lql_status status;
  value = (lua_lql_stream_value *)luaL_checkudata(lua, 1, LUA_LQL_STREAM_VALUE);
  luaL_argcheck(lua, value != NULL && value->value != NULL, 1,
                "expired lql stream value");
  luaL_checktype(lua, 2, LUA_TFUNCTION);
  memset(&bridge, 0, sizeof(bridge));
  bridge.lua = lua;
  bridge.reader_ref = LUA_NOREF;
  bridge.range_ref = LUA_NOREF;
  bridge.writer_ref = LUA_NOREF;
  bridge.decision_ref = LUA_NOREF;
  bridge.value_ref = LUA_NOREF;
  bridge.cancelled_ref = LUA_NOREF;
  bridge.time_now_ref = LUA_NOREF;
  bridge.callback_status = LQL_STATUS_OK;
  lua_pushvalue(lua, 2);
  bridge.writer_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  lql_error_init(&error);
  status = lql_stream_value_write_to(value->value, lua_lql_stream_output_writer,
                                     &bridge, &error);
  luaL_unref(lua, LUA_REGISTRYINDEX, bridge.writer_ref);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int lua_lql_stream_value_size(lua_State *lua) {
  lua_lql_stream_value *value;
  value = (lua_lql_stream_value *)luaL_checkudata(lua, 1, LUA_LQL_STREAM_VALUE);
  luaL_argcheck(lua, value != NULL && value->value != NULL, 1,
                "expired lql stream value");
  lua_pushinteger(lua, (lua_Integer)lql_stream_value_size(value->value));
  return 1;
}

static lql_stream_callback_result
lua_lql_stream_on_value(void *user, const lql_stream_value *value,
                        lql_error *error) {
  lua_lql_stream_bridge *bridge;
  lua_lql_stream_value *handle;
  int continue_running;
  int handle_ref;
  lql_status result_status;
  bridge = (lua_lql_stream_bridge *)user;
  if (bridge == NULL || bridge->value_ref == LUA_NOREF ||
      bridge->value_ref == LUA_REFNIL) {
    return LQL_STREAM_CALLBACK_CONTINUE;
  }
  handle = (lua_lql_stream_value *)lua_newuserdatauv(bridge->lua,
                                                     sizeof(*handle), 0);
  handle->value = value;
  luaL_getmetatable(bridge->lua, LUA_LQL_STREAM_VALUE);
  lua_setmetatable(bridge->lua, -2);
  /* Retain the callback-scoped view across lua_pcall; it is invalidated before
   * the registry reference is released. */
  lua_pushvalue(bridge->lua, -1);
  handle_ref = luaL_ref(bridge->lua, LUA_REGISTRYINDEX);
  if (!lua_lql_bridge_call(bridge, bridge->value_ref, 1, 2)) {
    lua_rawgeti(bridge->lua, LUA_REGISTRYINDEX, handle_ref);
    handle = (lua_lql_stream_value *)luaL_checkudata(bridge->lua, -1,
                                                     LUA_LQL_STREAM_VALUE);
    handle->value = NULL;
    lua_pop(bridge->lua, 1);
    luaL_unref(bridge->lua, LUA_REGISTRYINDEX, handle_ref);
    (void)lua_lql_bridge_status(bridge, error);
    return LQL_STREAM_CALLBACK_ERROR;
  }
  lua_rawgeti(bridge->lua, LUA_REGISTRYINDEX, handle_ref);
  handle = (lua_lql_stream_value *)luaL_checkudata(bridge->lua, -1,
                                                   LUA_LQL_STREAM_VALUE);
  handle->value = NULL;
  lua_pop(bridge->lua, 1);
  luaL_unref(bridge->lua, LUA_REGISTRYINDEX, handle_ref);
  if (lua_lql_callback_error_result(bridge->lua, -2, -1, error,
                                    &result_status)) {
    lua_pop(bridge->lua, 2);
    lua_lql_bridge_fail_status(
        bridge, result_status,
        error != NULL ? error->message : lql_status_string(result_status));
    return LQL_STREAM_CALLBACK_ERROR;
  }
  continue_running =
      !lua_isboolean(bridge->lua, -2) || lua_toboolean(bridge->lua, -2);
  lua_pop(bridge->lua, 2);
  return continue_running ? LQL_STREAM_CALLBACK_CONTINUE
                          : LQL_STREAM_CALLBACK_STOP;
}

static int lua_lql_stream_cancelled(void *user) {
  lua_lql_stream_bridge *bridge;
  int cancelled;
  bridge = (lua_lql_stream_bridge *)user;
  if (bridge == NULL || bridge->callback_status != LQL_STATUS_OK ||
      bridge->cancelled_ref == LUA_NOREF ||
      bridge->cancelled_ref == LUA_REFNIL) {
    return bridge != NULL && bridge->callback_status != LQL_STATUS_OK;
  }
  if (!lua_lql_bridge_call(bridge, bridge->cancelled_ref, 0, 2)) {
    return 1;
  }
  {
    lql_error error;
    lql_status result_status;
    lql_error_init(&error);
    if (lua_lql_callback_error_result(bridge->lua, -2, -1, &error,
                                      &result_status)) {
      lua_pop(bridge->lua, 2);
      lua_lql_bridge_fail_status(bridge, result_status, error.message);
      return 1;
    }
  }
  cancelled = lua_toboolean(bridge->lua, -2);
  lua_pop(bridge->lua, 2);
  return cancelled;
}

static time_t lua_lql_stream_time_now(void *user) {
  lua_lql_stream_bridge *bridge;
  lua_Integer value;
  bridge = (lua_lql_stream_bridge *)user;
  if (bridge == NULL || bridge->time_now_ref == LUA_NOREF ||
      bridge->time_now_ref == LUA_REFNIL) {
    return time(NULL);
  }
  if (!lua_lql_bridge_call(bridge, bridge->time_now_ref, 0, 2)) {
    return time(NULL);
  }
  {
    lql_error error;
    lql_status result_status;
    lql_error_init(&error);
    if (lua_lql_callback_error_result(bridge->lua, -2, -1, &error,
                                      &result_status)) {
      lua_pop(bridge->lua, 2);
      lua_lql_bridge_fail_status(bridge, result_status, error.message);
      return time(NULL);
    }
  }
  if (!lua_isinteger(bridge->lua, -2)) {
    lua_pop(bridge->lua, 2);
    lua_lql_bridge_fail(bridge, "Lua time_now callback must return an integer");
    return time(NULL);
  }
  value = lua_tointeger(bridge->lua, -2);
  lua_pop(bridge->lua, 2);
  return (time_t)value;
}

static void lua_lql_stream_bridge_init(lua_lql_stream_bridge *bridge,
                                       lua_State *lua) {
  memset(bridge, 0, sizeof(*bridge));
  bridge->lua = lua;
  bridge->reader_ref = LUA_NOREF;
  bridge->range_ref = LUA_NOREF;
  bridge->writer_ref = LUA_NOREF;
  bridge->decision_ref = LUA_NOREF;
  bridge->value_ref = LUA_NOREF;
  bridge->cancelled_ref = LUA_NOREF;
  bridge->time_now_ref = LUA_NOREF;
  bridge->projection_ref = LUA_NOREF;
  bridge->mutation_ref = LUA_NOREF;
  bridge->callback_status = LQL_STATUS_OK;
}

static void lua_lql_stream_bridge_unref(lua_lql_stream_bridge *bridge) {
#define LUA_LQL_UNREF_BRIDGE_FIELD(field)                                      \
  do {                                                                         \
    if (bridge->field != LUA_NOREF && bridge->field != LUA_REFNIL) {           \
      luaL_unref(bridge->lua, LUA_REGISTRYINDEX, bridge->field);               \
      bridge->field = LUA_NOREF;                                               \
    }                                                                          \
  } while (0)
  LUA_LQL_UNREF_BRIDGE_FIELD(reader_ref);
  LUA_LQL_UNREF_BRIDGE_FIELD(range_ref);
  LUA_LQL_UNREF_BRIDGE_FIELD(writer_ref);
  LUA_LQL_UNREF_BRIDGE_FIELD(decision_ref);
  LUA_LQL_UNREF_BRIDGE_FIELD(value_ref);
  LUA_LQL_UNREF_BRIDGE_FIELD(cancelled_ref);
  LUA_LQL_UNREF_BRIDGE_FIELD(time_now_ref);
  LUA_LQL_UNREF_BRIDGE_FIELD(projection_ref);
  LUA_LQL_UNREF_BRIDGE_FIELD(mutation_ref);
#undef LUA_LQL_UNREF_BRIDGE_FIELD
}

static void lua_lql_stream_bridge_hold(lua_lql_stream_bridge *bridge, int index,
                                       int *out_ref) {
  if (bridge == NULL || out_ref == NULL || *out_ref != LUA_NOREF) {
    return;
  }
  lua_pushvalue(bridge->lua, index);
  *out_ref = luaL_ref(bridge->lua, LUA_REGISTRYINDEX);
}

static void lua_lql_push_error(lua_State *lua, const lql_error *error,
                               lql_status fallback) {
  lql_status code;
  code = error != NULL ? error->code : fallback;
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)code);
  lua_setfield(lua, -2, "status");
  lua_pushstring(lua, lql_status_string(code));
  lua_setfield(lua, -2, "status_string");
  if (error != NULL && error->message[0] != '\0') {
    lua_pushstring(lua, error->message);
  } else {
    lua_pushstring(lua, lql_status_string(code));
  }
  lua_setfield(lua, -2, "message");
}

static int lua_lql_return_error(lua_State *lua, const lql_error *error,
                                lql_status fallback) {
  lua_pushnil(lua);
  lua_lql_push_error(lua, error, fallback);
  return 2;
}

static lua_lql_client *lua_lql_check_client(lua_State *lua, int index) {
  lua_lql_client *client;
  client = (lua_lql_client *)luaL_checkudata(lua, index, LUA_LQL_CLIENT);
  luaL_argcheck(lua, client != NULL && client->ctx != NULL, index,
                "closed lql client");
  return client;
}

static lua_lql_selector *lua_lql_test_selector(lua_State *lua, int index) {
  return (lua_lql_selector *)luaL_testudata(lua, index, LUA_LQL_SELECTOR);
}

static lua_lql_projection *lua_lql_test_projection(lua_State *lua, int index) {
  return (lua_lql_projection *)luaL_testudata(lua, index, LUA_LQL_PROJECTION);
}

static lua_lql_mutation *lua_lql_test_mutation(lua_State *lua, int index) {
  return (lua_lql_mutation *)luaL_testudata(lua, index, LUA_LQL_MUTATION);
}

static lua_lql_selector *lua_lql_check_selector(lua_State *lua, int index) {
  lua_lql_selector *selector;
  selector = (lua_lql_selector *)luaL_checkudata(lua, index, LUA_LQL_SELECTOR);
  luaL_argcheck(lua,
                selector != NULL && selector->ctx != NULL &&
                    selector->selector != NULL,
                index, "closed lql selector");
  return selector;
}

static lua_lql_selector_node *lua_lql_check_selector_node(lua_State *lua,
                                                          int index) {
  lua_lql_selector_node *node;
  node = (lua_lql_selector_node *)luaL_checkudata(lua, index,
                                                  LUA_LQL_SELECTOR_NODE);
  luaL_argcheck(lua,
                node != NULL && node->ctx != NULL && node->node.impl != NULL,
                index, "expired lql selector node");
  return node;
}

static lua_lql_projection *lua_lql_check_projection(lua_State *lua, int index) {
  lua_lql_projection *projection;
  projection =
      (lua_lql_projection *)luaL_checkudata(lua, index, LUA_LQL_PROJECTION);
  luaL_argcheck(lua,
                projection != NULL && projection->ctx != NULL &&
                    projection->projection != NULL,
                index, "closed lql projection");
  return projection;
}

static lua_lql_mutation *lua_lql_check_mutation(lua_State *lua, int index) {
  lua_lql_mutation *mutation;
  mutation = (lua_lql_mutation *)luaL_checkudata(lua, index, LUA_LQL_MUTATION);
  luaL_argcheck(lua,
                mutation != NULL && mutation->ctx != NULL &&
                    mutation->mutation != NULL,
                index, "closed lql mutation");
  return mutation;
}

static int lua_lql_push_selector(lua_State *lua, lua_lql_client *client,
                                 lql_selector *selector) {
  lua_lql_selector *handle;
  handle = (lua_lql_selector *)lua_newuserdatauv(lua, sizeof(*handle), 1);
  handle->ctx = client->ctx;
  handle->selector = selector;
  luaL_getmetatable(lua, LUA_LQL_SELECTOR);
  lua_setmetatable(lua, -2);
  lua_pushvalue(lua, 1);
  lua_setiuservalue(lua, -2, 1);
  return 1;
}

static int lua_lql_push_selector_node(lua_State *lua, int owner_index, lql *ctx,
                                      lql_selector_node node) {
  lua_lql_selector_node *handle;
  handle = (lua_lql_selector_node *)lua_newuserdatauv(lua, sizeof(*handle), 1);
  handle->ctx = ctx;
  handle->node = node;
  luaL_getmetatable(lua, LUA_LQL_SELECTOR_NODE);
  lua_setmetatable(lua, -2);
  lua_pushvalue(lua, owner_index);
  lua_setiuservalue(lua, -2, 1);
  return 1;
}

static void lua_lql_push_string_view(lua_State *lua, lql_string_view view) {
  lua_pushlstring(lua, view.data != NULL ? view.data : "", view.len);
}

static void lua_lql_push_optional_string_view(lua_State *lua,
                                              lql_string_view view) {
  if (view.data == NULL || view.len == 0u) {
    lua_pushnil(lua);
    return;
  }
  lua_pushlstring(lua, view.data, view.len);
}

static const char *lua_lql_selector_kind_name(lql_selector_node_kind kind) {
  switch (kind) {
  case LQL_SELECTOR_NODE_ALL:
    return "all";
  case LQL_SELECTOR_NODE_AND:
    return "and";
  case LQL_SELECTOR_NODE_OR:
    return "or";
  case LQL_SELECTOR_NODE_NOT:
    return "not";
  case LQL_SELECTOR_NODE_EQ:
    return "eq";
  case LQL_SELECTOR_NODE_CONTAINS:
    return "contains";
  case LQL_SELECTOR_NODE_ICONTAINS:
    return "icontains";
  case LQL_SELECTOR_NODE_PREFIX:
    return "prefix";
  case LQL_SELECTOR_NODE_IPREFIX:
    return "iprefix";
  case LQL_SELECTOR_NODE_RANGE:
    return "range";
  case LQL_SELECTOR_NODE_DATE:
    return "date";
  case LQL_SELECTOR_NODE_IN:
    return "in";
  case LQL_SELECTOR_NODE_EXISTS:
    return "exists";
  default:
    return "unknown";
  }
}

static int lua_lql_selector_kind_from_lua(lua_State *lua, int index,
                                          lql_selector_node_kind *out,
                                          lql_error *error) {
  const char *name;
  if (lua_type(lua, index) != LUA_TSTRING) {
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    strcpy(error->message, "selector kind must be a string");
    return 0;
  }
  name = lua_tostring(lua, index);
  if (strcmp(name, "and") == 0) {
    *out = LQL_SELECTOR_NODE_AND;
  } else if (strcmp(name, "or") == 0) {
    *out = LQL_SELECTOR_NODE_OR;
  } else if (strcmp(name, "not") == 0) {
    *out = LQL_SELECTOR_NODE_NOT;
  } else if (strcmp(name, "eq") == 0) {
    *out = LQL_SELECTOR_NODE_EQ;
  } else if (strcmp(name, "contains") == 0) {
    *out = LQL_SELECTOR_NODE_CONTAINS;
  } else if (strcmp(name, "icontains") == 0) {
    *out = LQL_SELECTOR_NODE_ICONTAINS;
  } else if (strcmp(name, "prefix") == 0) {
    *out = LQL_SELECTOR_NODE_PREFIX;
  } else if (strcmp(name, "iprefix") == 0) {
    *out = LQL_SELECTOR_NODE_IPREFIX;
  } else if (strcmp(name, "range") == 0) {
    *out = LQL_SELECTOR_NODE_RANGE;
  } else if (strcmp(name, "date") == 0) {
    *out = LQL_SELECTOR_NODE_DATE;
  } else if (strcmp(name, "in") == 0) {
    *out = LQL_SELECTOR_NODE_IN;
  } else if (strcmp(name, "exists") == 0) {
    *out = LQL_SELECTOR_NODE_EXISTS;
  } else {
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    strcpy(error->message, "unknown selector kind");
    return 0;
  }
  return 1;
}

static int lua_lql_push_projection(lua_State *lua, lua_lql_client *client,
                                   lql_projection *projection) {
  lua_lql_projection *handle;
  handle = (lua_lql_projection *)lua_newuserdatauv(lua, sizeof(*handle), 1);
  handle->ctx = client->ctx;
  handle->projection = projection;
  luaL_getmetatable(lua, LUA_LQL_PROJECTION);
  lua_setmetatable(lua, -2);
  lua_pushvalue(lua, 1);
  lua_setiuservalue(lua, -2, 1);
  return 1;
}

/* This weak-value registry map gives a C bridge a way to find the callback
 * table held by its mutation userdata. The userdata uservalue is the owning
 * reference, so cycles through a suspended coroutine remain collectible. */
static void lua_lql_push_mutation_callback_owners(lua_State *lua) {
  lua_getfield(lua, LUA_REGISTRYINDEX, LUA_LQL_MUTATION_CALLBACK_OWNERS);
  if (lua_istable(lua, -1)) {
    return;
  }
  lua_pop(lua, 1);
  lua_newtable(lua);
  lua_newtable(lua);
  lua_pushstring(lua, "v");
  lua_setfield(lua, -2, "__mode");
  lua_setmetatable(lua, -2);
  lua_pushvalue(lua, -1);
  lua_setfield(lua, LUA_REGISTRYINDEX, LUA_LQL_MUTATION_CALLBACK_OWNERS);
}

static int
lua_lql_mutation_bridge_push_callback(lua_lql_mutation_bridge *bridge,
                                      const char *name) {
  lua_State *lua;
  if (bridge == NULL || !bridge->callback_owner || name == NULL) {
    return 0;
  }
  lua = bridge->lua;
  lua_lql_push_mutation_callback_owners(lua);
  lua_pushlightuserdata(lua, bridge);
  lua_rawget(lua, -2);
  lua_remove(lua, -2);
  if (!lua_istable(lua, -1)) {
    lua_pop(lua, 1);
    return 0;
  }
  lua_getfield(lua, -1, name);
  lua_remove(lua, -2);
  if (lua_type(lua, -1) == LUA_TFUNCTION) {
    return 1;
  }
  lua_pop(lua, 1);
  return 0;
}

static void
lua_lql_mutation_bridge_bind_userdata(lua_State *lua,
                                      lua_lql_mutation_bridge *bridge,
                                      int mutation_index, int client_index) {
  int owner_index;
  mutation_index = lua_absindex(lua, mutation_index);
  client_index = lua_absindex(lua, client_index);
  lua_newtable(lua);
  owner_index = lua_absindex(lua, -1);
  lua_pushvalue(lua, client_index);
  lua_setfield(lua, owner_index, "client");
  lua_pushthread(lua);
  lua_setfield(lua, owner_index, "thread");
  if (bridge->file_open_ref != LUA_NOREF &&
      bridge->file_open_ref != LUA_REFNIL) {
    lua_rawgeti(lua, LUA_REGISTRYINDEX, bridge->file_open_ref);
    lua_setfield(lua, owner_index, "file_value_open");
  }
  if (bridge->time_now_ref != LUA_NOREF && bridge->time_now_ref != LUA_REFNIL) {
    lua_rawgeti(lua, LUA_REGISTRYINDEX, bridge->time_now_ref);
    lua_setfield(lua, owner_index, "time_now");
  }
  lua_pushvalue(lua, owner_index);
  lua_setiuservalue(lua, mutation_index, 1);
  lua_lql_push_mutation_callback_owners(lua);
  lua_pushlightuserdata(lua, bridge);
  lua_pushvalue(lua, owner_index);
  lua_rawset(lua, -3);
  lua_pop(lua, 2);
  if (bridge->file_open_ref != LUA_NOREF &&
      bridge->file_open_ref != LUA_REFNIL) {
    luaL_unref(lua, LUA_REGISTRYINDEX, bridge->file_open_ref);
  }
  if (bridge->time_now_ref != LUA_NOREF && bridge->time_now_ref != LUA_REFNIL) {
    luaL_unref(lua, LUA_REGISTRYINDEX, bridge->time_now_ref);
  }
  bridge->file_open_ref = LUA_NOREF;
  bridge->time_now_ref = LUA_NOREF;
  bridge->callback_owner = 1;
}

static int lua_lql_push_mutation(lua_State *lua, lua_lql_client *client,
                                 lql_mutation *mutation,
                                 lua_lql_mutation_bridge *bridge) {
  lua_lql_mutation *handle;
  handle = (lua_lql_mutation *)lua_newuserdatauv(lua, sizeof(*handle), 1);
  handle->ctx = client->ctx;
  handle->mutation = mutation;
  handle->bridge = bridge;
  luaL_getmetatable(lua, LUA_LQL_MUTATION);
  lua_setmetatable(lua, -2);
  lua_pushvalue(lua, 1);
  lua_setiuservalue(lua, -2, 1);
  return 1;
}

static void lua_lql_string_list_free(lua_State *lua, const char **items,
                                     size_t count);

static lql_status lua_lql_selector_arg(lua_State *lua, lua_lql_client *client,
                                       int index, lql_selector **out,
                                       int *owned, lql_error *error) {
  lua_lql_selector *handle;
  const char *expr;
  *out = NULL;
  *owned = 0;
  if (lua_isnoneornil(lua, index)) {
    return LQL_STATUS_OK;
  }
  handle = lua_lql_test_selector(lua, index);
  if (handle != NULL) {
    if (handle->ctx != client->ctx || handle->selector == NULL) {
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "selector belongs to another lql client");
      return LQL_STATUS_INVALID_ARGUMENT;
    }
    *out = handle->selector;
    return LQL_STATUS_OK;
  }
  if (!lua_isstring(lua, index)) {
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    strcpy(error->message, "selector must be a string or lql selector");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  expr = lua_tostring(lua, index);
  *owned = 1;
  return client->ctx->selector_parse(client->ctx, expr, out, error);
}

static lql_status lua_lql_string_list_arg(lua_State *lua, int index,
                                          const char ***out_items,
                                          size_t *out_count, lql_error *error) {
  const char **items;
  size_t count;
  size_t i;
  *out_items = NULL;
  *out_count = 0u;
  if (lua_type(lua, index) == LUA_TSTRING) {
    items = (const char **)lua_lql_alloc(lua, NULL, 0u, sizeof(*items));
    if (items == NULL) {
      error->code = LQL_STATUS_NO_MEMORY;
      strcpy(error->message, "out of memory");
      return LQL_STATUS_NO_MEMORY;
    }
    items[0] = lua_tostring(lua, index);
    *out_items = items;
    *out_count = 1u;
    return LQL_STATUS_OK;
  }
  if (lua_type(lua, index) != LUA_TTABLE) {
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    strcpy(error->message, "string or string list required");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  count = (size_t)lua_rawlen(lua, index);
  if (count == 0u) {
    return LQL_STATUS_OK;
  }
  if (count > ((size_t)-1) / sizeof(*items)) {
    error->code = LQL_STATUS_NO_MEMORY;
    strcpy(error->message, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  items = (const char **)lua_lql_alloc(lua, NULL, 0u, count * sizeof(*items));
  if (items == NULL) {
    error->code = LQL_STATUS_NO_MEMORY;
    strcpy(error->message, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, index, (lua_Integer)i + 1);
    if (lua_type(lua, -1) != LUA_TSTRING) {
      lua_pop(lua, 1);
      lua_lql_string_list_free(lua, items, count);
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "string list entries must be strings");
      return LQL_STATUS_INVALID_ARGUMENT;
    }
    items[i] = lua_tostring(lua, -1);
    lua_pop(lua, 1);
  }
  *out_items = items;
  *out_count = count;
  return LQL_STATUS_OK;
}

static void lua_lql_string_list_free(lua_State *lua, const char **items,
                                     size_t count) {
  if (items != NULL) {
    (void)lua_lql_alloc(lua, (void *)items, count * sizeof(*items), 0u);
  }
}

static void lua_lql_registry_refs_init(lua_lql_registry_refs *roots) {
  roots->refs = NULL;
  roots->count = 0u;
  roots->capacity = 0u;
}

static void lua_lql_registry_refs_release(lua_State *lua,
                                          lua_lql_registry_refs *roots) {
  size_t i;
  if (roots == NULL) {
    return;
  }
  for (i = 0u; i < roots->count; ++i) {
    if (roots->refs[i] != LUA_NOREF && roots->refs[i] != LUA_REFNIL) {
      luaL_unref(lua, LUA_REGISTRYINDEX, roots->refs[i]);
    }
  }
  if (roots->refs != NULL) {
    (void)lua_lql_alloc(lua, roots->refs,
                        roots->capacity * sizeof(*roots->refs), 0u);
  }
  lua_lql_registry_refs_init(roots);
}

/* Keep a Lua value alive after it leaves the C stack. This is needed whenever
 * a public C call borrows a string while a later table lookup can invoke
 * __index and collect the previous dynamic value. */
static int lua_lql_registry_refs_hold(lua_State *lua,
                                      lua_lql_registry_refs *roots, int index,
                                      lql_error *error) {
  size_t capacity;
  int *refs;
  if (roots->count == roots->capacity) {
    capacity = roots->capacity == 0u ? 4u : roots->capacity * 2u;
    if (capacity < roots->capacity ||
        capacity > ((size_t)-1) / sizeof(*roots->refs)) {
      error->code = LQL_STATUS_NO_MEMORY;
      strcpy(error->message, "out of memory");
      return 0;
    }
    refs = (int *)lua_lql_alloc(lua, roots->refs,
                                roots->capacity * sizeof(*roots->refs),
                                capacity * sizeof(*roots->refs));
    if (refs == NULL) {
      error->code = LQL_STATUS_NO_MEMORY;
      strcpy(error->message, "out of memory");
      return 0;
    }
    roots->refs = refs;
    roots->capacity = capacity;
  }
  lua_pushvalue(lua, index);
  roots->refs[roots->count] = luaL_ref(lua, LUA_REGISTRYINDEX);
  roots->count += 1u;
  return 1;
}

static lql_status lua_lql_string_list_root(lua_State *lua, int index,
                                           size_t count,
                                           lua_lql_registry_refs *roots,
                                           lql_error *error) {
  size_t i;
  index = lua_absindex(lua, index);
  lua_lql_registry_refs_init(roots);
  if (count == 0u) {
    return LQL_STATUS_OK;
  }
  for (i = 0u; i < count; ++i) {
    if (lua_type(lua, index) == LUA_TSTRING) {
      lua_pushvalue(lua, index);
    } else {
      lua_rawgeti(lua, index, (lua_Integer)i + 1);
    }
    if (lua_type(lua, -1) != LUA_TSTRING) {
      lua_pop(lua, 1);
      lua_lql_registry_refs_release(lua, roots);
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "string list entries must be strings");
      return LQL_STATUS_INVALID_ARGUMENT;
    }
    if (!lua_lql_registry_refs_hold(lua, roots, -1, error)) {
      lua_pop(lua, 1);
      lua_lql_registry_refs_release(lua, roots);
      return error->code;
    }
    lua_pop(lua, 1);
  }
  return LQL_STATUS_OK;
}

static lql_status lua_lql_projection_arg(lua_State *lua, lua_lql_client *client,
                                         int index, lql_projection **out,
                                         int *owned, lql_error *error) {
  lua_lql_projection *handle;
  const char **items;
  size_t count;
  lql_status status;
  *out = NULL;
  *owned = 0;
  if (lua_isnoneornil(lua, index)) {
    return LQL_STATUS_OK;
  }
  handle = lua_lql_test_projection(lua, index);
  if (handle != NULL) {
    if (handle->ctx != client->ctx || handle->projection == NULL) {
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "projection belongs to another lql client");
      return LQL_STATUS_INVALID_ARGUMENT;
    }
    *out = handle->projection;
    return LQL_STATUS_OK;
  }
  items = NULL;
  count = 0u;
  status = lua_lql_string_list_arg(lua, index, &items, &count, error);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  *owned = 1;
  status = client->ctx->projection_parse(client->ctx, items, count, out, error);
  lua_lql_string_list_free(lua, items, count);
  return status;
}

static void lua_lql_mutation_bridge_fail_status(lua_lql_mutation_bridge *bridge,
                                                lql_status status,
                                                const char *message) {
  if (bridge == NULL || bridge->callback_status != LQL_STATUS_OK) {
    return;
  }
  bridge->callback_status =
      status == LQL_STATUS_OK ? LQL_STATUS_CALLBACK_ERROR : status;
  if (message == NULL || message[0] == '\0') {
    strcpy(bridge->callback_message, "Lua mutation callback failed");
  } else {
    strncpy(bridge->callback_message, message,
            sizeof(bridge->callback_message) - 1u);
    bridge->callback_message[sizeof(bridge->callback_message) - 1u] = '\0';
  }
}

static void lua_lql_mutation_bridge_fail(lua_lql_mutation_bridge *bridge,
                                         const char *message) {
  lua_lql_mutation_bridge_fail_status(bridge, LQL_STATUS_CALLBACK_ERROR,
                                      message);
}

/* A callback failure is terminal for the C application that is in progress,
 * but a mutation userdata is reusable. Clear the previous application's state
 * before handing that userdata to a new public operation. */
static void
lua_lql_mutation_bridge_begin_application(lua_lql_mutation_bridge *bridge) {
  if (bridge == NULL) {
    return;
  }
  bridge->callback_status = LQL_STATUS_OK;
  bridge->callback_message[0] = '\0';
}

static lql_status
lua_lql_mutation_bridge_status(lua_lql_mutation_bridge *bridge,
                               lql_error *error) {
  if (bridge == NULL) {
    if (error != NULL) {
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "Lua mutation callback bridge is unavailable");
    }
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (bridge->callback_status != LQL_STATUS_OK && error != NULL) {
    error->code = bridge->callback_status;
    strcpy(error->message, bridge->callback_message);
  }
  return bridge->callback_status;
}

static int lua_lql_mutation_bridge_call(lua_lql_mutation_bridge *bridge,
                                        int ref, const char *callback_name,
                                        int arg_count, int result_count) {
  lua_State *lua;
  const char *message;
  if (bridge == NULL || bridge->callback_status != LQL_STATUS_OK) {
    return 0;
  }
  lua = bridge->lua;
  if (ref != LUA_NOREF && ref != LUA_REFNIL) {
    lua_rawgeti(lua, LUA_REGISTRYINDEX, ref);
  } else if (!lua_lql_mutation_bridge_push_callback(bridge, callback_name)) {
    lua_lql_mutation_bridge_fail(bridge,
                                 "Lua mutation callback is unavailable");
    return 0;
  }
  lua_insert(lua, -arg_count - 1);
  if (lua_pcall(lua, arg_count, result_count, 0) == LUA_OK) {
    return 1;
  }
  message = lua_tostring(lua, -1);
  lua_lql_mutation_bridge_fail(bridge, message);
  lua_pop(lua, 1);
  return 0;
}

static void lua_lql_mutation_bridge_destroy(lua_lql_mutation_bridge *bridge) {
  lua_State *lua;
  if (bridge == NULL) {
    return;
  }
  lua = bridge->lua;
  if (bridge->file_open_ref != LUA_NOREF &&
      bridge->file_open_ref != LUA_REFNIL) {
    luaL_unref(lua, LUA_REGISTRYINDEX, bridge->file_open_ref);
  }
  if (bridge->time_now_ref != LUA_NOREF && bridge->time_now_ref != LUA_REFNIL) {
    luaL_unref(lua, LUA_REGISTRYINDEX, bridge->time_now_ref);
  }
  if (bridge->callback_owner) {
    lua_lql_push_mutation_callback_owners(lua);
    lua_pushlightuserdata(lua, bridge);
    lua_pushnil(lua);
    lua_rawset(lua, -3);
    lua_pop(lua, 1);
  }
  (void)lua_lql_alloc(lua, bridge, sizeof(*bridge), 0u);
}

static lql_status lua_lql_mutation_file_reader(void *user,
                                               unsigned char *buffer,
                                               size_t capacity, size_t *out_len,
                                               lql_error *error) {
  lua_lql_mutation_reader *reader;
  lua_lql_mutation_bridge *bridge;
  lua_State *lua;
  const char *chunk;
  size_t chunk_len;
  lql_status result_status;
  reader = (lua_lql_mutation_reader *)user;
  if (reader == NULL || reader->bridge == NULL || buffer == NULL ||
      out_len == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  bridge = reader->bridge;
  lua = bridge->lua;
  *out_len = 0u;
  lua_pushinteger(lua, (lua_Integer)capacity);
  if (!lua_lql_mutation_bridge_call(bridge, reader->reader_ref, NULL, 1, 2)) {
    return lua_lql_mutation_bridge_status(bridge, error);
  }
  if (lua_lql_callback_error_result(lua, -2, -1, error, &result_status)) {
    lua_pop(lua, 2);
    lua_lql_mutation_bridge_fail_status(
        bridge, result_status,
        error != NULL ? error->message : lql_status_string(result_status));
    return result_status;
  }
  if (lua_isnil(lua, -2)) {
    lua_pop(lua, 2);
    return LQL_STATUS_OK;
  }
  if (lua_type(lua, -2) != LUA_TSTRING) {
    lua_pop(lua, 2);
    lua_lql_mutation_bridge_fail(bridge,
                                 "Lua file reader must return string or nil");
    if (error != NULL) {
      error->code = bridge->callback_status;
      strcpy(error->message, bridge->callback_message);
    }
    return bridge->callback_status;
  }
  chunk = lua_tolstring(lua, -2, &chunk_len);
  if (chunk_len > capacity) {
    lua_pop(lua, 2);
    lua_lql_mutation_bridge_fail(
        bridge, "Lua file reader returned more than requested bytes");
    if (error != NULL) {
      error->code = bridge->callback_status;
      strcpy(error->message, bridge->callback_message);
    }
    return bridge->callback_status;
  }
  if (chunk_len != 0u) {
    memcpy(buffer, chunk, chunk_len);
  }
  lua_pop(lua, 2);
  *out_len = chunk_len;
  return LQL_STATUS_OK;
}

static void lua_lql_mutation_file_close(void *user, void *reader_user) {
  lua_lql_mutation_bridge *bridge;
  lua_lql_mutation_reader *reader;
  bridge = (lua_lql_mutation_bridge *)user;
  reader = (lua_lql_mutation_reader *)reader_user;
  if (reader == NULL) {
    return;
  }
  if (reader->reader_ref != LUA_NOREF && reader->reader_ref != LUA_REFNIL) {
    luaL_unref(reader->bridge->lua, LUA_REGISTRYINDEX, reader->reader_ref);
  }
  (void)lua_lql_alloc(bridge != NULL ? bridge->lua : reader->bridge->lua,
                      reader, sizeof(*reader), 0u);
}

static lql_status lua_lql_mutation_file_open(void *user, lql_string_view path,
                                             lql_stream_reader_fn *out_reader,
                                             void **out_reader_user,
                                             lql_error *error) {
  lua_lql_mutation_bridge *bridge;
  lua_lql_mutation_reader *reader;
  lua_State *lua;
  lql_status result_status;
  bridge = (lua_lql_mutation_bridge *)user;
  if (bridge == NULL || out_reader == NULL || out_reader_user == NULL) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  lua = bridge->lua;
  *out_reader = NULL;
  *out_reader_user = NULL;
  lua_pushlstring(lua, path.data != NULL ? path.data : "", path.len);
  if (!lua_lql_mutation_bridge_call(bridge, bridge->file_open_ref,
                                    "file_value_open", 1, 2)) {
    return lua_lql_mutation_bridge_status(bridge, error);
  }
  if (lua_lql_callback_error_result(lua, -2, -1, error, &result_status)) {
    lua_pop(lua, 2);
    lua_lql_mutation_bridge_fail_status(
        bridge, result_status,
        error != NULL ? error->message : lql_status_string(result_status));
    return result_status;
  }
  if (lua_type(lua, -2) != LUA_TFUNCTION) {
    lua_pop(lua, 2);
    lua_lql_mutation_bridge_fail(bridge,
                                 "Lua file_value_open must return a function");
    if (error != NULL) {
      error->code = bridge->callback_status;
      strcpy(error->message, bridge->callback_message);
    }
    return bridge->callback_status;
  }
  reader =
      (lua_lql_mutation_reader *)lua_lql_alloc(lua, NULL, 0u, sizeof(*reader));
  if (reader == NULL) {
    lua_pop(lua, 2);
    if (error != NULL) {
      error->code = LQL_STATUS_NO_MEMORY;
      strcpy(error->message, "out of memory");
    }
    return LQL_STATUS_NO_MEMORY;
  }
  reader->bridge = bridge;
  lua_pop(lua, 1);
  reader->reader_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  *out_reader = lua_lql_mutation_file_reader;
  *out_reader_user = reader;
  return LQL_STATUS_OK;
}

static time_t lua_lql_mutation_time_now(void *user) {
  lua_lql_mutation_bridge *bridge;
  lua_Integer value;
  lql_error error;
  lql_status result_status;
  bridge = (lua_lql_mutation_bridge *)user;
  if (bridge == NULL || ((bridge->time_now_ref == LUA_NOREF ||
                          bridge->time_now_ref == LUA_REFNIL) &&
                         !bridge->callback_owner)) {
    return time(NULL);
  }
  if (!lua_lql_mutation_bridge_call(bridge, bridge->time_now_ref, "time_now", 0,
                                    2)) {
    return time(NULL);
  }
  lql_error_init(&error);
  if (lua_lql_callback_error_result(bridge->lua, -2, -1, &error,
                                    &result_status)) {
    lua_pop(bridge->lua, 2);
    lua_lql_mutation_bridge_fail_status(bridge, result_status, error.message);
    return time(NULL);
  }
  if (!lua_isinteger(bridge->lua, -2)) {
    lua_pop(bridge->lua, 2);
    lua_lql_mutation_bridge_fail(
        bridge, "Lua time_now callback must return an integer");
    return time(NULL);
  }
  value = lua_tointeger(bridge->lua, -2);
  lua_pop(bridge->lua, 2);
  return (time_t)value;
}

static int lua_lql_mutation_options(lua_State *lua, int index,
                                    lql_mutation_parse_options *options,
                                    lua_lql_mutation_bridge **out_bridge,
                                    lua_lql_registry_refs *roots,
                                    lql_error *error) {
  lua_lql_mutation_bridge *bridge;
  int file_open_ref;
  int time_now_ref;
  int has_file_open;
  int has_time_now;
  memset(options, 0, sizeof(*options));
  *out_bridge = NULL;
  if (!lua_istable(lua, index)) {
    return 1;
  }
  if (!lua_lql_getfield_protected(lua, index, "enable_file_mutations", error)) {
    return 0;
  }
  options->enable_file_values = lua_toboolean(lua, -1);
  lua_pop(lua, 1);
  if (!lua_lql_getfield_protected(lua, index, "enable_file_values", error)) {
    return 0;
  }
  if (lua_toboolean(lua, -1)) {
    options->enable_file_values = 1;
  }
  lua_pop(lua, 1);
  if (!lua_lql_getfield_protected(lua, index, "file_value_base_dir", error)) {
    return 0;
  }
  if (lua_type(lua, -1) == LUA_TSTRING) {
    if (!lua_lql_registry_refs_hold(lua, roots, -1, error)) {
      lua_pop(lua, 1);
      return 0;
    }
    options->file_value_base_dir.data =
        lua_tolstring(lua, -1, &options->file_value_base_dir.len);
  }
  lua_pop(lua, 1);
  if (options->enable_file_values &&
      options->file_value_base_dir.data == NULL) {
    options->file_value_base_dir.data = ".";
    options->file_value_base_dir.len = 1u;
  }
  has_file_open = 0;
  has_time_now = 0;
  file_open_ref = LUA_NOREF;
  time_now_ref = LUA_NOREF;
  if (!lua_lql_getfield_protected(lua, index, "file_value_open", error)) {
    goto fail_refs;
  }
  if (!lua_isnil(lua, -1)) {
    if (lua_type(lua, -1) != LUA_TFUNCTION) {
      lua_pop(lua, 1);
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "file_value_open must be a function");
      goto fail_refs;
    }
    has_file_open = 1;
    file_open_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  } else {
    lua_pop(lua, 1);
  }
  if (!lua_lql_getfield_protected(lua, index, "time_now", error)) {
    goto fail_refs;
  }
  if (!lua_isnil(lua, -1)) {
    if (lua_type(lua, -1) != LUA_TFUNCTION) {
      lua_pop(lua, 1);
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "time_now must be a function");
      goto fail_refs;
    }
    has_time_now = 1;
    time_now_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  } else {
    lua_pop(lua, 1);
  }
  if (!has_file_open && !has_time_now) {
    return 1;
  }
  bridge =
      (lua_lql_mutation_bridge *)lua_lql_alloc(lua, NULL, 0u, sizeof(*bridge));
  if (bridge == NULL) {
    error->code = LQL_STATUS_NO_MEMORY;
    strcpy(error->message, "out of memory");
    goto fail_refs;
  }
  memset(bridge, 0, sizeof(*bridge));
  bridge->lua = lua;
  bridge->callback_owner = 0;
  bridge->file_open_ref = file_open_ref;
  bridge->time_now_ref = time_now_ref;
  bridge->callback_status = LQL_STATUS_OK;
  if (has_file_open) {
    options->enable_file_values = 1;
    options->file_value_open = lua_lql_mutation_file_open;
    options->file_value_close = lua_lql_mutation_file_close;
    options->file_value_user = bridge;
  }
  if (has_time_now) {
    options->time_now = lua_lql_mutation_time_now;
    options->time_user = bridge;
  }
  *out_bridge = bridge;
  return 1;

fail_refs:
  if (file_open_ref != LUA_NOREF && file_open_ref != LUA_REFNIL) {
    luaL_unref(lua, LUA_REGISTRYINDEX, file_open_ref);
  }
  if (time_now_ref != LUA_NOREF && time_now_ref != LUA_REFNIL) {
    luaL_unref(lua, LUA_REGISTRYINDEX, time_now_ref);
  }
  return 0;
}

static lql_status lua_lql_mutation_arg(lua_State *lua, lua_lql_client *client,
                                       int index, int options_index,
                                       lql_mutation **out, int *owned,
                                       lua_lql_mutation_bridge **out_bridge,
                                       lql_error *error) {
  lua_lql_mutation *handle;
  const char **items;
  size_t count;
  lql_status status;
  lql_mutation_parse_options options;
  lua_lql_registry_refs roots;
  *out = NULL;
  *owned = 0;
  *out_bridge = NULL;
  if (lua_isnoneornil(lua, index)) {
    return LQL_STATUS_OK;
  }
  handle = lua_lql_test_mutation(lua, index);
  if (handle != NULL) {
    if (handle->ctx != client->ctx || handle->mutation == NULL) {
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error->message, "mutation belongs to another lql client");
      return LQL_STATUS_INVALID_ARGUMENT;
    }
    lua_lql_mutation_bridge_begin_application(handle->bridge);
    *out = handle->mutation;
    return LQL_STATUS_OK;
  }
  items = NULL;
  count = 0u;
  lua_lql_registry_refs_init(&roots);
  status = lua_lql_string_list_arg(lua, index, &items, &count, error);
  if (status != LQL_STATUS_OK) {
    return status;
  }
  status = lua_lql_string_list_root(lua, index, count, &roots, error);
  if (status != LQL_STATUS_OK) {
    lua_lql_string_list_free(lua, items, count);
    return status;
  }
  if (!lua_lql_mutation_options(lua, options_index, &options, out_bridge,
                                &roots, error)) {
    lua_lql_registry_refs_release(lua, &roots);
    lua_lql_string_list_free(lua, items, count);
    return error->code;
  }
  *owned = 1;
  status = client->ctx->mutation_parse_with_options(client->ctx, items, count,
                                                    &options, out, error);
  lua_lql_registry_refs_release(lua, &roots);
  lua_lql_string_list_free(lua, items, count);
  if (*out_bridge != NULL && (*out_bridge)->callback_status != LQL_STATUS_OK) {
    status = (*out_bridge)->callback_status;
    error->code = status;
    strcpy(error->message, (*out_bridge)->callback_message);
    if (*out != NULL) {
      client->ctx->mutation_destroy(client->ctx, *out);
      *out = NULL;
    }
  }
  if (status != LQL_STATUS_OK) {
    lua_lql_mutation_bridge_destroy(*out_bridge);
    *out_bridge = NULL;
  }
  return status;
}

static int lua_lql_client_gc(lua_State *lua) {
  lua_lql_client *client;
  client = (lua_lql_client *)luaL_checkudata(lua, 1, LUA_LQL_CLIENT);
  if (client != NULL && client->ctx != NULL) {
    client->ctx->destroy(client->ctx);
    client->ctx = NULL;
  }
  return 0;
}

static int lua_lql_selector_gc(lua_State *lua) {
  lua_lql_selector *selector;
  selector = (lua_lql_selector *)luaL_checkudata(lua, 1, LUA_LQL_SELECTOR);
  if (selector != NULL && selector->ctx != NULL && selector->selector != NULL) {
    selector->ctx->selector_destroy(selector->ctx, selector->selector);
    selector->selector = NULL;
  }
  if (selector != NULL) {
    selector->ctx = NULL;
  }
  return 0;
}

static int lua_lql_projection_gc(lua_State *lua) {
  lua_lql_projection *projection;
  projection =
      (lua_lql_projection *)luaL_checkudata(lua, 1, LUA_LQL_PROJECTION);
  if (projection != NULL && projection->ctx != NULL &&
      projection->projection != NULL) {
    projection->ctx->projection_destroy(projection->ctx,
                                        projection->projection);
    projection->projection = NULL;
  }
  if (projection != NULL) {
    projection->ctx = NULL;
  }
  return 0;
}

static int lua_lql_mutation_gc(lua_State *lua) {
  lua_lql_mutation *mutation;
  mutation = (lua_lql_mutation *)luaL_checkudata(lua, 1, LUA_LQL_MUTATION);
  if (mutation != NULL && mutation->ctx != NULL && mutation->mutation != NULL) {
    mutation->ctx->mutation_destroy(mutation->ctx, mutation->mutation);
    mutation->mutation = NULL;
  }
  if (mutation != NULL) {
    lua_lql_mutation_bridge_destroy(mutation->bridge);
    mutation->bridge = NULL;
    mutation->ctx = NULL;
  }
  return 0;
}

static int lua_lql_new(lua_State *lua) {
  lua_lql_client *client;
  lql_error error;
  lql_status status;
  client = (lua_lql_client *)lua_newuserdatauv(lua, sizeof(*client), 0);
  client->ctx = NULL;
  luaL_getmetatable(lua, LUA_LQL_CLIENT);
  lua_setmetatable(lua, -2);
  lql_error_init(&error);
  status = lql_new(&client->ctx, &error);
  if (status != LQL_STATUS_OK) {
    lua_pop(lua, 1);
    return lua_lql_return_error(lua, &error, status);
  }
  return 1;
}

static int lua_lql_client_version(lua_State *lua) {
  lua_lql_client *client;
  client = lua_lql_check_client(lua, 1);
  lua_pushstring(lua, client->ctx->version(client->ctx));
  return 1;
}

static int lua_lql_client_capabilities(lua_State *lua) {
  lua_lql_client *client;
  lql_capabilities caps;
  client = lua_lql_check_client(lua, 1);
  client->ctx->capabilities_get(client->ctx, &caps);
  lua_newtable(lua);
  lua_pushboolean(lua, caps.selector_parse);
  lua_setfield(lua, -2, "selector_parse");
  lua_pushboolean(lua, caps.selector_inspection);
  lua_setfield(lua, -2, "selector_inspection");
  lua_pushboolean(lua, caps.stream_apply);
  lua_setfield(lua, -2, "stream_apply");
  lua_pushboolean(lua, caps.stream_apply_spooled);
  lua_setfield(lua, -2, "stream_apply_spooled");
  lua_pushboolean(lua, 1);
  lua_setfield(lua, -2, "apply_string_spooled");
  lua_pushboolean(lua, caps.filter_file_spooled);
  lua_setfield(lua, -2, "filter_file_spooled");
  lua_pushboolean(lua, caps.rewrite_file_inline_spooled);
  lua_setfield(lua, -2, "rewrite_file_inline_spooled");
  lua_pushboolean(lua, caps.path_is_regular_file);
  lua_setfield(lua, -2, "path_is_regular_file");
  lua_pushboolean(lua, caps.projection_parse);
  lua_setfield(lua, -2, "projection_parse");
  lua_pushboolean(lua, caps.mutation_parse);
  lua_setfield(lua, -2, "mutation_parse");
  return 1;
}

static int lua_lql_client_path_is_regular_file(lua_State *lua) {
  lua_lql_client *client;
  const char *path;
  client = lua_lql_check_client(lua, 1);
  path = luaL_checkstring(lua, 2);
  lua_pushboolean(lua, client->ctx->path_is_regular_file(client->ctx, path));
  return 1;
}

static int lua_lql_client_selector_parse(lua_State *lua) {
  lua_lql_client *client;
  const char *expr;
  lql_selector *selector;
  lql_error error;
  lql_status status;
  client = lua_lql_check_client(lua, 1);
  expr = luaL_checkstring(lua, 2);
  selector = NULL;
  lql_error_init(&error);
  status = client->ctx->selector_parse(client->ctx, expr, &selector, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_selector(lua, client, selector);
}

static int lua_lql_client_selector_parse_or(lua_State *lua) {
  lua_lql_client *client;
  const char *expr;
  lql_selector *selector;
  lql_error error;
  lql_status status;
  client = lua_lql_check_client(lua, 1);
  expr = luaL_checkstring(lua, 2);
  selector = NULL;
  lql_error_init(&error);
  status = client->ctx->selector_parse_or(client->ctx, expr, &selector, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_selector(lua, client, selector);
}

static int lua_lql_client_selector_parse_json(lua_State *lua) {
  lua_lql_client *client;
  const char *json;
  size_t json_len;
  lql_selector *selector;
  lql_error error;
  lql_status status;
  client = lua_lql_check_client(lua, 1);
  json = luaL_checklstring(lua, 2, &json_len);
  selector = NULL;
  lql_error_init(&error);
  status = client->ctx->selector_parse_json(client->ctx, json, json_len,
                                            &selector, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_selector(lua, client, selector);
}

static int lua_lql_table_string_view(lua_State *lua, int table_index,
                                     const char *field, int required,
                                     lql_string_view *out,
                                     lua_lql_registry_refs *roots,
                                     lql_error *error) {
  memset(out, 0, sizeof(*out));
  if (!lua_lql_getfield_protected(lua, table_index, field, error)) {
    return 0;
  }
  if (lua_isnil(lua, -1) && !required) {
    lua_pop(lua, 1);
    return 1;
  }
  if (lua_type(lua, -1) != LUA_TSTRING) {
    lua_pop(lua, 1);
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    snprintf(error->message, sizeof(error->message),
             "selector term %s must be a string", field);
    return 0;
  }
  if (!lua_lql_registry_refs_hold(lua, roots, -1, error)) {
    lua_pop(lua, 1);
    return 0;
  }
  out->data = lua_tolstring(lua, -1, &out->len);
  lua_pop(lua, 1);
  return 1;
}

static lql_string_view *
lua_lql_table_string_views(lua_State *lua, int table_index, const char *field,
                           size_t *out_count, lua_lql_registry_refs *roots,
                           lql_error *error) {
  lql_string_view *values;
  size_t count;
  size_t i;
  *out_count = 0u;
  if (!lua_lql_getfield_protected(lua, table_index, field, error)) {
    return (lql_string_view *)-1;
  }
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return NULL;
  }
  if (lua_type(lua, -1) != LUA_TTABLE) {
    lua_pop(lua, 1);
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    snprintf(error->message, sizeof(error->message),
             "selector term %s must be a string list", field);
    return (lql_string_view *)-1;
  }
  count = (size_t)lua_rawlen(lua, -1);
  if (count > ((size_t)-1) / sizeof(*values)) {
    lua_pop(lua, 1);
    error->code = LQL_STATUS_NO_MEMORY;
    strcpy(error->message, "out of memory");
    return (lql_string_view *)-1;
  }
  values = count == 0u ? NULL
                       : (lql_string_view *)lua_lql_alloc(
                             lua, NULL, 0u, count * sizeof(*values));
  if (count > 0u && values == NULL) {
    lua_pop(lua, 1);
    error->code = LQL_STATUS_NO_MEMORY;
    strcpy(error->message, "out of memory");
    return (lql_string_view *)-1;
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, -1, (lua_Integer)i + 1);
    if (lua_type(lua, -1) != LUA_TSTRING) {
      lua_pop(lua, 2);
      (void)lua_lql_alloc(lua, values, count * sizeof(*values), 0u);
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      snprintf(error->message, sizeof(error->message),
               "selector term %s entries must be strings", field);
      return (lql_string_view *)-1;
    }
    if (!lua_lql_registry_refs_hold(lua, roots, -1, error)) {
      lua_pop(lua, 2);
      (void)lua_lql_alloc(lua, values, count * sizeof(*values), 0u);
      return (lql_string_view *)-1;
    }
    values[i].data = lua_tolstring(lua, -1, &values[i].len);
    lua_pop(lua, 1);
  }
  lua_pop(lua, 1);
  *out_count = count;
  return values;
}

static int lua_lql_selector_build_all(lua_State *lua) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_error error;
  lql_status status;
  client = lua_lql_check_client(lua, 1);
  selector = NULL;
  lql_error_init(&error);
  status = client->ctx->selector_build_all(client->ctx, &selector, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_selector(lua, client, selector);
}

static int lua_lql_selector_build_compound(lua_State *lua) {
  lua_lql_client *client;
  lql_selector_node_kind kind;
  const lql_selector **children;
  size_t count;
  size_t i;
  lua_lql_selector *child;
  lql_selector *selector;
  lql_error error;
  lql_status status;
  client = lua_lql_check_client(lua, 1);
  lql_error_init(&error);
  if (!lua_lql_selector_kind_from_lua(lua, 2, &kind, &error)) {
    return lua_lql_return_error(lua, &error, error.code);
  }
  if (kind != LQL_SELECTOR_NODE_AND && kind != LQL_SELECTOR_NODE_OR) {
    error.code = LQL_STATUS_INVALID_ARGUMENT;
    strcpy(error.message, "compound selector kind must be and or or");
    return lua_lql_return_error(lua, &error, error.code);
  }
  luaL_checktype(lua, 3, LUA_TTABLE);
  count = (size_t)lua_rawlen(lua, 3);
  children = count == 0u ? NULL
                         : (const lql_selector **)lua_lql_alloc(
                               lua, NULL, 0u, count * sizeof(*children));
  if (count > 0u && children == NULL) {
    error.code = LQL_STATUS_NO_MEMORY;
    strcpy(error.message, "out of memory");
    return lua_lql_return_error(lua, &error, error.code);
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 3, (lua_Integer)i + 1);
    child = lua_lql_test_selector(lua, -1);
    if (child == NULL || child->ctx != client->ctx || child->selector == NULL) {
      lua_pop(lua, 1);
      (void)lua_lql_alloc(lua, (void *)children, count * sizeof(*children), 0u);
      error.code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error.message,
             "compound selector children must belong to this client");
      return lua_lql_return_error(lua, &error, error.code);
    }
    children[i] = child->selector;
    lua_pop(lua, 1);
  }
  selector = NULL;
  status = client->ctx->selector_build_compound(client->ctx, kind, children,
                                                count, &selector, &error);
  (void)lua_lql_alloc(lua, (void *)children, count * sizeof(*children), 0u);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_selector(lua, client, selector);
}

static int lua_lql_selector_build_not(lua_State *lua) {
  lua_lql_client *client;
  lua_lql_selector *child;
  lql_selector *selector;
  lql_error error;
  lql_status status;
  client = lua_lql_check_client(lua, 1);
  child = lua_lql_check_selector(lua, 2);
  if (child->ctx != client->ctx) {
    lql_error_init(&error);
    error.code = LQL_STATUS_INVALID_ARGUMENT;
    strcpy(error.message, "selector belongs to another lql client");
    return lua_lql_return_error(lua, &error, error.code);
  }
  selector = NULL;
  lql_error_init(&error);
  status = client->ctx->selector_build_not(client->ctx, child->selector,
                                           &selector, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_selector(lua, client, selector);
}

static int lua_lql_selector_build_string(lua_State *lua) {
  lua_lql_client *client;
  lql_selector_node_kind kind;
  lql_selector_string_term term;
  lql_string_view *any_values;
  size_t any_count;
  lql_selector *selector;
  lql_error error;
  lql_status status;
  lua_lql_registry_refs roots;
  client = lua_lql_check_client(lua, 1);
  luaL_checktype(lua, 3, LUA_TTABLE);
  memset(&term, 0, sizeof(term));
  lql_error_init(&error);
  lua_lql_registry_refs_init(&roots);
  if (!lua_lql_selector_kind_from_lua(lua, 2, &kind, &error) ||
      !lua_lql_table_string_view(lua, 3, "field", 1, &term.field, &roots,
                                 &error)) {
    lua_lql_registry_refs_release(lua, &roots);
    return lua_lql_return_error(lua, &error, error.code);
  }
  if (kind != LQL_SELECTOR_NODE_EQ && kind != LQL_SELECTOR_NODE_CONTAINS &&
      kind != LQL_SELECTOR_NODE_ICONTAINS && kind != LQL_SELECTOR_NODE_PREFIX &&
      kind != LQL_SELECTOR_NODE_IPREFIX) {
    error.code = LQL_STATUS_INVALID_ARGUMENT;
    strcpy(error.message, "selector kind is not string-like");
    lua_lql_registry_refs_release(lua, &roots);
    return lua_lql_return_error(lua, &error, error.code);
  }
  if (!lua_lql_getfield_protected(lua, 3, "value", &error)) {
    lua_lql_registry_refs_release(lua, &roots);
    return lua_lql_return_error(lua, &error, error.code);
  }
  if (!lua_isnil(lua, -1)) {
    if (lua_type(lua, -1) != LUA_TSTRING) {
      lua_pop(lua, 1);
      error.code = LQL_STATUS_INVALID_ARGUMENT;
      strcpy(error.message, "selector term value must be a string");
      lua_lql_registry_refs_release(lua, &roots);
      return lua_lql_return_error(lua, &error, error.code);
    }
    if (!lua_lql_registry_refs_hold(lua, &roots, -1, &error)) {
      lua_pop(lua, 1);
      lua_lql_registry_refs_release(lua, &roots);
      return lua_lql_return_error(lua, &error, error.code);
    }
    term.value_present = 1;
    term.value.data = lua_tolstring(lua, -1, &term.value.len);
  }
  lua_pop(lua, 1);
  if (!lua_lql_getfield_protected(lua, 3, "ignore_case", &error)) {
    lua_lql_registry_refs_release(lua, &roots);
    return lua_lql_return_error(lua, &error, error.code);
  }
  term.ignore_case = lua_toboolean(lua, -1);
  lua_pop(lua, 1);
  any_values =
      lua_lql_table_string_views(lua, 3, "any", &any_count, &roots, &error);
  if (any_values == (lql_string_view *)-1) {
    lua_lql_registry_refs_release(lua, &roots);
    return lua_lql_return_error(lua, &error, error.code);
  }
  term.any_count = any_count;
  selector = NULL;
  status = client->ctx->selector_build_string(client->ctx, kind, &term,
                                              any_values, &selector, &error);
  (void)lua_lql_alloc(lua, any_values, any_count * sizeof(*any_values), 0u);
  lua_lql_registry_refs_release(lua, &roots);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_selector(lua, client, selector);
}

static int lua_lql_selector_bound_from_lua(lua_State *lua, int table_index,
                                           const char *field,
                                           lql_selector_range_bound *out,
                                           lua_lql_registry_refs *roots,
                                           lql_error *error) {
  const char *kind;
  memset(out, 0, sizeof(*out));
  if (!lua_lql_getfield_protected(lua, table_index, field, error)) {
    return 0;
  }
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return 1;
  }
  if (lua_type(lua, -1) != LUA_TTABLE) {
    lua_pop(lua, 1);
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    snprintf(error->message, sizeof(error->message),
             "range bound %s must be a table", field);
    return 0;
  }
  if (!lua_lql_getfield_protected(lua, -1, "kind", error)) {
    lua_pop(lua, 1);
    return 0;
  }
  if (lua_type(lua, -1) != LUA_TSTRING) {
    lua_pop(lua, 2);
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    snprintf(error->message, sizeof(error->message),
             "range bound %s kind must be a string", field);
    return 0;
  }
  if (!lua_lql_registry_refs_hold(lua, roots, -1, error)) {
    lua_pop(lua, 2);
    return 0;
  }
  kind = lua_tostring(lua, -1);
  lua_pop(lua, 1);
  if (strcmp(kind, "number") == 0) {
    out->kind = LQL_SELECTOR_BOUND_NUMBER;
    if (!lua_lql_getfield_protected(lua, -1, "number", error)) {
      lua_pop(lua, 1);
      return 0;
    }
    if (!lua_isnil(lua, -1) && !lua_isnumber(lua, -1)) {
      lua_pop(lua, 2);
      error->code = LQL_STATUS_INVALID_ARGUMENT;
      snprintf(error->message, sizeof(error->message),
               "range bound %s number must be numeric", field);
      return 0;
    }
    if (!lua_isnil(lua, -1)) {
      out->number = (double)lua_tonumber(lua, -1);
    }
    lua_pop(lua, 1);
    if (!lua_lql_table_string_view(lua, lua_gettop(lua), "number_text", 0,
                                   &out->number_text, roots, error)) {
      lua_pop(lua, 1);
      return 0;
    }
  } else if (strcmp(kind, "datetime") == 0) {
    out->kind = LQL_SELECTOR_BOUND_DATETIME;
    if (!lua_lql_table_string_view(lua, lua_gettop(lua), "datetime", 1,
                                   &out->datetime, roots, error)) {
      lua_pop(lua, 1);
      return 0;
    }
  } else if (strcmp(kind, "absent") != 0) {
    lua_pop(lua, 1);
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    snprintf(error->message, sizeof(error->message),
             "range bound %s kind is invalid", field);
    return 0;
  }
  lua_pop(lua, 1);
  return 1;
}

static int lua_lql_selector_build_range(lua_State *lua) {
  lua_lql_client *client;
  lql_selector_range_term term;
  lql_selector *selector;
  lql_error error;
  lql_status status;
  lua_lql_registry_refs roots;
  client = lua_lql_check_client(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  memset(&term, 0, sizeof(term));
  lql_error_init(&error);
  lua_lql_registry_refs_init(&roots);
  if (!lua_lql_table_string_view(lua, 2, "field", 1, &term.field, &roots,
                                 &error) ||
      !lua_lql_selector_bound_from_lua(lua, 2, "gt", &term.gt, &roots,
                                       &error) ||
      !lua_lql_selector_bound_from_lua(lua, 2, "gte", &term.gte, &roots,
                                       &error) ||
      !lua_lql_selector_bound_from_lua(lua, 2, "lt", &term.lt, &roots,
                                       &error) ||
      !lua_lql_selector_bound_from_lua(lua, 2, "lte", &term.lte, &roots,
                                       &error)) {
    lua_lql_registry_refs_release(lua, &roots);
    return lua_lql_return_error(lua, &error, error.code);
  }
  selector = NULL;
  status =
      client->ctx->selector_build_range(client->ctx, &term, &selector, &error);
  lua_lql_registry_refs_release(lua, &roots);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_selector(lua, client, selector);
}

static int lua_lql_selector_since_kind_from_lua(lua_State *lua, int index,
                                                lql_selector_since_kind *out,
                                                lql_error *error) {
  const char *kind;
  if (lua_isnoneornil(lua, index)) {
    *out = LQL_SELECTOR_SINCE_NONE;
    return 1;
  }
  if (lua_type(lua, index) != LUA_TSTRING) {
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    strcpy(error->message, "date since_kind must be a string");
    return 0;
  }
  kind = lua_tostring(lua, index);
  if (strcmp(kind, "none") == 0) {
    *out = LQL_SELECTOR_SINCE_NONE;
  } else if (strcmp(kind, "now") == 0) {
    *out = LQL_SELECTOR_SINCE_NOW;
  } else if (strcmp(kind, "today") == 0) {
    *out = LQL_SELECTOR_SINCE_TODAY;
  } else if (strcmp(kind, "yesterday") == 0) {
    *out = LQL_SELECTOR_SINCE_YESTERDAY;
  } else if (strcmp(kind, "literal") == 0) {
    *out = LQL_SELECTOR_SINCE_LITERAL;
  } else {
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    strcpy(error->message, "date since_kind is invalid");
    return 0;
  }
  return 1;
}

static const char *
lua_lql_selector_since_kind_to_lua(lql_selector_since_kind kind) {
  switch (kind) {
  case LQL_SELECTOR_SINCE_NONE:
    return "none";
  case LQL_SELECTOR_SINCE_NOW:
    return "now";
  case LQL_SELECTOR_SINCE_TODAY:
    return "today";
  case LQL_SELECTOR_SINCE_YESTERDAY:
    return "yesterday";
  case LQL_SELECTOR_SINCE_LITERAL:
    return "literal";
  }
  return "none";
}

static int lua_lql_selector_build_date(lua_State *lua) {
  lua_lql_client *client;
  lql_selector_date_term term;
  lql_selector *selector;
  lql_error error;
  lql_status status;
  lua_lql_registry_refs roots;
  client = lua_lql_check_client(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  memset(&term, 0, sizeof(term));
  lql_error_init(&error);
  lua_lql_registry_refs_init(&roots);
  if (!lua_lql_table_string_view(lua, 2, "field", 1, &term.field, &roots,
                                 &error) ||
      !lua_lql_table_string_view(lua, 2, "value", 0, &term.value, &roots,
                                 &error) ||
      !lua_lql_table_string_view(lua, 2, "since", 0, &term.since, &roots,
                                 &error) ||
      !lua_lql_table_string_view(lua, 2, "after", 0, &term.after, &roots,
                                 &error) ||
      !lua_lql_table_string_view(lua, 2, "before", 0, &term.before, &roots,
                                 &error) ||
      !lua_lql_table_string_view(lua, 2, "gt", 0, &term.gt, &roots, &error) ||
      !lua_lql_table_string_view(lua, 2, "gte", 0, &term.gte, &roots, &error) ||
      !lua_lql_table_string_view(lua, 2, "lt", 0, &term.lt, &roots, &error) ||
      !lua_lql_table_string_view(lua, 2, "lte", 0, &term.lte, &roots, &error)) {
    lua_lql_registry_refs_release(lua, &roots);
    return lua_lql_return_error(lua, &error, error.code);
  }
  if (!lua_lql_getfield_protected(lua, 2, "since_kind", &error)) {
    lua_lql_registry_refs_release(lua, &roots);
    return lua_lql_return_error(lua, &error, error.code);
  }
  if (!lua_lql_selector_since_kind_from_lua(lua, -1, &term.since_kind,
                                            &error)) {
    lua_pop(lua, 1);
    lua_lql_registry_refs_release(lua, &roots);
    return lua_lql_return_error(lua, &error, error.code);
  }
  lua_pop(lua, 1);
  selector = NULL;
  status =
      client->ctx->selector_build_date(client->ctx, &term, &selector, &error);
  lua_lql_registry_refs_release(lua, &roots);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_selector(lua, client, selector);
}

static int lua_lql_selector_build_in(lua_State *lua) {
  lua_lql_client *client;
  lql_selector_in_term term;
  lql_string_view *any_values;
  size_t any_count;
  lql_selector *selector;
  lql_error error;
  lql_status status;
  lua_lql_registry_refs roots;
  client = lua_lql_check_client(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  memset(&term, 0, sizeof(term));
  lql_error_init(&error);
  lua_lql_registry_refs_init(&roots);
  if (!lua_lql_table_string_view(lua, 2, "field", 1, &term.field, &roots,
                                 &error)) {
    lua_lql_registry_refs_release(lua, &roots);
    return lua_lql_return_error(lua, &error, error.code);
  }
  any_values =
      lua_lql_table_string_views(lua, 2, "any", &any_count, &roots, &error);
  if (any_values == (lql_string_view *)-1) {
    lua_lql_registry_refs_release(lua, &roots);
    return lua_lql_return_error(lua, &error, error.code);
  }
  term.any_count = any_count;
  selector = NULL;
  status = client->ctx->selector_build_in(client->ctx, &term, any_values,
                                          &selector, &error);
  (void)lua_lql_alloc(lua, any_values, any_count * sizeof(*any_values), 0u);
  lua_lql_registry_refs_release(lua, &roots);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_selector(lua, client, selector);
}

static int lua_lql_selector_build_exists(lua_State *lua) {
  lua_lql_client *client;
  lql_string_view path;
  lql_selector *selector;
  lql_error error;
  lql_status status;
  client = lua_lql_check_client(lua, 1);
  path.data = luaL_checklstring(lua, 2, &path.len);
  selector = NULL;
  lql_error_init(&error);
  status =
      client->ctx->selector_build_exists(client->ctx, path, &selector, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_selector(lua, client, selector);
}

static int lua_lql_client_projection_parse(lua_State *lua) {
  lua_lql_client *client;
  const char **items;
  size_t count;
  lql_projection *projection;
  lql_error error;
  lql_status status;
  client = lua_lql_check_client(lua, 1);
  items = NULL;
  count = 0u;
  lql_error_init(&error);
  status = lua_lql_string_list_arg(lua, 2, &items, &count, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  projection = NULL;
  status = client->ctx->projection_parse(client->ctx, items, count, &projection,
                                         &error);
  lua_lql_string_list_free(lua, items, count);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_projection(lua, client, projection);
}

static int lua_lql_client_mutation_parse(lua_State *lua) {
  lua_lql_client *client;
  const char **items;
  size_t count;
  lql_mutation *mutation;
  lql_mutation_parse_options options;
  lua_lql_mutation_bridge *bridge;
  lql_error error;
  lql_status status;
  lua_lql_registry_refs roots;
  client = lua_lql_check_client(lua, 1);
  items = NULL;
  count = 0u;
  lua_lql_registry_refs_init(&roots);
  lql_error_init(&error);
  status = lua_lql_string_list_arg(lua, 2, &items, &count, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  status = lua_lql_string_list_root(lua, 2, count, &roots, &error);
  if (status != LQL_STATUS_OK) {
    lua_lql_string_list_free(lua, items, count);
    return lua_lql_return_error(lua, &error, status);
  }
  bridge = NULL;
  if (!lua_lql_mutation_options(lua, 3, &options, &bridge, &roots, &error)) {
    lua_lql_registry_refs_release(lua, &roots);
    lua_lql_string_list_free(lua, items, count);
    return lua_lql_return_error(lua, &error, error.code);
  }
  mutation = NULL;
  status = client->ctx->mutation_parse_with_options(
      client->ctx, items, count, &options, &mutation, &error);
  lua_lql_registry_refs_release(lua, &roots);
  lua_lql_string_list_free(lua, items, count);
  if (bridge != NULL && bridge->callback_status != LQL_STATUS_OK) {
    status = bridge->callback_status;
    error.code = status;
    strcpy(error.message, bridge->callback_message);
    if (mutation != NULL) {
      client->ctx->mutation_destroy(client->ctx, mutation);
      mutation = NULL;
    }
  }
  if (status != LQL_STATUS_OK) {
    lua_lql_mutation_bridge_destroy(bridge);
    return lua_lql_return_error(lua, &error, status);
  }
  lua_lql_push_mutation(lua, client, mutation, bridge);
  if (bridge != NULL) {
    lua_lql_mutation_bridge_bind_userdata(lua, bridge, -1, 1);
  }
  return 1;
}

static int lua_lql_push_selector_capabilities(lua_State *lua,
                                              lql_selector_capabilities *caps) {
  lua_newtable(lua);
  lua_pushboolean(lua, caps->and_);
  lua_setfield(lua, -2, "and");
  lua_pushboolean(lua, caps->or_);
  lua_setfield(lua, -2, "or");
  lua_pushboolean(lua, caps->not_);
  lua_setfield(lua, -2, "not");
  lua_pushboolean(lua, caps->eq);
  lua_setfield(lua, -2, "eq");
  lua_pushboolean(lua, caps->range);
  lua_setfield(lua, -2, "range");
  lua_pushboolean(lua, caps->date);
  lua_setfield(lua, -2, "date");
  lua_pushboolean(lua, caps->in);
  lua_setfield(lua, -2, "in");
  lua_pushboolean(lua, caps->prefix);
  lua_setfield(lua, -2, "prefix");
  lua_pushboolean(lua, caps->contains);
  lua_setfield(lua, -2, "contains");
  lua_pushboolean(lua, caps->exists);
  lua_setfield(lua, -2, "exists");
  lua_pushboolean(lua, caps->wildcard_path);
  lua_setfield(lua, -2, "wildcard_path");
  lua_pushboolean(lua, caps->recursive_path);
  lua_setfield(lua, -2, "recursive_path");
  return 1;
}

static int lua_lql_client_selector_capabilities(lua_State *lua) {
  lua_lql_client *client;
  lql_selector *selector;
  int owned;
  lql_error error;
  lql_status status;
  lql_selector_capabilities caps;
  client = lua_lql_check_client(lua, 1);
  lql_error_init(&error);
  status = lua_lql_selector_arg(lua, client, 2, &selector, &owned, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  memset(&caps, 0, sizeof(caps));
  client->ctx->selector_capabilities_get(client->ctx, selector, &caps);
  if (owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  return lua_lql_push_selector_capabilities(lua, &caps);
}

static int lua_lql_selector_capabilities(lua_State *lua) {
  lua_lql_selector *selector;
  lql_selector_capabilities caps;
  selector = lua_lql_check_selector(lua, 1);
  memset(&caps, 0, sizeof(caps));
  selector->ctx->selector_capabilities_get(selector->ctx, selector->selector,
                                           &caps);
  return lua_lql_push_selector_capabilities(lua, &caps);
}

static int lua_lql_selector_is_empty(lua_State *lua) {
  lua_lql_selector *selector;
  selector = lua_lql_check_selector(lua, 1);
  lua_pushboolean(
      lua, selector->ctx->selector_is_empty(selector->ctx, selector->selector));
  return 1;
}

static int lua_lql_selector_root(lua_State *lua) {
  lua_lql_selector *selector;
  lql_selector_node node;
  lql_error error;
  lql_status status;
  selector = lua_lql_check_selector(lua, 1);
  memset(&node, 0, sizeof(node));
  lql_error_init(&error);
  status = selector->ctx->selector_root(selector->ctx, selector->selector,
                                        &node, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_selector_node(lua, 1, selector->ctx, node);
}

static int lua_lql_selector_write_json(lua_State *lua) {
  lua_lql_selector *selector;
  FILE *stream;
  long size;
  char *buffer;
  size_t read_count;
  lql_error error;
  lql_status status;
  selector = lua_lql_check_selector(lua, 1);
  stream = tmpfile();
  if (stream == NULL) {
    lql_error_init(&error);
    error.code = LQL_STATUS_IO_ERROR;
    strcpy(error.message, "unable to create selector JSON temporary stream");
    return lua_lql_return_error(lua, &error, LQL_STATUS_IO_ERROR);
  }
  lql_error_init(&error);
  status = selector->ctx->selector_write_json(selector->ctx, selector->selector,
                                              stream, &error);
  if (status != LQL_STATUS_OK || fflush(stream) != 0 ||
      fseek(stream, 0L, SEEK_END) != 0 || (size = ftell(stream)) < 0 ||
      fseek(stream, 0L, SEEK_SET) != 0) {
    if (status == LQL_STATUS_OK) {
      error.code = LQL_STATUS_IO_ERROR;
      strcpy(error.message, "unable to read selector JSON temporary stream");
      status = LQL_STATUS_IO_ERROR;
    }
    fclose(stream);
    return lua_lql_return_error(lua, &error, status);
  }
  buffer = (char *)lua_lql_alloc(lua, NULL, 0u, (size_t)size);
  if (size > 0L && buffer == NULL) {
    fclose(stream);
    lql_error_init(&error);
    error.code = LQL_STATUS_NO_MEMORY;
    strcpy(error.message, "out of memory");
    return lua_lql_return_error(lua, &error, LQL_STATUS_NO_MEMORY);
  }
  read_count = size > 0L ? fread(buffer, 1u, (size_t)size, stream) : 0u;
  fclose(stream);
  if (read_count != (size_t)size) {
    (void)lua_lql_alloc(lua, buffer, (size_t)size, 0u);
    lql_error_init(&error);
    error.code = LQL_STATUS_IO_ERROR;
    strcpy(error.message, "unable to read selector JSON temporary stream");
    return lua_lql_return_error(lua, &error, LQL_STATUS_IO_ERROR);
  }
  lua_pushlstring(lua, buffer != NULL ? buffer : "", (size_t)size);
  (void)lua_lql_alloc(lua, buffer, (size_t)size, 0u);
  return 1;
}

static int lua_lql_selector_node_kind(lua_State *lua) {
  lua_lql_selector_node *node;
  node = lua_lql_check_selector_node(lua, 1);
  lua_pushstring(lua, lua_lql_selector_kind_name(node->node.kind));
  return 1;
}

static int lua_lql_selector_node_child_count(lua_State *lua) {
  lua_lql_selector_node *node;
  size_t count;
  lql_error error;
  lql_status status;
  node = lua_lql_check_selector_node(lua, 1);
  count = 0u;
  lql_error_init(&error);
  status = node->ctx->selector_node_child_count(node->ctx, node->node, &count,
                                                &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  lua_pushinteger(lua, (lua_Integer)count);
  return 1;
}

static int lua_lql_selector_node_child(lua_State *lua) {
  lua_lql_selector_node *node;
  lua_Integer index;
  lql_selector_node child;
  lql_error error;
  lql_status status;
  node = lua_lql_check_selector_node(lua, 1);
  index = luaL_checkinteger(lua, 2);
  if (index < 1) {
    return luaL_argerror(lua, 2, "selector child index is 1-based");
  }
  memset(&child, 0, sizeof(child));
  lql_error_init(&error);
  status = node->ctx->selector_node_child(node->ctx, node->node,
                                          (size_t)(index - 1), &child, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_selector_node(lua, 1, node->ctx, child);
}

static void lua_lql_push_range_bound(lua_State *lua,
                                     lql_selector_range_bound bound) {
  lua_newtable(lua);
  if (bound.kind == LQL_SELECTOR_BOUND_NUMBER) {
    lua_pushstring(lua, "number");
  } else if (bound.kind == LQL_SELECTOR_BOUND_DATETIME) {
    lua_pushstring(lua, "datetime");
  } else {
    lua_pushstring(lua, "absent");
  }
  lua_setfield(lua, -2, "kind");
  if (bound.kind == LQL_SELECTOR_BOUND_NUMBER) {
    lua_pushnumber(lua, (lua_Number)bound.number);
    lua_setfield(lua, -2, "number");
    lua_lql_push_string_view(lua, bound.number_text);
    lua_setfield(lua, -2, "number_text");
  } else if (bound.kind == LQL_SELECTOR_BOUND_DATETIME) {
    lua_lql_push_string_view(lua, bound.datetime);
    lua_setfield(lua, -2, "datetime");
  }
}

static int lua_lql_selector_node_string_term(lua_State *lua) {
  lua_lql_selector_node *node;
  lql_selector_string_term term;
  lql_error error;
  lql_status status;
  node = lua_lql_check_selector_node(lua, 1);
  memset(&term, 0, sizeof(term));
  lql_error_init(&error);
  status = node->ctx->selector_node_string_term(node->ctx, node->node, &term,
                                                &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  lua_newtable(lua);
  lua_lql_push_string_view(lua, term.field);
  lua_setfield(lua, -2, "field");
  lua_pushboolean(lua, term.value_present);
  lua_setfield(lua, -2, "value_present");
  if (term.value_present) {
    lua_lql_push_string_view(lua, term.value);
    lua_setfield(lua, -2, "value");
  }
  lua_pushboolean(lua, term.ignore_case);
  lua_setfield(lua, -2, "ignore_case");
  lua_pushinteger(lua, (lua_Integer)term.any_count);
  lua_setfield(lua, -2, "any_count");
  return 1;
}

static int lua_lql_selector_node_string_term_any(lua_State *lua) {
  lua_lql_selector_node *node;
  lua_Integer index;
  lql_string_view value;
  lql_error error;
  lql_status status;
  node = lua_lql_check_selector_node(lua, 1);
  index = luaL_checkinteger(lua, 2);
  if (index < 1) {
    return luaL_argerror(lua, 2, "selector any index is 1-based");
  }
  memset(&value, 0, sizeof(value));
  lql_error_init(&error);
  status = node->ctx->selector_node_string_term_any(
      node->ctx, node->node, (size_t)(index - 1), &value, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  lua_lql_push_string_view(lua, value);
  return 1;
}

static int lua_lql_selector_node_range_term(lua_State *lua) {
  lua_lql_selector_node *node;
  lql_selector_range_term term;
  lql_error error;
  lql_status status;
  node = lua_lql_check_selector_node(lua, 1);
  memset(&term, 0, sizeof(term));
  lql_error_init(&error);
  status =
      node->ctx->selector_node_range_term(node->ctx, node->node, &term, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  lua_newtable(lua);
  lua_lql_push_string_view(lua, term.field);
  lua_setfield(lua, -2, "field");
  lua_lql_push_range_bound(lua, term.gt);
  lua_setfield(lua, -2, "gt");
  lua_lql_push_range_bound(lua, term.gte);
  lua_setfield(lua, -2, "gte");
  lua_lql_push_range_bound(lua, term.lt);
  lua_setfield(lua, -2, "lt");
  lua_lql_push_range_bound(lua, term.lte);
  lua_setfield(lua, -2, "lte");
  return 1;
}

static int lua_lql_selector_node_date_term(lua_State *lua) {
  lua_lql_selector_node *node;
  lql_selector_date_term term;
  lql_error error;
  lql_status status;
  node = lua_lql_check_selector_node(lua, 1);
  memset(&term, 0, sizeof(term));
  lql_error_init(&error);
  status =
      node->ctx->selector_node_date_term(node->ctx, node->node, &term, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  lua_newtable(lua);
  lua_lql_push_string_view(lua, term.field);
  lua_setfield(lua, -2, "field");
  lua_lql_push_optional_string_view(lua, term.value);
  lua_setfield(lua, -2, "value");
  lua_lql_push_optional_string_view(lua, term.since);
  lua_setfield(lua, -2, "since");
  lua_lql_push_optional_string_view(lua, term.after);
  lua_setfield(lua, -2, "after");
  lua_lql_push_optional_string_view(lua, term.before);
  lua_setfield(lua, -2, "before");
  lua_lql_push_optional_string_view(lua, term.gt);
  lua_setfield(lua, -2, "gt");
  lua_lql_push_optional_string_view(lua, term.gte);
  lua_setfield(lua, -2, "gte");
  lua_lql_push_optional_string_view(lua, term.lt);
  lua_setfield(lua, -2, "lt");
  lua_lql_push_optional_string_view(lua, term.lte);
  lua_setfield(lua, -2, "lte");
  lua_pushstring(lua, lua_lql_selector_since_kind_to_lua(term.since_kind));
  lua_setfield(lua, -2, "since_kind");
  return 1;
}

static int lua_lql_selector_node_in_term(lua_State *lua) {
  lua_lql_selector_node *node;
  lql_selector_in_term term;
  lql_error error;
  lql_status status;
  node = lua_lql_check_selector_node(lua, 1);
  memset(&term, 0, sizeof(term));
  lql_error_init(&error);
  status =
      node->ctx->selector_node_in_term(node->ctx, node->node, &term, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  lua_newtable(lua);
  lua_lql_push_string_view(lua, term.field);
  lua_setfield(lua, -2, "field");
  lua_pushinteger(lua, (lua_Integer)term.any_count);
  lua_setfield(lua, -2, "any_count");
  return 1;
}

static int lua_lql_selector_node_in_term_any(lua_State *lua) {
  lua_lql_selector_node *node;
  lua_Integer index;
  lql_string_view value;
  lql_error error;
  lql_status status;
  node = lua_lql_check_selector_node(lua, 1);
  index = luaL_checkinteger(lua, 2);
  if (index < 1) {
    return luaL_argerror(lua, 2, "selector any index is 1-based");
  }
  memset(&value, 0, sizeof(value));
  lql_error_init(&error);
  status = node->ctx->selector_node_in_term_any(
      node->ctx, node->node, (size_t)(index - 1), &value, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  lua_lql_push_string_view(lua, value);
  return 1;
}

static int lua_lql_selector_node_exists_path(lua_State *lua) {
  lua_lql_selector_node *node;
  lql_string_view path;
  lql_error error;
  lql_status status;
  node = lua_lql_check_selector_node(lua, 1);
  memset(&path, 0, sizeof(path));
  lql_error_init(&error);
  status = node->ctx->selector_node_exists_path(node->ctx, node->node, &path,
                                                &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  lua_lql_push_string_view(lua, path);
  return 1;
}

static int lua_lql_projection_path_count(lua_State *lua) {
  lua_lql_projection *projection;
  projection = lua_lql_check_projection(lua, 1);
  lua_pushinteger(lua, (lua_Integer)projection->ctx->projection_path_count(
                           projection->ctx, projection->projection));
  return 1;
}

static int lua_lql_projection_path(lua_State *lua) {
  lua_lql_projection *projection;
  lua_Integer index;
  lql_string_view view;
  lql_error error;
  lql_status status;
  projection = lua_lql_check_projection(lua, 1);
  index = luaL_checkinteger(lua, 2);
  if (index < 1) {
    luaL_argerror(lua, 2, "projection path index is 1-based");
  }
  lql_error_init(&error);
  status =
      projection->ctx->projection_path(projection->ctx, projection->projection,
                                       (size_t)(index - 1), &view, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  lua_pushlstring(lua, view.data, view.len);
  return 1;
}

static int lua_lql_mutation_count(lua_State *lua) {
  lua_lql_mutation *mutation;
  mutation = lua_lql_check_mutation(lua, 1);
  lua_pushinteger(lua, (lua_Integer)mutation->ctx->mutation_count(
                           mutation->ctx, mutation->mutation));
  return 1;
}

static int lua_lql_stream_option_function(lua_State *lua, int options_index,
                                          const char *name, int *out_ref,
                                          lql_error *error) {
  *out_ref = LUA_NOREF;
  if (!lua_istable(lua, options_index)) {
    return 1;
  }
  if (!lua_lql_getfield_protected(lua, options_index, name, error)) {
    return 0;
  }
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return 1;
  }
  if (lua_type(lua, -1) != LUA_TFUNCTION) {
    lua_pop(lua, 1);
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    snprintf(error->message, sizeof(error->message),
             "stream option %s must be a function", name);
    return 0;
  }
  *out_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  return 1;
}

static int lua_lql_stream_limit(lua_State *lua, int table_index,
                                const char *name, size_t *out,
                                lql_error *error) {
  lua_Integer value;
  table_index = lua_absindex(lua, table_index);
  if (!lua_lql_getfield_protected(lua, table_index, name, error)) {
    return 0;
  }
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return 1;
  }
  if (!lua_isinteger(lua, -1) || lua_tointeger(lua, -1) < 0) {
    lua_pop(lua, 1);
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    snprintf(error->message, sizeof(error->message),
             "stream limit %s must be a non-negative integer", name);
    return 0;
  }
  value = lua_tointeger(lua, -1);
  if ((lua_Integer)(size_t)value != value) {
    lua_pop(lua, 1);
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    snprintf(error->message, sizeof(error->message),
             "stream limit %s exceeds the target size_t range", name);
    return 0;
  }
  lua_pop(lua, 1);
  *out = (size_t)value;
  return 1;
}

static int lua_lql_stream_output_mode(lua_State *lua, int options_index,
                                      lql_stream_output_mode *out,
                                      lql_error *error) {
  const char *name;
  *out = LQL_STREAM_OUTPUT_DECISION_ONLY;
  if (!lua_istable(lua, options_index)) {
    return 1;
  }
  if (!lua_lql_getfield_protected(lua, options_index, "output_mode", error)) {
    return 0;
  }
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return 1;
  }
  if (lua_type(lua, -1) != LUA_TSTRING) {
    lua_pop(lua, 1);
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    strcpy(error->message, "stream output_mode must be a string");
    return 0;
  }
  name = lua_tostring(lua, -1);
  if (strcmp(name, "decision_only") == 0) {
    *out = LQL_STREAM_OUTPUT_DECISION_ONLY;
  } else if (strcmp(name, "selected_record") == 0) {
    *out = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  } else if (strcmp(name, "projection") == 0) {
    *out = LQL_STREAM_OUTPUT_PROJECTION;
  } else if (strcmp(name, "mutation") == 0) {
    *out = LQL_STREAM_OUTPUT_MUTATION;
  } else if (strcmp(name, "projection_then_mutation") == 0) {
    *out = LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION;
  } else {
    lua_pop(lua, 1);
    error->code = LQL_STATUS_INVALID_ARGUMENT;
    strcpy(error->message, "unknown stream output_mode");
    return 0;
  }
  lua_pop(lua, 1);
  return 1;
}

static void lua_lql_push_stream_result(lua_State *lua,
                                       const lql_stream_result *result) {
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)result->records_seen);
  lua_setfield(lua, -2, "records_seen");
  lua_pushinteger(lua, (lua_Integer)result->records_matched);
  lua_setfield(lua, -2, "records_matched");
  lua_pushinteger(lua, (lua_Integer)result->bytes_consumed);
  lua_setfield(lua, -2, "bytes_consumed");
  lua_pushboolean(lua, result->stopped_early);
  lua_setfield(lua, -2, "stopped_early");
  lua_pushinteger(lua, (lua_Integer)result->stop_reason);
  lua_setfield(lua, -2, "stop_reason");
}

static int lua_lql_stream_apply_common(lua_State *lua, int spooled) {
  lua_lql_client *client;
  lua_lql_stream_bridge bridge;
  lql_stream_request request;
  lql_stream_result result;
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
  lua_lql_mutation_bridge *mutation_bridge;
  int selector_owned;
  int projection_owned;
  int mutation_owned;
  lql_error error;
  lql_status status;
  int options_index;
  client = lua_lql_check_client(lua, 1);
  luaL_checktype(lua, 2, LUA_TFUNCTION);
  options_index = 4;
  if (!lua_isnoneornil(lua, options_index)) {
    luaL_checktype(lua, options_index, LUA_TTABLE);
  }
  lua_lql_stream_bridge_init(&bridge, lua);
  lua_pushvalue(lua, 2);
  bridge.reader_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  lql_error_init(&error);
  if (!lua_lql_stream_option_function(lua, options_index, "range_writer",
                                      &bridge.range_ref, &error) ||
      !lua_lql_stream_option_function(lua, options_index, "writer",
                                      &bridge.writer_ref, &error) ||
      !lua_lql_stream_option_function(lua, options_index, "on_decision",
                                      &bridge.decision_ref, &error) ||
      !lua_lql_stream_option_function(lua, options_index, "on_value",
                                      &bridge.value_ref, &error) ||
      !lua_lql_stream_option_function(lua, options_index, "cancelled",
                                      &bridge.cancelled_ref, &error) ||
      !lua_lql_stream_option_function(lua, options_index, "time_now",
                                      &bridge.time_now_ref, &error)) {
    lua_lql_stream_bridge_unref(&bridge);
    return lua_lql_return_error(lua, &error, error.code);
  }
  selector = NULL;
  projection = NULL;
  mutation = NULL;
  mutation_bridge = NULL;
  selector_owned = 0;
  projection_owned = 0;
  mutation_owned = 0;
  status =
      lua_lql_selector_arg(lua, client, 3, &selector, &selector_owned, &error);
  if (status == LQL_STATUS_OK && lua_istable(lua, options_index)) {
    if (!lua_lql_getfield_protected(lua, options_index, "projection", &error)) {
      status = error.code;
    } else {
      status = lua_lql_projection_arg(lua, client, -1, &projection,
                                      &projection_owned, &error);
      if (status == LQL_STATUS_OK && lua_lql_test_projection(lua, -1) != NULL) {
        lua_lql_stream_bridge_hold(&bridge, -1, &bridge.projection_ref);
      }
      lua_pop(lua, 1);
    }
  }
  if (status == LQL_STATUS_OK && lua_istable(lua, options_index)) {
    if (!lua_lql_getfield_protected(lua, options_index, "mutation", &error)) {
      status = error.code;
    } else {
      status = lua_lql_mutation_arg(lua, client, -1, options_index, &mutation,
                                    &mutation_owned, &mutation_bridge, &error);
      if (status == LQL_STATUS_OK && lua_lql_test_mutation(lua, -1) != NULL) {
        lua_lql_stream_bridge_hold(&bridge, -1, &bridge.mutation_ref);
      }
      lua_pop(lua, 1);
    }
  }
  memset(&request, 0, sizeof(request));
  if (status == LQL_STATUS_OK &&
      !lua_lql_stream_output_mode(lua, options_index, &request.output_mode,
                                  &error)) {
    status = error.code;
  }
  if (status == LQL_STATUS_OK && lua_istable(lua, options_index)) {
    if (!lua_lql_getfield_protected(lua, options_index, "input_is_compact",
                                    &error)) {
      status = error.code;
    } else {
      request.input_is_compact = lua_toboolean(lua, -1);
      lua_pop(lua, 1);
    }
    if (status == LQL_STATUS_OK &&
        !lua_lql_getfield_protected(lua, options_index, "matched_only",
                                    &error)) {
      status = error.code;
    } else if (status == LQL_STATUS_OK) {
      request.matched_only = lua_toboolean(lua, -1);
      lua_pop(lua, 1);
    }
    if (status == LQL_STATUS_OK &&
        !lua_lql_getfield_protected(lua, options_index, "limits", &error)) {
      status = error.code;
    } else if (status == LQL_STATUS_OK) {
      if (!lua_isnil(lua, -1)) {
        int limits_index;
        if (lua_type(lua, -1) != LUA_TTABLE) {
          error.code = LQL_STATUS_INVALID_ARGUMENT;
          strcpy(error.message, "stream limits must be a table");
          status = error.code;
        } else {
          limits_index = lua_absindex(lua, -1);
          if (!lua_lql_stream_limit(lua, limits_index, "max_records",
                                    &request.limits.max_records, &error) ||
              !lua_lql_stream_limit(lua, limits_index, "max_matches",
                                    &request.limits.max_matches, &error) ||
              !lua_lql_stream_limit(lua, limits_index, "max_bytes",
                                    &request.limits.max_bytes, &error)) {
            status = error.code;
          }
        }
      }
      lua_pop(lua, 1);
    }
  }
  request.reader = lua_lql_stream_reader;
  request.reader_user = &bridge;
  request.range_writer =
      bridge.range_ref == LUA_NOREF ? NULL : lua_lql_stream_range_writer;
  request.range_user = &bridge;
  request.writer =
      bridge.writer_ref == LUA_NOREF ? NULL : lua_lql_stream_output_writer;
  request.writer_user = &bridge;
  request.cancelled =
      bridge.cancelled_ref == LUA_NOREF ? NULL : lua_lql_stream_cancelled;
  request.cancel_user = &bridge;
  request.time_now =
      bridge.time_now_ref == LUA_NOREF ? NULL : lua_lql_stream_time_now;
  request.time_user = &bridge;
  request.on_decision =
      bridge.decision_ref == LUA_NOREF ? NULL : lua_lql_stream_decision;
  request.decision_user = &bridge;
  request.on_value =
      bridge.value_ref == LUA_NOREF ? NULL : lua_lql_stream_on_value;
  request.value_user = &bridge;
  request.selector = selector;
  request.projection = projection;
  request.mutation = mutation;
  memset(&result, 0, sizeof(result));
  if (status == LQL_STATUS_OK) {
    if (spooled) {
      status = client->ctx->stream_apply_spooled(client->ctx, &request, &result,
                                                 &error);
    } else {
      status =
          client->ctx->stream_apply(client->ctx, &request, &result, &error);
    }
  }
  if (bridge.callback_status != LQL_STATUS_OK) {
    status = bridge.callback_status;
    error.code = status;
    strcpy(error.message, bridge.callback_message);
  }
  if (mutation_owned) {
    client->ctx->mutation_destroy(client->ctx, mutation);
    lua_lql_mutation_bridge_destroy(mutation_bridge);
  }
  if (projection_owned) {
    client->ctx->projection_destroy(client->ctx, projection);
  }
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  lua_lql_stream_bridge_unref(&bridge);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  lua_lql_push_stream_result(lua, &result);
  return 1;
}

static int lua_lql_stream_apply(lua_State *lua) {
  return lua_lql_stream_apply_common(lua, 0);
}

static int lua_lql_stream_apply_spooled(lua_State *lua) {
  return lua_lql_stream_apply_common(lua, 1);
}

static int lua_lql_apply_string_spooled(lua_State *lua) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
  lua_lql_mutation_bridge *mutation_bridge;
  int owned;
  int projection_owned;
  int mutation_owned;
  const char *input;
  size_t input_len;
  int count_only;
  int matched_only;
  lql_error error;
  lql_status status;
  lql_stream_request request;
  lql_stream_result result;
  lua_lql_string_reader reader;
  lua_lql_buffer output;
  lua_lql_stream_bridge transform_roots;

  client = lua_lql_check_client(lua, 1);
  lua_lql_stream_bridge_init(&transform_roots, lua);
  input = luaL_checklstring(lua, 3, &input_len);
  count_only = 0;
  matched_only = 1;
  projection = NULL;
  mutation = NULL;
  mutation_bridge = NULL;
  projection_owned = 0;
  mutation_owned = 0;
  lql_error_init(&error);
  if (lua_istable(lua, 4)) {
    if (!lua_lql_getfield_protected(lua, 4, "count", &error)) {
      lua_lql_stream_bridge_unref(&transform_roots);
      return lua_lql_return_error(lua, &error, error.code);
    }
    count_only = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    if (!lua_lql_getfield_protected(lua, 4, "matched_only", &error)) {
      lua_lql_stream_bridge_unref(&transform_roots);
      return lua_lql_return_error(lua, &error, error.code);
    }
    if (!lua_isnil(lua, -1)) {
      matched_only = lua_toboolean(lua, -1);
    }
    lua_pop(lua, 1);
  }
  status = lua_lql_selector_arg(lua, client, 2, &selector, &owned, &error);
  if (status != LQL_STATUS_OK) {
    lua_lql_stream_bridge_unref(&transform_roots);
    return lua_lql_return_error(lua, &error, status);
  }
  if (lua_istable(lua, 4)) {
    if (!lua_lql_getfield_protected(lua, 4, "projection", &error)) {
      status = error.code;
    } else {
      status = lua_lql_projection_arg(lua, client, -1, &projection,
                                      &projection_owned, &error);
      if (status == LQL_STATUS_OK && lua_lql_test_projection(lua, -1) != NULL) {
        lua_lql_stream_bridge_hold(&transform_roots, -1,
                                   &transform_roots.projection_ref);
      }
      lua_pop(lua, 1);
    }
    if (status != LQL_STATUS_OK) {
      if (owned) {
        client->ctx->selector_destroy(client->ctx, selector);
      }
      lua_lql_stream_bridge_unref(&transform_roots);
      return lua_lql_return_error(lua, &error, status);
    }
    if (!lua_lql_getfield_protected(lua, 4, "mutation", &error)) {
      status = error.code;
    } else {
      status = lua_lql_mutation_arg(lua, client, -1, 4, &mutation,
                                    &mutation_owned, &mutation_bridge, &error);
      if (status == LQL_STATUS_OK && lua_lql_test_mutation(lua, -1) != NULL) {
        lua_lql_stream_bridge_hold(&transform_roots, -1,
                                   &transform_roots.mutation_ref);
      }
      lua_pop(lua, 1);
    }
    if (status != LQL_STATUS_OK) {
      if (projection_owned) {
        client->ctx->projection_destroy(client->ctx, projection);
      }
      if (owned) {
        client->ctx->selector_destroy(client->ctx, selector);
      }
      lua_lql_stream_bridge_unref(&transform_roots);
      return lua_lql_return_error(lua, &error, status);
    }
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = input;
  reader.len = input_len;
  lua_lql_buffer_init(&output, lua);
  memset(&request, 0, sizeof(request));
  request.reader = lua_lql_string_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.projection = count_only ? NULL : projection;
  request.mutation = count_only ? NULL : mutation;
  request.matched_only = matched_only;
  if (!count_only) {
    request.writer = lua_lql_buffer_write;
    request.writer_user = &output;
    if (projection != NULL && mutation != NULL) {
      request.output_mode = LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION;
    } else if (mutation != NULL) {
      request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
    } else if (projection != NULL) {
      request.output_mode = LQL_STREAM_OUTPUT_PROJECTION;
    } else {
      request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
    }
  }
  memset(&result, 0, sizeof(result));
  status =
      client->ctx->stream_apply_spooled(client->ctx, &request, &result, &error);
  if (mutation_owned) {
    client->ctx->mutation_destroy(client->ctx, mutation);
    lua_lql_mutation_bridge_destroy(mutation_bridge);
  }
  if (projection_owned) {
    client->ctx->projection_destroy(client->ctx, projection);
  }
  if (owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  lua_lql_stream_bridge_unref(&transform_roots);
  if (status != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&output);
    return lua_lql_return_error(lua, &error, status);
  }

  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)result.records_seen);
  lua_setfield(lua, -2, "records_seen");
  lua_pushinteger(lua, (lua_Integer)result.records_matched);
  lua_setfield(lua, -2, "records_matched");
  lua_pushinteger(lua, (lua_Integer)result.bytes_consumed);
  lua_setfield(lua, -2, "bytes_consumed");
  lua_pushboolean(lua, result.stopped_early);
  lua_setfield(lua, -2, "stopped_early");
  lua_pushinteger(lua, (lua_Integer)result.stop_reason);
  lua_setfield(lua, -2, "stop_reason");
  if (!count_only) {
    lua_pushlstring(lua, output.data != NULL ? output.data : "", output.len);
    lua_setfield(lua, -2, "output");
  }
  lua_lql_buffer_dispose(&output);
  return 1;
}

static int lua_lql_file_result(lua_State *lua, const lql_stream_result *result,
                               lua_lql_buffer *output, int include_output) {
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)result->records_seen);
  lua_setfield(lua, -2, "records_seen");
  lua_pushinteger(lua, (lua_Integer)result->records_matched);
  lua_setfield(lua, -2, "records_matched");
  lua_pushinteger(lua, (lua_Integer)result->bytes_consumed);
  lua_setfield(lua, -2, "bytes_consumed");
  lua_pushboolean(lua, result->stopped_early);
  lua_setfield(lua, -2, "stopped_early");
  lua_pushinteger(lua, (lua_Integer)result->stop_reason);
  lua_setfield(lua, -2, "stop_reason");
  if (include_output) {
    lua_pushlstring(lua, output->data != NULL ? output->data : "", output->len);
    lua_setfield(lua, -2, "output");
  }
  return 1;
}

static int lua_lql_filter_file_common(lua_State *lua, int rewrite_inline) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
  lua_lql_mutation_bridge *mutation_bridge;
  int owned;
  int projection_owned;
  int mutation_owned;
  const char *path;
  const char *output_path;
  int count_only;
  int stdout_output;
  int matched_only_set;
  lql_error error;
  lql_status status;
  lql_stream_result result;
  lql_file_filter_request file_request;
  lua_lql_buffer output;
  lua_lql_stream_bridge output_bridge;
  int include_output;
  int has_lua_writer;

  client = lua_lql_check_client(lua, 1);
  path = luaL_checkstring(lua, 3);
  output_path = NULL;
  count_only = 0;
  stdout_output = 0;
  matched_only_set = 0;
  projection = NULL;
  mutation = NULL;
  mutation_bridge = NULL;
  projection_owned = 0;
  mutation_owned = 0;
  has_lua_writer = 0;
  lua_lql_stream_bridge_init(&output_bridge, lua);
  lql_error_init(&error);
  if (lua_istable(lua, 4)) {
    if (!lua_lql_getfield_protected(lua, 4, "count", &error)) {
      lua_lql_stream_bridge_unref(&output_bridge);
      return lua_lql_return_error(lua, &error, error.code);
    }
    count_only = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    if (!lua_lql_getfield_protected(lua, 4, "stdout", &error)) {
      lua_lql_stream_bridge_unref(&output_bridge);
      return lua_lql_return_error(lua, &error, error.code);
    }
    stdout_output = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    if (!lua_lql_getfield_protected(lua, 4, "output_path", &error)) {
      lua_lql_stream_bridge_unref(&output_bridge);
      return lua_lql_return_error(lua, &error, error.code);
    }
    if (lua_type(lua, -1) == LUA_TSTRING) {
      output_path = lua_tostring(lua, -1);
      /* Keep the exact option string on this C frame while temporary mutation
       * parsing may re-enter Lua and clear options.output_path. */
    }
    if (output_path == NULL) {
      lua_pop(lua, 1);
    }
  }
  status = lua_lql_selector_arg(lua, client, 2, &selector, &owned, &error);
  if (status != LQL_STATUS_OK) {
    lua_lql_stream_bridge_unref(&output_bridge);
    return lua_lql_return_error(lua, &error, status);
  }
  if (lua_istable(lua, 4)) {
    if (!lua_lql_getfield_protected(lua, 4, "projection", &error)) {
      status = error.code;
    } else {
      status = lua_lql_projection_arg(lua, client, -1, &projection,
                                      &projection_owned, &error);
      if (status == LQL_STATUS_OK && lua_lql_test_projection(lua, -1) != NULL) {
        lua_lql_stream_bridge_hold(&output_bridge, -1,
                                   &output_bridge.projection_ref);
      }
      lua_pop(lua, 1);
    }
    if (status != LQL_STATUS_OK) {
      if (owned) {
        client->ctx->selector_destroy(client->ctx, selector);
      }
      lua_lql_stream_bridge_unref(&output_bridge);
      return lua_lql_return_error(lua, &error, status);
    }
    if (!lua_lql_getfield_protected(lua, 4, "mutation", &error)) {
      status = error.code;
    } else {
      status = lua_lql_mutation_arg(lua, client, -1, 4, &mutation,
                                    &mutation_owned, &mutation_bridge, &error);
      if (status == LQL_STATUS_OK && lua_lql_test_mutation(lua, -1) != NULL) {
        lua_lql_stream_bridge_hold(&output_bridge, -1,
                                   &output_bridge.mutation_ref);
      }
      lua_pop(lua, 1);
    }
    if (status != LQL_STATUS_OK) {
      if (projection_owned) {
        client->ctx->projection_destroy(client->ctx, projection);
      }
      if (owned) {
        client->ctx->selector_destroy(client->ctx, selector);
      }
      lua_lql_stream_bridge_unref(&output_bridge);
      return lua_lql_return_error(lua, &error, status);
    }
  }

  if (!lua_lql_stream_option_function(lua, 4, "writer",
                                      &output_bridge.writer_ref, &error)) {
    if (mutation_owned) {
      client->ctx->mutation_destroy(client->ctx, mutation);
      lua_lql_mutation_bridge_destroy(mutation_bridge);
    }
    if (projection_owned) {
      client->ctx->projection_destroy(client->ctx, projection);
    }
    if (owned) {
      client->ctx->selector_destroy(client->ctx, selector);
    }
    lua_lql_stream_bridge_unref(&output_bridge);
    return lua_lql_return_error(lua, &error, error.code);
  }
  has_lua_writer = output_bridge.writer_ref != LUA_NOREF;
  if (has_lua_writer &&
      (rewrite_inline || stdout_output || output_path != NULL)) {
    error.code = LQL_STATUS_INVALID_ARGUMENT;
    strcpy(error.message, "file writer cannot be combined with inline rewrite, "
                          "stdout, or output_path");
    if (mutation_owned) {
      client->ctx->mutation_destroy(client->ctx, mutation);
      lua_lql_mutation_bridge_destroy(mutation_bridge);
    }
    if (projection_owned) {
      client->ctx->projection_destroy(client->ctx, projection);
    }
    if (owned) {
      client->ctx->selector_destroy(client->ctx, selector);
    }
    lua_lql_stream_bridge_unref(&output_bridge);
    return lua_lql_return_error(lua, &error, error.code);
  }
  lua_lql_buffer_init(&output, lua);
  memset(&file_request, 0, sizeof(file_request));
  file_request.input_path = path;
  file_request.output_path = output_path;
  if (!rewrite_inline && stdout_output && output_path == NULL) {
    file_request.output_file = stdout;
  }
  file_request.selector = selector;
  file_request.projection = projection;
  file_request.mutation = mutation;
  file_request.matched_only = mutation != NULL ? 0 : 1;
  file_request.count_only = count_only;
  if (lua_istable(lua, 4)) {
    if (!lua_lql_getfield_protected(lua, 4, "matched_only", &error)) {
      if (mutation_owned) {
        client->ctx->mutation_destroy(client->ctx, mutation);
        lua_lql_mutation_bridge_destroy(mutation_bridge);
      }
      if (projection_owned) {
        client->ctx->projection_destroy(client->ctx, projection);
      }
      if (owned) {
        client->ctx->selector_destroy(client->ctx, selector);
      }
      lua_lql_buffer_dispose(&output);
      lua_lql_stream_bridge_unref(&output_bridge);
      return lua_lql_return_error(lua, &error, error.code);
    }
    if (!lua_isnil(lua, -1)) {
      file_request.matched_only = lua_toboolean(lua, -1);
      matched_only_set = 1;
    }
    lua_pop(lua, 1);
  }
  if (rewrite_inline && !matched_only_set) {
    file_request.matched_only = mutation != NULL ? 0 : 1;
  }
  if (projection != NULL && mutation != NULL) {
    file_request.output_mode = LQL_STREAM_OUTPUT_PROJECTION_THEN_MUTATION;
  } else if (mutation != NULL) {
    file_request.output_mode = LQL_STREAM_OUTPUT_MUTATION;
  } else if (projection != NULL) {
    file_request.output_mode = LQL_STREAM_OUTPUT_PROJECTION;
  } else {
    file_request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  }
  include_output = !rewrite_inline && !count_only && !stdout_output &&
                   output_path == NULL && !has_lua_writer;
  if (include_output) {
    file_request.output_writer = lua_lql_buffer_write;
    file_request.output_user = &output;
  } else if (has_lua_writer) {
    file_request.output_writer = lua_lql_stream_output_writer;
    file_request.output_user = &output_bridge;
  } else if (!rewrite_inline && count_only && !stdout_output &&
             output_path == NULL) {
    file_request.output_writer = lua_lql_discard_write;
  }
  memset(&result, 0, sizeof(result));
  if (rewrite_inline) {
    status = client->ctx->rewrite_file_inline_spooled(
        client->ctx, &file_request, &result, &error);
  } else {
    status = client->ctx->filter_file_spooled(client->ctx, &file_request,
                                              &result, &error);
  }
  if (output_bridge.callback_status != LQL_STATUS_OK) {
    status = output_bridge.callback_status;
    error.code = status;
    strcpy(error.message, output_bridge.callback_message);
  }
  if (mutation_owned) {
    client->ctx->mutation_destroy(client->ctx, mutation);
    lua_lql_mutation_bridge_destroy(mutation_bridge);
  }
  if (projection_owned) {
    client->ctx->projection_destroy(client->ctx, projection);
  }
  if (owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  lua_lql_stream_bridge_unref(&output_bridge);
  if (status != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&output);
    return lua_lql_return_error(lua, &error, status);
  }

  lua_lql_file_result(lua, &result, &output, include_output);
  lua_lql_buffer_dispose(&output);
  return 1;
}

static int lua_lql_filter_file_spooled(lua_State *lua) {
  return lua_lql_filter_file_common(lua, 0);
}

static int lua_lql_rewrite_file_inline_spooled(lua_State *lua) {
  return lua_lql_filter_file_common(lua, 1);
}

static int lua_lql_core_version(lua_State *lua) {
  lua_pushstring(lua, LQL_VERSION);
  return 1;
}

static int lua_lql_core_status_string(lua_State *lua) {
  lql_status status;
  status = (lql_status)luaL_checkinteger(lua, 1);
  lua_pushstring(lua, lql_status_string(status));
  return 1;
}

static int lua_lql_core_path_is_regular_file(lua_State *lua) {
  const char *path;
  path = luaL_checkstring(lua, 1);
  lua_pushboolean(lua, lql_path_is_regular_file(NULL, path));
  return 1;
}

static const luaL_Reg lua_lql_module_functions[] = {
    {"new", lua_lql_new},
    {"version", lua_lql_core_version},
    {"status_string", lua_lql_core_status_string},
    {"path_is_regular_file", lua_lql_core_path_is_regular_file},
    {NULL, NULL}};

static const luaL_Reg lua_lql_client_methods[] = {
    {"version", lua_lql_client_version},
    {"capabilities", lua_lql_client_capabilities},
    {"selector_parse", lua_lql_client_selector_parse},
    {"selector_parse_or", lua_lql_client_selector_parse_or},
    {"selector_parse_json", lua_lql_client_selector_parse_json},
    {"selector_build_all", lua_lql_selector_build_all},
    {"selector_build_compound", lua_lql_selector_build_compound},
    {"selector_build_not", lua_lql_selector_build_not},
    {"selector_build_string", lua_lql_selector_build_string},
    {"selector_build_range", lua_lql_selector_build_range},
    {"selector_build_date", lua_lql_selector_build_date},
    {"selector_build_in", lua_lql_selector_build_in},
    {"selector_build_exists", lua_lql_selector_build_exists},
    {"path_is_regular_file", lua_lql_client_path_is_regular_file},
    {"projection_parse", lua_lql_client_projection_parse},
    {"mutation_parse", lua_lql_client_mutation_parse},
    {"selector_capabilities", lua_lql_client_selector_capabilities},
    {"stream_apply", lua_lql_stream_apply},
    {"stream_apply_spooled", lua_lql_stream_apply_spooled},
    {"apply_string_spooled", lua_lql_apply_string_spooled},
    {"filter_file_spooled", lua_lql_filter_file_spooled},
    {"rewrite_file_inline_spooled", lua_lql_rewrite_file_inline_spooled},
    {NULL, NULL}};

static const luaL_Reg lua_lql_selector_methods[] = {
    {"capabilities", lua_lql_selector_capabilities},
    {"is_empty", lua_lql_selector_is_empty},
    {"root", lua_lql_selector_root},
    {"write_json", lua_lql_selector_write_json},
    {NULL, NULL}};

static const luaL_Reg lua_lql_selector_node_methods[] = {
    {"kind", lua_lql_selector_node_kind},
    {"child_count", lua_lql_selector_node_child_count},
    {"child", lua_lql_selector_node_child},
    {"string_term", lua_lql_selector_node_string_term},
    {"string_term_any", lua_lql_selector_node_string_term_any},
    {"range_term", lua_lql_selector_node_range_term},
    {"date_term", lua_lql_selector_node_date_term},
    {"in_term", lua_lql_selector_node_in_term},
    {"in_term_any", lua_lql_selector_node_in_term_any},
    {"exists_path", lua_lql_selector_node_exists_path},
    {NULL, NULL}};

static const luaL_Reg lua_lql_stream_value_methods[] = {
    {"size", lua_lql_stream_value_size},
    {"write_to", lua_lql_stream_value_write_to},
    {NULL, NULL}};

static const luaL_Reg lua_lql_stream_writer_methods[] = {
    {"write", lua_lql_stream_writer_write}, {NULL, NULL}};

static const luaL_Reg lua_lql_projection_methods[] = {
    {"path_count", lua_lql_projection_path_count},
    {"path", lua_lql_projection_path},
    {NULL, NULL}};

static const luaL_Reg lua_lql_mutation_methods[] = {
    {"count", lua_lql_mutation_count}, {NULL, NULL}};

static void lua_lql_register_type(lua_State *lua, const char *name,
                                  lua_CFunction gc, const luaL_Reg *methods) {
  luaL_newmetatable(lua, name);
  if (gc != NULL) {
    lua_pushcfunction(lua, gc);
    lua_setfield(lua, -2, "__gc");
  }
  lua_newtable(lua);
  luaL_setfuncs(lua, methods, 0);
  lua_setfield(lua, -2, "__index");
  lua_pop(lua, 1);
}

int luaopen_lql_core(lua_State *lua) {
  lua_lql_register_type(lua, LUA_LQL_CLIENT, lua_lql_client_gc,
                        lua_lql_client_methods);
  lua_lql_register_type(lua, LUA_LQL_SELECTOR, lua_lql_selector_gc,
                        lua_lql_selector_methods);
  lua_lql_register_type(lua, LUA_LQL_SELECTOR_NODE, NULL,
                        lua_lql_selector_node_methods);
  lua_lql_register_type(lua, LUA_LQL_STREAM_VALUE, NULL,
                        lua_lql_stream_value_methods);
  lua_lql_register_type(lua, LUA_LQL_STREAM_WRITER, NULL,
                        lua_lql_stream_writer_methods);
  lua_lql_register_type(lua, LUA_LQL_PROJECTION, lua_lql_projection_gc,
                        lua_lql_projection_methods);
  lua_lql_register_type(lua, LUA_LQL_MUTATION, lua_lql_mutation_gc,
                        lua_lql_mutation_methods);
  lua_newtable(lua);
  luaL_setfuncs(lua, lua_lql_module_functions, 0);
  return 1;
}
