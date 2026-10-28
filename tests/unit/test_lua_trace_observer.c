#include <assert.h>
#include <cpkt/lua_runtime.h>
#include <string.h>

typedef struct trace_state {
  cpkt_lua_runtime *runtime;
  unsigned lines, replacements, coroutine, collected, closed, destroyed;
  int action;
} trace_state;

static int source_is(const cpkt_lua_runtime_trace_event *event,
                     const char *name) {
  return event->source_size == strlen(name) &&
         memcmp(event->source, name, event->source_size) == 0;
}

static void replacement(void *context,
                        const cpkt_lua_runtime_trace_event *event) {
  trace_state *state = (trace_state *)context;
  assert(event != NULL && event->source != NULL && event->line > 0);
  ++state->replacements;
}

static void observe(void *context, const cpkt_lua_runtime_trace_event *event) {
  static const unsigned char empty[] = "";
  trace_state *state = (trace_state *)context;
  int action = state->action;
  assert(event != NULL && event->source != NULL && event->line > 0);
  ++state->lines;
  assert(state->lines < 100000u);
  state->coroutine += source_is(event, "=coroutine-event") ? 1u : 0u;
  state->collected += source_is(event, "=gc-event") ? 1u : 0u;
  state->closed += source_is(event, "=close-event") ? 1u : 0u;
  state->destroyed += source_is(event, "=shutdown-event") ? 1u : 0u;
  state->action = 0;
  if (action == 1) {
    assert(cpkt_lua_runtime_set_trace_callback(state->runtime, NULL, NULL) ==
           CPKT_LUA_RUNTIME_OK);
  } else if (action == 2) {
    assert(cpkt_lua_runtime_set_trace_callback(state->runtime, replacement,
                                               state) == CPKT_LUA_RUNTIME_OK);
  } else if (action == 3) {
    assert(cpkt_lua_runtime_clear_instruction_limit(state->runtime) ==
           CPKT_LUA_RUNTIME_ERR_ARG);
    assert(cpkt_lua_runtime_set_instruction_limit(state->runtime, 999999) ==
           CPKT_LUA_RUNTIME_ERR_ARG);
    assert(cpkt_lua_runtime_run_buffer(state->runtime, empty, 0u, "=reentry", 0,
                                       NULL, 0) == CPKT_LUA_RUNTIME_ERR_ARG);
    cpkt_lua_runtime_free(state->runtime); /* Must be a guarded no-op. */
  }
}

static void open_runtime(trace_state *state, int traced) {
  memset(state, 0, sizeof(*state));
  assert(cpkt_lua_runtime_new(&state->runtime) == CPKT_LUA_RUNTIME_OK);
  assert(cpkt_lua_runtime_openlibs(state->runtime) == CPKT_LUA_RUNTIME_OK);
  if (traced) {
    assert(cpkt_lua_runtime_set_trace_callback(state->runtime, observe,
                                               state) == CPKT_LUA_RUNTIME_OK);
  }
}

static cpkt_lua_runtime_status run(trace_state *state, const char *source) {
  return cpkt_lua_runtime_run_buffer(
      state->runtime, (const unsigned char *)source, strlen(source),
      "=observer-test", 0, NULL, 0);
}

int main(void) {
  static const char finite[] =
      "local value = 1\nvalue = value + 1\nassert(value == 2)\n";
  static const char lifetimes[] =
      "local c = coroutine.create(assert(load("
      "'local v=1\\ncoroutine.yield(v)\\nreturn v+1',"
      "'=coroutine-event')))\n"
      "assert(coroutine.resume(c))\nassert(coroutine.resume(c))\n"
      "do local value = setmetatable({}, {__gc=assert(load("
      "'_G.did_gc=true', '=gc-event'))}) end\n"
      "collectgarbage('collect')\nassert(did_gc)\n"
      "do local value <close> = setmetatable({}, {__close=assert(load("
      "'_G.did_close=true', '=close-event'))}) end\nassert(did_close)\n";
  static const char *const limits[] = {
      "while true do end",
      "pcall(function() while true do end end); error('escaped budget')",
      "xpcall(function() while true do end end, function() "
      "error('ran error handler') end); error('escaped budget')",
      "local c=coroutine.create(function() while true do coroutine.yield() "
      "end end); while true do assert(coroutine.resume(c)) end"};
  trace_state state;
  unsigned before;
  size_t i;
  int traced;

  open_runtime(&state, 1);
  state.action = 3;
  assert(run(&state, lifetimes) == CPKT_LUA_RUNTIME_OK);
  assert(state.lines > 0u && state.coroutine > 0u && state.collected > 0u &&
         state.closed > 0u && state.destroyed == 0u);
  assert(run(&state, "_G.shutdown_value=setmetatable({}, {__gc=assert(load("
                     "'local v=1\\nv=v+1','=shutdown-event'))})") ==
         CPKT_LUA_RUNTIME_OK);
  cpkt_lua_runtime_free(state.runtime);
  assert(state.destroyed > 0u);

  open_runtime(&state, 1);
  state.action = 1;
  assert(run(&state, finite) == CPKT_LUA_RUNTIME_OK && state.lines == 1u);
  assert(run(&state, finite) == CPKT_LUA_RUNTIME_OK && state.lines == 1u);
  assert(cpkt_lua_runtime_set_trace_callback(state.runtime, observe, &state) ==
         CPKT_LUA_RUNTIME_OK);
  state.action = 2;
  before = state.lines;
  assert(run(&state, finite) == CPKT_LUA_RUNTIME_OK);
  assert(state.lines == before + 1u && state.replacements > 0u);
  cpkt_lua_runtime_free(state.runtime);

  for (traced = 0; traced <= 1; ++traced) {
    for (i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
      open_runtime(&state, traced);
      assert(cpkt_lua_runtime_set_instruction_limit(state.runtime, 2000) ==
             CPKT_LUA_RUNTIME_OK);
      state.action = traced ? 3 : 0;
      assert(run(&state, limits[i]) == CPKT_LUA_RUNTIME_ERR_LIMIT);
      assert((state.lines > 0u) == traced);
      assert(cpkt_lua_runtime_clear_instruction_limit(state.runtime) ==
             CPKT_LUA_RUNTIME_OK);
      assert(run(&state, finite) == CPKT_LUA_RUNTIME_OK);
      cpkt_lua_runtime_free(state.runtime);
    }
  }
  return 0;
}
