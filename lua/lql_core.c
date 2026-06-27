#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "lql/lql.h"

#include <lauxlib.h>
#include <lua.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

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

typedef struct lua_lql_selector_handle {
  lql *ctx;
  lql_selector *selector;
} lua_lql_selector_handle;

typedef struct lua_lql_projection_handle {
  lql *ctx;
  lql_projection *projection;
} lua_lql_projection_handle;

typedef struct lua_lql_mutation_plan_handle {
  lql *ctx;
  lql_mutation_plan *plan;
} lua_lql_mutation_plan_handle;

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

typedef struct lua_lql_source_state {
  lua_State *lua;
  int read_ref;
  lql_error *error;
  int read_failed;
  char read_message[256];
} lua_lql_source_state;

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
#define LUA_LQL_SELECTOR "lql.selector"
#define LUA_LQL_PROJECTION "lql.projection"
#define LUA_LQL_MUTATION_PLAN "lql.mutation_plan"

static int lua_lql_fail(lua_State *L, const lql_error *error);
static void lua_lql_set_error(lql_error *error, lql_status status,
                              const char *message);
static lql_status lua_lql_file_to_buffer(FILE *file, lua_lql_buffer *buffer);

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

static lua_lql_selector_handle *lua_lql_test_selector(lua_State *L, int index) {
  return (lua_lql_selector_handle *)luaL_testudata(L, index, LUA_LQL_SELECTOR);
}

static lua_lql_projection_handle *lua_lql_test_projection(lua_State *L,
                                                          int index) {
  return (lua_lql_projection_handle *)luaL_testudata(L, index,
                                                     LUA_LQL_PROJECTION);
}

static lua_lql_mutation_plan_handle *lua_lql_test_mutation_plan(lua_State *L,
                                                                int index) {
  return (lua_lql_mutation_plan_handle *)luaL_testudata(
      L, index, LUA_LQL_MUTATION_PLAN);
}

static int lua_lql_selector_gc(lua_State *L) {
  lua_lql_selector_handle *handle;

  handle = (lua_lql_selector_handle *)luaL_checkudata(L, 1, LUA_LQL_SELECTOR);
  if (handle != NULL && handle->ctx != NULL && handle->selector != NULL) {
    handle->ctx->selector_destroy(handle->ctx, handle->selector);
    handle->selector = NULL;
  }
  handle->ctx = NULL;
  return 0;
}

static lua_lql_selector_handle *lua_lql_check_selector(lua_State *L,
                                                       int index) {
  lua_lql_selector_handle *handle;

  handle =
      (lua_lql_selector_handle *)luaL_checkudata(L, index, LUA_LQL_SELECTOR);
  luaL_argcheck(L, handle != NULL && handle->ctx != NULL &&
                       handle->selector != NULL,
                index, "closed lql selector");
  return handle;
}

static int lua_lql_push_selector(lua_State *L, lua_lql_client *client,
                                 lql_selector *selector) {
  lua_lql_selector_handle *handle;

  handle = (lua_lql_selector_handle *)lua_newuserdatauv(L, sizeof(*handle), 1);
  handle->ctx = client->ctx;
  handle->selector = selector;
  luaL_getmetatable(L, LUA_LQL_SELECTOR);
  lua_setmetatable(L, -2);
  lua_pushvalue(L, 1);
  lua_setiuservalue(L, -2, 1);
  return 1;
}

static int lua_lql_projection_gc(lua_State *L) {
  lua_lql_projection_handle *handle;

  handle =
      (lua_lql_projection_handle *)luaL_checkudata(L, 1, LUA_LQL_PROJECTION);
  if (handle != NULL && handle->ctx != NULL && handle->projection != NULL) {
    handle->ctx->projection_destroy(handle->ctx, handle->projection);
    handle->projection = NULL;
  }
  handle->ctx = NULL;
  return 0;
}

static int lua_lql_mutation_plan_gc(lua_State *L) {
  lua_lql_mutation_plan_handle *handle;

  handle = (lua_lql_mutation_plan_handle *)luaL_checkudata(
      L, 1, LUA_LQL_MUTATION_PLAN);
  if (handle != NULL && handle->ctx != NULL && handle->plan != NULL) {
    handle->ctx->mutation_plan_destroy(handle->ctx, handle->plan);
    handle->plan = NULL;
  }
  handle->ctx = NULL;
  return 0;
}

static lql_status lua_lql_selector_arg(lua_State *L, lua_lql_client *client,
                                       int index, lql_selector **out,
                                       int *out_owned, lql_error *error) {
  lua_lql_selector_handle *handle;
  const char *selector_expr;

  *out = NULL;
  *out_owned = 0;
  handle = lua_lql_test_selector(L, index);
  if (handle != NULL) {
    if (handle->ctx != client->ctx || handle->selector == NULL) {
      lua_lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                        "selector belongs to another lql client");
      return LQL_STATUS_INVALID_ARGUMENT;
    }
    *out = handle->selector;
    return LQL_STATUS_OK;
  }
  selector_expr = luaL_checkstring(L, index);
  *out_owned = 1;
  return client->ctx->selector_parse(client->ctx, selector_expr, out, error);
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

static int lua_lql_selector_parse(lua_State *L) {
  lua_lql_client *client;
  const char *selector_expr;
  lql_selector *selector;
  lql_error error;
  lql_status st;

  client = lua_lql_check_client(L, 1);
  selector_expr = luaL_checkstring(L, 2);
  selector = NULL;
  lql_error_init(&error);
  st = client->ctx->selector_parse(client->ctx, selector_expr,
                                   &selector, &error);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  return lua_lql_push_selector(L, client, selector);
}

static int lua_lql_selector_parse_or(lua_State *L) {
  lua_lql_client *client;
  const char *selector_expr;
  lql_selector *selector;
  lql_error error;
  lql_status st;

  client = lua_lql_check_client(L, 1);
  selector_expr = luaL_checkstring(L, 2);
  selector = NULL;
  lql_error_init(&error);
  st = client->ctx->selector_parse_or(client->ctx, selector_expr,
                                      &selector, &error);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  return lua_lql_push_selector(L, client, selector);
}

static void lua_lql_push_bool_field(lua_State *L, const char *name, int value) {
  lua_pushboolean(L, value ? 1 : 0);
  lua_setfield(L, -2, name);
}

static void lua_lql_push_capabilities(lua_State *L,
                                      const lql_capabilities *capabilities) {
  lua_newtable(L);
  lua_lql_push_bool_field(L, "selector_parse", capabilities->selector_parse);
  lua_lql_push_bool_field(L, "selector_inspection",
                          capabilities->selector_inspection);
  lua_lql_push_bool_field(L, "matches_json", capabilities->matches_json);
  lua_lql_push_bool_field(L, "file_decision_stream",
                          capabilities->file_decision_stream);
  lua_lql_push_bool_field(L, "source_decision_stream",
                          capabilities->source_decision_stream);
  lua_lql_push_bool_field(L, "file_match_stream",
                          capabilities->file_match_stream);
  lua_lql_push_bool_field(L, "seekable_range_payloads",
                          capabilities->seekable_range_payloads);
  lua_lql_push_bool_field(L, "source_spooled_match_stream",
                          capabilities->source_spooled_match_stream);
  lua_lql_push_bool_field(L, "spooled_payloads",
                          capabilities->spooled_payloads);
  lua_lql_push_bool_field(L, "payload_sink_write",
                          capabilities->payload_sink_write);
  lua_lql_push_bool_field(L, "payload_projection",
                          capabilities->payload_projection);
  lua_lql_push_bool_field(L, "projection_file_range",
                          capabilities->projection_file_range);
  lua_lql_push_bool_field(L, "projection_source",
                          capabilities->projection_source);
  lua_lql_push_bool_field(L, "projection_buffered_json",
                          capabilities->projection_buffered_json);
  lua_lql_push_bool_field(L, "compact_file_range",
                          capabilities->compact_file_range);
  lua_lql_push_bool_field(L, "compact_source", capabilities->compact_source);
  lua_lql_push_bool_field(L, "compact_buffered_json",
                          capabilities->compact_buffered_json);
  lua_lql_push_bool_field(L, "mutation_parse", capabilities->mutation_parse);
  lua_lql_push_bool_field(L, "mutation_file_range",
                          capabilities->mutation_file_range);
  lua_lql_push_bool_field(L, "mutation_file_range_candidates",
                          capabilities->mutation_file_range_candidates);
  lua_lql_push_bool_field(L, "mutation_source", capabilities->mutation_source);
  lua_lql_push_bool_field(L, "mutation_source_candidates",
                          capabilities->mutation_source_candidates);
  lua_lql_push_bool_field(
      L, "mutation_file_range_projected_candidates",
      capabilities->mutation_file_range_projected_candidates);
  lua_lql_push_bool_field(L, "mutation_source_projected_candidates",
                          capabilities->mutation_source_projected_candidates);
  lua_lql_push_bool_field(L, "mutation_buffered_json",
                          capabilities->mutation_buffered_json);
  lua_lql_push_bool_field(L, "mutation_file_values",
                          capabilities->mutation_file_values);
}

static int lua_client_version(lua_State *L) {
  lua_lql_client *client;

  client = lua_lql_check_client(L, 1);
  lua_pushstring(L, client->ctx->version(client->ctx));
  return 1;
}

static int lua_client_capabilities_get(lua_State *L) {
  lua_lql_client *client;
  lql_capabilities capabilities;

  client = lua_lql_check_client(L, 1);
  client->ctx->capabilities_get(client->ctx, &capabilities);
  lua_lql_push_capabilities(L, &capabilities);
  return 1;
}

static void lua_lql_push_selector_capabilities(
    lua_State *L, const lql_selector_capabilities *capabilities) {
  lua_newtable(L);
  lua_lql_push_bool_field(L, "and", capabilities->and_);
  lua_lql_push_bool_field(L, "or", capabilities->or_);
  lua_lql_push_bool_field(L, "not", capabilities->not_);
  lua_lql_push_bool_field(L, "eq", capabilities->eq);
  lua_lql_push_bool_field(L, "range", capabilities->range);
  lua_lql_push_bool_field(L, "date", capabilities->date);
  lua_lql_push_bool_field(L, "in", capabilities->in);
  lua_lql_push_bool_field(L, "prefix", capabilities->prefix);
  lua_lql_push_bool_field(L, "contains", capabilities->contains);
  lua_lql_push_bool_field(L, "exists", capabilities->exists);
  lua_lql_push_bool_field(L, "wildcard_path", capabilities->wildcard_path);
  lua_lql_push_bool_field(L, "recursive_path", capabilities->recursive_path);
}

static void lua_lql_push_selector_execution_traits(
    lua_State *L, const lql_selector_execution_traits *traits) {
  lua_newtable(L);
  lua_lql_push_bool_field(L, "uses_contains_like", traits->uses_contains_like);
  lua_lql_push_bool_field(L, "uses_recursive_path",
                          traits->uses_recursive_path);
  lua_lql_push_bool_field(L, "uses_wildcard_path", traits->uses_wildcard_path);
  lua_lql_push_bool_field(L, "requires_object_root",
                          traits->requires_object_root);
  lua_lql_push_bool_field(L, "early_non_match_likely",
                          traits->early_non_match_likely);
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
  }
  return "unknown";
}

static int lua_lql_selector_kind_from_string(const char *name,
                                             lql_selector_node_kind *out) {
  if (strcmp(name, "all") == 0) {
    *out = LQL_SELECTOR_NODE_ALL;
  } else if (strcmp(name, "and") == 0) {
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
    return 0;
  }
  return 1;
}

static const char *
lua_lql_selector_since_kind_name(lql_selector_since_kind kind) {
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
  return "unknown";
}

static int lua_lql_selector_since_kind_from_string(
    const char *name, lql_selector_since_kind *out) {
  if (strcmp(name, "none") == 0) {
    *out = LQL_SELECTOR_SINCE_NONE;
  } else if (strcmp(name, "now") == 0) {
    *out = LQL_SELECTOR_SINCE_NOW;
  } else if (strcmp(name, "today") == 0) {
    *out = LQL_SELECTOR_SINCE_TODAY;
  } else if (strcmp(name, "yesterday") == 0) {
    *out = LQL_SELECTOR_SINCE_YESTERDAY;
  } else if (strcmp(name, "literal") == 0) {
    *out = LQL_SELECTOR_SINCE_LITERAL;
  } else {
    return 0;
  }
  return 1;
}

static void lua_lql_push_view(lua_State *L, lql_string_view view) {
  lua_pushlstring(L, view.data != NULL ? view.data : "", view.len);
}

static void lua_lql_push_view_field(lua_State *L, const char *name,
                                    lql_string_view view) {
  if (view.data != NULL || view.len != 0u) {
    lua_lql_push_view(L, view);
    lua_setfield(L, -2, name);
  }
}

static void lua_lql_set_string_field(lua_State *L, const char *name,
                                     const char *value) {
  lua_pushstring(L, value);
  lua_setfield(L, -2, name);
}

static lql_status lua_lql_push_selector_node(lua_State *L, const lql *ctx,
                                             lql_selector_node node,
                                             lql_error *error);

static lql_status lua_lql_push_selector_children(lua_State *L, const lql *ctx,
                                                 lql_selector_node node,
                                                 lql_error *error) {
  lql_selector_node child;
  lql_status st;
  size_t count;
  size_t i;

  count = 0u;
  st = ctx->selector_node_child_count(ctx, node, &count, error);
  if (st != LQL_STATUS_OK) {
    return st;
  }
  lua_newtable(L);
  for (i = 0u; i < count; ++i) {
    st = ctx->selector_node_child(ctx, node, i, &child, error);
    if (st != LQL_STATUS_OK) {
      lua_pop(L, 1);
      return st;
    }
    st = lua_lql_push_selector_node(L, ctx, child, error);
    if (st != LQL_STATUS_OK) {
      lua_pop(L, 1);
      return st;
    }
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  lua_setfield(L, -2, "children");
  return LQL_STATUS_OK;
}

static lql_status lua_lql_push_selector_any(lua_State *L, const lql *ctx,
                                            lql_selector_node node,
                                            size_t count, int in_term,
                                            lql_error *error) {
  lql_string_view value;
  lql_status st;
  size_t i;

  lua_newtable(L);
  for (i = 0u; i < count; ++i) {
    st = in_term ? ctx->selector_node_in_term_any(ctx, node, i, &value, error)
                 : ctx->selector_node_string_term_any(ctx, node, i, &value,
                                                      error);
    if (st != LQL_STATUS_OK) {
      lua_pop(L, 1);
      return st;
    }
    lua_lql_push_view(L, value);
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  lua_setfield(L, -2, "any");
  return LQL_STATUS_OK;
}

static void lua_lql_push_selector_range_bound(
    lua_State *L, const char *name, lql_selector_range_bound bound) {
  if (bound.kind == LQL_SELECTOR_BOUND_ABSENT) {
    return;
  }
  if (bound.kind == LQL_SELECTOR_BOUND_NUMBER) {
    lua_pushnumber(L, (lua_Number)bound.number);
  } else {
    lua_lql_push_view(L, bound.datetime);
  }
  lua_setfield(L, -2, name);
}

static void lua_lql_set_range_kind_field(lua_State *L, const char *name,
                                         lql_selector_range_bound bound) {
  char key[16];

  if (bound.kind == LQL_SELECTOR_BOUND_ABSENT) {
    return;
  }
  strcpy(key, name);
  strcat(key, "_kind");
  lua_pushstring(L, bound.kind == LQL_SELECTOR_BOUND_NUMBER ? "number"
                                                           : "datetime");
  lua_setfield(L, -2, key);
}

static lql_status lua_lql_push_selector_node(lua_State *L, const lql *ctx,
                                             lql_selector_node node,
                                             lql_error *error) {
  lql_selector_string_term string_term;
  lql_selector_range_term range_term;
  lql_selector_date_term date_term;
  lql_selector_in_term in_term;
  lql_string_view path;
  lql_status st;

  lua_newtable(L);
  lua_lql_set_string_field(L, "kind", lua_lql_selector_kind_name(node.kind));
  if (node.kind == LQL_SELECTOR_NODE_AND || node.kind == LQL_SELECTOR_NODE_OR ||
      node.kind == LQL_SELECTOR_NODE_NOT) {
    return lua_lql_push_selector_children(L, ctx, node, error);
  }
  if (node.kind == LQL_SELECTOR_NODE_EQ ||
      node.kind == LQL_SELECTOR_NODE_CONTAINS ||
      node.kind == LQL_SELECTOR_NODE_ICONTAINS ||
      node.kind == LQL_SELECTOR_NODE_PREFIX ||
      node.kind == LQL_SELECTOR_NODE_IPREFIX) {
    memset(&string_term, 0, sizeof(string_term));
    st = ctx->selector_node_string_term(ctx, node, &string_term, error);
    if (st != LQL_STATUS_OK) {
      lua_pop(L, 1);
      return st;
    }
    lua_lql_push_view_field(L, "field", string_term.field);
    lua_pushboolean(L, string_term.value_present);
    lua_setfield(L, -2, "value_present");
    if (string_term.value_present || string_term.value.len != 0u) {
      lua_lql_push_view(L, string_term.value);
      lua_setfield(L, -2, "value");
    }
    lua_pushboolean(L, string_term.ignore_case);
    lua_setfield(L, -2, "ignore_case");
    return lua_lql_push_selector_any(L, ctx, node, string_term.any_count, 0,
                                     error);
  }
  if (node.kind == LQL_SELECTOR_NODE_RANGE) {
    memset(&range_term, 0, sizeof(range_term));
    st = ctx->selector_node_range_term(ctx, node, &range_term, error);
    if (st != LQL_STATUS_OK) {
      lua_pop(L, 1);
      return st;
    }
    lua_lql_push_view_field(L, "field", range_term.field);
    lua_lql_push_selector_range_bound(L, "gt", range_term.gt);
    lua_lql_push_selector_range_bound(L, "gte", range_term.gte);
    lua_lql_push_selector_range_bound(L, "lt", range_term.lt);
    lua_lql_push_selector_range_bound(L, "lte", range_term.lte);
    lua_lql_set_range_kind_field(L, "gt", range_term.gt);
    lua_lql_set_range_kind_field(L, "gte", range_term.gte);
    lua_lql_set_range_kind_field(L, "lt", range_term.lt);
    lua_lql_set_range_kind_field(L, "lte", range_term.lte);
    return LQL_STATUS_OK;
  }
  if (node.kind == LQL_SELECTOR_NODE_DATE) {
    memset(&date_term, 0, sizeof(date_term));
    st = ctx->selector_node_date_term(ctx, node, &date_term, error);
    if (st != LQL_STATUS_OK) {
      lua_pop(L, 1);
      return st;
    }
    lua_lql_push_view_field(L, "field", date_term.field);
    lua_lql_push_view_field(L, "value", date_term.value);
    lua_lql_push_view_field(L, "since", date_term.since);
    lua_lql_set_string_field(L, "since_kind",
                             lua_lql_selector_since_kind_name(
                                 date_term.since_kind));
    lua_lql_push_view_field(L, "after", date_term.after);
    lua_lql_push_view_field(L, "before", date_term.before);
    lua_lql_push_view_field(L, "gt", date_term.gt);
    lua_lql_push_view_field(L, "gte", date_term.gte);
    lua_lql_push_view_field(L, "lt", date_term.lt);
    lua_lql_push_view_field(L, "lte", date_term.lte);
    return LQL_STATUS_OK;
  }
  if (node.kind == LQL_SELECTOR_NODE_IN) {
    memset(&in_term, 0, sizeof(in_term));
    st = ctx->selector_node_in_term(ctx, node, &in_term, error);
    if (st != LQL_STATUS_OK) {
      lua_pop(L, 1);
      return st;
    }
    lua_lql_push_view_field(L, "field", in_term.field);
    return lua_lql_push_selector_any(L, ctx, node, in_term.any_count, 1,
                                     error);
  }
  if (node.kind == LQL_SELECTOR_NODE_EXISTS) {
    path.data = NULL;
    path.len = 0u;
    st = ctx->selector_node_exists_path(ctx, node, &path, error);
    if (st != LQL_STATUS_OK) {
      lua_pop(L, 1);
      return st;
    }
    lua_lql_push_view_field(L, "path", path);
  }
  return LQL_STATUS_OK;
}

static int lua_lql_selector_capabilities_get(lua_State *L) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_selector_capabilities capabilities;
  lql_error error;
  lql_status st;
  int selector_owned;

  client = lua_lql_check_client(L, 1);
  selector = NULL;
  selector_owned = 0;
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st == LQL_STATUS_OK) {
    client->ctx->selector_capabilities_get(client->ctx, selector,
                                           &capabilities);
  }
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  lua_lql_push_selector_capabilities(L, &capabilities);
  return 1;
}

static int lua_lql_selector_execution_traits_get(lua_State *L) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_selector_execution_traits traits;
  lql_error error;
  lql_status st;
  int selector_owned;

  client = lua_lql_check_client(L, 1);
  selector = NULL;
  selector_owned = 0;
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st == LQL_STATUS_OK) {
    client->ctx->selector_execution_traits_get(client->ctx, selector, &traits);
  }
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  lua_lql_push_selector_execution_traits(L, &traits);
  return 1;
}

static int lua_lql_selector_parse_json(lua_State *L) {
  lua_lql_client *client;
  const char *json;
  size_t json_len;
  lql_selector *selector;
  lql_error error;
  lql_status st;

  client = lua_lql_check_client(L, 1);
  json = luaL_checklstring(L, 2, &json_len);
  selector = NULL;
  lql_error_init(&error);
  st = client->ctx->selector_parse_json(client->ctx, json, json_len,
                                        &selector, &error);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  return lua_lql_push_selector(L, client, selector);
}

static int lua_lql_selector_push_root(lua_State *L, lql *ctx,
                                      lql_selector *selector) {
  lql_selector_node root;
  lql_error error;
  lql_status st;

  memset(&root, 0, sizeof(root));
  lql_error_init(&error);
  st = ctx->selector_root(ctx, selector, &root, &error);
  if (st == LQL_STATUS_OK) {
    st = lua_lql_push_selector_node(L, ctx, root, &error);
  }
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  return 1;
}

static int lua_lql_selector_root(lua_State *L) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int selector_owned;

  client = lua_lql_check_client(L, 1);
  selector = NULL;
  selector_owned = 0;
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  st = lua_lql_selector_push_root(L, client->ctx, selector);
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  return st;
}

static int lua_lql_selector_method_root(lua_State *L) {
  lua_lql_selector_handle *handle;

  handle = lua_lql_check_selector(L, 1);
  return lua_lql_selector_push_root(L, handle->ctx, handle->selector);
}

static int lua_lql_selector_push_json(lua_State *L, lql *ctx,
                                      const lql_selector *selector) {
  lua_lql_buffer buffer;
  lql_error error;
  lql_status st;
  FILE *out;

  out = tmpfile();
  lua_lql_buffer_init(&buffer, L);
  lql_error_init(&error);
  if (out == NULL) {
    lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                      "failed to create Lua selector output file");
    return lua_lql_fail(L, &error);
  }
  st = ctx->selector_write_json(ctx, selector, out, &error);
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

static int lua_lql_selector_json(lua_State *L) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_error error;
  int selector_owned;
  int result_count;
  lql_status st;

  client = lua_lql_check_client(L, 1);
  selector = NULL;
  selector_owned = 0;
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  result_count = lua_lql_selector_push_json(L, client->ctx, selector);
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  return result_count;
}

static int lua_lql_selector_method_json(lua_State *L) {
  lua_lql_selector_handle *handle;

  handle = lua_lql_check_selector(L, 1);
  return lua_lql_selector_push_json(L, handle->ctx, handle->selector);
}

static int lua_lql_selector_method_is_empty(lua_State *L) {
  lua_lql_selector_handle *handle;

  handle = lua_lql_check_selector(L, 1);
  lua_pushboolean(L, handle->ctx->selector_is_empty(handle->ctx,
                                                    handle->selector));
  return 1;
}

static int lua_lql_selector_method_capabilities(lua_State *L) {
  lua_lql_selector_handle *handle;
  lql_selector_capabilities capabilities;

  handle = lua_lql_check_selector(L, 1);
  handle->ctx->selector_capabilities_get(handle->ctx, handle->selector,
                                         &capabilities);
  lua_lql_push_selector_capabilities(L, &capabilities);
  return 1;
}

static int lua_lql_selector_method_execution_traits(lua_State *L) {
  lua_lql_selector_handle *handle;
  lql_selector_execution_traits traits;

  handle = lua_lql_check_selector(L, 1);
  handle->ctx->selector_execution_traits_get(handle->ctx, handle->selector,
                                             &traits);
  lua_lql_push_selector_execution_traits(L, &traits);
  return 1;
}

static lql_string_view lua_lql_check_view(lua_State *L, int index) {
  lql_string_view view;

  view.data = luaL_checklstring(L, index, &view.len);
  return view;
}

static lql_string_view lua_lql_table_view(lua_State *L, int index,
                                          const char *key) {
  lql_string_view view;

  view.data = NULL;
  view.len = 0u;
  lua_getfield(L, index, key);
  if (!lua_isnil(L, -1)) {
    view.data = luaL_checklstring(L, -1, &view.len);
  }
  lua_pop(L, 1);
  return view;
}

static int lua_lql_table_bool(lua_State *L, int index, const char *key) {
  int value;

  lua_getfield(L, index, key);
  value = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return value;
}

static int lua_lql_table_has(lua_State *L, int index, const char *key) {
  int present;

  lua_getfield(L, index, key);
  present = !lua_isnil(L, -1);
  lua_pop(L, 1);
  return present;
}

static void lua_lql_range_bound_from_table(lua_State *L, int index,
                                           const char *key,
                                           lql_selector_range_bound *out) {
  size_t len;

  memset(out, 0, sizeof(*out));
  lua_getfield(L, index, key);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  if (lua_isnumber(L, -1)) {
    out->kind = LQL_SELECTOR_BOUND_NUMBER;
    out->number = (double)lua_tonumber(L, -1);
  } else {
    out->kind = LQL_SELECTOR_BOUND_DATETIME;
    out->datetime.data = luaL_checklstring(L, -1, &len);
    out->datetime.len = len;
  }
  lua_pop(L, 1);
}

static int lua_lql_view_array(lua_State *L, int index,
                              lql_string_view **out_values,
                              size_t *out_count) {
  lql_string_view *values;
  size_t count;
  size_t i;

  values = NULL;
  luaL_checktype(L, index, LUA_TTABLE);
  count = (size_t)lua_rawlen(L, index);
  if (count > 0u) {
    values = (lql_string_view *)lua_lql_alloc(
        L, NULL, 0u, sizeof(values[0]) * count);
    if (values == NULL) {
      return 0;
    }
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(L, index, (lua_Integer)i + 1);
    values[i] = lua_lql_check_view(L, -1);
    lua_pop(L, 1);
  }
  *out_values = values;
  *out_count = count;
  return 1;
}

static void lua_lql_view_array_release(lua_State *L, lql_string_view *values,
                                       size_t count) {
  if (values != NULL) {
    (void)lua_lql_alloc(L, values, sizeof(values[0]) * count, 0u);
  }
}

static int lua_lql_selector_build_all(lua_State *L) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_error error;
  lql_status st;

  client = lua_lql_check_client(L, 1);
  selector = NULL;
  lql_error_init(&error);
  st = client->ctx->selector_build_all(client->ctx, &selector, &error);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  return lua_lql_push_selector(L, client, selector);
}

static int lua_lql_selector_build_compound(lua_State *L) {
  lua_lql_client *client;
  const char *kind_name;
  lql_selector_node_kind kind;
  const lql_selector **children;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  size_t count;
  size_t i;

  client = lua_lql_check_client(L, 1);
  kind_name = luaL_checkstring(L, 2);
  luaL_checktype(L, 3, LUA_TTABLE);
  if (!lua_lql_selector_kind_from_string(kind_name, &kind) ||
      (kind != LQL_SELECTOR_NODE_AND && kind != LQL_SELECTOR_NODE_OR)) {
    lql_error_init(&error);
    lua_lql_set_error(&error, LQL_STATUS_INVALID_ARGUMENT,
                      "compound selector kind must be and or or");
    return lua_lql_fail(L, &error);
  }
  count = (size_t)lua_rawlen(L, 3);
  children = NULL;
  if (count > 0u) {
    children = (const lql_selector **)lua_lql_alloc(
        L, NULL, 0u, sizeof(children[0]) * count);
    if (children == NULL) {
      lql_error_init(&error);
      lua_lql_set_error(&error, LQL_STATUS_NO_MEMORY, "out of memory");
      return lua_lql_fail(L, &error);
    }
  }
  for (i = 0u; i < count; ++i) {
    lua_lql_selector_handle *child;
    lua_rawgeti(L, 3, (lua_Integer)i + 1);
    child = lua_lql_check_selector(L, -1);
    luaL_argcheck(L, child->ctx == client->ctx, 3,
                  "selector belongs to another lql client");
    children[i] = child->selector;
    lua_pop(L, 1);
  }
  selector = NULL;
  lql_error_init(&error);
  st = client->ctx->selector_build_compound(client->ctx, kind, children, count,
                                           &selector, &error);
  if (children != NULL) {
    (void)lua_lql_alloc(L, (void *)children, sizeof(children[0]) * count, 0u);
  }
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  return lua_lql_push_selector(L, client, selector);
}

static int lua_lql_selector_build_not(lua_State *L) {
  lua_lql_client *client;
  lua_lql_selector_handle *child;
  lql_selector *selector;
  lql_error error;
  lql_status st;

  client = lua_lql_check_client(L, 1);
  child = lua_lql_check_selector(L, 2);
  luaL_argcheck(L, child->ctx == client->ctx, 2,
                "selector belongs to another lql client");
  selector = NULL;
  lql_error_init(&error);
  st = client->ctx->selector_build_not(client->ctx, child->selector,
                                      &selector, &error);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  return lua_lql_push_selector(L, client, selector);
}

static int lua_lql_selector_build_string(lua_State *L) {
  lua_lql_client *client;
  const char *kind_name;
  lql_selector_node_kind kind;
  lql_selector_string_term term;
  lql_string_view *any_values;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  size_t any_count;

  client = lua_lql_check_client(L, 1);
  kind_name = luaL_checkstring(L, 2);
  luaL_checktype(L, 3, LUA_TTABLE);
  if (!lua_lql_selector_kind_from_string(kind_name, &kind)) {
    lql_error_init(&error);
    lua_lql_set_error(&error, LQL_STATUS_INVALID_ARGUMENT,
                      "string selector kind invalid");
    return lua_lql_fail(L, &error);
  }
  memset(&term, 0, sizeof(term));
  term.field = lua_lql_table_view(L, 3, "field");
  term.value = lua_lql_table_view(L, 3, "value");
  term.value_present = lua_lql_table_has(L, 3, "value") ||
                       lua_lql_table_bool(L, 3, "value_present");
  term.ignore_case = lua_lql_table_bool(L, 3, "ignore_case");
  any_values = NULL;
  any_count = 0u;
  if (!lua_isnoneornil(L, 4)) {
    if (!lua_lql_view_array(L, 4, &any_values, &any_count)) {
      lql_error_init(&error);
      lua_lql_set_error(&error, LQL_STATUS_NO_MEMORY, "out of memory");
      return lua_lql_fail(L, &error);
    }
    term.any_count = any_count;
  }
  selector = NULL;
  lql_error_init(&error);
  st = client->ctx->selector_build_string(client->ctx, kind, &term, any_values,
                                          &selector, &error);
  lua_lql_view_array_release(L, any_values, any_count);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  return lua_lql_push_selector(L, client, selector);
}

static int lua_lql_selector_build_range(lua_State *L) {
  lua_lql_client *client;
  lql_selector_range_term term;
  lql_selector *selector;
  lql_error error;
  lql_status st;

  client = lua_lql_check_client(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  memset(&term, 0, sizeof(term));
  term.field = lua_lql_table_view(L, 2, "field");
  lua_lql_range_bound_from_table(L, 2, "gt", &term.gt);
  lua_lql_range_bound_from_table(L, 2, "gte", &term.gte);
  lua_lql_range_bound_from_table(L, 2, "lt", &term.lt);
  lua_lql_range_bound_from_table(L, 2, "lte", &term.lte);
  selector = NULL;
  lql_error_init(&error);
  st = client->ctx->selector_build_range(client->ctx, &term, &selector,
                                         &error);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  return lua_lql_push_selector(L, client, selector);
}

static int lua_lql_selector_build_date(lua_State *L) {
  lua_lql_client *client;
  lql_selector_date_term term;
  lql_selector *selector;
  const char *since_kind;
  lql_error error;
  lql_status st;

  client = lua_lql_check_client(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  memset(&term, 0, sizeof(term));
  term.field = lua_lql_table_view(L, 2, "field");
  term.value = lua_lql_table_view(L, 2, "value");
  term.since = lua_lql_table_view(L, 2, "since");
  term.after = lua_lql_table_view(L, 2, "after");
  term.before = lua_lql_table_view(L, 2, "before");
  term.gt = lua_lql_table_view(L, 2, "gt");
  term.gte = lua_lql_table_view(L, 2, "gte");
  term.lt = lua_lql_table_view(L, 2, "lt");
  term.lte = lua_lql_table_view(L, 2, "lte");
  term.since_kind = LQL_SELECTOR_SINCE_NONE;
  lua_getfield(L, 2, "since_kind");
  if (!lua_isnil(L, -1)) {
    since_kind = luaL_checkstring(L, -1);
    if (!lua_lql_selector_since_kind_from_string(since_kind,
                                                 &term.since_kind)) {
      lua_pop(L, 1);
      lql_error_init(&error);
      lua_lql_set_error(&error, LQL_STATUS_INVALID_ARGUMENT,
                        "date selector since_kind invalid");
      return lua_lql_fail(L, &error);
    }
  }
  lua_pop(L, 1);
  selector = NULL;
  lql_error_init(&error);
  st = client->ctx->selector_build_date(client->ctx, &term, &selector, &error);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  return lua_lql_push_selector(L, client, selector);
}

static int lua_lql_selector_build_in(lua_State *L) {
  lua_lql_client *client;
  lql_selector_in_term term;
  lql_string_view *any_values;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  size_t any_count;

  client = lua_lql_check_client(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  memset(&term, 0, sizeof(term));
  term.field = lua_lql_table_view(L, 2, "field");
  any_values = NULL;
  any_count = 0u;
  if (!lua_lql_view_array(L, 3, &any_values, &any_count)) {
    lql_error_init(&error);
    lua_lql_set_error(&error, LQL_STATUS_NO_MEMORY, "out of memory");
    return lua_lql_fail(L, &error);
  }
  term.any_count = any_count;
  selector = NULL;
  lql_error_init(&error);
  st = client->ctx->selector_build_in(client->ctx, &term, any_values,
                                      &selector, &error);
  lua_lql_view_array_release(L, any_values, any_count);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  return lua_lql_push_selector(L, client, selector);
}

static int lua_lql_selector_build_exists(lua_State *L) {
  lua_lql_client *client;
  lql_string_view path;
  lql_selector *selector;
  lql_error error;
  lql_status st;

  client = lua_lql_check_client(L, 1);
  path = lua_lql_check_view(L, 2);
  selector = NULL;
  lql_error_init(&error);
  st = client->ctx->selector_build_exists(client->ctx, path, &selector,
                                          &error);
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  return lua_lql_push_selector(L, client, selector);
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

static lql_status lua_lql_file_size(FILE *file, lql_uint64 *out_size,
                                    lql_error *error) {
  off_t size;

  if (file == NULL || out_size == NULL) {
    lua_lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                      "invalid Lua file size arguments");
    return LQL_STATUS_INVALID_ARGUMENT;
  }
  if (fseeko(file, (off_t)0, SEEK_END) != 0) {
    lua_lql_set_error(error, LQL_STATUS_JSON_ERROR,
                      "failed to seek Lua input file");
    return LQL_STATUS_JSON_ERROR;
  }
  size = ftello(file);
  if (size < (off_t)0) {
    lua_lql_set_error(error, LQL_STATUS_JSON_ERROR,
                      "failed to tell Lua input file size");
    return LQL_STATUS_JSON_ERROR;
  }
  if (fseeko(file, (off_t)0, SEEK_SET) != 0) {
    lua_lql_set_error(error, LQL_STATUS_JSON_ERROR,
                      "failed to rewind Lua input file");
    return LQL_STATUS_JSON_ERROR;
  }
  *out_size = (lql_uint64)size;
  return LQL_STATUS_OK;
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
    next =
        (char *)lua_lql_alloc(buffer->lua, buffer->data, buffer->cap, next_cap);
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
    fields =
        (const char **)lua_lql_alloc(L, NULL, 0u, sizeof(fields[0]) * count);
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

static const char *lua_lql_options_string(lua_State *L, int index,
                                          const char *key) {
  const char *value;

  value = NULL;
  if (lua_istable(L, index)) {
    lua_getfield(L, index, key);
    if (!lua_isnil(L, -1)) {
      value = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
  }
  return value;
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

static lql_read_result
lua_lql_read_source_chunk(void *user, unsigned char *buffer, size_t capacity) {
  lua_lql_source_state *state;
  lql_read_result result;
  lua_State *L;
  const char *chunk;
  size_t chunk_len;
  int ok;

  state = (lua_lql_source_state *)user;
  memset(&result, 0, sizeof(result));
  if (state == NULL || state->read_failed) {
    result.eof = 1;
    result.error_code = 1;
    return result;
  }
  L = state->lua;
  lua_rawgeti(L, LUA_REGISTRYINDEX, state->read_ref);
  lua_pushinteger(L, (lua_Integer)capacity);
  ok = lua_pcall(L, 1, 1, 0);
  if (ok != LUA_OK) {
    chunk = lua_tostring(L, -1);
    state->read_failed = 1;
    strncpy(state->read_message,
            chunk != NULL ? chunk : "source read callback failed",
            sizeof(state->read_message) - 1u);
    state->read_message[sizeof(state->read_message) - 1u] = '\0';
    lua_lql_set_error(state->error, LQL_STATUS_JSON_ERROR, state->read_message);
    lua_pop(L, 1);
    result.eof = 1;
    result.error_code = 1;
    return result;
  }
  if (lua_isnoneornil(L, -1) || lua_isboolean(L, -1)) {
    result.eof = 1;
    lua_pop(L, 1);
    return result;
  }
  chunk = luaL_checklstring(L, -1, &chunk_len);
  if (chunk_len == 0u) {
    result.eof = 1;
    lua_pop(L, 1);
    return result;
  }
  if (chunk_len > capacity) {
    state->read_failed = 1;
    strncpy(state->read_message,
            "source read callback returned chunk larger than capacity",
            sizeof(state->read_message) - 1u);
    state->read_message[sizeof(state->read_message) - 1u] = '\0';
    lua_lql_set_error(state->error, LQL_STATUS_JSON_ERROR, state->read_message);
    lua_pop(L, 1);
    result.eof = 1;
    result.error_code = 1;
    return result;
  }
  memcpy(buffer, chunk, chunk_len);
  result.bytes_read = chunk_len;
  lua_pop(L, 1);
  return result;
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

static lql_status lua_lql_on_select_payload(void *user,
                                            const lql_query_match *match) {
  lua_lql_file_state *state;
  lql_status st;
  lql_error error;

  state = (lua_lql_file_state *)user;
  if (!match->decision.matched) {
    return LQL_STATUS_OK;
  }
  lql_error_init(&error);
  st = state->ctx->payload_write_json(state->ctx, &match->payload, state->out,
                                      state->error != NULL ? state->error
                                                           : &error);
  if (st == LQL_STATUS_OK && fputc('\n', state->out) == EOF) {
    lua_lql_set_error(state->error, LQL_STATUS_JSON_ERROR,
                      "failed to write Lua output file");
    return LQL_STATUS_JSON_ERROR;
  }
  return st;
}

static lql_status lua_lql_on_project_payload(void *user,
                                             const lql_query_match *match) {
  lua_lql_file_state *state;
  lql_status st;
  lql_error error;
  int found;

  state = (lua_lql_file_state *)user;
  if (!match->decision.matched) {
    return LQL_STATUS_OK;
  }
  found = 0;
  lql_error_init(&error);
  st = state->ctx->payload_project_json(
      state->ctx, &match->payload, state->projection, state->out, &found,
      state->error != NULL ? state->error : &error);
  if (st == LQL_STATUS_OK && found && fputc('\n', state->out) == EOF) {
    lua_lql_set_error(state->error, LQL_STATUS_JSON_ERROR,
                      "failed to write Lua output file");
    return LQL_STATUS_JSON_ERROR;
  }
  return st;
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

static lql_status lua_lql_projection_arg(lua_State *L, lua_lql_client *client,
                                         int index, lql_projection **out,
                                         int *out_owned, lql_error *error) {
  lua_lql_projection_handle *handle;
  const char **fields;
  size_t field_count;
  lql_status st;

  *out = NULL;
  *out_owned = 0;
  handle = lua_lql_test_projection(L, index);
  if (handle != NULL) {
    if (handle->ctx != client->ctx || handle->projection == NULL) {
      lua_lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                        "projection belongs to another lql client");
      return LQL_STATUS_INVALID_ARGUMENT;
    }
    *out = handle->projection;
    return LQL_STATUS_OK;
  }

  fields = NULL;
  field_count = 0u;
  if (!lua_lql_fields(L, index, &fields, &field_count)) {
    lua_lql_set_error(error, LQL_STATUS_NO_MEMORY, "out of memory");
    return LQL_STATUS_NO_MEMORY;
  }
  *out_owned = 1;
  st = client->ctx->projection_parse(client->ctx,
                                     (const char *const *)fields, field_count,
                                     out, error);
  if (fields != NULL) {
    (void)lua_lql_alloc(L, (void *)fields, sizeof(fields[0]) * field_count,
                        0u);
  }
  return st;
}

static lql_status lua_lql_mutation_plan_arg(lua_State *L,
                                            lua_lql_client *client,
                                            int expr_index, int options_index,
                                            const char *base_dir,
                                            lql_mutation_plan **out,
                                            int *out_owned,
                                            lql_error *error) {
  lua_lql_mutation_plan_handle *handle;

  *out = NULL;
  *out_owned = 0;
  handle = lua_lql_test_mutation_plan(L, expr_index);
  if (handle != NULL) {
    if (handle->ctx != client->ctx || handle->plan == NULL) {
      lua_lql_set_error(error, LQL_STATUS_INVALID_ARGUMENT,
                        "mutation plan belongs to another lql client");
      return LQL_STATUS_INVALID_ARGUMENT;
    }
    *out = handle->plan;
    return LQL_STATUS_OK;
  }

  *out_owned = 1;
  return lua_lql_parse_mutation_plan(L, client->ctx, expr_index, options_index,
                                     base_dir, out, error);
}

static int lua_lql_projection_parse(lua_State *L) {
  lua_lql_client *client;
  lua_lql_projection_handle *handle;
  const char **fields;
  size_t field_count;
  lql_error error;
  lql_status st;

  client = lua_lql_check_client(L, 1);
  fields = NULL;
  field_count = 0u;
  if (!lua_lql_fields(L, 2, &fields, &field_count)) {
    return luaL_error(L, "out of memory");
  }
  handle = (lua_lql_projection_handle *)lua_newuserdatauv(L, sizeof(*handle), 1);
  handle->ctx = client->ctx;
  handle->projection = NULL;
  luaL_getmetatable(L, LUA_LQL_PROJECTION);
  lua_setmetatable(L, -2);
  lua_pushvalue(L, 1);
  lua_setiuservalue(L, -2, 1);
  lql_error_init(&error);
  st = client->ctx->projection_parse(client->ctx,
                                     (const char *const *)fields, field_count,
                                     &handle->projection, &error);
  if (fields != NULL) {
    (void)lua_lql_alloc(L, (void *)fields, sizeof(fields[0]) * field_count,
                        0u);
  }
  if (st != LQL_STATUS_OK) {
    lua_pop(L, 1);
    return lua_lql_fail(L, &error);
  }
  return 1;
}

static int lua_lql_mutation_plan_parse(lua_State *L) {
  lua_lql_client *client;
  lua_lql_mutation_plan_handle *handle;
  lql_error error;
  lql_status st;

  client = lua_lql_check_client(L, 1);
  handle =
      (lua_lql_mutation_plan_handle *)lua_newuserdatauv(L, sizeof(*handle), 1);
  handle->ctx = client->ctx;
  handle->plan = NULL;
  luaL_getmetatable(L, LUA_LQL_MUTATION_PLAN);
  lua_setmetatable(L, -2);
  lua_pushvalue(L, 1);
  lua_setiuservalue(L, -2, 1);
  lql_error_init(&error);
  st = lua_lql_parse_mutation_plan(
      L, client->ctx, 2, 3, lua_lql_options_string(L, 3, "file_value_base_dir"),
      &handle->plan, &error);
  if (st != LQL_STATUS_OK) {
    lua_pop(L, 1);
    return lua_lql_fail(L, &error);
  }
  return 1;
}

static int lua_lql_mutation_plan_count(lua_State *L) {
  lua_lql_client *client;
  lql_mutation_plan *plan;
  lql_error error;
  lql_status st;
  int plan_owned;
  size_t count;

  client = lua_lql_check_client(L, 1);
  plan = NULL;
  plan_owned = 0;
  lql_error_init(&error);
  st = lua_lql_mutation_plan_arg(
      L, client, 2, 3, lua_lql_options_string(L, 3, "file_value_base_dir"),
      &plan, &plan_owned, &error);
  if (st == LQL_STATUS_OK) {
    count = client->ctx->mutation_plan_count(client->ctx, plan);
  } else {
    count = 0u;
  }
  if (plan_owned) {
    client->ctx->mutation_plan_destroy(client->ctx, plan);
  }
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  lua_pushinteger(L, (lua_Integer)count);
  return 1;
}

static int lua_lql_compact_json(lua_State *L) {
  lua_lql_client *client;
  const char *json;
  size_t json_len;
  lql_error error;
  lql_status st;
  FILE *out;
  lua_lql_buffer buffer;

  client = lua_lql_check_client(L, 1);
  json = luaL_checklstring(L, 2, &json_len);
  out = NULL;
  lua_lql_buffer_init(&buffer, L);
  lql_error_init(&error);
  out = tmpfile();
  if (out == NULL) {
    lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                      "failed to create Lua output file");
    st = LQL_STATUS_JSON_ERROR;
  } else {
    st = client->ctx->compact_json(client->ctx, json, json_len, out, &error);
    if (st == LQL_STATUS_OK) {
      st = lua_lql_file_to_buffer(out, &buffer);
    }
    fclose(out);
  }
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static int lua_lql_compact_file(lua_State *L) {
  lua_lql_client *client;
  const char *path;
  lql_error error;
  lql_status st;
  FILE *input;
  FILE *out;
  long size;
  lua_lql_buffer buffer;

  client = lua_lql_check_client(L, 1);
  path = luaL_checkstring(L, 2);
  input = NULL;
  out = NULL;
  size = 0L;
  lua_lql_buffer_init(&buffer, L);
  lql_error_init(&error);
  input = fopen(path, "rb");
  if (input == NULL) {
    lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                      "failed to open Lua input file");
    st = LQL_STATUS_JSON_ERROR;
  } else if (fseek(input, 0L, SEEK_END) != 0 || (size = ftell(input)) < 0L ||
             fseek(input, 0L, SEEK_SET) != 0) {
    lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                      "failed to inspect Lua input file");
    st = LQL_STATUS_JSON_ERROR;
  } else {
    out = tmpfile();
    if (out == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to create Lua output file");
      st = LQL_STATUS_JSON_ERROR;
    } else {
      st = client->ctx->compact_file_range(client->ctx, input, 0u,
                                           (lql_uint64)size, out, &error);
      if (st == LQL_STATUS_OK) {
        st = lua_lql_file_to_buffer(out, &buffer);
      }
      fclose(out);
    }
  }
  if (input != NULL) {
    fclose(input);
  }
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static int lua_lql_compact_source(lua_State *L) {
  lua_lql_client *client;
  lql_error error;
  lql_status st;
  FILE *out;
  lua_lql_buffer buffer;
  lua_lql_source_state source_state;

  client = lua_lql_check_client(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  out = NULL;
  lua_lql_buffer_init(&buffer, L);
  memset(&source_state, 0, sizeof(source_state));
  lql_error_init(&error);
  out = tmpfile();
  if (out == NULL) {
    lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                      "failed to create Lua output file");
    st = LQL_STATUS_JSON_ERROR;
  } else {
    lua_pushvalue(L, 2);
    source_state.read_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    source_state.lua = L;
    source_state.error = &error;
    st = client->ctx->compact_source(client->ctx, lua_lql_read_source_chunk,
                                     &source_state, out, &error);
    luaL_unref(L, LUA_REGISTRYINDEX, source_state.read_ref);
    if (source_state.read_failed) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        source_state.read_message);
    }
    if (st == LQL_STATUS_OK) {
      st = lua_lql_file_to_buffer(out, &buffer);
    }
    fclose(out);
  }
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static int lua_lql_matches_json(lua_State *L) {
  lua_lql_client *client;
  const char *json;
  size_t json_len;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;
  int selector_owned;

  client = lua_lql_check_client(L, 1);
  json = luaL_checklstring(L, 3, &json_len);
  selector = NULL;
  selector_owned = 0;
  matched = 0;
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st == LQL_STATUS_OK) {
    st = client->ctx->matches_json(client->ctx, selector, json, json_len,
                                   &matched, &error);
  }
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  if (st != LQL_STATUS_OK) {
    return lua_lql_fail(L, &error);
  }
  lua_pushboolean(L, matched);
  return 1;
}

static int lua_lql_select_json(lua_State *L) {
  lua_lql_client *client;
  const char *json;
  size_t json_len;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  int matched;
  int compact;
  int selector_owned;
  FILE *out;
  lua_lql_buffer buffer;

  client = lua_lql_check_client(L, 1);
  json = luaL_checklstring(L, 3, &json_len);
  compact = lua_lql_options_bool(L, 4, "compact");
  selector = NULL;
  selector_owned = 0;
  matched = 0;
  lua_lql_buffer_init(&buffer, L);
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
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
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
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
  const char *json;
  size_t json_len;
  lql_selector *selector;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  int matched;
  int found;
  int selector_owned;
  int projection_owned;
  FILE *out;
  lua_lql_buffer buffer;

  client = lua_lql_check_client(L, 1);
  json = luaL_checklstring(L, 3, &json_len);
  selector = NULL;
  selector_owned = 0;
  projection = NULL;
  projection_owned = 0;
  matched = 0;
  found = 0;
  lua_lql_buffer_init(&buffer, L);
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st == LQL_STATUS_OK) {
    st = lua_lql_projection_arg(L, client, 4, &projection, &projection_owned,
                                &error);
  }
  if (st == LQL_STATUS_OK) {
    st = client->ctx->matches_json(client->ctx, selector, json, json_len,
                                   &matched, &error);
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
  if (projection_owned) {
    client->ctx->projection_destroy(client->ctx, projection);
  }
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
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
  const char *json;
  size_t json_len;
  lql_selector *selector;
  lql_mutation_plan *plan;
  lql_error error;
  lql_status st;
  int matched;
  int matches_only;
  int selector_owned;
  int plan_owned;
  FILE *out;
  lua_lql_buffer buffer;

  client = lua_lql_check_client(L, 1);
  json = luaL_checklstring(L, 3, &json_len);
  matches_only = lua_lql_options_bool(L, 5, "matches_only");
  selector = NULL;
  selector_owned = 0;
  plan = NULL;
  plan_owned = 0;
  matched = 0;
  lua_lql_buffer_init(&buffer, L);
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st == LQL_STATUS_OK) {
    st = client->ctx->matches_json(client->ctx, selector, json, json_len,
                                   &matched, &error);
  }
  if (st == LQL_STATUS_OK) {
    st = lua_lql_mutation_plan_arg(
        L, client, 4, 5, lua_lql_options_string(L, 5, "file_value_base_dir"),
        &plan, &plan_owned, &error);
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
  if (plan_owned) {
    client->ctx->mutation_plan_destroy(client->ctx, plan);
  }
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
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
  const char *path;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  FILE *input;
  FILE *out;
  lua_lql_buffer buffer;
  lua_lql_file_state state;
  int selector_owned;

  client = lua_lql_check_client(L, 1);
  path = luaL_checkstring(L, 3);
  selector = NULL;
  selector_owned = 0;
  input = NULL;
  out = NULL;
  lua_lql_buffer_init(&buffer, L);
  memset(&state, 0, sizeof(state));
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
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
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static int lua_lql_select_source(lua_State *L) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  FILE *out;
  lua_lql_buffer buffer;
  lua_lql_file_state state;
  lua_lql_source_state source_state;
  lql_query_result result;
  int selector_owned;

  client = lua_lql_check_client(L, 1);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  selector = NULL;
  selector_owned = 0;
  out = NULL;
  lua_lql_buffer_init(&buffer, L);
  memset(&state, 0, sizeof(state));
  memset(&source_state, 0, sizeof(source_state));
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st == LQL_STATUS_OK) {
    out = tmpfile();
    if (out == NULL) {
      lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR,
                        "failed to create Lua output file");
      st = LQL_STATUS_JSON_ERROR;
    }
  }
  if (st == LQL_STATUS_OK) {
    lua_pushvalue(L, 3);
    source_state.read_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    source_state.lua = L;
    source_state.error = &error;
    state.ctx = client->ctx;
    state.out = out;
    state.error = &error;
    st = client->ctx->query_source_spooled_matches(
        client->ctx, selector, lua_lql_read_source_chunk, &source_state,
        lua_lql_on_select_payload, &state, &result, &error);
    luaL_unref(L, LUA_REGISTRYINDEX, source_state.read_ref);
  }
  if (source_state.read_failed) {
    lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR, source_state.read_message);
  }
  if (st == LQL_STATUS_OK) {
    st = lua_lql_file_to_buffer(out, &buffer);
  }
  if (out != NULL) {
    fclose(out);
  }
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
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
  const char *path;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  FILE *input;
  lql_query_options options;
  lql_query_result result;
  lua_lql_file_state state;
  int selector_owned;

  client = lua_lql_check_client(L, 1);
  path = luaL_checkstring(L, 3);
  luaL_checktype(L, 4, LUA_TFUNCTION);
  selector = NULL;
  selector_owned = 0;
  input = NULL;
  memset(&state, 0, sizeof(state));
  memset(&result, 0, sizeof(result));
  lua_lql_options_query(L, 5, &options);
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
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
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  if (st != LQL_STATUS_OK && st != LQL_STATUS_STOP) {
    return lua_lql_fail(L, &error);
  }
  lua_lql_push_query_result(L, &result);
  return 1;
}

static int lua_lql_query_source(lua_State *L) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  lql_query_options options;
  lql_query_result result;
  lua_lql_file_state callback_state;
  lua_lql_source_state source_state;
  int selector_owned;

  client = lua_lql_check_client(L, 1);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  luaL_checktype(L, 4, LUA_TFUNCTION);
  selector = NULL;
  selector_owned = 0;
  memset(&callback_state, 0, sizeof(callback_state));
  memset(&source_state, 0, sizeof(source_state));
  memset(&result, 0, sizeof(result));
  lua_lql_options_query(L, 5, &options);
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st == LQL_STATUS_OK) {
    lua_pushvalue(L, 3);
    source_state.read_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    source_state.lua = L;
    source_state.error = &error;
    lua_pushvalue(L, 4);
    callback_state.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    callback_state.ctx = client->ctx;
    callback_state.lua = L;
    callback_state.error = &error;
    st = client->ctx->query_source_decisions_with_options(
        client->ctx, selector, lua_lql_read_source_chunk, &source_state,
        &options, lua_lql_on_query_file_decision, &callback_state, &result,
        &error);
    luaL_unref(L, LUA_REGISTRYINDEX, callback_state.callback_ref);
    luaL_unref(L, LUA_REGISTRYINDEX, source_state.read_ref);
  }
  if (source_state.read_failed) {
    lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR, source_state.read_message);
  }
  if (callback_state.callback_failed) {
    lua_lql_set_error(&error, LQL_STATUS_INVALID_ARGUMENT,
                      callback_state.callback_message);
  }
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  if (st != LQL_STATUS_OK && st != LQL_STATUS_STOP) {
    return lua_lql_fail(L, &error);
  }
  lua_lql_push_query_result(L, &result);
  return 1;
}

static int lua_lql_each_match_file(lua_State *L) {
  lua_lql_client *client;
  const char *path;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  FILE *input;
  lql_query_options options;
  lql_query_result result;
  lua_lql_file_state state;
  int selector_owned;

  client = lua_lql_check_client(L, 1);
  path = luaL_checkstring(L, 3);
  luaL_checktype(L, 4, LUA_TFUNCTION);
  selector = NULL;
  selector_owned = 0;
  input = NULL;
  memset(&state, 0, sizeof(state));
  memset(&result, 0, sizeof(result));
  lua_lql_options_query(L, 5, &options);
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
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
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  if (st != LQL_STATUS_OK && st != LQL_STATUS_STOP) {
    return lua_lql_fail(L, &error);
  }
  lua_lql_push_query_result(L, &result);
  return 1;
}

static int lua_lql_each_match_source(lua_State *L) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_error error;
  lql_status st;
  lql_query_options options;
  lql_query_result result;
  lua_lql_file_state callback_state;
  lua_lql_source_state source_state;
  int selector_owned;

  client = lua_lql_check_client(L, 1);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  luaL_checktype(L, 4, LUA_TFUNCTION);
  selector = NULL;
  selector_owned = 0;
  memset(&callback_state, 0, sizeof(callback_state));
  memset(&source_state, 0, sizeof(source_state));
  memset(&result, 0, sizeof(result));
  lua_lql_options_query(L, 5, &options);
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st == LQL_STATUS_OK) {
    lua_pushvalue(L, 3);
    source_state.read_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    source_state.lua = L;
    source_state.error = &error;
    lua_pushvalue(L, 4);
    callback_state.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    callback_state.ctx = client->ctx;
    callback_state.lua = L;
    callback_state.error = &error;
    st = client->ctx->query_source_spooled_matches_with_options(
        client->ctx, selector, lua_lql_read_source_chunk, &source_state,
        &options, lua_lql_on_each_match_file, &callback_state, &result, &error);
    luaL_unref(L, LUA_REGISTRYINDEX, callback_state.callback_ref);
    luaL_unref(L, LUA_REGISTRYINDEX, source_state.read_ref);
  }
  if (source_state.read_failed) {
    lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR, source_state.read_message);
  }
  if (callback_state.callback_failed) {
    lua_lql_set_error(&error, LQL_STATUS_INVALID_ARGUMENT,
                      callback_state.callback_message);
  }
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  if (st != LQL_STATUS_OK && st != LQL_STATUS_STOP) {
    return lua_lql_fail(L, &error);
  }
  lua_lql_push_query_result(L, &result);
  return 1;
}

static int lua_lql_project_file(lua_State *L) {
  lua_lql_client *client;
  const char *path;
  lql_selector *selector;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  FILE *input;
  FILE *out;
  lua_lql_buffer buffer;
  lua_lql_file_state state;
  int selector_owned;
  int projection_owned;

  client = lua_lql_check_client(L, 1);
  path = luaL_checkstring(L, 3);
  selector = NULL;
  selector_owned = 0;
  projection = NULL;
  projection_owned = 0;
  input = NULL;
  out = NULL;
  lua_lql_buffer_init(&buffer, L);
  memset(&state, 0, sizeof(state));
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st == LQL_STATUS_OK) {
    st = lua_lql_projection_arg(L, client, 4, &projection, &projection_owned,
                                &error);
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
  if (projection_owned) {
    client->ctx->projection_destroy(client->ctx, projection);
  }
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static int lua_lql_project_source(lua_State *L) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_projection *projection;
  lql_error error;
  lql_status st;
  FILE *out;
  lua_lql_buffer buffer;
  lua_lql_file_state state;
  lua_lql_source_state source_state;
  lql_query_result result;
  int selector_owned;
  int projection_owned;

  client = lua_lql_check_client(L, 1);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  selector = NULL;
  selector_owned = 0;
  projection = NULL;
  projection_owned = 0;
  out = NULL;
  lua_lql_buffer_init(&buffer, L);
  memset(&state, 0, sizeof(state));
  memset(&source_state, 0, sizeof(source_state));
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st == LQL_STATUS_OK) {
    st = lua_lql_projection_arg(L, client, 4, &projection, &projection_owned,
                                &error);
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
    lua_pushvalue(L, 3);
    source_state.read_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    source_state.lua = L;
    source_state.error = &error;
    state.ctx = client->ctx;
    state.out = out;
    state.projection = projection;
    state.error = &error;
    st = client->ctx->query_source_spooled_matches(
        client->ctx, selector, lua_lql_read_source_chunk, &source_state,
        lua_lql_on_project_payload, &state, &result, &error);
    luaL_unref(L, LUA_REGISTRYINDEX, source_state.read_ref);
  }
  if (source_state.read_failed) {
    lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR, source_state.read_message);
  }
  if (st == LQL_STATUS_OK) {
    st = lua_lql_file_to_buffer(out, &buffer);
  }
  if (out != NULL) {
    fclose(out);
  }
  if (projection_owned) {
    client->ctx->projection_destroy(client->ctx, projection);
  }
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
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
  const char *path;
  lql_selector *selector;
  lql_mutation_plan *plan;
  lql_error error;
  lql_status st;
  lql_query_options options;
  lql_query_result result;
  FILE *input;
  FILE *out;
  lua_lql_buffer buffer;
  lql_uint64 input_size;
  int selector_owned;
  int plan_owned;

  client = lua_lql_check_client(L, 1);
  path = luaL_checkstring(L, 3);
  selector = NULL;
  selector_owned = 0;
  plan = NULL;
  plan_owned = 0;
  input = NULL;
  out = NULL;
  input_size = 0u;
  lua_lql_options_query(L, 5, &options);
  lua_lql_buffer_init(&buffer, L);
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st == LQL_STATUS_OK) {
    st = lua_lql_mutation_plan_arg(
        L, client, 4, 5, lua_lql_options_string(L, 5, "file_value_base_dir"),
        &plan, &plan_owned, &error);
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
    st = lua_lql_file_size(input, &input_size, &error);
  }
  if (st == LQL_STATUS_OK) {
    st = client->ctx->mutate_file_range_candidates_with_options(
        client->ctx, selector, plan, input, 0u, input_size, out,
        lua_lql_options_bool(L, 5, "compact"),
        lua_lql_options_bool(L, 5, "matches_only"), &options, &result, &error);
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
  if (plan_owned) {
    client->ctx->mutation_plan_destroy(client->ctx, plan);
  }
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static int lua_lql_mutate_source(lua_State *L) {
  lua_lql_client *client;
  lql_selector *selector;
  lql_mutation_plan *plan;
  lql_error error;
  lql_status st;
  lql_query_options options;
  FILE *out;
  lua_lql_buffer buffer;
  lua_lql_source_state source_state;
  lql_query_result result;
  int selector_owned;
  int plan_owned;

  client = lua_lql_check_client(L, 1);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  selector = NULL;
  selector_owned = 0;
  plan = NULL;
  plan_owned = 0;
  out = NULL;
  lua_lql_options_query(L, 5, &options);
  lua_lql_buffer_init(&buffer, L);
  memset(&source_state, 0, sizeof(source_state));
  memset(&result, 0, sizeof(result));
  lql_error_init(&error);
  st = lua_lql_selector_arg(L, client, 2, &selector, &selector_owned, &error);
  if (st == LQL_STATUS_OK) {
    st = lua_lql_mutation_plan_arg(
        L, client, 4, 5, lua_lql_options_string(L, 5, "file_value_base_dir"),
        &plan, &plan_owned, &error);
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
    lua_pushvalue(L, 3);
    source_state.read_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    source_state.lua = L;
    source_state.error = &error;
    st = client->ctx->mutate_source_candidates_with_options(
        client->ctx, selector, plan, lua_lql_read_source_chunk, &source_state,
        out, lua_lql_options_bool(L, 5, "compact"),
        lua_lql_options_bool(L, 5, "matches_only"), &options, &result, &error);
    luaL_unref(L, LUA_REGISTRYINDEX, source_state.read_ref);
  }
  if (source_state.read_failed) {
    lua_lql_set_error(&error, LQL_STATUS_JSON_ERROR, source_state.read_message);
  }
  if (st == LQL_STATUS_OK) {
    st = lua_lql_file_to_buffer(out, &buffer);
  }
  if (out != NULL) {
    fclose(out);
  }
  if (plan_owned) {
    client->ctx->mutation_plan_destroy(client->ctx, plan);
  }
  if (selector_owned) {
    client->ctx->selector_destroy(client->ctx, selector);
  }
  if (st != LQL_STATUS_OK) {
    lua_lql_buffer_dispose(&buffer);
    return lua_lql_fail(L, &error);
  }
  lua_pushlstring(L, buffer.data != NULL ? buffer.data : "", buffer.len);
  lua_lql_buffer_dispose(&buffer);
  return 1;
}

static const luaL_Reg lua_lql_client_methods[] = {
    {"version", lua_client_version},
    {"capabilities", lua_client_capabilities_get},
    {"selector_parse", lua_lql_selector_parse},
    {"selector_parse_or", lua_lql_selector_parse_or},
    {"selector_parse_json", lua_lql_selector_parse_json},
    {"selector_json", lua_lql_selector_json},
    {"selector_root", lua_lql_selector_root},
    {"selector_capabilities", lua_lql_selector_capabilities_get},
    {"selector_execution_traits", lua_lql_selector_execution_traits_get},
    {"selector_all", lua_lql_selector_build_all},
    {"selector_compound", lua_lql_selector_build_compound},
    {"selector_not", lua_lql_selector_build_not},
    {"selector_string", lua_lql_selector_build_string},
    {"selector_range", lua_lql_selector_build_range},
    {"selector_date", lua_lql_selector_build_date},
    {"selector_in", lua_lql_selector_build_in},
    {"selector_exists", lua_lql_selector_build_exists},
    {"projection_parse", lua_lql_projection_parse},
    {"mutation_plan_parse", lua_lql_mutation_plan_parse},
    {"mutation_plan_count", lua_lql_mutation_plan_count},
    {"matches_json", lua_lql_matches_json},
    {"compact_json", lua_lql_compact_json},
    {"compact_file", lua_lql_compact_file},
    {"compact_source", lua_lql_compact_source},
    {"select_json", lua_lql_select_json},
    {"select_file", lua_lql_select_file},
    {"select_source", lua_lql_select_source},
    {"query_file", lua_lql_query_file},
    {"query_source", lua_lql_query_source},
    {"each_match_file", lua_lql_each_match_file},
    {"each_match_source", lua_lql_each_match_source},
    {"project_json", lua_lql_project_json},
    {"project_file", lua_lql_project_file},
    {"project_source", lua_lql_project_source},
    {"mutate_json", lua_lql_mutate_json},
    {"mutate_file", lua_lql_mutate_file},
    {"mutate_source", lua_lql_mutate_source},
    {NULL, NULL}};

static const luaL_Reg lua_lql_client_meta[] = {{"__gc", lua_lql_client_gc},
                                               {NULL, NULL}};

static const luaL_Reg lua_lql_selector_meta[] = {{"__gc", lua_lql_selector_gc},
                                                 {NULL, NULL}};

static const luaL_Reg lua_lql_selector_methods[] = {
    {"root", lua_lql_selector_method_root},
    {"json", lua_lql_selector_method_json},
    {"is_empty", lua_lql_selector_method_is_empty},
    {"capabilities", lua_lql_selector_method_capabilities},
    {"execution_traits", lua_lql_selector_method_execution_traits},
    {NULL, NULL}};

static const luaL_Reg lua_lql_projection_meta[] = {
    {"__gc", lua_lql_projection_gc}, {NULL, NULL}};

static const luaL_Reg lua_lql_mutation_plan_meta[] = {
    {"__gc", lua_lql_mutation_plan_gc}, {NULL, NULL}};

static const luaL_Reg lua_lql_functions[] = {{"new", lua_lql_new_client},
                                             {NULL, NULL}};

int luaopen_lql_core(lua_State *L) {
  luaL_newmetatable(L, LUA_LQL_CLIENT);
  luaL_setfuncs(L, lua_lql_client_meta, 0);
  lua_newtable(L);
  luaL_setfuncs(L, lua_lql_client_methods, 0);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  luaL_newmetatable(L, LUA_LQL_SELECTOR);
  luaL_setfuncs(L, lua_lql_selector_meta, 0);
  lua_newtable(L);
  luaL_setfuncs(L, lua_lql_selector_methods, 0);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  luaL_newmetatable(L, LUA_LQL_PROJECTION);
  luaL_setfuncs(L, lua_lql_projection_meta, 0);
  lua_pop(L, 1);

  luaL_newmetatable(L, LUA_LQL_MUTATION_PLAN);
  luaL_setfuncs(L, lua_lql_mutation_plan_meta, 0);
  lua_pop(L, 1);

  luaL_newlib(L, lua_lql_functions);
  return 1;
}
