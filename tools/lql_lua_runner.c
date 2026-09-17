#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <lql/lql.h>

#include <stdio.h>

static int initialize_liblql(void) {
  lql *ctx = NULL;
  lql_error error;
  lql_status status;

  lql_error_init(&error);
  status = lql_new(&ctx, &error);
  if (status != LQL_STATUS_OK) {
    fprintf(stderr, "lql-lua-runner: liblql initialization failed: %s\n",
            error.message);
    return 1;
  }
  ctx->destroy(ctx);
  return 0;
}

static void set_script_arguments(lua_State *state, int argc, char **argv) {
  int index;

  lua_createtable(state, argc - 1, 0);
  lua_pushstring(state, argv[1]);
  lua_rawseti(state, -2, 0);
  for (index = 2; index < argc; ++index) {
    lua_pushstring(state, argv[index]);
    lua_rawseti(state, -2, index - 1);
  }
  lua_setglobal(state, "arg");
}

int main(int argc, char **argv) {
  lua_State *state;
  int status;

  if (argc < 2) {
    fprintf(stderr, "usage: lql-lua-runner script.lua [argument ...]\n");
    return 2;
  }
  if (initialize_liblql() != 0) {
    return 1;
  }
  state = luaL_newstate();
  if (state == NULL) {
    fprintf(stderr, "lql-lua-runner: cannot create Lua state\n");
    return 1;
  }
  luaL_openlibs(state);
  set_script_arguments(state, argc, argv);
  status = luaL_loadfile(state, argv[1]);
  if (status == LUA_OK) {
    status = lua_pcall(state, 0, LUA_MULTRET, 0);
  }
  if (status != LUA_OK) {
    fprintf(stderr, "lql-lua-runner: %s\n", lua_tostring(state, -1));
  }
  lua_close(state);
  return status == LUA_OK ? 0 : 1;
}
