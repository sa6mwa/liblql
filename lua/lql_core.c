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

typedef struct lua_lql_client {
  lql *ctx;
} lua_lql_client;

typedef struct lua_lql_selector {
  lql *ctx;
  lql_selector *selector;
} lua_lql_selector;

typedef struct lua_lql_string_reader {
  const char *data;
  size_t len;
  size_t offset;
} lua_lql_string_reader;

typedef struct lua_lql_file_reader {
  FILE *file;
} lua_lql_file_reader;

typedef struct lua_lql_file_writer {
  FILE *file;
} lua_lql_file_writer;

typedef struct lua_lql_buffer {
  lua_State *lua;
  char *data;
  size_t len;
  size_t cap;
} lua_lql_buffer;

static void *lua_lql_alloc(lua_State *lua, void *ptr, size_t old_size,
                           size_t new_size) {
  lua_Alloc allocf;
  void *user;
  allocf = lua_getallocf(lua, &user);
  return allocf(user, ptr, old_size, new_size);
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

static lql_status lua_lql_file_read(void *user, unsigned char *buffer,
                                    size_t capacity, size_t *out_len,
                                    lql_error *error) {
  lua_lql_file_reader *reader;
  size_t amount;
  (void)error;
  reader = (lua_lql_file_reader *)user;
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

static lql_status lua_lql_file_write(void *user, const void *data, size_t len,
                                     lql_error *error) {
  lua_lql_file_writer *writer;
  (void)error;
  writer = (lua_lql_file_writer *)user;
  if (writer == NULL || writer->file == NULL || (len != 0u && data == NULL)) {
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (len != 0u && fwrite(data, 1u, len, writer->file) != len) {
    return LQL_STATUS_IO_ERROR;
  }
  return LQL_STATUS_OK;
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

static lua_lql_selector *lua_lql_check_selector(lua_State *lua, int index) {
  lua_lql_selector *selector;
  selector = (lua_lql_selector *)luaL_checkudata(lua, index, LUA_LQL_SELECTOR);
  luaL_argcheck(lua,
                selector != NULL && selector->ctx != NULL &&
                    selector->selector != NULL,
                index, "closed lql selector");
  return selector;
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

static lql_status lua_lql_selector_arg(lua_State *lua, lua_lql_client *client,
                                       int index, lql_selector **out,
                                       int *owned, lql_error *error) {
  lua_lql_selector *handle;
  const char *expr;
  *out = NULL;
  *owned = 0;
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
  expr = luaL_checkstring(lua, index);
  *owned = 1;
  return client->ctx->selector_parse(client->ctx, expr, out, error);
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
  lua_pushboolean(lua, 1);
  lua_setfield(lua, -2, "execute_string");
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

static int lua_lql_execute_string(lua_State *lua) {
  lua_lql_client *client;
  lql_selector *selector;
  int owned;
  const char *input;
  size_t input_len;
  int count_only;
  lql_error error;
  lql_status status;
  lql_stream_request request;
  lql_stream_result result;
  lua_lql_string_reader reader;
  lua_lql_buffer output;

  client = lua_lql_check_client(lua, 1);
  lql_error_init(&error);
  status = lua_lql_selector_arg(lua, client, 2, &selector, &owned, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  input = luaL_checklstring(lua, 3, &input_len);
  count_only = 0;
  if (lua_istable(lua, 4)) {
    lua_getfield(lua, 4, "count");
    count_only = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
  }

  memset(&reader, 0, sizeof(reader));
  reader.data = input;
  reader.len = input_len;
  lua_lql_buffer_init(&output, lua);
  memset(&request, 0, sizeof(request));
  request.reader = lua_lql_string_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.matched_only = 1;
  if (!count_only) {
    request.writer = lua_lql_buffer_write;
    request.writer_user = &output;
    request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
  }
  memset(&result, 0, sizeof(result));
  status = client->ctx->stream_execute_spooled(client->ctx, &request, &result,
                                               &error);
  if (owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
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
  if (!count_only) {
    lua_pushlstring(lua, output.data != NULL ? output.data : "", output.len);
    lua_setfield(lua, -2, "output");
  }
  lua_lql_buffer_dispose(&output);
  return 1;
}

static int lua_lql_execute_file(lua_State *lua) {
  lua_lql_client *client;
  lql_selector *selector;
  int owned;
  const char *path;
  int count_only;
  int stdout_output;
  FILE *input;
  lql_error error;
  lql_status status;
  lql_stream_request request;
  lql_stream_result result;
  lua_lql_file_reader reader;
  lua_lql_file_writer file_writer;
  lua_lql_buffer output;

  client = lua_lql_check_client(lua, 1);
  lql_error_init(&error);
  status = lua_lql_selector_arg(lua, client, 2, &selector, &owned, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  path = luaL_checkstring(lua, 3);
  count_only = 0;
  stdout_output = 0;
  if (lua_istable(lua, 4)) {
    lua_getfield(lua, 4, "count");
    count_only = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, 4, "stdout");
    stdout_output = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
  }

  input = stdin;
  if (strcmp(path, "-") != 0) {
    input = fopen(path, "rb");
    if (input == NULL) {
      if (owned) {
        client->ctx->selector_destroy(client->ctx, selector);
      }
      error.code = LQL_STATUS_IO_ERROR;
      strcpy(error.message, "unable to open input file");
      return lua_lql_return_error(lua, &error, LQL_STATUS_IO_ERROR);
    }
  }

  memset(&reader, 0, sizeof(reader));
  reader.file = input;
  memset(&file_writer, 0, sizeof(file_writer));
  file_writer.file = stdout;
  lua_lql_buffer_init(&output, lua);
  memset(&request, 0, sizeof(request));
  request.reader = lua_lql_file_read;
  request.reader_user = &reader;
  request.selector = selector;
  request.matched_only = 1;
  if (!count_only) {
    request.output_mode = LQL_STREAM_OUTPUT_SELECTED_RECORD;
    if (stdout_output) {
      request.writer = lua_lql_file_write;
      request.writer_user = &file_writer;
    } else {
      request.writer = lua_lql_buffer_write;
      request.writer_user = &output;
    }
  }
  memset(&result, 0, sizeof(result));
  status = client->ctx->stream_execute_spooled(client->ctx, &request, &result,
                                               &error);
  if (owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  if (input != stdin) {
    fclose(input);
  }
  if (status == LQL_STATUS_OK && stdout_output && fflush(stdout) != 0) {
    status = LQL_STATUS_IO_ERROR;
    error.code = LQL_STATUS_IO_ERROR;
    strcpy(error.message, "unable to flush stdout");
  }
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
  if (!count_only && !stdout_output) {
    lua_pushlstring(lua, output.data != NULL ? output.data : "", output.len);
    lua_setfield(lua, -2, "output");
  }
  lua_lql_buffer_dispose(&output);
  return 1;
}

static int lua_lql_core_version(lua_State *lua) {
  lua_pushstring(lua, LQL_VERSION);
  return 1;
}

static const luaL_Reg lua_lql_module_functions[] = {
    {"new", lua_lql_new},
    {"version", lua_lql_core_version},
    {"has_core", lua_lql_core_version},
    {NULL, NULL}};

static const luaL_Reg lua_lql_client_methods[] = {
    {"__gc", lua_lql_client_gc},
    {"version", lua_lql_client_version},
    {"capabilities", lua_lql_client_capabilities},
    {"selector_parse", lua_lql_client_selector_parse},
    {"selector_parse_or", lua_lql_client_selector_parse_or},
    {"selector_parse_json", lua_lql_client_selector_parse_json},
    {"selector_capabilities", lua_lql_client_selector_capabilities},
    {"execute_string", lua_lql_execute_string},
    {"execute_file", lua_lql_execute_file},
    {NULL, NULL}};

static const luaL_Reg lua_lql_selector_methods[] = {
    {"__gc", lua_lql_selector_gc},
    {"capabilities", lua_lql_selector_capabilities},
    {"is_empty", lua_lql_selector_is_empty},
    {NULL, NULL}};

static void lua_lql_register_type(lua_State *lua, const char *name,
                                  const luaL_Reg *methods) {
  luaL_newmetatable(lua, name);
  luaL_setfuncs(lua, methods, 0);
  lua_pushvalue(lua, -1);
  lua_setfield(lua, -2, "__index");
  lua_pop(lua, 1);
}

int luaopen_lql_core(lua_State *lua) {
  lua_lql_register_type(lua, LUA_LQL_CLIENT, lua_lql_client_methods);
  lua_lql_register_type(lua, LUA_LQL_SELECTOR, lua_lql_selector_methods);
  lua_newtable(lua);
  luaL_setfuncs(lua, lua_lql_module_functions, 0);
  lua_pushboolean(lua, 1);
  lua_setfield(lua, -2, "core_loaded");
  return 1;
}
