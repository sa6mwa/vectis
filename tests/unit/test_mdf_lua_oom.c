/* Deterministic public Lua allocator faults exercise the released MDF binding.
 * Cases cover the upstream 0.17 callback/adoption repairs without modifying it.
 */
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>

int luaopen_libmdf_core(lua_State *lua);
typedef struct fault_state {
  size_t live;
  int armed;
} fault_state;
static const char *module_path;
static void require_ok(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "MDF allocator check: %s\n", message);
    exit(1);
  }
}
static void *allocate(void *context, void *ptr, size_t old_size, size_t size) {
  fault_state *fault = (fault_state *)context;
  void *next;
  if (size == 0u) {
    if (ptr != NULL)
      --fault->live;
    free(ptr);
    return NULL;
  }
  if (fault->armed && (ptr == NULL || size > old_size))
    return NULL;
  next = realloc(ptr, size);
  require_ok(next != NULL, "real allocator exhausted");
  if (ptr == NULL)
    ++fault->live;
  return next;
}
static int arm(lua_State *lua) {
  void *context;
  (void)lua_getallocf(lua, &context);
  ((fault_state *)context)->armed = 1;
  return 0;
}
static void fault_case(const char *name, const char *body,
                       const char *recovery) {
  fault_state fault;
  lua_State *lua;
  int status;
  fault.live = 0u;
  fault.armed = 0;
  lua = lua_newstate(allocate, &fault, 0u);
  require_ok(lua != NULL, "VM allocation failed");
  luaL_openlibs(lua);
  luaL_requiref(lua, "libmdf.core", luaopen_libmdf_core, 0);
  lua_pop(lua, 1);
  require_ok(luaL_dofile(lua, module_path) == LUA_OK, "released module failed");
  lua_getglobal(lua, "package");
  lua_getfield(lua, -1, "loaded");
  lua_pushvalue(lua, -3);
  lua_setfield(lua, -2, "libmdf");
  lua_pop(lua, 3);
  lua_pushcfunction(lua, arm);
  lua_setglobal(lua, "arm");
  require_ok(luaL_loadstring(lua, body) == LUA_OK, "fixture syntax");
  status = lua_pcall(lua, 0, 0, 0);
  fault.armed = 0;
  if (status != LUA_ERRMEM) {
    fprintf(stderr, "%s returned %d: %s\n", name, status,
            lua_tostring(lua, -1));
    require_ok(0, "expected deterministic allocation failure");
  }
  lua_settop(lua, 0);
  require_ok(luaL_dostring(lua, recovery) == LUA_OK, "recovery/close failed");
  lua_close(lua);
  require_ok(fault.live == 0u, "Lua allocations remain");
  printf("%s cleanup passed\n", name);
}
#define START                                                                  \
  "local m=require('libmdf');local data=string.rep('a',1000)..'\\n';"
#define READ_ARM                                                               \
  "function()if once then return nil end;once=true;arm();return data end"
#define HEALTHY "assert(require('libmdf').render('healthy'));"
int main(int argc, char **argv) {
  require_ok(argc == 2, "expected released Lua module path");
  module_path = argv[1];
  fault_case("sink argument",
             START "local once=false;m.render_stream(" READ_ARM
                   ",function()return true end,{width=1200})",
             HEALTHY);
  fault_case("source body",
             START "m.render_stream(function()arm();return "
                   "string.rep('x',2000)end,function()end)",
             HEALTHY);
  fault_case("sink body",
             START
             "local once=false;m.render_stream(function()if once then return "
             "nil end;once=true;return data end,function()arm();return "
             "string.rep('x',2000)end,{width=1200})",
             HEALTHY);
  fault_case("trace argument",
             START
             "local once=false;m.render_stream(function()if once then return "
             "nil end;once=true;return data "
             "end,function()arm()end,{width=1200,write_trace=function()end})",
             HEALTHY);
  fault_case("trace body",
             START "m.render(data,{width=1200,write_trace=function()arm();"
                   "return string.rep('x',2000)end})",
             HEALTHY);
  fault_case("result adoption",
             START
             "local n=0;m.render(data,{width=1200,write_trace=function()n=n+1 "
             "end});local "
             "count=0;m.render(data,{width=1200,write_trace=function()count="
             "count+1;if count==n then arm()end end})",
             HEALTHY);
  fault_case("handle result",
             START "h=m.new({width=1200});arm();h:render(data)",
             "assert(h:render('healthy'));h:close();h:close()");
  fault_case("handle callback",
             START "h=m.new({width=1200});h:set_sink(function()end);local "
                   "once=false;h:render_stream(" READ_ARM ")",
             "h:reset();h:close()");
  fault_case("document callback",
             START "h=m.document_stream({width=1200},function()end);arm();h:"
                   "write(data);h:finish_document()",
             "h:reset();h:close()");
  fault_case("literal callback",
             START "h=m.new({width=1200});h:set_sink(function()arm();"
                   "return string.rep('x',2000)end);h:set_word_buffer(8,false);"
                   "h:write_literal(data);h:finish()",
             "h:reset();h:close()");
  fault_case("escaped result", START "h=m.new();arm();h:escape_markdown(data)",
             "assert(h:escape_markdown('**healthy**'));h:close()");
  return 0;
}
