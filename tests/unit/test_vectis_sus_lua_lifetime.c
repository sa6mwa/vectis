#include <assert.h>
#include <cpkt/sus.h>
#include <lualib.h>
#include <stdlib.h>
#include <string.h>

static size_t frame_allocations;
static void *frame_malloc(size_t size) {
  void *p = malloc(size);
  if (p != NULL)
    ++frame_allocations;
  return p;
}
static void frame_free(void *p) {
  if (p != NULL) {
    assert(frame_allocations != 0u);
    --frame_allocations;
  }
  free(p);
}

static cpkt_sus_log_sink native_log_sink;
static int native_log_set_calls;
static void *native_log_user;
static void fixture_log_set(cpkt_sus_log_sink sink, void *user) {
  ++native_log_set_calls;
  native_log_sink = sink;
  native_log_user = user;
}

/* Exercise the actual Lua adapter with heap-allocated receiver fixtures.
 * No model download or inference hardware is required for ownership tests. */
#define malloc frame_malloc
#define free frame_free
#define cpkt_sus_log_set fixture_log_set
#include "../../src/vectis_sus_lua.c"
#undef cpkt_sus_log_set
#undef malloc
#undef free

typedef struct model_fixture {
  cpkt_sus shell;
  size_t children;
} model_fixture;
typedef struct transcriber_fixture {
  cpkt_sus_transcriber shell;
  model_fixture *model;
  cpkt_sus_transcriber_config config;
} transcriber_fixture;
static int live_models;
static int live_transcribers;
static int fail_create;

static void model_destroy(cpkt_sus *shell) {
  model_fixture *model = (model_fixture *)shell;
  assert(model->children == 0u);
  --live_models;
  free(model);
}
static void transcriber_destroy(cpkt_sus_transcriber *shell) {
  transcriber_fixture *t = (transcriber_fixture *)shell;
  assert(t->model->children != 0u);
  --t->model->children;
  --live_transcribers;
  free(t);
}
static cpkt_sus_result transcribe(cpkt_sus_transcriber *shell,
                                  const float *frames, unsigned long count) {
  transcriber_fixture *t = (transcriber_fixture *)shell;
  assert(t->model->children != 0u);
  assert(count == 3u && frames[0] == 0.0f && frames[2] == 0.5f);
  if (t->config.language != NULL) {
    assert(strlen(t->config.language) == 128u);
    assert(strspn(t->config.language, "e") == 128u);
    assert(strlen(t->config.initial_prompt) == 257u);
    assert(strspn(t->config.initial_prompt, "p") == 257u);
  }
  if (t->config.progress_sink != NULL)
    assert(t->config.progress_sink(100, t->config.progress_user) == 0);
  return CPKT_SUS_OK;
}
static cpkt_sus_result
transcribe_decoder(cpkt_sus_transcriber *shell, cpkt_audio_decoder *decoder,
                   const cpkt_sus_segmented_config *config) {
  transcriber_fixture *t = (transcriber_fixture *)shell;
  float frame;
  size_t count;
  (void)config;
  assert(t->model->children != 0u);
  if (t->config.progress_sink != NULL)
    assert(t->config.progress_sink(100, t->config.progress_user) == 0);
  /* Model the native EOF path: the adapter must still propagate reader errors.
   */
  (void)decoder->read_f32_mono_16k(decoder, &frame, 1u, &count);
  return CPKT_SUS_OK;
}
static cpkt_sus_result
transcribe_decoder_text(cpkt_sus_transcriber *shell,
                        cpkt_audio_decoder *decoder,
                        const cpkt_sus_segmented_config *config, char **text) {
  cpkt_sus_result result = transcribe_decoder(shell, decoder, config);
  *text = (char *)malloc(8u);
  assert(*text != NULL);
  memcpy(*text, "partial", 8u);
  return result;
}
static cpkt_sus_result
create_transcriber(cpkt_sus *shell, cpkt_sus_transcriber **out,
                   const cpkt_sus_transcriber_config *config) {
  model_fixture *model = (model_fixture *)shell;
  transcriber_fixture *t;
  if (fail_create) {
    *out = NULL;
    return CPKT_SUS_ERR_ALLOC;
  }
  t = (transcriber_fixture *)calloc(1u, sizeof(*t));
  assert(t != NULL);
  t->model = model;
  t->config = *config; /* Same borrowed configuration contract as SUS. */
  t->shell.destroy = transcriber_destroy;
  t->shell.transcribe_f32_mono_16k = transcribe;
  t->shell.transcribe_audio_decoder_segmented = transcribe_decoder;
  t->shell.transcribe_audio_decoder_segmented_text = transcribe_decoder_text;
  ++model->children;
  ++live_transcribers;
  *out = &t->shell;
  return CPKT_SUS_OK;
}
static int new_model(lua_State *lua) {
  model_fixture *model = (model_fixture *)calloc(1u, sizeof(*model));
  assert(model != NULL);
  model->shell.destroy = model_destroy;
  model->shell.create_transcriber = create_transcriber;
  ++live_models;
  vectis_sus_lua_new_model(lua, &model->shell);
  return 1;
}
static int read_frames(lua_State *lua) {
  size_t count;
  float *frames = vectis_sus_lua_frame_array(lua, 1, &count);
  frame_free(frames);
  lua_pushinteger(lua, (lua_Integer)count);
  return 1;
}
static void run(lua_State *lua, const char *code) {
  int rc = luaL_dostring(lua, code);
  if (rc != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(lua, -1));
  assert(rc == LUA_OK);
}
int luaopen_audio(lua_State *lua);

static lua_State *new_state(void) {
  lua_State *lua = luaL_newstate();
  assert(lua != NULL);
  luaL_openlibs(lua);
  luaL_requiref(lua, "audio", luaopen_audio, 1);
  lua_pop(lua, 1);
  luaL_requiref(lua, "sus", luaopen_sus, 1);
  lua_pop(lua, 1);
  lua_pushcfunction(lua, new_model);
  lua_setglobal(lua, "new_model");
  lua_pushcfunction(lua, read_frames);
  lua_setglobal(lua, "read_frames");
  return lua;
}
static void *emit_foreign(void *unused) {
  size_t i;
  cpkt_sus_log_event event;
  char oversized[SUS_LOG_TEXT_LIMIT + 1u];
  (void)unused;
  memset(&event, 0, sizeof(event));
  event.level = CPKT_SUS_LOG_INFO;
  event.message = "fixture";
  assert(native_log_user == NULL);
  for (i = 0u; i < SUS_LOG_CAPACITY + 3u; ++i)
    native_log_sink(&event, native_log_user);
  memset(oversized, 'x', sizeof(oversized));
  oversized[sizeof(oversized) - 1u] = 0;
  event.message = oversized;
  native_log_sink(&event, native_log_user);
  return NULL;
}
typedef struct foreign_callbacks {
  vectis_sus_transcriber_lua transcriber;
  vectis_sus_segmented_lua segmented;
  vectis_sus_cache_status_lua cache;
} foreign_callbacks;
static void *check_foreign_callbacks(void *arg) {
  foreign_callbacks *callbacks = (foreign_callbacks *)arg;
  /* Lua pointers are deliberately NULL: rejection must precede any Lua use. */
  assert(vectis_sus_lua_segment_sink(NULL, &callbacks->transcriber) == -1);
  assert(vectis_sus_lua_progress_sink(50, &callbacks->transcriber) == -1);
  assert(vectis_sus_lua_abort_fn(&callbacks->transcriber) == 1);
  assert(vectis_sus_lua_segmented_sink(NULL, &callbacks->segmented) == -1);
  assert(vectis_sus_lua_cache_status_sink(NULL, &callbacks->cache) == -1);
  return NULL;
}
typedef struct log_gate {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  int waiting, release;
  cpkt_sus_log_sink sink;
} log_gate;
static void *emit_across_generation(void *arg) {
  log_gate *gate = (log_gate *)arg;
  cpkt_sus_log_event event;
  memset(&event, 0, sizeof(event));
  event.message = "fixture";
  gate->sink(&event, NULL);
  (void)pthread_mutex_lock(&gate->mutex);
  gate->waiting = 1;
  (void)pthread_cond_signal(&gate->condition);
  while (!gate->release)
    (void)pthread_cond_wait(&gate->condition, &gate->mutex);
  (void)pthread_mutex_unlock(&gate->mutex);
  gate->sink(&event,
             NULL); /* retained static hook, after replace or VM close */
  return NULL;
}
static void gate_start(log_gate *gate, pthread_t *thread) {
  memset(gate, 0, sizeof(*gate));
  gate->sink = native_log_sink;
  assert(pthread_mutex_init(&gate->mutex, NULL) == 0);
  assert(pthread_cond_init(&gate->condition, NULL) == 0);
  assert(pthread_create(thread, NULL, emit_across_generation, gate) == 0);
  (void)pthread_mutex_lock(&gate->mutex);
  while (!gate->waiting)
    (void)pthread_cond_wait(&gate->condition, &gate->mutex);
  (void)pthread_mutex_unlock(&gate->mutex);
}
static void gate_finish(log_gate *gate, pthread_t thread) {
  (void)pthread_mutex_lock(&gate->mutex);
  gate->release = 1;
  (void)pthread_cond_signal(&gate->condition);
  (void)pthread_mutex_unlock(&gate->mutex);
  assert(pthread_join(thread, NULL) == 0);
  (void)pthread_cond_destroy(&gate->condition);
  (void)pthread_mutex_destroy(&gate->mutex);
}
static void emit_log(void) {
  cpkt_sus_log_event event;
  memset(&event, 0, sizeof(event));
  event.level = CPKT_SUS_LOG_INFO;
  event.message = "fixture";
  assert(native_log_sink != NULL);
  native_log_sink(&event, native_log_user);
}
typedef struct vm_race {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  int arrived, start, attempted, successes, finish;
} vm_race;
static void *register_vm(void *arg) {
  vm_race *race = (vm_race *)arg;
  lua_State *lua = new_state();
  int success;
  (void)pthread_mutex_lock(&race->mutex);
  ++race->arrived;
  (void)pthread_cond_broadcast(&race->condition);
  while (!race->start)
    (void)pthread_cond_wait(&race->condition, &race->mutex);
  (void)pthread_mutex_unlock(&race->mutex);
  assert(luaL_dostring(lua, "return pcall(sus.set_log_sink,function() end)") ==
         LUA_OK);
  success =
      lua_toboolean(lua, 1); /* pcall failure also returns an error string */
  (void)pthread_mutex_lock(&race->mutex);
  race->successes += success;
  ++race->attempted;
  (void)pthread_cond_broadcast(&race->condition);
  while (!race->finish)
    (void)pthread_cond_wait(&race->condition, &race->mutex);
  (void)pthread_mutex_unlock(&race->mutex);
  lua_close(lua);
  return NULL;
}
static void check_vm_race(void) {
  vm_race race;
  pthread_t threads[2];
  memset(&race, 0, sizeof(race));
  assert(pthread_mutex_init(&race.mutex, NULL) == 0);
  assert(pthread_cond_init(&race.condition, NULL) == 0);
  assert(pthread_create(&threads[0], NULL, register_vm, &race) == 0);
  assert(pthread_create(&threads[1], NULL, register_vm, &race) == 0);
  (void)pthread_mutex_lock(&race.mutex);
  while (race.arrived != 2)
    (void)pthread_cond_wait(&race.condition, &race.mutex);
  race.start = 1;
  (void)pthread_cond_broadcast(&race.condition);
  while (race.attempted != 2)
    (void)pthread_cond_wait(&race.condition, &race.mutex);
  assert(race.successes == 1);
  race.finish = 1;
  (void)pthread_cond_broadcast(&race.condition);
  (void)pthread_mutex_unlock(&race.mutex);
  assert(pthread_join(threads[0], NULL) == 0);
  assert(pthread_join(threads[1], NULL) == 0);
  (void)pthread_cond_destroy(&race.condition);
  (void)pthread_mutex_destroy(&race.mutex);
}

int main(void) {
  lua_State *lua = new_state();
  lua_State *other;
  pthread_t emitter;
  foreign_callbacks callbacks;
  log_gate gate;
  cpkt_sus_log_sink late_sink;
  cpkt_sus_log_event late_event;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.transcriber.thread = callbacks.segmented.thread =
      callbacks.cache.thread = pthread_self();
  assert(pthread_create(&emitter, NULL, check_foreign_callbacks, &callbacks) ==
         0);
  assert(pthread_join(emitter, NULL) == 0);
  run(lua,
      "local audio=require('audio')\n"
      "local payload=string.rep('\\0',32000)\n"
      "local wav='RIFF'..string.pack('<I4',36+#payload)..'WAVEfmt '..\n"
      "  string.pack('<I4I2I2I4I4I2I2',16,1,1,16000,32000,2,16)..\n"
      "  'data'..string.pack('<I4',#payload)..payload\n"
      "local decoder,fail,attempts\n"
      "attempts=0\n"
      "local m=new_model()\n"
      "local t=assert(m:create_transcriber({progress=function()\n"
      "  attempts=attempts+1\n"
      "  local ok,err=pcall(decoder.close,decoder)\n"
      "  assert(not ok and err:find('audio decoder is busy',1,true))\n"
      "end}))\n"
      "for _,method in ipairs({'transcribe_audio_decoder_segmented',\n"
      "                        'transcribe_audio_decoder_segmented_text'}) do\n"
      "  for phase=1,2 do\n"
      "  local pos=1\n"
      "  fail=nil\n"
      "  "
      "decoder=assert(audio.decoder.open_reader({encoding='wav',read=function("
      "n)\n"
      "    if fail then error(fail,0) end\n"
      "    local chunk=wav:sub(pos,pos+n-1); pos=pos+#chunk; return chunk\n"
      "  end,seek=function(offset,origin)\n"
      "    if origin==audio.SEEK_SET then pos=offset+1\n"
      "    elseif origin==audio.SEEK_CUR then pos=pos+offset\n"
      "    else pos=#wav+offset+1 end\n"
      "    return true\n"
      "  end}))\n"
      "  local ok,err=pcall(t[method],t,decoder,{read_frames='invalid'})\n"
      "  assert(not ok); assert(decoder:info())\n"
      "  local weak=setmetatable({}, {__mode='v'})\n"
      "  local function options()\n"
      "    local callback=function() end; weak[1]=callback\n"
      "    return {segmented=callback}\n"
      "  end\n"
      "  if phase==1 then\n"
      "  fail={message='transcription reader failed'}\n"
      "  ok,err=pcall(t[method],t,decoder,options())\n"
      "  assert(not ok and err==fail)\n"
      "  collectgarbage('collect'); assert(weak[1]==nil)\n"
      "  ok,err=pcall(t[method],t,decoder,options())\n"
      "  assert(not ok and err==fail)\n"
      "  collectgarbage('collect'); assert(weak[1]==nil)\n"
      "  else\n"
      "    local value=assert(t[method](t,decoder,options()))\n"
      "    if method:find('_text',1,true) then assert(value=='partial')\n"
      "    else assert(value==true) end\n"
      "    collectgarbage('collect'); assert(weak[1]==nil)\n"
      "    assert(decoder:info())\n"
      "  end\n"
      "  assert(decoder:close()); decoder=nil\n"
      "  end\n"
      "end\n"
      "assert(attempts==4); assert(t:close()); assert(m:close())\n");
  run(lua, "progress = 0; weak = setmetatable({}, {__mode='v'})\n"
           "local co = coroutine.create(function()\n"
           "  local m = new_model(); weak.model = m\n"
           "  local opts = {language=string.rep('e',128),\n"
           "    initial_prompt=string.rep('p',257),\n"
           "    progress=function() progress=progress+1 end}\n"
           "  t = m:create_transcriber(opts); opts.language = 'changed'\n"
           "  opts.initial_prompt = 'changed'; opts=nil; m=nil\n"
           "end)\n"
           "assert(coroutine.resume(co)); co=nil; collectgarbage('collect')\n"
           "assert(weak.model ~= nil)\n"
           "assert(t:transcribe_f32_mono_16k({0, 0.25, 0.5}))\n"
           "assert(progress == 1)\n"
           "assert(t:close()); assert(t:close()); t=nil\n"
           "collectgarbage('collect'); collectgarbage('collect')\n"
           "assert(weak.model == nil)\n");
  assert(live_models == 0 && live_transcribers == 0);
  run(lua,
      "m=new_model(); t=m:create_transcriber(); t2=m:create_transcriber({})\n"
      "assert(m:close()); assert(m:close())\n"
      "assert(not pcall(m.create_transcriber, m))\n"
      "assert(not pcall(m.info, m)); assert(not "
      "pcall(m.reset_transcript_spacing,m))\n"
      "assert(t:transcribe_f32_mono_16k({0,0.25,0.5})); assert(t:close())\n"
      "assert(t2:transcribe_f32_mono_16k({0,0.25,0.5}))\n");
  assert(live_models == 1 && live_transcribers == 1);
  run(lua, "t2:close(); m=nil; t=nil; t2=nil; collectgarbage('collect')");
  assert(live_models == 0 && live_transcribers == 0);
  fail_create = 1;
  run(lua,
      "local m=new_model(); "
      "assert(m:create_transcriber({language=string.rep('e',128)}) == nil)\n"
      "assert(not pcall(m.create_transcriber,m,{language='en',progress={}}))\n"
      "m:close(); m=nil; collectgarbage('collect'); "
      "collectgarbage('collect')\n");
  fail_create = 0;
  assert(live_models == 0 && live_transcribers == 0);
  run(lua, "for i=1,30 do\n"
           "  local frames={}; for j=1,1600 do frames[j]=0 end\n"
           "  frames[i%2 == 0 and 1 or 1600] = {}\n"
           "  assert(not pcall(read_frames,frames))\n"
           "end\n"
           "assert(read_frames({1,2,3})==3); assert(read_frames({})==0)\n");
  assert(frame_allocations == 0u);
  run(lua, "logs=0; local co=coroutine.create(function()\n"
           "sus.set_log_sink(function(e) assert(e.message=='fixture'); "
           "logs=logs+1 end)\n"
           "end); weak.co=co; assert(coroutine.resume(co)); co=nil\n"
           "collectgarbage('collect'); assert(weak.co==nil)\n"
           "assert(not pcall(sus.set_log_sink, {}))\n");
  emit_log();
  assert(pthread_create(&emitter, NULL, emit_foreign, NULL) == 0);
  assert(pthread_join(emitter, NULL) == 0);
  run(lua, "assert(logs==1); local n,s=sus.pump_logs(0); assert(n==0 and "
           "s.queued==64 and s.overflow==3 and s.oversize==1)\n"
           "n,s=sus.pump_logs(2); assert(n==2 and logs==3 and s.queued==62)\n"
           "n,s=sus.pump_logs(); assert(n==62 and logs==65 and s.queued==0)\n"
           "assert(not pcall(sus.pump_logs,-1)); assert(not "
           "pcall(sus.pump_logs,65))\n"
           "sus.set_log_sink(function() error('log callback fault') end)\n");
  emit_log();
  run(lua, "local n,s=sus.pump_logs(0); assert(n==0 and s.callback_errors==1)\n"
           "sus.set_log_sink(function() logs=logs+1 end)\n");
  assert(pthread_create(&emitter, NULL, emit_foreign, NULL) == 0);
  assert(pthread_join(emitter, NULL) == 0);
  run(lua, "sus.set_log_sink(function() logs=logs+1 end); local "
           "n,s=sus.pump_logs(); assert(n==0 and s.queued==0)\n"
           "assert(sus.set_log_sink(nil))");
  assert(native_log_sink == vectis_sus_lua_log_sink &&
         native_log_set_calls == 1);
  run(lua,
      "sus.set_log_sink(function() logs=logs+1 end); before_generation=logs");
  gate_start(&gate, &emitter);
  run(lua, "sus.set_log_sink(function() logs=logs+1 end); "
           "assert(logs==before_generation)");
  gate_finish(&gate, emitter);
  run(lua,
      "local n=sus.pump_logs(); assert(n==1 and logs==before_generation+1)");
  other = new_state();
  run(other, "logs=0; sus.set_log_sink(function() logs=logs+1 end)");
  /* Closing the previous state must not unregister the replacement. */
  lua_close(lua);
  emit_log();
  run(other, "assert(logs==1); assert(sus.set_log_sink(false))\n"
             "sus.set_log_sink(function() end)\n"
             "m=new_model(); t=m:create_transcriber()\n");
  late_sink = native_log_sink;
  gate_start(&gate, &emitter);
  lua_close(other);
  gate_finish(&gate, emitter);
  memset(&late_event, 0, sizeof(late_event));
  late_event.message = "late static callback";
  late_sink(&late_event, NULL);
  assert(native_log_sink == vectis_sus_lua_log_sink &&
         vectis_sus_lua_log == NULL);
  assert(native_log_set_calls == 1);
  check_vm_race();
  assert(vectis_sus_lua_log == NULL && native_log_set_calls == 1);
  assert(live_models == 0 && live_transcribers == 0);
  assert(frame_allocations == 0u);
  return 0;
}
