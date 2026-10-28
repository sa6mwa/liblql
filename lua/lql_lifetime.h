/* Private facade ownership. Native work must return normally before Lua errors
 * propagate: a protected method owns temporary plans, roots and scratch memory.
 * Finalizers detach userdata immediately, but native objects used by an active
 * method remain alive until the outermost method returns. */
typedef enum lua_lql_resource_kind {
  LUA_LQL_OWN_SELECTOR,
  LUA_LQL_OWN_PROJECTION,
  LUA_LQL_OWN_MUTATION
} lua_lql_resource_kind;

struct lua_lql_owner {
  lql *ctx;
  size_t refs;
  size_t active;
  int closed;
  lua_lql_resource *deferred;
};

struct lua_lql_resource {
  lua_lql_owner *owner;
  void *ptr;
  lua_lql_resource_kind kind;
  int closed;
  lua_lql_mutation_bridge *bridge;
  lua_lql_resource *next;
};

typedef struct lua_lql_scratch {
  void *ptr;
  size_t size;
  struct lua_lql_scratch *next;
} lua_lql_scratch;

typedef struct lua_lql_root {
  int ref;
  struct lua_lql_root *next;
} lua_lql_root;

typedef struct lua_lql_scope {
  struct lua_lql_scope *previous;
  lua_lql_owner *owner;
  lua_lql_resource *resources;
  lua_lql_resource *borrowed[4];
  size_t borrowed_count;
  lua_lql_scratch *scratch;
  lua_lql_root *roots;
} lua_lql_scope;

static char lua_lql_scope_key;

static lua_lql_scope *lua_lql_current_scope(lua_State *lua) {
  lua_lql_scope *scope;
  lua_rawgetp(lua, LUA_REGISTRYINDEX, &lua_lql_scope_key);
  scope = (lua_lql_scope *)lua_touserdata(lua, -1);
  lua_pop(lua, 1);
  return scope;
}

static void lua_lql_set_scope(lua_State *lua, lua_lql_scope *scope) {
  lua_pushlightuserdata(lua, scope);
  lua_rawsetp(lua, LUA_REGISTRYINDEX, &lua_lql_scope_key);
}

static void lua_lql_owner_release(lua_lql_owner *owner) {
  if (--owner->refs == 0u) {
    owner->ctx->destroy(owner->ctx);
    free(owner);
  }
}

static void lua_lql_native_destroy(lua_lql_owner *owner, void *ptr,
                                   lua_lql_resource_kind kind) {
  if (kind == LUA_LQL_OWN_SELECTOR) {
    owner->ctx->selector_destroy(owner->ctx, (lql_selector *)ptr);
  } else if (kind == LUA_LQL_OWN_PROJECTION) {
    owner->ctx->projection_destroy(owner->ctx, (lql_projection *)ptr);
  } else {
    owner->ctx->mutation_destroy(owner->ctx, (lql_mutation *)ptr);
  }
}

static void lua_lql_resource_dispose(lua_lql_resource *resource) {
  lua_lql_owner *owner;
  owner = resource->owner;
  lua_lql_native_destroy(owner, resource->ptr, resource->kind);
  lua_lql_mutation_bridge_destroy(resource->bridge);
  free(resource);
  lua_lql_owner_release(owner);
}

static void lua_lql_resource_close(lua_lql_resource *resource) {
  if (resource == NULL || resource->closed) {
    return;
  }
  resource->closed = 1;
  if (resource->owner->active != 0u) {
    resource->next = resource->owner->deferred;
    resource->owner->deferred = resource;
  } else {
    lua_lql_resource_dispose(resource);
  }
}

static int lua_lql_scope_failed(lua_State *lua) {
  lua_lql_scope *scope;
  size_t i;
  scope = lua_lql_current_scope(lua);
  if (scope == NULL) {
    return 0;
  }
  if (scope->owner != NULL && scope->owner->closed) {
    return 1;
  }
  for (i = 0u; i < scope->borrowed_count; ++i) {
    if (scope->borrowed[i]->closed) {
      return 1;
    }
  }
  return 0;
}

static void lua_lql_borrow(lua_State *lua, lua_lql_resource *resource) {
  lua_lql_scope *scope;
  size_t i;
  scope = lua_lql_current_scope(lua);
  if (scope == NULL || resource == NULL) {
    return;
  }
  for (i = 0u; i < scope->borrowed_count; ++i) {
    if (scope->borrowed[i] == resource) {
      return;
    }
  }
  /* Receiver plus selector/projection/mutation request arguments. */
  if (scope->borrowed_count >= 4u) {
    (void)luaL_error(lua, "too many borrowed lql handles");
    return;
  }
  scope->borrowed[scope->borrowed_count++] = resource;
}

static lql_status lua_lql_track_native(lua_State *lua, void *ptr,
                                       lua_lql_resource_kind kind,
                                       lql_error *error) {
  lua_lql_scope *scope;
  lua_lql_resource *resource;
  if (ptr == NULL) {
    return LQL_STATUS_OK;
  }
  scope = lua_lql_current_scope(lua);
  resource = (lua_lql_resource *)malloc(sizeof(*resource));
  if (resource == NULL) {
    lua_lql_native_destroy(scope->owner, ptr, kind);
    error->code = LQL_STATUS_NO_MEMORY;
    strcpy(error->message, "out of memory");
    return error->code;
  }
  memset(resource, 0, sizeof(*resource));
  resource->owner = scope->owner;
  resource->owner->refs++;
  resource->ptr = ptr;
  resource->kind = kind;
  resource->next = scope->resources;
  scope->resources = resource;
  return LQL_STATUS_OK;
}

static lua_lql_resource *lua_lql_take_native(lua_State *lua, void *ptr) {
  lua_lql_scope *scope;
  lua_lql_resource **slot;
  lua_lql_resource *resource;
  scope = lua_lql_current_scope(lua);
  slot = &scope->resources;
  while (*slot != NULL && (*slot)->ptr != ptr) {
    slot = &(*slot)->next;
  }
  resource = *slot;
  if (resource == NULL) {
    (void)luaL_error(lua, "unowned lql handle");
    return NULL;
  }
  *slot = resource->next;
  resource->next = NULL;
  return resource;
}

static void lua_lql_destroy_temporary(lua_State *lua, void *ptr) {
  if (ptr != NULL) {
    lua_lql_resource_dispose(lua_lql_take_native(lua, ptr));
  }
}

static void lua_lql_forget_scratch(lua_State *lua, void *ptr) {
  lua_lql_scope *scope;
  lua_lql_scratch **slot;
  lua_lql_scratch *allocation;
  scope = lua_lql_current_scope(lua);
  if (scope == NULL) {
    return;
  }
  slot = &scope->scratch;
  while (*slot != NULL && (*slot)->ptr != ptr) {
    slot = &(*slot)->next;
  }
  if (*slot != NULL) {
    allocation = *slot;
    *slot = allocation->next;
    free(allocation);
  }
}

static void *lua_lql_scratch_resize(lua_State *lua, void *ptr, size_t old_size,
                                    size_t new_size) {
  lua_lql_scope *scope;
  lua_lql_scratch *allocation;
  lua_Alloc allocator;
  void *user;
  void *next;
  scope = lua_lql_current_scope(lua);
  allocation = scope != NULL ? scope->scratch : NULL;
  while (allocation != NULL && allocation->ptr != ptr) {
    allocation = allocation->next;
  }
  if (new_size == 0u) {
    lua_lql_forget_scratch(lua, ptr);
  } else if (ptr == NULL && scope != NULL) {
    allocation = (lua_lql_scratch *)malloc(sizeof(*allocation));
    if (allocation == NULL) {
      return NULL;
    }
    allocation->ptr = NULL;
    allocation->size = 0u;
    allocation->next = scope->scratch;
    scope->scratch = allocation;
  }
  allocator = lua_getallocf(lua, &user);
  next = allocator(user, ptr, old_size, new_size);
  if (next != NULL && new_size != 0u && allocation != NULL) {
    allocation->ptr = next;
    allocation->size = new_size;
  } else if (ptr == NULL && allocation != NULL) {
    lua_lql_forget_scratch(lua, NULL);
  }
  return next;
}

static void lua_lql_forget_ref(lua_State *lua, int ref) {
  lua_lql_scope *scope;
  lua_lql_root **slot;
  lua_lql_root *root;
  if (ref == LUA_NOREF || ref == LUA_REFNIL) {
    return;
  }
  scope = lua_lql_current_scope(lua);
  if (scope == NULL) {
    return;
  }
  slot = &scope->roots;
  while (*slot != NULL && (*slot)->ref != ref) {
    slot = &(*slot)->next;
  }
  if (*slot != NULL) {
    root = *slot;
    *slot = root->next;
    free(root);
  }
}

static int lua_lql_ref(lua_State *lua, int table) {
  lua_lql_scope *scope;
  lua_lql_root *root;
  int ref;
  scope = lua_lql_current_scope(lua);
  root = NULL;
  if (scope != NULL) {
    root = (lua_lql_root *)malloc(sizeof(*root));
    if (root == NULL) {
      return luaL_error(lua, "out of memory retaining lql callback");
    }
    root->ref = LUA_NOREF;
    root->next = scope->roots;
    scope->roots = root;
  }
  ref = luaL_ref(lua, table);
  if (root != NULL) {
    root->ref = ref;
  }
  return ref;
}

static void lua_lql_unref(lua_State *lua, int table, int ref) {
  lua_lql_forget_ref(lua, ref);
  luaL_unref(lua, table, ref);
}

static void lua_lql_scope_cleanup(lua_State *lua, lua_lql_scope *scope) {
  lua_lql_resource *resource;
  lua_lql_scratch *allocation;
  lua_lql_root *root;
  lua_Alloc allocator;
  void *user;
  while (scope->resources != NULL) {
    resource = scope->resources;
    scope->resources = resource->next;
    lua_lql_resource_dispose(resource);
  }
  if (scope->owner != NULL && --scope->owner->active == 0u) {
    while (scope->owner->deferred != NULL) {
      resource = scope->owner->deferred;
      scope->owner->deferred = resource->next;
      lua_lql_resource_dispose(resource);
    }
  }
  while (scope->roots != NULL) {
    root = scope->roots;
    scope->roots = root->next;
    luaL_unref(lua, LUA_REGISTRYINDEX, root->ref);
    free(root);
  }
  allocator = lua_getallocf(lua, &user);
  while (scope->scratch != NULL) {
    allocation = scope->scratch;
    scope->scratch = allocation->next;
    (void)allocator(user, allocation->ptr, allocation->size, 0u);
    free(allocation);
  }
  if (scope->owner != NULL) {
    lua_lql_owner_release(scope->owner);
  }
}

static int lua_lql_guard_method(lua_State *lua) {
  lua_lql_scope scope;
  lua_lql_client *client;
  lua_lql_selector *selector;
  lua_lql_selector_node *node;
  lua_lql_projection *projection;
  lua_lql_mutation *mutation;
  lua_lql_resource *receiver;
  int count;
  int status;
  int closed;
  count = lua_gettop(lua);
  if (!lua_checkstack(lua, 16)) {
    return luaL_error(lua, "out of memory entering lql method");
  }
  memset(&scope, 0, sizeof(scope));
  receiver = NULL;
  client = (lua_lql_client *)luaL_testudata(lua, 1, LUA_LQL_CLIENT);
  selector = (lua_lql_selector *)luaL_testudata(lua, 1, LUA_LQL_SELECTOR);
  node = (lua_lql_selector_node *)luaL_testudata(lua, 1, LUA_LQL_SELECTOR_NODE);
  projection = (lua_lql_projection *)luaL_testudata(lua, 1, LUA_LQL_PROJECTION);
  mutation = (lua_lql_mutation *)luaL_testudata(lua, 1, LUA_LQL_MUTATION);
  if (client != NULL) {
    scope.owner = client->owner;
  } else if (selector != NULL) {
    receiver = selector->resource;
  } else if (node != NULL && node->owner != NULL) {
    receiver = node->owner->resource;
  } else if (projection != NULL) {
    receiver = projection->resource;
  } else if (mutation != NULL) {
    receiver = mutation->resource;
  }
  if (receiver != NULL) {
    scope.owner = receiver->owner;
    scope.borrowed[scope.borrowed_count++] = receiver;
  }
  if (scope.owner != NULL) {
    scope.owner->refs++;
    scope.owner->active++;
  }
  scope.previous = lua_lql_current_scope(lua);
  lua_lql_set_scope(lua, &scope);
  lua_pushvalue(lua, lua_upvalueindex(1));
  lua_insert(lua, 1);
  status = lua_pcall(lua, count, LUA_MULTRET, 0);
  closed = lua_lql_scope_failed(lua);
  lua_lql_scope_cleanup(lua, &scope);
  lua_lql_set_scope(lua, scope.previous);
  if (status != LUA_OK) {
    return lua_error(lua);
  }
  if (closed && !(lua_gettop(lua) == 2 && lua_isnil(lua, 1))) {
    lua_settop(lua, 0);
    return lua_lql_return_error(lua, NULL, LQL_STATUS_CALLBACK_ERROR);
  }
  return lua_gettop(lua);
}
