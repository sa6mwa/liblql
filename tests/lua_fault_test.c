/* Faults use Lua's public allocator; the binding still links the installed
 * shared SDK. Memcheck observes native allocations outside this allocator. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>

int luaopen_lql_core(lua_State *lua);

typedef struct fault_fixture {
  size_t bytes;
  size_t calls;
  size_t fail_after;
  size_t failures;
  int armed;
} fault_fixture;

static void check(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "Lua fault test: %s\n", message);
    exit(1);
  }
}

static void *fault_alloc(void *user, void *ptr, size_t old_size,
                         size_t new_size) {
  fault_fixture *fixture;
  void *next;
  fixture = (fault_fixture *)user;
  if (ptr == NULL) {
    old_size = 0u;
  }
  if (new_size == 0u) {
    fixture->bytes -= old_size;
    free(ptr);
    return NULL;
  }
  if (fixture->armed && new_size > old_size) {
    fixture->calls++;
    if (fixture->calls >= fixture->fail_after) {
      fixture->failures++;
      return NULL;
    }
  }
  next = realloc(ptr, new_size);
  check(next != NULL, "test allocator exhausted real memory");
  fixture->bytes = fixture->bytes - old_size + new_size;
  return next;
}

static int arm(lua_State *lua) {
  fault_fixture *fixture;
  void *user;
  (void)lua_getallocf(lua, &user);
  fixture = (fault_fixture *)user;
  fixture->armed = 1;
  return 0;
}

static size_t run_case(const char *path, size_t index, size_t fail_after,
                       char *name, size_t name_capacity, size_t *out_calls) {
  fault_fixture fixture;
  lua_State *lua;
  int status;
  const char *case_name;
  size_t count;
  fixture.bytes = 0u;
  fixture.calls = 0u;
  fixture.fail_after = fail_after;
  fixture.failures = 0u;
  fixture.armed = 0;
  lua = lua_newstate(fault_alloc, &fixture, 0u);
  check(lua != NULL, "lua_newstate failed");
  luaL_openlibs(lua);
  luaL_requiref(lua, "lql.core", luaopen_lql_core, 1);
  lua_pop(lua, 1);
  lua_pushcfunction(lua, arm);
  lua_setglobal(lua, "arm");
  status = luaL_loadfile(lua, path);
  check(status == LUA_OK, "cannot load fault fixtures");
  status = lua_pcall(lua, 0, 1, 0);
  check(status == LUA_OK && lua_istable(lua, -1), "invalid fault fixtures");
  count = (size_t)lua_rawlen(lua, -1);
  check(count >= 25u, "missing fault cases");
  lua_rawgeti(lua, -1, (lua_Integer)index);
  lua_getfield(lua, -1, "name");
  case_name = lua_tostring(lua, -1);
  check(case_name != NULL, "missing fault case name");
  (void)snprintf(name, name_capacity, "%s", case_name);
  lua_pop(lua, 1);
  lua_getfield(lua, -1, "run");
  status = lua_pcall(lua, 0, 2, 0);
  fixture.armed = 0;
  if (out_calls != NULL) {
    *out_calls = fixture.calls;
  }
  if (status != LUA_OK && status != LUA_ERRMEM) {
    fprintf(stderr, "%s fault %lu: %s\n", name, (unsigned long)fail_after,
            lua_tostring(lua, -1));
    check(0, "unexpected Lua exception");
  }
  if (fixture.failures == 0u) {
    if (status == LUA_OK && lua_isnil(lua, -2)) {
      lua_getfield(lua, -1, "message");
      fprintf(stderr, "%s control: %s\n", name, lua_tostring(lua, -1));
      lua_pop(lua, 1);
    }
    check(status == LUA_OK && !lua_isnil(lua, -2),
          "fault-free operation did not succeed");
  }
  lua_settop(lua, 0);
  /* Any borrowed callback views retained before the failure must be expired. */
  status = luaL_dostring(
      lua,
      "if saved_value then assert(not pcall(saved_value.size, saved_value)); "
      "assert(not pcall(saved_value.write_to, saved_value, function() end)) "
      "end; "
      "if saved_writer then assert(not pcall(saved_writer.write, saved_writer, "
      "'x')) end; "
      "if opened_readers then assert(opened_readers == closed_readers) end");
  check(status == LUA_OK,
        "callback lifetime or paired cleanup contract failed");
  lua_close(lua);
  if (fixture.bytes != 0u) {
    fprintf(stderr, "%s fault %lu leaked %lu Lua/scratch bytes\n", name,
            (unsigned long)fail_after, (unsigned long)fixture.bytes);
    check(0, "facade scratch memory leaked");
  }
  return count;
}

int main(int argc, char **argv) {
  size_t index;
  size_t ordinal;
  size_t count;
  size_t calls;
  char name[128];
  check(argc == 2, "usage: lql_lua_fault_test <fixture.lua>");
  count = run_case(argv[1], 1u, (size_t)-1, name, sizeof(name), NULL);
  for (index = 1u; index <= count; ++index) {
    (void)run_case(argv[1], index, (size_t)-1, name, sizeof(name), &calls);
    check(calls > 0u && calls < 512u,
          "fault fixture has no bounded allocation path");
    for (ordinal = 1u; ordinal <= calls + 1u; ++ordinal) {
      (void)run_case(argv[1], index, ordinal, name, sizeof(name), NULL);
    }
    printf("Lua allocation regression: %s (%lu failure positions)\n", name,
           (unsigned long)calls);
  }
  return 0;
}
