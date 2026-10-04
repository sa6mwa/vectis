#include "../../src/vectis_cli.c"
#include <assert.h>
#include <lualib.h>

/* Exercise native destruction while a callback source is still owned. The
 * instrumented close delegates to the real native app destructor exactly once.
 */
static vectis_lua_spooled_source *retained_source;
static void (*native_close)(vectis_app *);
static int native_closes;
static void close_with_source(vectis_app *app) {
  ++native_closes;
  vectis_lua_spooled_source_free(retained_source);
  retained_source = NULL;
  native_close(app);
}
static void check_chunk(lua_State *lua, vectis_lua_spooled_source *source) {
  char buffer[4];
  lc_error error;
  lc_error_init(&error);
  assert(vectis_lua_spooled_source_reset(source, &error) == LC_OK);
  assert(vectis_lua_spooled_source_read(source, buffer, sizeof(buffer),
                                        &error) == 4u);
  assert(memcmp(buffer, "abcd", 4u) == 0);
  assert(lua_gettop(lua) == 0);
  assert(vectis_lua_spooled_source_read(source, buffer, sizeof(buffer),
                                        &error) == 2u);
  assert(memcmp(buffer, "ef", 2u) == 0);
  assert(lua_gettop(lua) == 0);
  assert(vectis_lua_spooled_source_reset(source, &error) == LC_OK);
  assert(vectis_lua_spooled_source_read(source, buffer, sizeof(buffer),
                                        &error) == 4u);
  assert(memcmp(buffer, "abcd", 4u) == 0);
  assert(lua_gettop(lua) == 0);
  /* Reset must discard the still-pending suffix before the next read. */
  assert(vectis_lua_spooled_source_reset(source, &error) == LC_OK);
  assert(vectis_lua_spooled_source_read(source, buffer, sizeof(buffer),
                                        &error) == 4u);
  assert(memcmp(buffer, "abcd", 4u) == 0);
  assert(lua_gettop(lua) == 0);
  lc_error_cleanup(&error);
}
static int conversion_owner;
static int convert_response(lua_State *lua) {
  vectis_error error;
  vectis_response *response;
  vectis_status status;
  int top = lua_gettop(lua);
  vectis_error_clear(&error);
  response = vectis_internal_response_new(&error);
  assert(response != NULL);
  status = vectis_lua_apply_route_response(lua, &conversion_owner, 1, response,
                                           &error);
  assert(lua_gettop(lua) == top);
  if (status == VECTIS_OK) {
    size_t expected_size;
    const char *expected = luaL_optlstring(lua, 4, "resolved", &expected_size);
    vectis_bytes body = vectis_internal_response_body(response);
    if (body.size == 0u) {
      const char *path = vectis_internal_response_file_path(response);
      char bytes[64];
      FILE *file;
      assert(path != NULL);
      file = fopen(path, "rb");
      assert(expected_size < sizeof(bytes));
      assert(file != NULL &&
             fread(bytes, 1u, sizeof(bytes), file) == expected_size);
      assert(memcmp(bytes, expected, expected_size) == 0);
      assert(fclose(file) == 0);
    } else {
      assert(body.size == expected_size &&
             memcmp(body.data, expected, expected_size) == 0);
    }
    assert(strcmp(vectis_internal_response_content_type(response),
                  luaL_optstring(
                      lua, 2,
                      "text/"
                      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")) == 0);
    if (lua_toboolean(lua, 3)) {
      assert(vectis_internal_response_header_count(response) == 1u);
      assert(strcmp(vectis_internal_response_header_name(response, 0u), "7") ==
             0);
      assert(strcmp(vectis_internal_response_header_value(response, 0u), "8") ==
             0);
    }
  }
  vectis_internal_response_free(response);
  lua_pushboolean(lua, status == VECTIS_OK);
  return 1;
}
static int checked_script(lua_State *lua, const char *script) {
  int rc = luaL_dostring(lua, script);
  if (rc != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(lua, -1));
  return rc;
}
/* Fault only after the source callback returns, so the failing allocations are
 * chunk normalization or pending retention in the actual CLI/native consumer.
 * Leave the fault armed through close: cleanup must work under memory pressure.
 */
typedef struct {
  int kind, armed, failures, reads, closes;
  int refs[8192];
  size_t nrefs;
  char spool[PATH_MAX];
  vectis_lua_app *owner;
} source_fault;

static void *source_allocator(void *userdata, void *ptr, size_t old_size,
                              size_t size) {
  source_fault *fault = userdata;
  if (size == 0u) {
    free(ptr);
    return NULL;
  }
  if (fault->armed &&
      ((fault->kind == 1 && ptr == NULL && old_size == LUA_TSTRING) ||
       (fault->kind == 2 && size >= 1024u && size > old_size))) {
    ++fault->failures;
    return NULL; /* Includes Lua's retry after emergency GC. */
  }
  return realloc(ptr, size);
}

static int descriptor_count(source_fault *fault) {
#if defined(__APPLE__)
  DIR *dir = opendir("/dev/fd");
#else
  DIR *dir = opendir("/proc/self/fd");
#endif
  struct dirent *entry;
  int count = 0;
  assert(dir != NULL);
  while ((entry = readdir(dir)) != NULL) {
    char path[PATH_MAX], target[PATH_MAX];
    ssize_t size;
    if (entry->d_name[0] == '.')
      continue;
    ++count;
    if (fault == NULL)
      continue;
#if defined(__APPLE__)
    (void)path;
    size = fcntl(atoi(entry->d_name), F_GETPATH, target) == 0
               ? (ssize_t)strlen(target)
               : -1;
#else
    assert(snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name) > 0);
    size = readlink(path, target, sizeof(target) - 1u);
#endif
    if (size <= 0)
      continue;
    target[size] = '\0';
    if (strrchr(target, '/') != NULL &&
        strncmp(strrchr(target, '/') + 1, "vectis-response-", 16u) == 0) {
      assert(fault->spool[0] == '\0');
      memcpy(fault->spool, target, (size_t)size + 1u);
    }
  }
  assert(closedir(dir) == 0);
  return count;
}

static int fault_read(lua_State *lua) {
  source_fault *fault = lua_touserdata(lua, lua_upvalueindex(1));
  assert(fault->owner->callback_depth != 0u);
  if (fault->reads++ != 0) {
    lua_pushnil(lua);
    return 1;
  }
  (void)descriptor_count(fault);
  assert(fault->spool[0] != '\0');
  if (fault->kind == 2) {
    size_t length;
    /* Consume free reference slots, then fill to a public registry length
     * boundary. The next pending reference must grow the registry array. */
    do {
      assert(fault->nrefs < sizeof(fault->refs) / sizeof(fault->refs[0]));
      lua_pushboolean(lua, 1);
      fault->refs[fault->nrefs++] = luaL_ref(lua, LUA_REGISTRYINDEX);
      length = lua_rawlen(lua, LUA_REGISTRYINDEX);
    } while (fault->nrefs < 512u || (length & (length - 1u)) != 0u);
  }
  lua_gc(lua, LUA_GCCOLLECT);
  fault->armed = 1;
  if (fault->kind == 1)
    lua_pushnumber(lua, 9384756.125);
  else
    lua_pushvalue(lua, lua_upvalueindex(3));
  return 1;
}

static int fault_close(lua_State *lua) {
  source_fault *fault = lua_touserdata(lua, lua_upvalueindex(1));
  assert(fault->armed && fault->failures >= 2);
  assert(fault->owner->app != NULL && fault->owner->callback_depth != 0u);
  assert(++fault->closes == 1);
  return 0;
}

static int fault_convert(lua_State *lua) {
  vectis_error error;
  vectis_response *response;
  vectis_status status;
  int top = lua_gettop(lua);
  vectis_error_clear(&error);
  response = vectis_internal_response_new(&error);
  assert(response != NULL);
  status = vectis_lua_apply_route_response(lua, &conversion_owner, 1, response,
                                           &error);
  assert(status == VECTIS_ERR_NOMEM && error.code == VECTIS_ERR_NOMEM);
  assert(strstr(error.message, "allocation failed") != NULL);
  assert(lua_gettop(lua) == top);
  vectis_internal_response_free(response);
  return 0;
}

static void allocator_regression(int kind) {
  source_fault fault;
  lua_State *lua;
  vectis_app_config config;
  int descriptors;
  size_t i;
  memset(&fault, 0, sizeof(fault));
  fault.kind = kind;
  lua = lua_newstate(source_allocator, &fault, 0u);
  assert(lua != NULL);
  luaL_openlibs(lua);
  luaL_newmetatable(lua, VECTIS_LUA_APP);
  lua_pushcfunction(lua, vectis_lua_app_close);
  lua_setfield(lua, -2, "close");
  lua_pushcfunction(lua, vectis_lua_app_close);
  lua_setfield(lua, -2, "__gc");
  lua_pushvalue(lua, -1);
  lua_setfield(lua, -2, "__index");
  lua_pop(lua, 1);
  fault.owner = lua_newuserdatauv(lua, sizeof(*fault.owner), 1);
  memset(fault.owner, 0, sizeof(*fault.owner));
  vectis_lua_app_log_seed_init(fault.owner);
  luaL_setmetatable(lua, VECTIS_LUA_APP);
  lua_newtable(lua);
  lua_setiuservalue(lua, -2, 1);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  fault.owner->app = vectis_app_new(&config, NULL);
  assert(fault.owner->app != NULL);
  vectis_lua_app_retain_callback_owner(lua, -1, &conversion_owner);
  lua_setglobal(lua, "app");
  lua_pushlightuserdata(lua, &fault);
  lua_getglobal(lua, "app");
  lua_pushliteral(lua, "");
  if (kind == 2) {
    char chunk[9000];
    memset(chunk, 'x', sizeof(chunk));
    lua_pop(lua, 1);
    lua_pushlstring(lua, chunk, sizeof(chunk));
  }
  lua_pushcclosure(lua, fault_read, 3);
  lua_setglobal(lua, "fault_read");
  lua_pushlightuserdata(lua, &fault);
  lua_getglobal(lua, "app");
  lua_pushcclosure(lua, fault_close, 2);
  lua_setglobal(lua, "fault_close");
  lua_pushcfunction(lua, fault_convert);
  lua_setglobal(lua, "fault_convert");
  lua_pushcfunction(lua, convert_response);
  lua_setglobal(lua, "convert_response");
  assert(checked_script(
             lua,
             "weak=setmetatable({app,fault_read,fault_close},{__mode='v'}); "
             "response={spooled_source={read=fault_read,close=fault_close}}; "
             "fault_read=nil; fault_close=nil") == LUA_OK);
  descriptors = descriptor_count(NULL);
  assert(checked_script(lua, "fault_convert(response)") == LUA_OK);
  assert(fault.armed && fault.failures >= 2 && fault.closes == 1);
  assert(descriptor_count(NULL) == descriptors);
  assert(access(fault.spool, F_OK) != 0 && errno == ENOENT);
  assert(lua_gettop(lua) == 0 && fault.owner->callback_depth == 0u &&
         fault.owner->owned_call_depth == 0u && !fault.owner->closing);
  /* Controlled boundary: only now permit healthy allocations again. */
  fault.armed = 0;
  for (i = 0u; i < fault.nrefs; ++i)
    luaL_unref(lua, LUA_REGISTRYINDEX, fault.refs[i]);
  assert(checked_script(
             lua, "local done=false; assert(convert_response({spooled_source={"
                  "read=function() if done then return nil end; done=true; "
                  "return 'resolved' end}}, 'text/plain; charset=utf-8')); "
                  "response=nil; app:close(); app=nil; "
                  "collectgarbage('collect'); collectgarbage('collect'); "
                  "assert(next(weak)==nil)") == LUA_OK);
  assert(lua_gettop(lua) == 0 && descriptor_count(NULL) == descriptors);
  printf("source allocator %s: %d failures, exact-once close, collected roots, "
         "balanced guards, no fd/spool leak, healthy conversion ok\n",
         kind == 1 ? "numeric chunk" : "pending registry growth",
         fault.failures);
  lua_close(lua);
}

int main(void) {
  lua_State *lua;
  vectis_lua_app *server;
  vectis_app_config config;
  vectis_lua_spooled_source *source;
  allocator_regression(1);
  allocator_regression(2);
  lua = luaL_newstate();
  assert(lua != NULL);
  luaL_openlibs(lua);
  luaL_newmetatable(lua, VECTIS_LUA_APP);
  lua_pushcfunction(lua, vectis_lua_app_close);
  lua_setfield(lua, -2, "close");
  lua_pushvalue(lua, -1);
  lua_setfield(lua, -2, "__index");
  lua_pop(lua, 1);
  server = lua_newuserdatauv(lua, sizeof(*server), 1);
  memset(server, 0, sizeof(*server));
  vectis_lua_app_log_seed_init(server);
  luaL_setmetatable(lua, VECTIS_LUA_APP);
  lua_newtable(lua);
  lua_setiuservalue(lua, -2, 1);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  server->app = vectis_app_new(&config, NULL);
  assert(server->app != NULL);
  native_close = server->app->close;
  server->app->close = close_with_source;
  lua_setglobal(lua, "app");
  assert(luaL_dostring(
             lua, "function reject() collectgarbage('collect'); local "
                  "ok,e=pcall(app.close,app); "
                  "assert(not ok and (e:find('app callback',1,true) or "
                  "e:find('while closing',1,true))) end\n"
                  "function read() reject(); return 'abcdef' end\n"
                  "function reset() reject(); return true end\n"
                  "function cleanup() reject(); cleanup_count=(cleanup_count "
                  "or 0)+1; error('cleanup fault') end") == LUA_OK);
  lua_getglobal(lua, "app");
  vectis_lua_app_retain_callback_owner(lua, -1, &conversion_owner);
  lua_pop(lua, 1);
  lua_pushcfunction(lua, convert_response);
  lua_setglobal(lua, "convert_response");
  assert(
      checked_script(
          lua,
          "local weak=setmetatable({}, {__mode='v'}); local count=0\n"
          "for _,phase in "
          "ipairs({'read','reset','close','content_type','headers'}) do\n"
          " for _,fault in ipairs({true,false}) do\n"
          "  local source=setmetatable({}, {__index=function(_,key)\n"
          "   if key==phase then reject(); if fault then error('getter fault') "
          "end end\n"
          "   local fn; if key=='read' then local done=false; fn=function() if "
          "done then return nil end; done=true; return 'resolved' end\n"
          "   elseif key=='reset' then fn=function() return true end\n"
          "   elseif key=='close' then fn=function() reject() end end\n"
          "   if fn then count=count+1; weak[count]=fn end; return fn end})\n"
          "  local response=setmetatable({}, {__index=function(_,key)\n"
          "   if key==phase then reject(); if fault then error('getter fault') "
          "end end\n"
          "   if key=='content_type' then return 'text/'..string.rep('x',100) "
          "end\n"
          "   if key=='spooled_source' then collectgarbage('collect'); return "
          "source end end})\n"
          "  assert(convert_response(response)==not fault)\n"
          " end\n"
          " collectgarbage('collect'); collectgarbage('collect'); "
          "assert(next(weak)==nil)\n"
          "end\n"
          "local done=false; local "
          "response={status_code=200,content_type='text/"
          "'..string.rep('x',100),headers={[7]=8},\n"
          "spooled_source={read=function() collectgarbage('collect'); if done "
          "then return nil end; done=true; return 'resolved' end}}\n"
          "setmetatable(response,{__index=function(_,key) if key=='status' "
          "then error('unused fallback accessed') end end})\n"
          "assert(convert_response(response,nil,true))\n"
          "assert(not convert_response({file_path=false}))\n"
          "assert(convert_response({content_type=1234,body='resolved'},'1234'))"
          "\n"
          "assert(not convert_response({spooled_source={read=function() "
          "end,reset=42}}))\n"
          "for _,phase in ipairs({'read','reset'}) do\n"
          " for _,shape in ipairs({'number','table'}) do\n"
          "  local n=0; local s={read=function() return nil end, "
          "close=function() reject(); n=n+1 end}; "
          "s[phase]=function() error(shape=='number' and 9384756.125 or "
          "setmetatable({}, {__tostring=function() error('coerced error') "
          "end})) end; "
          "assert(not convert_response({spooled_source=s})); assert(n==1)\n"
          " end\nend\n"
          "for _,value in ipairs({true,{}}) do local n=0; "
          "assert(not convert_response({spooled_source={read=function() return "
          "value end, "
          "close=function() n=n+1 end}})); assert(n==1) end\n"
          "for _,eof in ipairs({false,'nil'}) do "
          "assert(convert_response({spooled_source={read=function() "
          "if eof==false then return false end end}}, "
          "'text/plain; charset=utf-8',false,'')) end\n"
          "local sent=false; "
          "assert(convert_response({spooled_source={read=function() "
          "if sent then return nil end; sent=true; return 1234 end}}, "
          "'text/plain; charset=utf-8',false,'1234'))\n"
          "collectgarbage('collect')\n") == LUA_OK);
  assert(server->callback_depth == 0u && server->app != NULL);
  source = calloc(1u, sizeof(*source));
  assert(source != NULL);
  source->lua = lua;
  source->owner = server;
  source->pending_ref = LUA_NOREF;
  lua_getglobal(lua, "read");
  source->read_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  lua_getglobal(lua, "reset");
  source->reset_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  lua_getglobal(lua, "cleanup");
  source->close_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  lua_getglobal(lua, "app");
  source->owner_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  check_chunk(lua, source);
  assert(server->callback_depth == 0u);
  retained_source = source;
  /* A caught teardown failure must clear closing and permit a later close.
   * Corrupt the Lua-visible registration table before native destruction. */
  lua_getglobal(lua, "app");
  lua_pushnil(lua);
  lua_setiuservalue(lua, -2, 1);
  lua_pop(lua, 1);
  assert(checked_script(lua, "assert(not pcall(app.close,app))") == LUA_OK);
  assert(server->closing == 0 && server->app != NULL && native_closes == 0);
  lua_getglobal(lua, "app");
  lua_newtable(lua);
  lua_setiuservalue(lua, -2, 1);
  lua_pop(lua, 1);
  assert(checked_script(lua,
                        "app:close(); app:close(); assert(cleanup_count==1)") ==
         LUA_OK);
  assert(native_closes == 1 && server->app == NULL && retained_source == NULL);
  assert(server->closing == 0 && server->callback_depth == 0u);
  assert(lua_gettop(lua) == 0);
  lua_close(lua);
  puts("app source read/reset/destruction reentry and caught teardown failure "
       "ok");
  return 0;
}
