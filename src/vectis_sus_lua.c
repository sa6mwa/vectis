#include "vectis_audio_lua.h"

#include <cpkt/sus.h>
#include <vectis/vectis.h>

#include <lauxlib.h>
#include <lua.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define VECTIS_SUS_MODEL "sus.model"
#define VECTIS_SUS_TRANSCRIBER "sus.transcriber"

typedef struct vectis_sus_model_lua {
  cpkt_sus *sus;
} vectis_sus_model_lua;

typedef struct vectis_sus_transcriber_lua {
  cpkt_sus_transcriber *transcriber;
  lua_State *lua;
  int segment_ref;
  int progress_ref;
  int abort_ref;
} vectis_sus_transcriber_lua;

typedef struct vectis_sus_cache_status_lua {
  lua_State *lua;
  int ref;
} vectis_sus_cache_status_lua;

typedef struct vectis_sus_segmented_lua {
  lua_State *lua;
  int ref;
} vectis_sus_segmented_lua;

typedef struct vectis_sus_log_lua {
  lua_State *lua;
  int ref;
} vectis_sus_log_lua;

int luaopen_sus(lua_State *lua);

static vectis_sus_log_lua vectis_sus_lua_log = {NULL, LUA_NOREF};

static void vectis_sus_lua_set_string(lua_State *lua, const char *key,
                                      const char *value) {
  lua_pushstring(lua, value != NULL ? value : "");
  lua_setfield(lua, -2, key);
}

static const char *vectis_sus_lua_status_string(vectis_status status) {
  switch (status) {
  case VECTIS_ERR_INVALID:
    return "invalid";
  case VECTIS_ERR_NOMEM:
    return "nomem";
  case VECTIS_ERR_STATE:
    return "state";
  default:
    return "unknown";
  }
}

static int vectis_sus_lua_push_error(lua_State *lua, cpkt_sus_result result,
                                     const char *context) {
  const char *result_string;
  vectis_status status;

  result_string = cpkt_sus_result_string(result);
  switch (result) {
  case CPKT_SUS_ERR_ARG:
    status = VECTIS_ERR_INVALID;
    break;
  case CPKT_SUS_ERR_ALLOC:
    status = VECTIS_ERR_NOMEM;
    break;
  default:
    status = VECTIS_ERR_STATE;
    break;
  }
  lua_pushnil(lua);
  lua_newtable(lua);
  lua_pushinteger(lua, status);
  lua_setfield(lua, -2, "status");
  vectis_sus_lua_set_string(lua, "status_string",
                            vectis_sus_lua_status_string(status));
  lua_pushinteger(lua, VECTIS_ERROR_SOURCE_CPKT);
  lua_setfield(lua, -2, "source_code");
  vectis_sus_lua_set_string(lua, "source", "cpkt");
  vectis_sus_lua_set_string(lua, "dependency", "sus");
  lua_pushinteger(lua, (lua_Integer)result);
  lua_setfield(lua, -2, "dependency_code");
  lua_pushinteger(lua, (lua_Integer)result);
  lua_setfield(lua, -2, "result");
  vectis_sus_lua_set_string(lua, "result_string", result_string);
  if (context != NULL && context[0] != '\0') {
    lua_pushfstring(lua, "%s: %s", context,
                    result_string != NULL ? result_string : "sus error");
  } else {
    lua_pushstring(lua, result_string != NULL ? result_string : "sus error");
  }
  lua_setfield(lua, -2, "message");
  return 2;
}

static int vectis_sus_lua_table_bool(lua_State *lua, int index,
                                     const char *field, int fallback) {
  int value;

  if (index < 0) {
    index = lua_gettop(lua) + index + 1;
  }
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return fallback;
  }
  value = lua_toboolean(lua, -1) ? 1 : 0;
  lua_pop(lua, 1);
  return value;
}

static int vectis_sus_lua_table_int(lua_State *lua, int index,
                                    const char *field, int fallback) {
  lua_Integer value;

  if (index < 0) {
    index = lua_gettop(lua) + index + 1;
  }
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return fallback;
  }
  value = luaL_checkinteger(lua, -1);
  lua_pop(lua, 1);
  return (int)value;
}

static unsigned long vectis_sus_lua_table_ulong(lua_State *lua, int index,
                                                const char *field,
                                                unsigned long fallback) {
  lua_Integer value;

  if (index < 0) {
    index = lua_gettop(lua) + index + 1;
  }
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return fallback;
  }
  value = luaL_checkinteger(lua, -1);
  lua_pop(lua, 1);
  if (value < 0) {
    luaL_error(lua, "sus %s must be non-negative", field);
    return 0u;
  }
  return (unsigned long)value;
}

static float vectis_sus_lua_table_float(lua_State *lua, int index,
                                        const char *field, float fallback) {
  lua_Number value;

  if (index < 0) {
    index = lua_gettop(lua) + index + 1;
  }
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return fallback;
  }
  value = luaL_checknumber(lua, -1);
  lua_pop(lua, 1);
  return (float)value;
}

static const char *vectis_sus_lua_table_string(lua_State *lua, int index,
                                               const char *field) {
  const char *value;

  if (index < 0) {
    index = lua_gettop(lua) + index + 1;
  }
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return NULL;
  }
  value = luaL_checkstring(lua, -1);
  lua_pop(lua, 1);
  return value;
}

static int vectis_sus_lua_table_function_ref(lua_State *lua, int index,
                                             const char *field) {
  int ref;

  if (index < 0) {
    index = lua_gettop(lua) + index + 1;
  }
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return LUA_NOREF;
  }
  luaL_checktype(lua, -1, LUA_TFUNCTION);
  ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  return ref;
}

static void vectis_sus_lua_push_segment(lua_State *lua,
                                        const cpkt_sus_segment *segment) {
  lua_newtable(lua);
  if (segment->text != NULL) {
    lua_pushlstring(lua, segment->text, (size_t)segment->text_length);
  } else {
    lua_pushliteral(lua, "");
  }
  lua_setfield(lua, -2, "text");
  lua_pushinteger(lua, (lua_Integer)segment->text_length);
  lua_setfield(lua, -2, "text_length");
  lua_pushinteger(lua, (lua_Integer)segment->t0);
  lua_setfield(lua, -2, "t0");
  lua_pushinteger(lua, (lua_Integer)segment->t1);
  lua_setfield(lua, -2, "t1");
}

static void
vectis_sus_lua_push_segmented_event(lua_State *lua,
                                    const cpkt_sus_segmented_event *event) {
  lua_newtable(lua);
  if (event->text != NULL) {
    lua_pushlstring(lua, event->text, (size_t)event->text_length);
  } else {
    lua_pushliteral(lua, "");
  }
  lua_setfield(lua, -2, "text");
  lua_pushinteger(lua, (lua_Integer)event->text_length);
  lua_setfield(lua, -2, "text_length");
  lua_pushinteger(lua, (lua_Integer)event->t0);
  lua_setfield(lua, -2, "t0");
  lua_pushinteger(lua, (lua_Integer)event->t1);
  lua_setfield(lua, -2, "t1");
  lua_pushinteger(lua, (lua_Integer)event->step_index);
  lua_setfield(lua, -2, "step_index");
  lua_pushboolean(lua, event->is_final != 0);
  lua_setfield(lua, -2, "is_final");
}

static int vectis_sus_lua_continue_callback_result(lua_State *lua, int index) {
  if (lua_type(lua, index) == LUA_TBOOLEAN) {
    return lua_toboolean(lua, index) ? 0 : -1;
  }
  if (lua_type(lua, index) == LUA_TNUMBER) {
    return lua_tointeger(lua, index) == 0 ? 0 : -1;
  }
  return 0;
}

static int vectis_sus_lua_abort_callback_result(lua_State *lua, int index) {
  if (lua_type(lua, index) == LUA_TBOOLEAN) {
    return lua_toboolean(lua, index) ? 1 : 0;
  }
  if (lua_type(lua, index) == LUA_TNUMBER) {
    return lua_tointeger(lua, index) == 0 ? 0 : 1;
  }
  return 0;
}

static int vectis_sus_lua_segment_sink(const cpkt_sus_segment *segment,
                                       void *user) {
  vectis_sus_transcriber_lua *handle;
  lua_State *lua;
  int top;
  int failed;

  handle = (vectis_sus_transcriber_lua *)user;
  lua = handle->lua;
  if (handle->segment_ref == LUA_NOREF) {
    return 0;
  }
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, handle->segment_ref);
  vectis_sus_lua_push_segment(lua, segment);
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    lua_settop(lua, top);
    return -1;
  }
  failed = vectis_sus_lua_continue_callback_result(lua, -1);
  lua_settop(lua, top);
  return failed;
}

static int vectis_sus_lua_progress_sink(int progress, void *user) {
  vectis_sus_transcriber_lua *handle;
  lua_State *lua;
  int top;
  int failed;

  handle = (vectis_sus_transcriber_lua *)user;
  lua = handle->lua;
  if (handle->progress_ref == LUA_NOREF) {
    return 0;
  }
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, handle->progress_ref);
  lua_pushinteger(lua, (lua_Integer)progress);
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    lua_settop(lua, top);
    return -1;
  }
  failed = vectis_sus_lua_continue_callback_result(lua, -1);
  lua_settop(lua, top);
  return failed;
}

static int vectis_sus_lua_abort_fn(void *user) {
  vectis_sus_transcriber_lua *handle;
  lua_State *lua;
  int top;
  int abort_requested;

  handle = (vectis_sus_transcriber_lua *)user;
  lua = handle->lua;
  if (handle->abort_ref == LUA_NOREF) {
    return 0;
  }
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, handle->abort_ref);
  if (lua_pcall(lua, 0, 1, 0) != LUA_OK) {
    lua_settop(lua, top);
    return 1;
  }
  abort_requested = vectis_sus_lua_abort_callback_result(lua, -1);
  lua_settop(lua, top);
  return abort_requested;
}

static int vectis_sus_lua_segmented_sink(const cpkt_sus_segmented_event *event,
                                         void *user) {
  vectis_sus_segmented_lua *sink;
  lua_State *lua;
  int top;
  int failed;

  sink = (vectis_sus_segmented_lua *)user;
  lua = sink->lua;
  if (sink->ref == LUA_NOREF) {
    return 0;
  }
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, sink->ref);
  vectis_sus_lua_push_segmented_event(lua, event);
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    lua_settop(lua, top);
    return -1;
  }
  failed = vectis_sus_lua_continue_callback_result(lua, -1);
  lua_settop(lua, top);
  return failed;
}

static void vectis_sus_lua_log_sink(const cpkt_sus_log_event *event,
                                    void *user) {
  vectis_sus_log_lua *sink;
  lua_State *lua;
  int top;

  sink = (vectis_sus_log_lua *)user;
  if (sink == NULL || sink->lua == NULL || sink->ref == LUA_NOREF ||
      event == NULL) {
    return;
  }
  lua = sink->lua;
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, sink->ref);
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)event->level);
  lua_setfield(lua, -2, "level");
  vectis_sus_lua_set_string(lua, "component", event->component);
  vectis_sus_lua_set_string(lua, "message", event->message);
  if (lua_pcall(lua, 1, 0, 0) != LUA_OK) {
    lua_settop(lua, top);
    return;
  }
  lua_settop(lua, top);
}

static int
vectis_sus_lua_cache_status_sink(const cpkt_sus_cache_status_event *event,
                                 void *user) {
  vectis_sus_cache_status_lua *status;
  lua_State *lua;
  int top;
  int failed;

  status = (vectis_sus_cache_status_lua *)user;
  lua = status->lua;
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, status->ref);
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)event->phase);
  lua_setfield(lua, -2, "phase");
  vectis_sus_lua_set_string(lua, "model", event->model);
  vectis_sus_lua_set_string(lua, "cache_path", event->cache_path);
  vectis_sus_lua_set_string(lua, "source_url", event->source_url);
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    lua_settop(lua, top);
    return -1;
  }
  failed = 0;
  if (lua_type(lua, -1) == LUA_TNUMBER) {
    failed = lua_tointeger(lua, -1) == 0 ? 0 : -1;
  } else if (lua_type(lua, -1) == LUA_TBOOLEAN) {
    failed = lua_toboolean(lua, -1) ? 0 : -1;
  }
  lua_settop(lua, top);
  return failed;
}

static float *vectis_sus_lua_frame_array(lua_State *lua, int index,
                                         size_t *out_count) {
  lua_Unsigned raw_count;
  size_t count;
  size_t i;
  float *frames;

  luaL_checktype(lua, index, LUA_TTABLE);
  raw_count = (lua_Unsigned)lua_rawlen(lua, index);
  if (raw_count > (((size_t)-1) / sizeof(float))) {
    luaL_error(lua, "sus frame array is too large");
    return NULL;
  }
  count = (size_t)raw_count;
  if (count == 0u) {
    *out_count = 0u;
    return NULL;
  }
  frames = (float *)malloc(count * sizeof(float));
  if (frames == NULL) {
    luaL_error(lua, "sus frame allocation failed");
    return NULL;
  }
  for (i = 0u; i < count; i++) {
    lua_rawgeti(lua, index, (lua_Integer)i + 1);
    frames[i] = (float)luaL_checknumber(lua, -1);
    lua_pop(lua, 1);
  }
  *out_count = count;
  return frames;
}

static cpkt_sus_segment_mode vectis_sus_lua_segment_mode(lua_State *lua,
                                                         int index) {
  const char *name;

  if (lua_type(lua, index) == LUA_TNUMBER) {
    return (cpkt_sus_segment_mode)luaL_checkinteger(lua, index);
  }
  name = luaL_checkstring(lua, index);
  if (strcmp(name, "simplex") == 0) {
    return CPKT_SUS_SEGMENT_MODE_SIMPLEX;
  }
  if (strcmp(name, "continuous") == 0) {
    return CPKT_SUS_SEGMENT_MODE_CONTINUOUS;
  }
  return (cpkt_sus_segment_mode)luaL_error(
      lua, "unsupported sus segment mode: %s", name);
}

static void vectis_sus_lua_segmented_config(lua_State *lua, int index,
                                            cpkt_sus_segmented_config *config,
                                            vectis_sus_segmented_lua *sink) {
  cpkt_sus_segmented_config_default(config);
  sink->lua = lua;
  sink->ref = LUA_NOREF;
  if (index == 0 || lua_isnoneornil(lua, index)) {
    return;
  }
  luaL_checktype(lua, index, LUA_TTABLE);
  lua_getfield(lua, index, "mode");
  if (!lua_isnil(lua, -1)) {
    config->mode = vectis_sus_lua_segment_mode(lua, -1);
  }
  lua_pop(lua, 1);
  config->read_frames =
      vectis_sus_lua_table_ulong(lua, index, "read_frames", 0u);
  config->step_ms = vectis_sus_lua_table_ulong(lua, index, "step_ms", 0u);
  config->length_ms = vectis_sus_lua_table_ulong(lua, index, "length_ms", 0u);
  config->keep_ms = vectis_sus_lua_table_ulong(lua, index, "keep_ms", 0u);
  config->keep_context =
      vectis_sus_lua_table_int(lua, index, "keep_context", 0);
  config->vox_threshold =
      vectis_sus_lua_table_float(lua, index, "vox_threshold", 0.0f);
  if (config->vox_threshold == 0.0f) {
    config->vox_threshold =
        vectis_sus_lua_table_float(lua, index, "threshold", 0.0f);
  }
  config->prebuffer_ms =
      vectis_sus_lua_table_ulong(lua, index, "prebuffer_ms", 0u);
  config->memory_spool_bytes =
      vectis_sus_lua_table_ulong(lua, index, "memory_spool_bytes", 0u);
  config->max_spool_bytes =
      vectis_sus_lua_table_ulong(lua, index, "max_spool_bytes", 0u);
  config->audio_ctx = vectis_sus_lua_table_ulong(lua, index, "audio_ctx", 0u);
  config->max_tokens = vectis_sus_lua_table_ulong(lua, index, "max_tokens", 0u);
  sink->ref = vectis_sus_lua_table_function_ref(lua, index, "segmented");
  if (sink->ref == LUA_NOREF) {
    sink->ref = vectis_sus_lua_table_function_ref(lua, index, "on_segmented");
  }
  if (sink->ref == LUA_NOREF) {
    sink->ref = vectis_sus_lua_table_function_ref(lua, index, "on_transcript");
  }
  if (sink->ref != LUA_NOREF) {
    config->segmented_sink = vectis_sus_lua_segmented_sink;
    config->segmented_user = sink;
  }
}

static void vectis_sus_lua_push_model_entry(lua_State *lua,
                                            const cpkt_sus_model_entry *entry) {
  lua_newtable(lua);
  vectis_sus_lua_set_string(lua, "name", entry->name);
  vectis_sus_lua_set_string(lua, "provider", entry->provider);
  vectis_sus_lua_set_string(lua, "source_url", entry->source_url);
  vectis_sus_lua_set_string(lua, "filename", entry->filename);
  vectis_sus_lua_set_string(lua, "sha256", entry->sha256);
  lua_pushinteger(lua, (lua_Integer)entry->size_bytes);
  lua_setfield(lua, -2, "size_bytes");
  vectis_sus_lua_set_string(lua, "license", entry->license);
  vectis_sus_lua_set_string(lua, "quantization", entry->quantization);
  lua_pushboolean(lua, entry->is_default != 0);
  lua_setfield(lua, -2, "is_default");
}

static vectis_sus_model_lua *vectis_sus_lua_new_model(lua_State *lua,
                                                      cpkt_sus *sus) {
  vectis_sus_model_lua *handle;

  handle = (vectis_sus_model_lua *)lua_newuserdatauv(lua, sizeof(*handle), 0);
  handle->sus = sus;
  luaL_getmetatable(lua, VECTIS_SUS_MODEL);
  lua_setmetatable(lua, -2);
  return handle;
}

static vectis_sus_model_lua *vectis_sus_lua_check_model(lua_State *lua,
                                                        int index) {
  return (vectis_sus_model_lua *)luaL_checkudata(lua, index, VECTIS_SUS_MODEL);
}

static vectis_sus_transcriber_lua *
vectis_sus_lua_check_transcriber(lua_State *lua, int index) {
  return (vectis_sus_transcriber_lua *)luaL_checkudata(lua, index,
                                                       VECTIS_SUS_TRANSCRIBER);
}

static void
vectis_sus_lua_transcriber_unref(vectis_sus_transcriber_lua *handle) {
  if (handle == NULL || handle->lua == NULL) {
    return;
  }
  if (handle->segment_ref != LUA_NOREF) {
    luaL_unref(handle->lua, LUA_REGISTRYINDEX, handle->segment_ref);
    handle->segment_ref = LUA_NOREF;
  }
  if (handle->progress_ref != LUA_NOREF) {
    luaL_unref(handle->lua, LUA_REGISTRYINDEX, handle->progress_ref);
    handle->progress_ref = LUA_NOREF;
  }
  if (handle->abort_ref != LUA_NOREF) {
    luaL_unref(handle->lua, LUA_REGISTRYINDEX, handle->abort_ref);
    handle->abort_ref = LUA_NOREF;
  }
}

static vectis_sus_transcriber_lua *
vectis_sus_lua_new_transcriber(lua_State *lua) {
  vectis_sus_transcriber_lua *handle;

  handle =
      (vectis_sus_transcriber_lua *)lua_newuserdatauv(lua, sizeof(*handle), 0);
  handle->transcriber = NULL;
  handle->lua = lua;
  handle->segment_ref = LUA_NOREF;
  handle->progress_ref = LUA_NOREF;
  handle->abort_ref = LUA_NOREF;
  luaL_getmetatable(lua, VECTIS_SUS_TRANSCRIBER);
  lua_setmetatable(lua, -2);
  return handle;
}

static int vectis_sus_lua_model_close(lua_State *lua) {
  vectis_sus_model_lua *handle;

  handle = vectis_sus_lua_check_model(lua, 1);
  if (handle->sus != NULL) {
    handle->sus->destroy(handle->sus);
    handle->sus = NULL;
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_sus_lua_transcriber_close(lua_State *lua) {
  vectis_sus_transcriber_lua *handle;

  handle = vectis_sus_lua_check_transcriber(lua, 1);
  if (handle->transcriber != NULL) {
    handle->transcriber->destroy(handle->transcriber);
    handle->transcriber = NULL;
  }
  vectis_sus_lua_transcriber_unref(handle);
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_sus_lua_model_info(lua_State *lua) {
  vectis_sus_model_lua *handle;
  cpkt_sus_info info;
  cpkt_sus_result result;

  handle = vectis_sus_lua_check_model(lua, 1);
  if (handle->sus == NULL) {
    return luaL_error(lua, "sus model is closed");
  }
  memset(&info, 0, sizeof(info));
  result = handle->sus->info(handle->sus, &info);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result, "sus model info");
  }
  lua_newtable(lua);
  vectis_sus_lua_set_string(lua, "backend_version", info.backend_version);
  vectis_sus_lua_set_string(lua, "backend_system_info",
                            info.backend_system_info);
  lua_pushboolean(lua, info.cpu_only != 0);
  lua_setfield(lua, -2, "cpu_only");
  return 1;
}

static void
vectis_sus_lua_transcriber_config(lua_State *lua, int index,
                                  cpkt_sus_transcriber_config *config,
                                  vectis_sus_transcriber_lua *handle) {
  cpkt_sus_transcriber_config_default(config);
  if (index == 0 || lua_isnoneornil(lua, index)) {
    return;
  }
  luaL_checktype(lua, index, LUA_TTABLE);
  config->threads = vectis_sus_lua_table_int(lua, index, "threads", 0);
  config->cpu_only = vectis_sus_lua_table_bool(lua, index, "cpu_only", 0);
  config->language = vectis_sus_lua_table_string(lua, index, "language");
  config->translate = vectis_sus_lua_table_bool(lua, index, "translate", 0);
  config->timestamps = vectis_sus_lua_table_bool(lua, index, "timestamps", 0);
  config->initial_prompt =
      vectis_sus_lua_table_string(lua, index, "initial_prompt");
  handle->segment_ref =
      vectis_sus_lua_table_function_ref(lua, index, "segment");
  if (handle->segment_ref == LUA_NOREF) {
    handle->segment_ref =
        vectis_sus_lua_table_function_ref(lua, index, "on_segment");
  }
  handle->progress_ref =
      vectis_sus_lua_table_function_ref(lua, index, "progress");
  if (handle->progress_ref == LUA_NOREF) {
    handle->progress_ref =
        vectis_sus_lua_table_function_ref(lua, index, "on_progress");
  }
  handle->abort_ref = vectis_sus_lua_table_function_ref(lua, index, "abort");
  if (handle->abort_ref == LUA_NOREF) {
    handle->abort_ref =
        vectis_sus_lua_table_function_ref(lua, index, "should_abort");
  }
  if (handle->segment_ref != LUA_NOREF) {
    config->segment_sink = vectis_sus_lua_segment_sink;
    config->segment_user = handle;
  }
  if (handle->progress_ref != LUA_NOREF) {
    config->progress_sink = vectis_sus_lua_progress_sink;
    config->progress_user = handle;
  }
  if (handle->abort_ref != LUA_NOREF) {
    config->abort = vectis_sus_lua_abort_fn;
    config->abort_user = handle;
  }
}

static int vectis_sus_lua_model_create_transcriber(lua_State *lua) {
  vectis_sus_model_lua *model;
  vectis_sus_transcriber_lua *handle;
  cpkt_sus_transcriber_config config;
  cpkt_sus_result result;

  model = vectis_sus_lua_check_model(lua, 1);
  if (model->sus == NULL) {
    return luaL_error(lua, "sus model is closed");
  }
  handle = vectis_sus_lua_new_transcriber(lua);
  vectis_sus_lua_transcriber_config(lua, 2, &config, handle);
  result =
      model->sus->create_transcriber(model->sus, &handle->transcriber, &config);
  if (result != CPKT_SUS_OK) {
    vectis_sus_lua_transcriber_unref(handle);
    lua_pop(lua, 1);
    return vectis_sus_lua_push_error(lua, result, "sus create_transcriber");
  }
  return 1;
}

static int vectis_sus_lua_model_reset_spacing(lua_State *lua) {
  vectis_sus_model_lua *handle;
  cpkt_sus_result result;

  handle = vectis_sus_lua_check_model(lua, 1);
  if (handle->sus == NULL) {
    return luaL_error(lua, "sus model is closed");
  }
  result = handle->sus->reset_transcript_spacing(handle->sus);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result,
                                     "sus reset_transcript_spacing");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_sus_lua_transcriber_transcribe(lua_State *lua) {
  vectis_sus_transcriber_lua *handle;
  float *frames;
  size_t frame_count;
  cpkt_sus_result result;

  handle = vectis_sus_lua_check_transcriber(lua, 1);
  if (handle->transcriber == NULL) {
    return luaL_error(lua, "sus transcriber is closed");
  }
  frame_count = 0u;
  frames = vectis_sus_lua_frame_array(lua, 2, &frame_count);
  result = handle->transcriber->transcribe_f32_mono_16k(
      handle->transcriber, frames, (unsigned long)frame_count);
  free(frames);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result,
                                     "sus transcribe_f32_mono_16k");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_sus_lua_transcriber_transcribe_text(lua_State *lua) {
  vectis_sus_transcriber_lua *handle;
  float *frames;
  size_t frame_count;
  char *text;
  cpkt_sus_result result;

  handle = vectis_sus_lua_check_transcriber(lua, 1);
  if (handle->transcriber == NULL) {
    return luaL_error(lua, "sus transcriber is closed");
  }
  frame_count = 0u;
  text = NULL;
  frames = vectis_sus_lua_frame_array(lua, 2, &frame_count);
  result = handle->transcriber->transcribe_f32_mono_16k_text(
      handle->transcriber, frames, (unsigned long)frame_count, &text);
  free(frames);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result,
                                     "sus transcribe_f32_mono_16k_text");
  }
  lua_pushstring(lua, text != NULL ? text : "");
  if (text != NULL) {
    cpkt_sus_string_free(text);
  }
  return 1;
}

static int vectis_sus_lua_transcriber_transcribe_decoder(lua_State *lua) {
  vectis_sus_transcriber_lua *handle;
  cpkt_audio_decoder *decoder;
  cpkt_sus_segmented_config config;
  vectis_sus_segmented_lua sink;
  cpkt_sus_result result;

  handle = vectis_sus_lua_check_transcriber(lua, 1);
  if (handle->transcriber == NULL) {
    return luaL_error(lua, "sus transcriber is closed");
  }
  if (!vectis_audio_lua_borrow_decoder(lua, 2, &decoder)) {
    return vectis_sus_lua_push_error(lua, CPKT_SUS_ERR_ARG,
                                     "sus segmented decoder transcription");
  }
  vectis_sus_lua_segmented_config(lua, 3, &config, &sink);
  result = handle->transcriber->transcribe_audio_decoder_segmented(
      handle->transcriber, decoder, &config);
  if (sink.ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, sink.ref);
  }
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result,
                                     "sus transcribe_audio_decoder_segmented");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_sus_lua_transcriber_transcribe_decoder_text(lua_State *lua) {
  vectis_sus_transcriber_lua *handle;
  cpkt_audio_decoder *decoder;
  cpkt_sus_segmented_config config;
  vectis_sus_segmented_lua sink;
  cpkt_sus_result result;
  char *text;

  handle = vectis_sus_lua_check_transcriber(lua, 1);
  if (handle->transcriber == NULL) {
    return luaL_error(lua, "sus transcriber is closed");
  }
  if (!vectis_audio_lua_borrow_decoder(lua, 2, &decoder)) {
    return vectis_sus_lua_push_error(
        lua, CPKT_SUS_ERR_ARG, "sus segmented decoder transcription text");
  }
  text = NULL;
  vectis_sus_lua_segmented_config(lua, 3, &config, &sink);
  result = handle->transcriber->transcribe_audio_decoder_segmented_text(
      handle->transcriber, decoder, &config, &text);
  if (sink.ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, sink.ref);
  }
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(
        lua, result, "sus transcribe_audio_decoder_segmented_text");
  }
  lua_pushstring(lua, text != NULL ? text : "");
  if (text != NULL) {
    cpkt_sus_string_free(text);
  }
  return 1;
}

static int vectis_sus_lua_transcriber_transcribe_vox_segment(lua_State *lua) {
  vectis_sus_transcriber_lua *handle;
  cpkt_audio_vox_segment *segment;
  cpkt_sus_segmented_config config;
  vectis_sus_segmented_lua sink;
  cpkt_sus_result result;

  handle = vectis_sus_lua_check_transcriber(lua, 1);
  if (handle->transcriber == NULL) {
    return luaL_error(lua, "sus transcriber is closed");
  }
  if (!vectis_audio_lua_borrow_vox_segment(lua, 2, &segment)) {
    return vectis_sus_lua_push_error(lua, CPKT_SUS_ERR_ARG,
                                     "sus VOX segment transcription");
  }
  vectis_sus_lua_segmented_config(lua, 3, &config, &sink);
  result = handle->transcriber->transcribe_audio_vox_segment(
      handle->transcriber, segment, &config);
  if (sink.ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, sink.ref);
  }
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result,
                                     "sus transcribe_audio_vox_segment");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_sus_lua_transcriber_revised_text(lua_State *lua) {
  vectis_sus_transcriber_lua *handle;
  cpkt_sus_result result;
  char *text;

  handle = vectis_sus_lua_check_transcriber(lua, 1);
  if (handle->transcriber == NULL) {
    return luaL_error(lua, "sus transcriber is closed");
  }
  text = NULL;
  result = handle->transcriber->revised_text(handle->transcriber, &text);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result, "sus revised_text");
  }
  lua_pushstring(lua, text != NULL ? text : "");
  if (text != NULL) {
    cpkt_sus_string_free(text);
  }
  return 1;
}

static int vectis_sus_lua_result_string(lua_State *lua) {
  lua_pushstring(
      lua, cpkt_sus_result_string((cpkt_sus_result)luaL_checkinteger(lua, 1)));
  return 1;
}

static int vectis_sus_lua_backend_version(lua_State *lua) {
  lua_pushstring(lua, cpkt_sus_backend_version());
  return 1;
}

static int vectis_sus_lua_backend_system_info(lua_State *lua) {
  lua_pushstring(lua, cpkt_sus_backend_system_info());
  return 1;
}

static int vectis_sus_lua_backend_capabilities(lua_State *lua) {
  lua_pushstring(lua, cpkt_sus_backend_capabilities());
  return 1;
}

static int vectis_sus_lua_facade_version(lua_State *lua) {
  lua_pushstring(lua, cpkt_sus_facade_version());
  return 1;
}

static int vectis_sus_lua_set_log_sink(lua_State *lua) {
  if (vectis_sus_lua_log.lua != NULL && vectis_sus_lua_log.ref != LUA_NOREF) {
    luaL_unref(vectis_sus_lua_log.lua, LUA_REGISTRYINDEX,
               vectis_sus_lua_log.ref);
  }
  vectis_sus_lua_log.lua = NULL;
  vectis_sus_lua_log.ref = LUA_NOREF;
  if (lua_isnoneornil(lua, 1) ||
      (lua_type(lua, 1) == LUA_TBOOLEAN && !lua_toboolean(lua, 1))) {
    cpkt_sus_log_set(NULL, NULL);
    lua_pushboolean(lua, 1);
    return 1;
  }
  luaL_checktype(lua, 1, LUA_TFUNCTION);
  lua_pushvalue(lua, 1);
  vectis_sus_lua_log.ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  vectis_sus_lua_log.lua = lua;
  cpkt_sus_log_set(vectis_sus_lua_log_sink, &vectis_sus_lua_log);
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_sus_lua_catalog_count(lua_State *lua) {
  lua_pushinteger(lua, (lua_Integer)cpkt_sus_model_catalog_count());
  return 1;
}

static int vectis_sus_lua_catalog_entry(lua_State *lua) {
  cpkt_sus_model_entry entry;
  cpkt_sus_result result;
  lua_Integer index;

  index = luaL_checkinteger(lua, 1);
  if (index < 1) {
    return luaL_error(lua, "sus catalog index is 1-based");
  }
  memset(&entry, 0, sizeof(entry));
  result = cpkt_sus_model_catalog_entry((unsigned long)(index - 1), &entry);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result, "sus catalog entry");
  }
  vectis_sus_lua_push_model_entry(lua, &entry);
  return 1;
}

static int vectis_sus_lua_catalog_default(lua_State *lua) {
  cpkt_sus_model_entry entry;
  cpkt_sus_result result;

  memset(&entry, 0, sizeof(entry));
  result = cpkt_sus_model_catalog_default(&entry);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result, "sus catalog default");
  }
  vectis_sus_lua_push_model_entry(lua, &entry);
  return 1;
}

static int vectis_sus_lua_catalog_find(lua_State *lua) {
  cpkt_sus_model_entry entry;
  cpkt_sus_result result;
  const char *name;

  name = NULL;
  if (!lua_isnoneornil(lua, 1)) {
    name = luaL_checkstring(lua, 1);
  }
  memset(&entry, 0, sizeof(entry));
  result = cpkt_sus_model_catalog_find(name, &entry);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result, "sus catalog find");
  }
  vectis_sus_lua_push_model_entry(lua, &entry);
  return 1;
}

static int vectis_sus_lua_open_path(lua_State *lua) {
  cpkt_sus_config config;
  cpkt_sus *sus;
  cpkt_sus_result result;

  luaL_checktype(lua, 1, LUA_TTABLE);
  cpkt_sus_config_default(&config);
  config.model_path = vectis_sus_lua_table_string(lua, 1, "model_path");
  if (config.model_path == NULL) {
    config.model_path = vectis_sus_lua_table_string(lua, 1, "path");
  }
  config.cpu_only = vectis_sus_lua_table_bool(lua, 1, "cpu_only", 0);
  config.preserve_initial_space_after_first_transcriber =
      vectis_sus_lua_table_bool(
          lua, 1, "preserve_initial_space_after_first_transcriber", 0);
  sus = NULL;
  result = cpkt_sus_open_path(&sus, &config);
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result, "sus open_path");
  }
  vectis_sus_lua_new_model(lua, sus);
  return 1;
}

static int vectis_sus_lua_open_cached(lua_State *lua) {
  cpkt_sus_cache_config config;
  cpkt_sus *sus;
  cpkt_sus_result result;
  vectis_sus_cache_status_lua status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  cpkt_sus_cache_config_default(&config);
  config.model = vectis_sus_lua_table_string(lua, 1, "model");
  config.cache_dir = vectis_sus_lua_table_string(lua, 1, "cache_dir");
  config.sha256 = vectis_sus_lua_table_string(lua, 1, "sha256");
  config.source_url = vectis_sus_lua_table_string(lua, 1, "source_url");
  config.insecure_no_checksum =
      vectis_sus_lua_table_bool(lua, 1, "insecure_no_checksum", 0);
  config.offline = vectis_sus_lua_table_bool(lua, 1, "offline", 0);
  config.cpu_only = vectis_sus_lua_table_bool(lua, 1, "cpu_only", 0);
  config.preserve_initial_space_after_first_transcriber =
      vectis_sus_lua_table_bool(
          lua, 1, "preserve_initial_space_after_first_transcriber", 0);
  status.lua = lua;
  status.ref = vectis_sus_lua_table_function_ref(lua, 1, "status");
  if (status.ref != LUA_NOREF) {
    config.status_sink = vectis_sus_lua_cache_status_sink;
    config.status_user = &status;
  }
  sus = NULL;
  result = cpkt_sus_open_cached(&sus, &config);
  if (status.ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, status.ref);
  }
  if (result != CPKT_SUS_OK) {
    return vectis_sus_lua_push_error(lua, result, "sus open_cached");
  }
  vectis_sus_lua_new_model(lua, sus);
  return 1;
}

static void vectis_sus_lua_set_constants(lua_State *lua) {
  lua_pushinteger(lua, CPKT_SUS_OK);
  lua_setfield(lua, -2, "OK");
  lua_pushinteger(lua, CPKT_SUS_ERR_ARG);
  lua_setfield(lua, -2, "ERR_ARG");
  lua_pushinteger(lua, CPKT_SUS_ERR_ALLOC);
  lua_setfield(lua, -2, "ERR_ALLOC");
  lua_pushinteger(lua, CPKT_SUS_ERR_MODEL);
  lua_setfield(lua, -2, "ERR_MODEL");
  lua_pushinteger(lua, CPKT_SUS_ERR_UPSTREAM);
  lua_setfield(lua, -2, "ERR_UPSTREAM");
  lua_pushinteger(lua, CPKT_SUS_ERR_CALLBACK);
  lua_setfield(lua, -2, "ERR_CALLBACK");
  lua_pushinteger(lua, CPKT_SUS_ERR_LOOKUP);
  lua_setfield(lua, -2, "ERR_LOOKUP");
  lua_pushinteger(lua, CPKT_SUS_ERR_IO);
  lua_setfield(lua, -2, "ERR_IO");
  lua_pushinteger(lua, CPKT_SUS_ERR_CHECKSUM);
  lua_setfield(lua, -2, "ERR_CHECKSUM");
  lua_pushinteger(lua, CPKT_SUS_ERR_NETWORK);
  lua_setfield(lua, -2, "ERR_NETWORK");
  lua_pushinteger(lua, CPKT_SUS_ABORTED);
  lua_setfield(lua, -2, "ABORTED");
  lua_pushinteger(lua, CPKT_SUS_CACHE_STATUS_LOOKUP);
  lua_setfield(lua, -2, "CACHE_STATUS_LOOKUP");
  lua_pushinteger(lua, CPKT_SUS_CACHE_STATUS_HIT);
  lua_setfield(lua, -2, "CACHE_STATUS_HIT");
  lua_pushinteger(lua, CPKT_SUS_CACHE_STATUS_MISS);
  lua_setfield(lua, -2, "CACHE_STATUS_MISS");
  lua_pushinteger(lua, CPKT_SUS_CACHE_STATUS_DOWNLOAD_BEGIN);
  lua_setfield(lua, -2, "CACHE_STATUS_DOWNLOAD_BEGIN");
  lua_pushinteger(lua, CPKT_SUS_CACHE_STATUS_DOWNLOAD_COMPLETE);
  lua_setfield(lua, -2, "CACHE_STATUS_DOWNLOAD_COMPLETE");
  lua_pushinteger(lua, CPKT_SUS_CACHE_STATUS_VERIFY_BEGIN);
  lua_setfield(lua, -2, "CACHE_STATUS_VERIFY_BEGIN");
  lua_pushinteger(lua, CPKT_SUS_CACHE_STATUS_VERIFY_COMPLETE);
  lua_setfield(lua, -2, "CACHE_STATUS_VERIFY_COMPLETE");
  lua_pushinteger(lua, CPKT_SUS_CACHE_STATUS_LOAD_BEGIN);
  lua_setfield(lua, -2, "CACHE_STATUS_LOAD_BEGIN");
  lua_pushinteger(lua, CPKT_SUS_LOG_NONE);
  lua_setfield(lua, -2, "LOG_NONE");
  lua_pushinteger(lua, CPKT_SUS_LOG_DEBUG);
  lua_setfield(lua, -2, "LOG_DEBUG");
  lua_pushinteger(lua, CPKT_SUS_LOG_INFO);
  lua_setfield(lua, -2, "LOG_INFO");
  lua_pushinteger(lua, CPKT_SUS_LOG_WARN);
  lua_setfield(lua, -2, "LOG_WARN");
  lua_pushinteger(lua, CPKT_SUS_LOG_ERROR);
  lua_setfield(lua, -2, "LOG_ERROR");
  lua_pushinteger(lua, CPKT_SUS_LOG_CONT);
  lua_setfield(lua, -2, "LOG_CONT");
}

int luaopen_sus(lua_State *lua) {
  static const luaL_Reg model_methods[] = {
      {"info", vectis_sus_lua_model_info},
      {"create_transcriber", vectis_sus_lua_model_create_transcriber},
      {"reset_transcript_spacing", vectis_sus_lua_model_reset_spacing},
      {"close", vectis_sus_lua_model_close},
      {"__gc", vectis_sus_lua_model_close},
      {NULL, NULL}};
  static const luaL_Reg transcriber_methods[] = {
      {"transcribe_f32_mono_16k", vectis_sus_lua_transcriber_transcribe},
      {"transcribe_f32_mono_16k_text",
       vectis_sus_lua_transcriber_transcribe_text},
      {"transcribe_audio_decoder_segmented",
       vectis_sus_lua_transcriber_transcribe_decoder},
      {"transcribe_audio_decoder_segmented_text",
       vectis_sus_lua_transcriber_transcribe_decoder_text},
      {"transcribe_audio_vox_segment",
       vectis_sus_lua_transcriber_transcribe_vox_segment},
      {"revised_text", vectis_sus_lua_transcriber_revised_text},
      {"close", vectis_sus_lua_transcriber_close},
      {"__gc", vectis_sus_lua_transcriber_close},
      {NULL, NULL}};
  static const luaL_Reg funcs[] = {
      {"result_string", vectis_sus_lua_result_string},
      {"backend_version", vectis_sus_lua_backend_version},
      {"backend_system_info", vectis_sus_lua_backend_system_info},
      {"backend_capabilities", vectis_sus_lua_backend_capabilities},
      {"facade_version", vectis_sus_lua_facade_version},
      {"set_log_sink", vectis_sus_lua_set_log_sink},
      {"model_catalog_count", vectis_sus_lua_catalog_count},
      {"model_catalog_entry", vectis_sus_lua_catalog_entry},
      {"model_catalog_default", vectis_sus_lua_catalog_default},
      {"model_catalog_find", vectis_sus_lua_catalog_find},
      {"open_path", vectis_sus_lua_open_path},
      {"open_cached", vectis_sus_lua_open_cached},
      {NULL, NULL}};

  luaL_newmetatable(lua, VECTIS_SUS_MODEL);
  lua_pushvalue(lua, -1);
  lua_setfield(lua, -2, "__index");
  luaL_setfuncs(lua, model_methods, 0);
  lua_pop(lua, 1);

  luaL_newmetatable(lua, VECTIS_SUS_TRANSCRIBER);
  lua_pushvalue(lua, -1);
  lua_setfield(lua, -2, "__index");
  luaL_setfuncs(lua, transcriber_methods, 0);
  lua_pop(lua, 1);

  luaL_newlib(lua, funcs);
  vectis_sus_lua_set_constants(lua);
  return 1;
}
