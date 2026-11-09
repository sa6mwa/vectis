#define _GNU_SOURCE
#include <assert.h>
#include <cpkt/lua_runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef struct stack_case {
  cpkt_lua_runtime *runtime;
} stack_case;
static void run(cpkt_lua_runtime *runtime, const char *source) {
  cpkt_lua_runtime_status status;
  status = cpkt_lua_runtime_run_buffer(runtime, (const unsigned char *)source,
                                       strlen(source), "=stack", 0, NULL, 0);
  if (status != CPKT_LUA_RUNTIME_OK)
    fprintf(stderr, "Lua stack regression: %s\n",
            cpkt_lua_runtime_error(runtime));
  assert(status == CPKT_LUA_RUNTIME_OK);
}
static void *exercise(void *context) {
  stack_case *test = (stack_case *)context;
  run(test->runtime,
      "local function f()local ok,e=pcall(f);return ok and e or tostring(e)end;"
      "assert(f():find('stack overflow',1,true));assert(6*7==42)");
  run(test->runtime,
      "local function h(e)return string.gsub('x','x',function()return "
      "h(e)end)end;"
      "local ok,e=xpcall(function()error('sentinel')end,h);"
      "assert(not ok and tostring(e):find('error in error handling',1,true))");
  run(test->runtime, "local function f()return string.gsub('x','x',f)end;"
                     "local ok,e=pcall(f);assert(not ok and "
                     "tostring(e):find('stack overflow',1,true));"
                     "local function c()local co=coroutine.create(c);local "
                     "ok,e=coroutine.resume(co);"
                     "return ok and e or tostring(e)end;assert(c():find('stack "
                     "overflow',1,true))");
  run(test->runtime, "assert(coroutine.resume(saved)==true);assert(20+22==42)");
  return NULL;
}
static void *scenario(void *unused) {
  stack_case test;
  pthread_t worker;
  int i;
  (void)unused;
  assert(cpkt_lua_runtime_new(&test.runtime) == CPKT_LUA_RUNTIME_OK);
  assert(cpkt_lua_runtime_openlibs(test.runtime) == CPKT_LUA_RUNTIME_OK);
  for (i = 0; i < 2; ++i) {
    run(test.runtime,
        "saved=coroutine.create(function()coroutine.yield();return 42 end);"
        "assert(coroutine.resume(saved))");
    assert(pthread_create(&worker, NULL, exercise, &test) == 0);
    assert(pthread_join(worker, NULL) == 0);
    run(test.runtime,
        "assert(coroutine.status(saved)=='dead');assert(6*7==42)");
  }
  cpkt_lua_runtime_free(test.runtime);
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, scenario, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  puts("Default-worker native Lua overflow, handlers, migration and recovery "
       "passed");
  return 0;
}
