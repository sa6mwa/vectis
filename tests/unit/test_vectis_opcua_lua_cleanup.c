#include <assert.h>
#include <lualib.h>
#include <open62541/server.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static size_t allocations;
static void *tracked_malloc(size_t size) {
  void *p = malloc(size);
  if (p != NULL)
    ++allocations;
  return p;
}
static void *tracked_calloc(size_t count, size_t size) {
  void *p = calloc(count, size);
  if (p != NULL)
    ++allocations;
  return p;
}
static void tracked_free(void *p) {
  if (p != NULL) {
    assert(allocations != 0u);
    --allocations;
  }
  free(p);
}

/* Isolate array helper ownership across Lua nonlocal error unwinding. */
#define malloc tracked_malloc
#define calloc tracked_calloc
#define free tracked_free
#include "../../src/vectis_opcua_lua.c"
#undef malloc
#undef calloc
#undef free

static int arrays(lua_State *lua) {
  size_t count;
  int *integers;
  unsigned long *dimensions;
  cpkt_opcua_value *values;
  lua_settop(lua, 4);
  integers = vectis_opcua_lua_int_array_field(lua, 1, "first", &count);
  assert(count == 2u && integers[0] == 1 && integers[1] == 2);
  (void)vectis_opcua_lua_int_array_field(lua, 1, "second", &count);
  dimensions =
      vectis_opcua_lua_ulong_array_from_lua(lua, 2, &count, "dimensions");
  assert(count == 2u && dimensions[0] == 1u && dimensions[1] == 2u);
  values = vectis_opcua_lua_value_array_from_lua(lua, 3, &count, "inputs");
  assert(count == 2u && values[0].integer_value == 1);
  if (lua_toboolean(lua, 4))
    return luaL_error(lua, "later option rejected");
  lua_pushboolean(lua, 1);
  return 1;
}

/* Exercise every client callback path with the real Lua receiver shell. */
static int invoke_callback(lua_State *lua) {
  vectis_opcua_lua_client *client = vectis_opcua_lua_check_client(lua, 1);
  int kind = (int)luaL_checkinteger(lua, 3);
  luaL_checktype(lua, 2, LUA_TFUNCTION);
  if (kind <= 5) {
    vectis_opcua_lua_async_callback *callback =
        vectis_opcua_lua_async_callback_new(lua, client, 2);
    vectis_opcua_lua_client_keep_async_callback(client, callback);
    switch (kind) {
    case 1:
      vectis_opcua_lua_async_value_cb(1u, CPKT_OPCUA_ERR_UPSTREAM, NULL, 0u,
                                      callback);
      break;
    case 2:
      vectis_opcua_lua_async_status_cb(1u, CPKT_OPCUA_OK, 0u, callback);
      break;
    case 3:
      vectis_opcua_lua_async_browse_cb(1u, CPKT_OPCUA_OK, 0u, callback);
      break;
    case 4:
      vectis_opcua_lua_async_call_cb(1u, CPKT_OPCUA_OK, NULL, 0u, 0u, callback);
      break;
    case 5:
      vectis_opcua_lua_async_node_cb(1u, CPKT_OPCUA_ERR_UPSTREAM, NULL, 0u,
                                     callback);
      break;
    }
  } else {
    vectis_opcua_lua_monitor_callback *callback =
        vectis_opcua_lua_monitor_callback_new(lua, client, 2, 1u);
    cpkt_opcua_value value;
    cpkt_opcua_event event;
    cpkt_opcua_value_integer(&value, 7L);
    memset(&event, 0, sizeof(event));
    vectis_opcua_lua_client_keep_monitor_callback(client, callback);
    if (kind == 6)
      vectis_opcua_lua_data_change_cb(1u, 1u, &value, 0u, callback);
    else if (kind == 7)
      vectis_opcua_lua_event_cb(1u, 1u, &event, 0u, callback);
    else
      vectis_opcua_lua_event_fields_cb(1u, 1u, NULL, 0u, 0u, callback);
  }
  return 0;
}

/* The public CPKT native visitor calls a real registered Lua method through
 * open62541. No network, private upstream layout or delay is involved. */
static cpkt_opcua_status native_method(void *native, void *user) {
  UA_CallMethodRequest request;
  UA_CallMethodResult result;
  (void)user;
  UA_CallMethodRequest_init(&request);
  request.objectId = UA_NODEID_NUMERIC(0, 85);
  request.methodId = UA_NODEID_NUMERIC(1, 9001);
  result = UA_Server_call((UA_Server *)native, &request);
  assert(result.statusCode == UA_STATUSCODE_GOOD);
  assert(result.outputArgumentsSize == 1u);
  UA_CallMethodResult_clear(&result);
  return UA_STATUSCODE_GOOD;
}
static int invoke_server(lua_State *lua) {
  vectis_opcua_lua_server *server = vectis_opcua_lua_check_server(lua, 1);
  if (lua_toboolean(lua, 2)) {
    assert(vectis_opcua_lua_access_control_cb("owner", 5u, NULL, 0u, server) ==
           0u);
  } else {
    assert(cpkt_opcua_server_native(server->server, native_method, NULL) ==
           CPKT_OPCUA_OK);
  }
  assert(server->callback_depth == 0u);
  return 0;
}
/* Simulate the app-owned lease boundary during a configuration getter. */
static int logger_managed(lua_State *lua) {
  cpkt_opcua_server *native = NULL;
  if (lua_toboolean(lua, 2)) {
    assert(!vectis_opcua_lua_borrow_server(lua, 1, &native, NULL));
    vectis_opcua_lua_server_managed(lua, 1, 1);
  } else {
    vectis_opcua_lua_server_managed(lua, 1, 0);
  }
  return 0;
}
extern int luaopen_pslog_core(lua_State *);
static void *emit_native_record(void *logger) {
  cpkt_opcua_log_record record;
  memset(&record, 0, sizeof(record));
  record.level = CPKT_OPCUA_LOG_FATAL;
  record.category = CPKT_OPCUA_LOG_SECURITY;
  record.message = "before\0after";
  record.message_length = 12u;
  record.format_status = 7u;
  vectis_opcua_log_record(&record, logger);
  return NULL;
}
static int native_record(lua_State *lua) {
  pslog_lua_logger_view view;
  pslog_lua_logger_ref ref;
  pthread_t emitter;
  memset(&view, 0, sizeof(view));
  view.size = sizeof(view);
  view.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  assert(pslog_lua_check_logger(lua, 1, &view) == 0 &&
         (view.flags & PSLOG_LUA_LOGGER_NATIVE_SINK));
  opcua_logger_init(&ref);
  assert(pslog_lua_ref_logger(lua, 1, NULL, 0u, &ref) == 0);
  assert(pthread_create(&emitter, NULL, emit_native_record, ref.borrowed) == 0);
  assert(pthread_join(emitter, NULL) == 0);
  pslog_lua_unref_logger(&ref);
  return 0;
}
int main(void) {
  lua_State *lua = luaL_newstate();
  int i;
  assert(lua != NULL);
  luaL_openlibs(lua);
  luaL_requiref(lua, "pslog.core", luaopen_pslog_core, 0);
  lua_pop(lua, 1);
  lua_pushcfunction(lua, native_record);
  lua_setglobal(lua, "native_record");
  lua_pushliteral(lua, VECTIS_TEST_OUTPUT_DIR "/opcua-lua-cleanup.log");
  lua_setglobal(lua, "native_log_path");
  i = luaL_dostring(
      lua,
      "local file=assert(io.open(native_log_path,'w+b')); local "
      "core=require('pslog.core')\n"
      "local "
      "logger=core.new_logger({output=file,mode='json',level='trace',"
      "disable_timestamp=true})\n"
      "native_record(logger); file:flush(); file:seek('set',0); local "
      "bytes=file:read('a')\n"
      "assert(bytes:find([=[before\\u0000after]=],1,true),bytes); "
      "assert(bytes:find('\"category\":6',1,true))\n"
      "assert(bytes:find('\"format_status\":7',1,true)); "
      "assert(bytes:find('\"lvl\":\"fatal\"',1,true),bytes)\n"
      "logger:close(); file:close(); assert(os.remove(native_log_path))\n");
  if (i != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(lua, -1));
  assert(i == LUA_OK);
  assert(luaopen_opcua(lua) == 1);
  lua_setglobal(lua, "opcua");
  lua_pushcfunction(lua, invoke_callback);
  lua_setglobal(lua, "invoke_callback");
  lua_pushcfunction(lua, invoke_server);
  lua_setglobal(lua, "invoke_server");
  lua_pushcfunction(lua, arrays);
  lua_setglobal(lua, "arrays");
  for (i = 0; i < 30; ++i) {
    int rc = luaL_dostring(
        lua,
        "assert(not pcall(arrays, {first={1,2},second={1,{}}}, {1,2}, {1,2}))\n"
        "assert(not pcall(arrays, {first={1,2}}, {1,{}}, {1,2}))\n"
        "assert(not pcall(arrays, {first={1,2}}, {1,-1}, {1,2}))\n"
        "assert(not pcall(arrays, {first={1,2}}, {1,2}, {1,{}}))\n"
        "assert(not pcall(arrays, {first={1,2}}, {1,2}, {1,2}, true))\n"
        "assert(arrays({first={1,2},second={}}, {1,2}, {1,2}))\n");
    if (rc != LUA_OK)
      fprintf(stderr, "%s\n", lua_tostring(lua, -1));
    assert(rc == LUA_OK);
    /* No collection required: __close cleans allocations immediately. */
    assert(allocations == 0u);
  }
  assert(luaL_dostring(lua, "local co=coroutine.create(function()\n"
                            "  arrays({first={1,2}}, {1,{}}, {1,2})\n"
                            "end)\n"
                            "assert(not coroutine.resume(co)); co=nil; "
                            "collectgarbage('collect')\n") == LUA_OK);
  assert(allocations == 0u);
  {
    int rc = luaL_dostring(
        lua,
        "for kind=1,8 do\n"
        "  local client=assert(opcua.client()); local calls=0\n"
        "  invoke_callback(client,function()\n"
        "    calls=calls+1; collectgarbage('collect')\n"
        "    for _,name in "
        "ipairs({'close','disconnect','connect','iterate','__gc'}) do\n"
        "      local fn=name=='__gc' and getmetatable(client).__gc or "
        "client[name]\n"
        "      local ok,err=pcall(fn,client)\n"
        "      assert(not ok and tostring(err):find('opcua client is "
        "busy',1,true))\n"
        "    end\n"
        "  end,kind)\n"
        "  assert(calls==1); assert(client:close()); assert(client:close())\n"
        "end\n");
    if (rc != LUA_OK)
      fprintf(stderr, "%s\n", lua_tostring(lua, -1));
    assert(rc == LUA_OK);
    assert(allocations == 0u);
  }
  {
    int rc = luaL_dostring(
        lua,
        "local s=assert(opcua.server({logger_disabled=true}))\n"
        "local calls=0; local function guarded()\n"
        " calls=calls+1; collectgarbage('collect')\n"
        " for _,name in "
        "ipairs({'close','set_logger','iterate','set_access_control','add_"
        "method'}) do\n"
        "  local ok,err=pcall(s[name],s,{logger_disabled=true})\n"
        "  assert(not ok and tostring(err):find('opcua server is "
        "busy',1,true))\n"
        " end\n"
        " return opcua.value_integer(42)\n"
        "end\n"
        "assert(s:add_method({node_id=opcua.node_id_numeric(1,9001),parent_"
        "node_id="
        "opcua.node_id_numeric(0,85),\n"
        " browse_name='guarded',output_type=opcua.VALUE_INTEGER,input_types={},"
        "callback=guarded}))\n"
        "invoke_server(s,false); assert(calls==1)\n"
        "assert(s:set_access_control({allow_anonymous=false,handler=function() "
        "guarded();return true end}))\n"
        "invoke_server(s,true); assert(calls==2)\n"
        "assert(s:set_logger({logger_disabled=true})); assert(s:close())\n");
    if (rc != LUA_OK)
      fprintf(stderr, "%s\n", lua_tostring(lua, -1));
    assert(rc == LUA_OK);
    assert(allocations == 0u);
  }
  lua_pushcfunction(lua, logger_managed);
  lua_setglobal(lua, "logger_managed");
  {
    int rc = luaL_dostring(
        lua,
        "local core=require('pslog.core'); local weak=setmetatable({}, "
        "{__mode='v'})\n"
        "local function logger() local "
        "l=core.new_logger({output=io.stderr,level='fatal'}); weak[1]=l; "
        "return l end\n"
        "local c=assert(opcua.client({logger_disabled=true})); "
        "assert(c:close())\n"
        "do local l=logger(); assert(not pcall(c.set_logger,c,{logger=l})); "
        "assert(l:close()) end\n"
        "collectgarbage('collect'); assert(weak[1]==nil,'closed client "
        "retained logger')\n"
        "for _,kind in ipairs({'client','server'}) do\n"
        " for _,field in ipairs({'logger_disabled','logger'}) do\n"
        "  local obj=assert(opcua[kind]({logger_disabled=true}))\n"
        "  do local l=logger(); local opts=setmetatable({}, "
        "{__index=function(_,key)\n"
        "   if key==field then collectgarbage('collect'); local "
        "ok,e=pcall(obj.close,obj); assert(not ok and e:find('busy',1,true)) "
        "end\n"
        "   if key=='logger' then return l end\n"
        "  end}); assert(obj:set_logger(opts));\n"
        "  end; collectgarbage('collect'); assert(weak[1]~=nil,'live sink not "
        "retained'); assert(obj:set_logger({logger_disabled=true}))\n"
        "  collectgarbage('collect'); assert(weak[1]==nil); "
        "assert(obj:close())\n"
        " end\n"
        "end\n"
        "local s=assert(opcua.server({logger_disabled=true}))\n"
        "do local l=logger(); local opts=setmetatable({}, "
        "{__index=function(_,key)\n"
        " if key=='logger' then logger_managed(s,true); return l end end})\n"
        " assert(not pcall(s.set_logger,s,opts)); assert(l:close()) end\n"
        "collectgarbage('collect'); assert(weak[1]==nil,'ownership change "
        "retained logger')\n"
        "logger_managed(s,false); assert(s:close())\n"
        "local t=assert(opcua.server({logger_disabled=true}))\n"
        "assert(not pcall(t.set_logger,t,setmetatable({}, {__index=function() "
        "error('getter fault') end})))\n"
        "assert(t:set_logger({logger_disabled=true})); assert(t:close())\n");
    if (rc != LUA_OK)
      fprintf(stderr, "%s\n", lua_tostring(lua, -1));
    assert(rc == LUA_OK);
    assert(allocations == 0u);
  }
  lua_close(lua);
  assert(allocations == 0u);
  return 0;
}
