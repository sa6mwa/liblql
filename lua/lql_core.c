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
#define LUA_LQL_PROJECTION "lql.projection"
#define LUA_LQL_MUTATION "lql.mutation"

typedef struct lua_lql_client {
  lql *ctx;
} lua_lql_client;

typedef struct lua_lql_selector {
  lql *ctx;
  lql_selector *selector;
} lua_lql_selector;

typedef struct lua_lql_projection {
  lql *ctx;
  lql_projection *projection;
} lua_lql_projection;

typedef struct lua_lql_mutation {
  lql *ctx;
  lql_mutation *mutation;
} lua_lql_mutation;

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

static lql_status lua_lql_discard_write(void *user, const void *data,
                                        size_t len, lql_error *error) {
  (void)user;
  (void)data;
  (void)len;
  (void)error;
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

static int lua_lql_push_mutation(lua_State *lua, lua_lql_client *client,
                                 lql_mutation *mutation) {
  lua_lql_mutation *handle;
  handle = (lua_lql_mutation *)lua_newuserdatauv(lua, sizeof(*handle), 1);
  handle->ctx = client->ctx;
  handle->mutation = mutation;
  luaL_getmetatable(lua, LUA_LQL_MUTATION);
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
  expr = luaL_checkstring(lua, index);
  *owned = 1;
  return client->ctx->selector_parse(client->ctx, expr, out, error);
}

static int lua_lql_string_list_arg(lua_State *lua, int index,
                                   const char ***out_items, size_t *out_count) {
  const char **items;
  size_t count;
  size_t i;
  *out_items = NULL;
  *out_count = 0u;
  if (lua_type(lua, index) == LUA_TSTRING) {
    items = (const char **)lua_lql_alloc(lua, NULL, 0u, sizeof(*items));
    if (items == NULL) {
      return 0;
    }
    items[0] = lua_tostring(lua, index);
    *out_items = items;
    *out_count = 1u;
    return 1;
  }
  luaL_checktype(lua, index, LUA_TTABLE);
  count = (size_t)lua_rawlen(lua, index);
  if (count == 0u) {
    return 1;
  }
  if (count > ((size_t)-1) / sizeof(*items)) {
    return 0;
  }
  items = (const char **)lua_lql_alloc(lua, NULL, 0u, count * sizeof(*items));
  if (items == NULL) {
    return 0;
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, index, (lua_Integer)i + 1);
    items[i] = luaL_checkstring(lua, -1);
    lua_pop(lua, 1);
  }
  *out_items = items;
  *out_count = count;
  return 1;
}

static void lua_lql_string_list_free(lua_State *lua, const char **items,
                                     size_t count) {
  if (items != NULL) {
    (void)lua_lql_alloc(lua, (void *)items, count * sizeof(*items), 0u);
  }
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
  if (!lua_lql_string_list_arg(lua, index, &items, &count)) {
    error->code = LQL_STATUS_NO_MEMORY;
    strcpy(error->message, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  *owned = 1;
  status = client->ctx->projection_parse(client->ctx, items, count, out, error);
  lua_lql_string_list_free(lua, items, count);
  return status;
}

static void lua_lql_mutation_options(lua_State *lua, int index,
                                     lql_mutation_parse_options *options) {
  memset(options, 0, sizeof(*options));
  if (!lua_istable(lua, index)) {
    return;
  }
  lua_getfield(lua, index, "enable_file_mutations");
  options->enable_file_values = lua_toboolean(lua, -1);
  lua_pop(lua, 1);
  lua_getfield(lua, index, "enable_file_values");
  if (lua_toboolean(lua, -1)) {
    options->enable_file_values = 1;
  }
  lua_pop(lua, 1);
  lua_getfield(lua, index, "file_value_base_dir");
  if (lua_type(lua, -1) == LUA_TSTRING) {
    options->file_value_base_dir.data =
        lua_tolstring(lua, -1, &options->file_value_base_dir.len);
  }
  lua_pop(lua, 1);
  if (options->enable_file_values &&
      options->file_value_base_dir.data == NULL) {
    options->file_value_base_dir.data = ".";
    options->file_value_base_dir.len = 1u;
  }
}

static lql_status lua_lql_mutation_arg(lua_State *lua, lua_lql_client *client,
                                       int index, int options_index,
                                       lql_mutation **out, int *owned,
                                       lql_error *error) {
  lua_lql_mutation *handle;
  const char **items;
  size_t count;
  lql_status status;
  lql_mutation_parse_options options;
  *out = NULL;
  *owned = 0;
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
    *out = handle->mutation;
    return LQL_STATUS_OK;
  }
  items = NULL;
  count = 0u;
  if (!lua_lql_string_list_arg(lua, index, &items, &count)) {
    error->code = LQL_STATUS_NO_MEMORY;
    strcpy(error->message, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  lua_lql_mutation_options(lua, options_index, &options);
  *owned = 1;
  status = client->ctx->mutation_parse_with_options(client->ctx, items, count,
                                                    &options, out, error);
  lua_lql_string_list_free(lua, items, count);
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
  lua_pushboolean(lua, 1);
  lua_setfield(lua, -2, "execute_string");
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
  if (!lua_lql_string_list_arg(lua, 2, &items, &count)) {
    lql_error_init(&error);
    error.code = LQL_STATUS_NO_MEMORY;
    strcpy(error.message, "out of memory");
    return lua_lql_return_error(lua, &error, LQL_STATUS_NO_MEMORY);
  }
  projection = NULL;
  lql_error_init(&error);
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
  lql_error error;
  lql_status status;
  client = lua_lql_check_client(lua, 1);
  items = NULL;
  count = 0u;
  if (!lua_lql_string_list_arg(lua, 2, &items, &count)) {
    lql_error_init(&error);
    error.code = LQL_STATUS_NO_MEMORY;
    strcpy(error.message, "out of memory");
    return lua_lql_return_error(lua, &error, LQL_STATUS_NO_MEMORY);
  }
  lua_lql_mutation_options(lua, 3, &options);
  mutation = NULL;
  lql_error_init(&error);
  status = client->ctx->mutation_parse_with_options(
      client->ctx, items, count, &options, &mutation, &error);
  lua_lql_string_list_free(lua, items, count);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  return lua_lql_push_mutation(lua, client, mutation);
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

static int lua_lql_execute_string(lua_State *lua) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_projection *projection;
  lql_mutation *mutation;
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

  client = lua_lql_check_client(lua, 1);
  input = luaL_checklstring(lua, 3, &input_len);
  count_only = 0;
  matched_only = 1;
  projection = NULL;
  mutation = NULL;
  projection_owned = 0;
  mutation_owned = 0;
  if (lua_istable(lua, 4)) {
    lua_getfield(lua, 4, "count");
    count_only = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, 4, "matched_only");
    if (!lua_isnil(lua, -1)) {
      matched_only = lua_toboolean(lua, -1);
    }
    lua_pop(lua, 1);
  }
  lql_error_init(&error);
  status = lua_lql_selector_arg(lua, client, 2, &selector, &owned, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  if (lua_istable(lua, 4)) {
    lua_getfield(lua, 4, "projection");
    status = lua_lql_projection_arg(lua, client, -1, &projection,
                                    &projection_owned, &error);
    lua_pop(lua, 1);
    if (status != LQL_STATUS_OK) {
      if (owned) {
        client->ctx->selector_destroy(client->ctx, selector);
      }
      return lua_lql_return_error(lua, &error, status);
    }
    lua_getfield(lua, 4, "mutation");
    status = lua_lql_mutation_arg(lua, client, -1, 4, &mutation,
                                  &mutation_owned, &error);
    lua_pop(lua, 1);
    if (status != LQL_STATUS_OK) {
      if (projection_owned) {
        client->ctx->projection_destroy(client->ctx, projection);
      }
      if (owned) {
        client->ctx->selector_destroy(client->ctx, selector);
      }
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
  request.matched_only = mutation != NULL ? matched_only : 1;
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
  status = client->ctx->stream_execute_spooled(client->ctx, &request, &result,
                                               &error);
  if (mutation_owned) {
    client->ctx->mutation_destroy(client->ctx, mutation);
  }
  if (projection_owned) {
    client->ctx->projection_destroy(client->ctx, projection);
  }
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
  int include_output;

  client = lua_lql_check_client(lua, 1);
  path = luaL_checkstring(lua, 3);
  output_path = NULL;
  count_only = 0;
  stdout_output = 0;
  matched_only_set = 0;
  projection = NULL;
  mutation = NULL;
  projection_owned = 0;
  mutation_owned = 0;
  if (lua_istable(lua, 4)) {
    lua_getfield(lua, 4, "count");
    count_only = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, 4, "stdout");
    stdout_output = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, 4, "output_path");
    if (lua_type(lua, -1) == LUA_TSTRING) {
      output_path = lua_tostring(lua, -1);
    }
    lua_pop(lua, 1);
  }
  lql_error_init(&error);
  status = lua_lql_selector_arg(lua, client, 2, &selector, &owned, &error);
  if (status != LQL_STATUS_OK) {
    return lua_lql_return_error(lua, &error, status);
  }
  if (lua_istable(lua, 4)) {
    lua_getfield(lua, 4, "projection");
    status = lua_lql_projection_arg(lua, client, -1, &projection,
                                    &projection_owned, &error);
    lua_pop(lua, 1);
    if (status != LQL_STATUS_OK) {
      if (owned) {
        client->ctx->selector_destroy(client->ctx, selector);
      }
      return lua_lql_return_error(lua, &error, status);
    }
    lua_getfield(lua, 4, "mutation");
    status = lua_lql_mutation_arg(lua, client, -1, 4, &mutation,
                                  &mutation_owned, &error);
    lua_pop(lua, 1);
    if (status != LQL_STATUS_OK) {
      if (projection_owned) {
        client->ctx->projection_destroy(client->ctx, projection);
      }
      if (owned) {
        client->ctx->selector_destroy(client->ctx, selector);
      }
      return lua_lql_return_error(lua, &error, status);
    }
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
    lua_getfield(lua, 4, "matched_only");
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
  include_output =
      !rewrite_inline && !count_only && !stdout_output && output_path == NULL;
  if (include_output) {
    file_request.output_writer = lua_lql_buffer_write;
    file_request.output_user = &output;
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
  if (mutation_owned) {
    client->ctx->mutation_destroy(client->ctx, mutation);
  }
  if (projection_owned) {
    client->ctx->projection_destroy(client->ctx, projection);
  }
  if (owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
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
    {"path_is_regular_file", lua_lql_client_path_is_regular_file},
    {"projection_parse", lua_lql_client_projection_parse},
    {"mutation_parse", lua_lql_client_mutation_parse},
    {"selector_capabilities", lua_lql_client_selector_capabilities},
    {"execute_string", lua_lql_execute_string},
    {"filter_file_spooled", lua_lql_filter_file_spooled},
    {"rewrite_file_inline_spooled", lua_lql_rewrite_file_inline_spooled},
    {NULL, NULL}};

static const luaL_Reg lua_lql_selector_methods[] = {
    {"capabilities", lua_lql_selector_capabilities},
    {"is_empty", lua_lql_selector_is_empty},
    {NULL, NULL}};

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
  lua_lql_register_type(lua, LUA_LQL_PROJECTION, lua_lql_projection_gc,
                        lua_lql_projection_methods);
  lua_lql_register_type(lua, LUA_LQL_MUTATION, lua_lql_mutation_gc,
                        lua_lql_mutation_methods);
  lua_newtable(lua);
  luaL_setfuncs(lua, lua_lql_module_functions, 0);
  return 1;
}
