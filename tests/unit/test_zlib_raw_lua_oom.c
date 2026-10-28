#include <assert.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdlib.h>
#include <string.h>
void vectis_lua_zlib_raw_register(lua_State *);
typedef struct fixture {
  int failing;
  int allowance;
  size_t live;
} fixture;
static void *allocate(void *user, void *ptr, size_t old, size_t size) {
  fixture *state = (fixture *)user;
  void *next;
  if (!size) {
    if (ptr)
      --state->live;
    free(ptr);
    return NULL;
  }
  if (state->failing && (!ptr || size > old) && state->allowance-- <= 0)
    return NULL;
  next = realloc(ptr, size);
  if (next && !ptr)
    ++state->live;
  return next;
}
static int arm(lua_State *L) {
  void *user;
  lua_getallocf(L, &user);
  ((fixture *)user)->failing = 1;
  ((fixture *)user)->allowance =
      lua_isnoneornil(L, 1) ? 0 : (int)luaL_checkinteger(L, 1);
  return 0;
}
static void run(const char *body, const char *after) {
  fixture state;
  lua_State *L;
  memset(&state, 0, sizeof(state));
  L = lua_newstate(allocate, &state, 0u);
  assert(L);
  luaL_openlibs(L);
  lua_newtable(L);
  vectis_lua_zlib_raw_register(L);
  lua_setglobal(L, "z");
  lua_pushcfunction(L, arm);
  lua_setglobal(L, "arm");
  assert(luaL_loadstring(L, body) == LUA_OK);
  assert(lua_pcall(L, 0, 1, 0) == LUA_ERRMEM);
  state.failing = 0;
  lua_settop(L, 0);
  if (after)
    assert(luaL_dostring(L, after) == LUA_OK);
  lua_close(L);
  assert(state.live == 0);
}
int main(void) {
  run("local data=string.rep('a',1000);owner=z.deflateInit(6);arm();return "
      "owner:process_materialized(data,4096,4)",
      "assert(not pcall(owner.fields,owner));owner:close()");
  run("owner=z.deflateInit(6);arm();return owner:copy()",
      "assert(type(owner:fields())=='table');owner:close()");
  run("owner=z.deflateInit(6);arm();return owner:fields()",
      "assert(type(owner:fields())=='table');owner:close()");
  run("arm();return z.deflateInit(6)", NULL);
  run("local "
      "b=require('vectis.buffer.core');input=b.from_materialized('bound');"
      "output=b.new(128);owner=z.deflateInit(6);owner:set_input(input);"
      "owner:set_output(output);arm();return owner:run(4)",
      "assert(not pcall(owner.fields,owner));input:resize(1);output:resize(1);"
      "input:close();output:close();owner:close()");
  run("local "
      "b=require('vectis.buffer.core');input=b.from_materialized('fields');"
      "owner=z.deflateInit(6);owner:set_input(input);arm();return "
      "owner:fields()",
      "assert(type(owner:fields())=='table');owner:close();collectgarbage('"
      "collect');"
      "input:close()");
  run("local "
      "b=require('vectis.buffer.core');input=b.from_materialized('fields');"
      "output=b.new(128);owner=z.deflateInit(6);owner:set_input(input);"
      "owner:set_output(output);local warm=owner:fields();warm.next_in:close();"
      "warm.next_out:close();arm(3);return owner:fields()",
      "owner:close();collectgarbage('collect');input:resize(1);output:resize(1)"
      ";"
      "input:close();output:close()");
  run("local "
      "b=require('vectis.buffer.core');name=b.from_materialized('name'..string."
      "char(0));"
      "local options={name=name};arm();return z.gzip_header(options)",
      "name:resize(1);name:close()");
  run("local "
      "b=require('vectis.buffer.core');name=b.from_materialized('name'..string."
      "char(0));"
      "header=z.gzip_header({name=name});arm();return header:fields()",
      "header:close();name:resize(1);name:close()");
  run("local b=require('vectis.buffer.core');window=b.new(32768);arm();return "
      "z.inflateBackInit(15,window)",
      "window:resize(1);window:close()");
  run("local "
      "b=require('vectis.buffer.core');window=b.new(32768);owner=z."
      "inflateBackInit(15,window);"
      "return owner:back(function() arm();return string.rep('x',2000) "
      "end,function() return true end)",
      "assert(not "
      "pcall(owner.fields,owner));owner:close();window:resize(1);window:close("
      ")");
  run("local b=require('vectis.buffer.core');local "
      "encoder=z.deflateInit2(6,8,-15,8,0);"
      "local "
      "compressed=encoder:process_materialized('output',128,4).output;encoder:"
      "close();"
      "window=b.new(32768);owner=z.inflateBackInit(15,window);local "
      "once=false;return owner:back("
      "function() if once then return nil end;once=true;return compressed end,"
      "function() arm();return string.rep('x',2000) end)",
      "assert(not "
      "pcall(owner.fields,owner));owner:close();window:resize(1);window:close("
      ")");
  run("local b=require('vectis.buffer.core');return z.deflateInit(6,{"
      "zalloc=function(items,size) arm();return string.rep('x',2000) end,"
      "zfree=function() end})",
      NULL);
  run("local b=require('vectis.buffer.core');owner=z.deflateInit(6,{"
      "zalloc=function(items,size) return b.new(items*size) end,"
      "zfree=function(view) saved=view;arm();return string.rep('x',2000) end});"
      "return owner:close()",
      "assert(not pcall(saved.size,saved));owner:close()");
  run("local "
      "b=require('vectis.buffer.core');copying=false;owner=z.deflateInit(6,{"
      "zalloc=function(items,size) if copying then arm();return "
      "string.rep('x',2000) end;"
      "return b.new(items*size) end,zfree=function() end});copying=true;return "
      "owner:copy()",
      "copying=false;assert(type(owner:fields())=='table');owner:close()");
  run("local b=require('vectis.buffer.core');local encoder=z.deflateInit(6);"
      "local "
      "data=encoder:process_materialized(string.rep('a',2000),4096,4).output;"
      "encoder:close();"
      "local calls=0;owner=z.inflateInit({zalloc=function(items,size) "
      "calls=calls+1;"
      "if calls>1 then arm();return string.rep('x',2000) end;return "
      "b.new(items*size) end,"
      "zfree=function() end});return owner:process_materialized(data,1,0)",
      "owner:close()");
  run("local b=require('vectis.buffer.core');output=b.new(128);arm();"
      "return z.compress(output,false)",
      "output:resize(1);output:close()");
  run("arm();return z.get_crc_table()", NULL);
  run("local "
      "b=require('vectis.buffer.core');input=b.from_materialized('input');"
      "arm();return z.crc32(0,input,10)",
      "input:resize(1);input:close()");
  return 0;
}
