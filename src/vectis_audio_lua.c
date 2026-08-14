#include <cpkt/audio.h>
#include <lauxlib.h>
#include <lua.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define VECTIS_AUDIO_DECODER "audio.decoder"
#define VECTIS_AUDIO_ENCODER "audio.encoder"
#define VECTIS_AUDIO_CAPTURE "audio.capture_handle"
#define VECTIS_AUDIO_PLAYBACK "audio.playback_handle"
#define VECTIS_AUDIO_VOX "audio.vox_handle"
#define VECTIS_AUDIO_PTT "audio.ptt_handle"
#define VECTIS_AUDIO_SEGMENT "audio.segment"

typedef struct vectis_audio_decoder_lua {
  cpkt_audio_decoder *decoder;
  lua_State *lua;
  int read_ref;
  int seek_ref;
} vectis_audio_decoder_lua;

typedef struct vectis_audio_encoder_lua {
  cpkt_audio_encoder *encoder;
  lua_State *lua;
  int write_ref;
  int seek_ref;
  unsigned long channels;
} vectis_audio_encoder_lua;

typedef struct vectis_audio_capture_lua {
  cpkt_audio_capture *capture;
  lua_State *lua;
  int state_ref;
} vectis_audio_capture_lua;

typedef struct vectis_audio_playback_lua {
  cpkt_audio_playback *playback;
} vectis_audio_playback_lua;

typedef struct vectis_audio_segmenter_lua {
  union {
    cpkt_audio_vox *vox;
    cpkt_audio_ptt *ptt;
  } handle;
  lua_State *lua;
  int segment_ref;
  int state_ref;
} vectis_audio_segmenter_lua;

typedef struct vectis_audio_segment_lua {
  cpkt_audio_vox_segment *segment;
} vectis_audio_segment_lua;

int luaopen_audio(lua_State *lua);

static void vectis_audio_lua_set_string(lua_State *lua, const char *key,
                                        const char *value) {
  lua_pushstring(lua, value != NULL ? value : "");
  lua_setfield(lua, -2, key);
}

static int vectis_audio_lua_push_error(lua_State *lua,
                                       cpkt_audio_result result,
                                       const char *context) {
  const char *result_string;

  result_string = cpkt_audio_result_string(result);
  lua_pushnil(lua);
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)result);
  lua_setfield(lua, -2, "result");
  vectis_audio_lua_set_string(lua, "result_string", result_string);
  if (context != NULL && context[0] != '\0') {
    lua_pushfstring(lua, "%s: %s", context,
                    result_string != NULL ? result_string : "audio error");
  } else {
    lua_pushstring(lua, result_string != NULL ? result_string : "audio error");
  }
  lua_setfield(lua, -2, "message");
  return 2;
}

static unsigned long vectis_audio_lua_table_ulong(lua_State *lua, int index,
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
    luaL_error(lua, "audio %s must be non-negative", field);
    return 0u;
  }
  return (unsigned long)value;
}

static double vectis_audio_lua_table_number(lua_State *lua, int index,
                                            const char *field,
                                            double fallback) {
  double value;

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
  return value;
}

static int vectis_audio_lua_table_function_ref(lua_State *lua, int index,
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

static int vectis_audio_lua_format(lua_State *lua, int index) {
  const char *name;

  if (lua_type(lua, index) == LUA_TNUMBER) {
    return (int)luaL_checkinteger(lua, index);
  }
  name = luaL_checkstring(lua, index);
  if (strcmp(name, "wav") == 0) {
    return CPKT_AUDIO_FORMAT_WAV;
  }
  if (strcmp(name, "flac") == 0) {
    return CPKT_AUDIO_FORMAT_FLAC;
  }
  if (strcmp(name, "mp3") == 0) {
    return CPKT_AUDIO_FORMAT_MP3;
  }
  if (strcmp(name, "unknown") == 0) {
    return CPKT_AUDIO_FORMAT_UNKNOWN;
  }
  return (int)luaL_error(lua, "unsupported audio format: %s", name);
}

static int vectis_audio_lua_encoding(lua_State *lua, int index) {
  const char *name;

  if (lua_type(lua, index) == LUA_TNUMBER) {
    return (int)luaL_checkinteger(lua, index);
  }
  name = luaL_checkstring(lua, index);
  if (strcmp(name, "wav") == 0) {
    return CPKT_AUDIO_ENCODING_WAV;
  }
  if (strcmp(name, "flac") == 0) {
    return CPKT_AUDIO_ENCODING_FLAC;
  }
  if (strcmp(name, "mp3") == 0) {
    return CPKT_AUDIO_ENCODING_MP3;
  }
  if (strcmp(name, "unknown") == 0) {
    return CPKT_AUDIO_ENCODING_UNKNOWN;
  }
  return (int)luaL_error(lua, "unsupported audio encoding: %s", name);
}

static void vectis_audio_lua_decoder_config(lua_State *lua, int index,
                                            cpkt_audio_decoder_config *config) {
  memset(config, 0, sizeof(*config));
  if (index == 0 || lua_isnoneornil(lua, index)) {
    return;
  }
  luaL_checktype(lua, index, LUA_TTABLE);
  lua_getfield(lua, index, "encoding");
  if (!lua_isnil(lua, -1)) {
    config->encoding = vectis_audio_lua_encoding(lua, -1);
  }
  lua_pop(lua, 1);
}

static void vectis_audio_lua_encoder_config(lua_State *lua, int index,
                                            cpkt_audio_encoder_config *config) {
  memset(config, 0, sizeof(*config));
  if (index == 0 || lua_isnoneornil(lua, index)) {
    return;
  }
  luaL_checktype(lua, index, LUA_TTABLE);
  lua_getfield(lua, index, "format");
  if (!lua_isnil(lua, -1)) {
    config->format = vectis_audio_lua_format(lua, -1);
  }
  lua_pop(lua, 1);
  config->sample_rate =
      vectis_audio_lua_table_ulong(lua, index, "sample_rate", 0u);
  config->channels = vectis_audio_lua_table_ulong(lua, index, "channels", 0u);
}

static void vectis_audio_lua_capture_config(lua_State *lua, int index,
                                            cpkt_audio_capture_config *config) {
  memset(config, 0, sizeof(*config));
  if (index == 0 || lua_isnoneornil(lua, index)) {
    return;
  }
  luaL_checktype(lua, index, LUA_TTABLE);
  config->backend = (int)vectis_audio_lua_table_ulong(lua, index, "backend", 0u);
  config->buffer_ms =
      vectis_audio_lua_table_ulong(lua, index, "buffer_ms", 0u);
  config->period_ms =
      vectis_audio_lua_table_ulong(lua, index, "period_ms", 0u);
}

static void vectis_audio_lua_playback_config(
    lua_State *lua, int index, cpkt_audio_playback_config *config) {
  memset(config, 0, sizeof(*config));
  if (index == 0 || lua_isnoneornil(lua, index)) {
    return;
  }
  luaL_checktype(lua, index, LUA_TTABLE);
  config->backend = (int)vectis_audio_lua_table_ulong(lua, index, "backend", 0u);
  config->buffer_ms =
      vectis_audio_lua_table_ulong(lua, index, "buffer_ms", 0u);
  config->period_ms =
      vectis_audio_lua_table_ulong(lua, index, "period_ms", 0u);
}

static size_t vectis_audio_lua_read_cb(void *user, void *buffer,
                                       size_t bytes_to_read) {
  vectis_audio_decoder_lua *decoder;
  lua_State *lua;
  const char *chunk;
  size_t chunk_size;
  int top;
  size_t copied;

  decoder = (vectis_audio_decoder_lua *)user;
  lua = decoder->lua;
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, decoder->read_ref);
  lua_pushinteger(lua, (lua_Integer)bytes_to_read);
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    lua_settop(lua, top);
    return 0u;
  }
  if (lua_type(lua, -1) != LUA_TSTRING) {
    lua_settop(lua, top);
    return 0u;
  }
  chunk = lua_tolstring(lua, -1, &chunk_size);
  copied = chunk_size < bytes_to_read ? chunk_size : bytes_to_read;
  memcpy(buffer, chunk, copied);
  lua_settop(lua, top);
  return copied;
}

static int vectis_audio_lua_seek_cb(void *user, long offset, int origin) {
  vectis_audio_decoder_lua *decoder;
  lua_State *lua;
  int top;
  int failed;

  decoder = (vectis_audio_decoder_lua *)user;
  lua = decoder->lua;
  if (decoder->seek_ref == LUA_NOREF) {
    return -1;
  }
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, decoder->seek_ref);
  lua_pushinteger(lua, (lua_Integer)offset);
  lua_pushinteger(lua, (lua_Integer)origin);
  if (lua_pcall(lua, 2, 1, 0) != LUA_OK) {
    lua_settop(lua, top);
    return -1;
  }
  failed = lua_toboolean(lua, -1) ? 0 : -1;
  if (lua_type(lua, -1) == LUA_TNUMBER) {
    failed = lua_tointeger(lua, -1) == 0 ? 0 : -1;
  }
  lua_settop(lua, top);
  return failed;
}

static size_t vectis_audio_lua_write_cb(void *user, const void *buffer,
                                        size_t bytes_to_write) {
  vectis_audio_encoder_lua *encoder;
  lua_State *lua;
  lua_Integer written;
  int top;

  encoder = (vectis_audio_encoder_lua *)user;
  lua = encoder->lua;
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, encoder->write_ref);
  lua_pushlstring(lua, (const char *)buffer, bytes_to_write);
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    lua_settop(lua, top);
    return 0u;
  }
  if (lua_isnil(lua, -1)) {
    lua_settop(lua, top);
    return bytes_to_write;
  }
  if (lua_type(lua, -1) != LUA_TNUMBER) {
    lua_settop(lua, top);
    return 0u;
  }
  written = lua_tointeger(lua, -1);
  lua_settop(lua, top);
  if (written < 0) {
    return 0u;
  }
  if ((size_t)written > bytes_to_write) {
    return bytes_to_write;
  }
  return (size_t)written;
}

static int vectis_audio_lua_writer_seek_cb(void *user, long offset,
                                           int origin) {
  vectis_audio_encoder_lua *encoder;
  lua_State *lua;
  int top;
  int failed;

  encoder = (vectis_audio_encoder_lua *)user;
  lua = encoder->lua;
  if (encoder->seek_ref == LUA_NOREF) {
    return -1;
  }
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, encoder->seek_ref);
  lua_pushinteger(lua, (lua_Integer)offset);
  lua_pushinteger(lua, (lua_Integer)origin);
  if (lua_pcall(lua, 2, 1, 0) != LUA_OK) {
    lua_settop(lua, top);
    return -1;
  }
  failed = lua_toboolean(lua, -1) ? 0 : -1;
  if (lua_type(lua, -1) == LUA_TNUMBER) {
    failed = lua_tointeger(lua, -1) == 0 ? 0 : -1;
  }
  lua_settop(lua, top);
  return failed;
}

static float *vectis_audio_lua_frame_array(lua_State *lua, int index,
                                           size_t *out_count) {
  lua_Unsigned raw_count;
  size_t count;
  size_t i;
  float *frames;

  luaL_checktype(lua, index, LUA_TTABLE);
  raw_count = (lua_Unsigned)lua_rawlen(lua, index);
  if (raw_count > (((size_t)-1) / sizeof(float))) {
    luaL_error(lua, "audio frame array is too large");
    return NULL;
  }
  count = (size_t)raw_count;
  if (count == 0u) {
    *out_count = 0u;
    return NULL;
  }
  frames = (float *)malloc(count * sizeof(float));
  if (frames == NULL) {
    luaL_error(lua, "audio frame allocation failed");
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

static vectis_audio_decoder_lua *
vectis_audio_lua_new_decoder(lua_State *lua, cpkt_audio_decoder *decoder) {
  vectis_audio_decoder_lua *handle;

  handle = (vectis_audio_decoder_lua *)lua_newuserdatauv(lua, sizeof(*handle),
                                                         0);
  handle->decoder = decoder;
  handle->lua = lua;
  handle->read_ref = LUA_NOREF;
  handle->seek_ref = LUA_NOREF;
  luaL_getmetatable(lua, VECTIS_AUDIO_DECODER);
  lua_setmetatable(lua, -2);
  return handle;
}

static vectis_audio_decoder_lua *
vectis_audio_lua_check_decoder(lua_State *lua, int index) {
  return (vectis_audio_decoder_lua *)luaL_checkudata(lua, index,
                                                     VECTIS_AUDIO_DECODER);
}

static int vectis_audio_lua_decoder_close(lua_State *lua) {
  vectis_audio_decoder_lua *handle;

  handle = vectis_audio_lua_check_decoder(lua, 1);
  if (handle->decoder != NULL) {
    handle->decoder->destroy(handle->decoder);
    handle->decoder = NULL;
  }
  if (handle->read_ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, handle->read_ref);
    handle->read_ref = LUA_NOREF;
  }
  if (handle->seek_ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, handle->seek_ref);
    handle->seek_ref = LUA_NOREF;
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_decoder_info(lua_State *lua) {
  vectis_audio_decoder_lua *handle;
  cpkt_audio_stream_info info;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_decoder(lua, 1);
  if (handle->decoder == NULL) {
    return luaL_error(lua, "audio decoder is closed");
  }
  memset(&info, 0, sizeof(info));
  result = handle->decoder->info(handle->decoder, &info);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio decoder info");
  }
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)info.source_format);
  lua_setfield(lua, -2, "source_format");
  lua_pushinteger(lua, (lua_Integer)info.output_sample_rate);
  lua_setfield(lua, -2, "output_sample_rate");
  lua_pushinteger(lua, (lua_Integer)info.output_channels);
  lua_setfield(lua, -2, "output_channels");
  lua_pushinteger(lua, (lua_Integer)info.output_frame_count);
  lua_setfield(lua, -2, "output_frame_count");
  return 1;
}

static int vectis_audio_lua_decoder_read(lua_State *lua) {
  vectis_audio_decoder_lua *handle;
  lua_Integer requested;
  float *frames;
  size_t frames_read;
  size_t i;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_decoder(lua, 1);
  if (handle->decoder == NULL) {
    return luaL_error(lua, "audio decoder is closed");
  }
  requested = luaL_checkinteger(lua, 2);
  if (requested < 0) {
    return luaL_error(lua, "audio decoder frame capacity must be non-negative");
  }
  if ((lua_Unsigned)requested > ((lua_Unsigned)((size_t)-1) / sizeof(float))) {
    return luaL_error(lua, "audio decoder frame capacity is too large");
  }
  frames = NULL;
  if (requested > 0) {
    frames = (float *)malloc((size_t)requested * sizeof(float));
    if (frames == NULL) {
      return luaL_error(lua, "audio decoder frame allocation failed");
    }
  }
  frames_read = 0u;
  result = handle->decoder->read_f32_mono_16k(
      handle->decoder, frames, (size_t)requested, &frames_read);
  if (result != CPKT_AUDIO_OK && result != CPKT_AUDIO_AT_END) {
    free(frames);
    return vectis_audio_lua_push_error(lua, result, "audio decoder read");
  }
  lua_createtable(lua, (int)frames_read, 0);
  for (i = 0u; i < frames_read; i++) {
    lua_pushnumber(lua, (lua_Number)frames[i]);
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  free(frames);
  lua_pushinteger(lua, (lua_Integer)frames_read);
  lua_pushinteger(lua, (lua_Integer)result);
  return 3;
}

static int vectis_audio_lua_decoder_open_file(lua_State *lua) {
  cpkt_audio_decoder_config config;
  cpkt_audio_decoder *decoder;
  cpkt_audio_result result;
  const char *path;
  int config_index;

  if (lua_istable(lua, 1)) {
    lua_getfield(lua, 1, "path");
    path = luaL_checkstring(lua, -1);
    lua_pop(lua, 1);
    config_index = 1;
  } else {
    path = luaL_checkstring(lua, 1);
    config_index = 2;
  }
  vectis_audio_lua_decoder_config(lua, config_index, &config);
  decoder = NULL;
  result = cpkt_audio_decoder_open_file(&decoder, path, &config);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio decoder open_file");
  }
  vectis_audio_lua_new_decoder(lua, decoder);
  return 1;
}

static int vectis_audio_lua_decoder_open_url(lua_State *lua) {
  cpkt_audio_decoder_config config;
  cpkt_audio_decoder *decoder;
  cpkt_audio_result result;
  const char *url;
  int config_index;

  if (lua_istable(lua, 1)) {
    lua_getfield(lua, 1, "url");
    url = luaL_checkstring(lua, -1);
    lua_pop(lua, 1);
    config_index = 1;
  } else {
    url = luaL_checkstring(lua, 1);
    config_index = 2;
  }
  vectis_audio_lua_decoder_config(lua, config_index, &config);
  decoder = NULL;
  result = cpkt_audio_decoder_open_url(&decoder, url, &config);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio decoder open_url");
  }
  vectis_audio_lua_new_decoder(lua, decoder);
  return 1;
}

static int vectis_audio_lua_decoder_open_reader(lua_State *lua) {
  cpkt_audio_decoder_config config;
  cpkt_audio_decoder *decoder;
  cpkt_audio_reader reader;
  cpkt_audio_result result;
  vectis_audio_decoder_lua *handle;

  luaL_checktype(lua, 1, LUA_TTABLE);
  memset(&reader, 0, sizeof(reader));
  vectis_audio_lua_decoder_config(lua, 1, &config);
  handle = vectis_audio_lua_new_decoder(lua, NULL);
  handle->read_ref = vectis_audio_lua_table_function_ref(lua, 1, "read");
  if (handle->read_ref == LUA_NOREF) {
    return luaL_error(lua, "audio decoder reader read callback is required");
  }
  handle->seek_ref = vectis_audio_lua_table_function_ref(lua, 1, "seek");
  reader.user = handle;
  reader.read = vectis_audio_lua_read_cb;
  reader.seek = handle->seek_ref == LUA_NOREF ? NULL : vectis_audio_lua_seek_cb;
  decoder = NULL;
  result = cpkt_audio_decoder_open_reader(&decoder, &reader, &config);
  if (result != CPKT_AUDIO_OK) {
    lua_pop(lua, 1);
    if (handle->read_ref != LUA_NOREF) {
      luaL_unref(lua, LUA_REGISTRYINDEX, handle->read_ref);
    }
    if (handle->seek_ref != LUA_NOREF) {
      luaL_unref(lua, LUA_REGISTRYINDEX, handle->seek_ref);
    }
    return vectis_audio_lua_push_error(lua, result,
                                      "audio decoder open_reader");
  }
  handle->decoder = decoder;
  return 1;
}

static vectis_audio_encoder_lua *
vectis_audio_lua_new_encoder(lua_State *lua, cpkt_audio_encoder *encoder,
                             unsigned long channels) {
  vectis_audio_encoder_lua *handle;

  handle = (vectis_audio_encoder_lua *)lua_newuserdatauv(lua, sizeof(*handle),
                                                         0);
  handle->encoder = encoder;
  handle->lua = lua;
  handle->write_ref = LUA_NOREF;
  handle->seek_ref = LUA_NOREF;
  handle->channels = channels == 0u ? 1u : channels;
  luaL_getmetatable(lua, VECTIS_AUDIO_ENCODER);
  lua_setmetatable(lua, -2);
  return handle;
}

static vectis_audio_encoder_lua *
vectis_audio_lua_check_encoder(lua_State *lua, int index) {
  return (vectis_audio_encoder_lua *)luaL_checkudata(lua, index,
                                                     VECTIS_AUDIO_ENCODER);
}

static int vectis_audio_lua_encoder_close(lua_State *lua) {
  vectis_audio_encoder_lua *handle;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_encoder(lua, 1);
  result = CPKT_AUDIO_OK;
  if (handle->encoder != NULL) {
    result = handle->encoder->close(handle->encoder);
    handle->encoder->destroy(handle->encoder);
    handle->encoder = NULL;
  }
  if (handle->write_ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, handle->write_ref);
    handle->write_ref = LUA_NOREF;
  }
  if (handle->seek_ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, handle->seek_ref);
    handle->seek_ref = LUA_NOREF;
  }
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio encoder close");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_encoder_write_f32(lua_State *lua) {
  vectis_audio_encoder_lua *handle;
  float *frames;
  size_t sample_count;
  size_t frame_count;
  size_t frames_written;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_encoder(lua, 1);
  if (handle->encoder == NULL) {
    return luaL_error(lua, "audio encoder is closed");
  }
  frames = vectis_audio_lua_frame_array(lua, 2, &sample_count);
  if (handle->channels == 0u || sample_count % handle->channels != 0u) {
    free(frames);
    return luaL_error(lua,
                      "audio encoder frame array must match channel count");
  }
  frame_count = sample_count / handle->channels;
  frames_written = 0u;
  result = handle->encoder->write_f32(handle->encoder, frames, frame_count,
                                      &frames_written);
  free(frames);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio encoder write_f32");
  }
  lua_pushinteger(lua, (lua_Integer)frames_written);
  return 1;
}

static int vectis_audio_lua_encoder_open_file(lua_State *lua) {
  cpkt_audio_encoder_config config;
  cpkt_audio_encoder *encoder;
  cpkt_audio_result result;
  const char *path;
  int config_index;

  if (lua_istable(lua, 1)) {
    lua_getfield(lua, 1, "path");
    path = luaL_checkstring(lua, -1);
    lua_pop(lua, 1);
    config_index = 1;
  } else {
    path = luaL_checkstring(lua, 1);
    config_index = 2;
  }
  vectis_audio_lua_encoder_config(lua, config_index, &config);
  encoder = NULL;
  result = cpkt_audio_encoder_open_file(&encoder, path, &config);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio encoder open_file");
  }
  vectis_audio_lua_new_encoder(lua, encoder, config.channels);
  return 1;
}

static int vectis_audio_lua_encoder_open_writer(lua_State *lua) {
  cpkt_audio_encoder_config config;
  cpkt_audio_encoder *encoder;
  cpkt_audio_writer writer;
  cpkt_audio_result result;
  vectis_audio_encoder_lua *handle;

  luaL_checktype(lua, 1, LUA_TTABLE);
  memset(&writer, 0, sizeof(writer));
  vectis_audio_lua_encoder_config(lua, 1, &config);
  handle = vectis_audio_lua_new_encoder(lua, NULL, config.channels);
  handle->write_ref = vectis_audio_lua_table_function_ref(lua, 1, "write");
  handle->seek_ref = vectis_audio_lua_table_function_ref(lua, 1, "seek");
  if (handle->write_ref == LUA_NOREF || handle->seek_ref == LUA_NOREF) {
    return luaL_error(lua,
                      "audio encoder writer write and seek callbacks are required");
  }
  writer.user = handle;
  writer.write = vectis_audio_lua_write_cb;
  writer.seek = vectis_audio_lua_writer_seek_cb;
  encoder = NULL;
  result = cpkt_audio_encoder_open_writer(&encoder, &writer, &config);
  if (result != CPKT_AUDIO_OK) {
    lua_pop(lua, 1);
    if (handle->write_ref != LUA_NOREF) {
      luaL_unref(lua, LUA_REGISTRYINDEX, handle->write_ref);
    }
    if (handle->seek_ref != LUA_NOREF) {
      luaL_unref(lua, LUA_REGISTRYINDEX, handle->seek_ref);
    }
    return vectis_audio_lua_push_error(lua, result,
                                      "audio encoder open_writer");
  }
  handle->encoder = encoder;
  return 1;
}

static int vectis_audio_lua_capture_state_sink(
    const cpkt_audio_capture_state_event *event, void *user) {
  vectis_audio_capture_lua *handle;
  lua_State *lua;
  int top;
  int failed;

  handle = (vectis_audio_capture_lua *)user;
  lua = handle->lua;
  if (handle->state_ref == LUA_NOREF) {
    return 0;
  }
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, handle->state_ref);
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)event->state);
  lua_setfield(lua, -2, "state");
  lua_pushinteger(lua, (lua_Integer)event->frame_count);
  lua_setfield(lua, -2, "frame_count");
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

static vectis_audio_capture_lua *
vectis_audio_lua_new_capture(lua_State *lua, cpkt_audio_capture *capture) {
  vectis_audio_capture_lua *handle;

  handle = (vectis_audio_capture_lua *)lua_newuserdatauv(lua, sizeof(*handle),
                                                         0);
  handle->capture = capture;
  handle->lua = lua;
  handle->state_ref = LUA_NOREF;
  luaL_getmetatable(lua, VECTIS_AUDIO_CAPTURE);
  lua_setmetatable(lua, -2);
  return handle;
}

static vectis_audio_capture_lua *
vectis_audio_lua_check_capture(lua_State *lua, int index) {
  return (vectis_audio_capture_lua *)luaL_checkudata(lua, index,
                                                     VECTIS_AUDIO_CAPTURE);
}

static int vectis_audio_lua_capture_close(lua_State *lua) {
  vectis_audio_capture_lua *handle;

  handle = vectis_audio_lua_check_capture(lua, 1);
  if (handle->capture != NULL) {
    handle->capture->destroy(handle->capture);
    handle->capture = NULL;
  }
  if (handle->state_ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, handle->state_ref);
    handle->state_ref = LUA_NOREF;
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_capture_start(lua_State *lua) {
  vectis_audio_capture_lua *handle;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_capture(lua, 1);
  if (handle->capture == NULL) {
    return luaL_error(lua, "audio capture is closed");
  }
  result = handle->capture->start(handle->capture);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio capture start");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_capture_read(lua_State *lua) {
  vectis_audio_capture_lua *handle;
  lua_Integer requested;
  float *frames;
  size_t frames_read;
  size_t i;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_capture(lua, 1);
  if (handle->capture == NULL) {
    return luaL_error(lua, "audio capture is closed");
  }
  requested = luaL_checkinteger(lua, 2);
  if (requested < 0) {
    return luaL_error(lua, "audio capture frame capacity must be non-negative");
  }
  frames = NULL;
  if (requested > 0) {
    frames = (float *)malloc((size_t)requested * sizeof(float));
    if (frames == NULL) {
      return luaL_error(lua, "audio capture frame allocation failed");
    }
  }
  frames_read = 0u;
  result = handle->capture->read_f32_mono_16k(handle->capture, frames,
                                              (size_t)requested, &frames_read);
  if (result != CPKT_AUDIO_OK) {
    free(frames);
    return vectis_audio_lua_push_error(lua, result, "audio capture read");
  }
  lua_createtable(lua, (int)frames_read, 0);
  for (i = 0u; i < frames_read; i++) {
    lua_pushnumber(lua, (lua_Number)frames[i]);
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  free(frames);
  lua_pushinteger(lua, (lua_Integer)frames_read);
  return 2;
}

static int vectis_audio_lua_capture_wait_ready(lua_State *lua) {
  vectis_audio_capture_lua *handle;
  unsigned long timeout_ms;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_capture(lua, 1);
  if (handle->capture == NULL) {
    return luaL_error(lua, "audio capture is closed");
  }
  timeout_ms = 0u;
  if (!lua_isnoneornil(lua, 2)) {
    lua_Integer value;
    value = luaL_checkinteger(lua, 2);
    if (value < 0) {
      return luaL_error(lua, "audio capture timeout must be non-negative");
    }
    timeout_ms = (unsigned long)value;
  }
  result = handle->capture->wait_ready(handle->capture, timeout_ms);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result,
                                      "audio capture wait_ready");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_capture_stop(lua_State *lua) {
  vectis_audio_capture_lua *handle;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_capture(lua, 1);
  if (handle->capture == NULL) {
    return luaL_error(lua, "audio capture is closed");
  }
  result = handle->capture->stop(handle->capture);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio capture stop");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_capture_open_default(lua_State *lua) {
  cpkt_audio_capture_config config;
  cpkt_audio_capture *capture;
  cpkt_audio_result result;
  vectis_audio_capture_lua *handle;

  vectis_audio_lua_capture_config(lua, 1, &config);
  handle = vectis_audio_lua_new_capture(lua, NULL);
  if (!lua_isnoneornil(lua, 1)) {
    handle->state_ref = vectis_audio_lua_table_function_ref(lua, 1, "state");
    if (handle->state_ref != LUA_NOREF) {
      config.state_sink = vectis_audio_lua_capture_state_sink;
      config.state_user = handle;
    }
  }
  capture = NULL;
  result = cpkt_audio_capture_open_default(&capture, &config);
  if (result != CPKT_AUDIO_OK) {
    lua_pop(lua, 1);
    if (handle->state_ref != LUA_NOREF) {
      luaL_unref(lua, LUA_REGISTRYINDEX, handle->state_ref);
    }
    return vectis_audio_lua_push_error(lua, result,
                                      "audio capture open_default");
  }
  handle->capture = capture;
  return 1;
}

static vectis_audio_playback_lua *
vectis_audio_lua_new_playback(lua_State *lua, cpkt_audio_playback *playback) {
  vectis_audio_playback_lua *handle;

  handle = (vectis_audio_playback_lua *)lua_newuserdatauv(lua, sizeof(*handle),
                                                          0);
  handle->playback = playback;
  luaL_getmetatable(lua, VECTIS_AUDIO_PLAYBACK);
  lua_setmetatable(lua, -2);
  return handle;
}

static vectis_audio_playback_lua *
vectis_audio_lua_check_playback(lua_State *lua, int index) {
  return (vectis_audio_playback_lua *)luaL_checkudata(lua, index,
                                                      VECTIS_AUDIO_PLAYBACK);
}

static int vectis_audio_lua_playback_close(lua_State *lua) {
  vectis_audio_playback_lua *handle;

  handle = vectis_audio_lua_check_playback(lua, 1);
  if (handle->playback != NULL) {
    handle->playback->destroy(handle->playback);
    handle->playback = NULL;
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_playback_start(lua_State *lua) {
  vectis_audio_playback_lua *handle;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_playback(lua, 1);
  if (handle->playback == NULL) {
    return luaL_error(lua, "audio playback is closed");
  }
  result = handle->playback->start(handle->playback);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio playback start");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_playback_write(lua_State *lua) {
  vectis_audio_playback_lua *handle;
  float *frames;
  size_t frame_count;
  size_t frames_written;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_playback(lua, 1);
  if (handle->playback == NULL) {
    return luaL_error(lua, "audio playback is closed");
  }
  frames = vectis_audio_lua_frame_array(lua, 2, &frame_count);
  frames_written = 0u;
  result = handle->playback->write_f32_mono_16k(
      handle->playback, frames, frame_count, &frames_written);
  free(frames);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio playback write");
  }
  lua_pushinteger(lua, (lua_Integer)frames_written);
  return 1;
}

static int vectis_audio_lua_playback_drain(lua_State *lua) {
  vectis_audio_playback_lua *handle;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_playback(lua, 1);
  if (handle->playback == NULL) {
    return luaL_error(lua, "audio playback is closed");
  }
  result = handle->playback->drain(handle->playback);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio playback drain");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_playback_stop(lua_State *lua) {
  vectis_audio_playback_lua *handle;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_playback(lua, 1);
  if (handle->playback == NULL) {
    return luaL_error(lua, "audio playback is closed");
  }
  result = handle->playback->stop(handle->playback);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio playback stop");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_playback_open_default(lua_State *lua) {
  cpkt_audio_playback_config config;
  cpkt_audio_playback *playback;
  cpkt_audio_result result;

  vectis_audio_lua_playback_config(lua, 1, &config);
  playback = NULL;
  result = cpkt_audio_playback_open_default(&playback, &config);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result,
                                      "audio playback open_default");
  }
  vectis_audio_lua_new_playback(lua, playback);
  return 1;
}

static vectis_audio_segment_lua *
vectis_audio_lua_new_segment(lua_State *lua, cpkt_audio_vox_segment *segment) {
  vectis_audio_segment_lua *handle;

  handle = (vectis_audio_segment_lua *)lua_newuserdatauv(lua, sizeof(*handle),
                                                         0);
  handle->segment = segment;
  luaL_getmetatable(lua, VECTIS_AUDIO_SEGMENT);
  lua_setmetatable(lua, -2);
  return handle;
}

static vectis_audio_segment_lua *
vectis_audio_lua_check_segment(lua_State *lua, int index) {
  return (vectis_audio_segment_lua *)luaL_checkudata(lua, index,
                                                     VECTIS_AUDIO_SEGMENT);
}

static int vectis_audio_lua_segment_info(lua_State *lua) {
  vectis_audio_segment_lua *handle;
  cpkt_audio_vox_segment *segment;

  handle = vectis_audio_lua_check_segment(lua, 1);
  segment = handle->segment;
  if (segment == NULL) {
    return luaL_error(lua, "audio segment is valid only during callback");
  }
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)segment->frame_count);
  lua_setfield(lua, -2, "frame_count");
  lua_pushinteger(lua, (lua_Integer)segment->t0);
  lua_setfield(lua, -2, "t0");
  lua_pushinteger(lua, (lua_Integer)segment->t1);
  lua_setfield(lua, -2, "t1");
  lua_pushinteger(lua, (lua_Integer)segment->segment_index);
  lua_setfield(lua, -2, "segment_index");
  lua_pushboolean(lua, segment->hard_cut != 0);
  lua_setfield(lua, -2, "hard_cut");
  lua_pushboolean(lua, segment->is_final != 0);
  lua_setfield(lua, -2, "is_final");
  return 1;
}

static int vectis_audio_lua_segment_read(lua_State *lua) {
  vectis_audio_segment_lua *handle;
  cpkt_audio_vox_segment *segment;
  lua_Integer requested;
  float *frames;
  size_t frames_read;
  size_t i;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_segment(lua, 1);
  segment = handle->segment;
  if (segment == NULL) {
    return luaL_error(lua, "audio segment is valid only during callback");
  }
  requested = luaL_checkinteger(lua, 2);
  if (requested < 0) {
    return luaL_error(lua, "audio segment frame capacity must be non-negative");
  }
  frames = NULL;
  if (requested > 0) {
    frames = (float *)malloc((size_t)requested * sizeof(float));
    if (frames == NULL) {
      return luaL_error(lua, "audio segment frame allocation failed");
    }
  }
  frames_read = 0u;
  result = segment->read_f32_mono_16k(segment, frames, (size_t)requested,
                                      &frames_read);
  if (result != CPKT_AUDIO_OK && result != CPKT_AUDIO_AT_END) {
    free(frames);
    return vectis_audio_lua_push_error(lua, result, "audio segment read");
  }
  lua_createtable(lua, (int)frames_read, 0);
  for (i = 0u; i < frames_read; i++) {
    lua_pushnumber(lua, (lua_Number)frames[i]);
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  free(frames);
  lua_pushinteger(lua, (lua_Integer)frames_read);
  lua_pushinteger(lua, (lua_Integer)result);
  return 3;
}

static int vectis_audio_lua_segment_sink(cpkt_audio_vox_segment *segment,
                                         void *user) {
  vectis_audio_segmenter_lua *segmenter;
  lua_State *lua;
  vectis_audio_segment_lua *segment_handle;
  int top;
  int failed;

  segmenter = (vectis_audio_segmenter_lua *)user;
  lua = segmenter->lua;
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, segmenter->segment_ref);
  segment_handle = vectis_audio_lua_new_segment(lua, segment);
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    lua_settop(lua, top);
    return -1;
  }
  segment_handle->segment = NULL;
  failed = 0;
  if (lua_type(lua, -1) == LUA_TNUMBER) {
    failed = lua_tointeger(lua, -1) == 0 ? 0 : -1;
  } else if (lua_type(lua, -1) == LUA_TBOOLEAN) {
    failed = lua_toboolean(lua, -1) ? 0 : -1;
  }
  lua_settop(lua, top);
  return failed;
}

static int vectis_audio_lua_state_sink(const cpkt_audio_vox_state_event *event,
                                       void *user) {
  vectis_audio_segmenter_lua *segmenter;
  lua_State *lua;
  int top;
  int failed;

  segmenter = (vectis_audio_segmenter_lua *)user;
  lua = segmenter->lua;
  if (segmenter->state_ref == LUA_NOREF) {
    return 0;
  }
  top = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, segmenter->state_ref);
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)event->state);
  lua_setfield(lua, -2, "state");
  lua_pushinteger(lua, (lua_Integer)event->segment_index);
  lua_setfield(lua, -2, "segment_index");
  lua_pushnumber(lua, (lua_Number)event->threshold);
  lua_setfield(lua, -2, "threshold");
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

static vectis_audio_segmenter_lua *
vectis_audio_lua_new_segmenter(lua_State *lua, const char *metatable) {
  vectis_audio_segmenter_lua *handle;

  handle = (vectis_audio_segmenter_lua *)lua_newuserdatauv(lua,
                                                           sizeof(*handle), 0);
  memset(handle, 0, sizeof(*handle));
  handle->lua = lua;
  handle->segment_ref = LUA_NOREF;
  handle->state_ref = LUA_NOREF;
  luaL_getmetatable(lua, metatable);
  lua_setmetatable(lua, -2);
  return handle;
}

static vectis_audio_segmenter_lua *
vectis_audio_lua_check_vox(lua_State *lua, int index) {
  return (vectis_audio_segmenter_lua *)luaL_checkudata(lua, index,
                                                       VECTIS_AUDIO_VOX);
}

static vectis_audio_segmenter_lua *
vectis_audio_lua_check_ptt(lua_State *lua, int index) {
  return (vectis_audio_segmenter_lua *)luaL_checkudata(lua, index,
                                                       VECTIS_AUDIO_PTT);
}

static void vectis_audio_lua_unref_segmenter(lua_State *lua,
                                             vectis_audio_segmenter_lua *handle) {
  if (handle->segment_ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, handle->segment_ref);
    handle->segment_ref = LUA_NOREF;
  }
  if (handle->state_ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, handle->state_ref);
    handle->state_ref = LUA_NOREF;
  }
}

static int vectis_audio_lua_vox_close(lua_State *lua) {
  vectis_audio_segmenter_lua *handle;

  handle = vectis_audio_lua_check_vox(lua, 1);
  if (handle->handle.vox != NULL) {
    handle->handle.vox->destroy(handle->handle.vox);
    handle->handle.vox = NULL;
  }
  vectis_audio_lua_unref_segmenter(lua, handle);
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_ptt_close(lua_State *lua) {
  vectis_audio_segmenter_lua *handle;

  handle = vectis_audio_lua_check_ptt(lua, 1);
  if (handle->handle.ptt != NULL) {
    handle->handle.ptt->destroy(handle->handle.ptt);
    handle->handle.ptt = NULL;
  }
  vectis_audio_lua_unref_segmenter(lua, handle);
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_vox_push(lua_State *lua) {
  vectis_audio_segmenter_lua *handle;
  float *frames;
  size_t frame_count;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_vox(lua, 1);
  if (handle->handle.vox == NULL) {
    return luaL_error(lua, "audio vox is closed");
  }
  frames = vectis_audio_lua_frame_array(lua, 2, &frame_count);
  result = handle->handle.vox->push_f32_mono_16k(handle->handle.vox, frames,
                                                 frame_count);
  free(frames);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio vox push");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_vox_flush(lua_State *lua) {
  vectis_audio_segmenter_lua *handle;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_vox(lua, 1);
  if (handle->handle.vox == NULL) {
    return luaL_error(lua, "audio vox is closed");
  }
  result = handle->handle.vox->flush(handle->handle.vox);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio vox flush");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_ptt_press(lua_State *lua) {
  vectis_audio_segmenter_lua *handle;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_ptt(lua, 1);
  if (handle->handle.ptt == NULL) {
    return luaL_error(lua, "audio ptt is closed");
  }
  result = handle->handle.ptt->press(handle->handle.ptt);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio ptt press");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_ptt_push(lua_State *lua) {
  vectis_audio_segmenter_lua *handle;
  float *frames;
  size_t frame_count;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_ptt(lua, 1);
  if (handle->handle.ptt == NULL) {
    return luaL_error(lua, "audio ptt is closed");
  }
  frames = vectis_audio_lua_frame_array(lua, 2, &frame_count);
  result = handle->handle.ptt->push_f32_mono_16k(handle->handle.ptt, frames,
                                                 frame_count);
  free(frames);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio ptt push");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_ptt_release(lua_State *lua) {
  vectis_audio_segmenter_lua *handle;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_ptt(lua, 1);
  if (handle->handle.ptt == NULL) {
    return luaL_error(lua, "audio ptt is closed");
  }
  result = handle->handle.ptt->release(handle->handle.ptt);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio ptt release");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_ptt_flush(lua_State *lua) {
  vectis_audio_segmenter_lua *handle;
  cpkt_audio_result result;

  handle = vectis_audio_lua_check_ptt(lua, 1);
  if (handle->handle.ptt == NULL) {
    return luaL_error(lua, "audio ptt is closed");
  }
  result = handle->handle.ptt->flush(handle->handle.ptt);
  if (result != CPKT_AUDIO_OK) {
    return vectis_audio_lua_push_error(lua, result, "audio ptt flush");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_audio_lua_vox_open(lua_State *lua) {
  cpkt_audio_vox_config config;
  cpkt_audio_result result;
  vectis_audio_segmenter_lua *handle;

  luaL_checktype(lua, 1, LUA_TTABLE);
  memset(&config, 0, sizeof(config));
  handle = vectis_audio_lua_new_segmenter(lua, VECTIS_AUDIO_VOX);
  handle->segment_ref = vectis_audio_lua_table_function_ref(lua, 1, "segment");
  if (handle->segment_ref == LUA_NOREF) {
    return luaL_error(lua, "audio vox segment callback is required");
  }
  handle->state_ref = vectis_audio_lua_table_function_ref(lua, 1, "state");
  config.threshold =
      (float)vectis_audio_lua_table_number(lua, 1, "threshold", 0.0);
  config.release_silence_ms =
      vectis_audio_lua_table_ulong(lua, 1, "release_silence_ms", 0u);
  config.prebuffer_ms =
      vectis_audio_lua_table_ulong(lua, 1, "prebuffer_ms", 0u);
  config.max_segment_ms =
      vectis_audio_lua_table_ulong(lua, 1, "max_segment_ms", 0u);
  config.min_segment_ms =
      vectis_audio_lua_table_ulong(lua, 1, "min_segment_ms", 0u);
  config.memory_spool_bytes =
      vectis_audio_lua_table_ulong(lua, 1, "memory_spool_bytes", 0u);
  config.max_spool_bytes =
      vectis_audio_lua_table_ulong(lua, 1, "max_spool_bytes", 0u);
  config.segment_sink = vectis_audio_lua_segment_sink;
  config.segment_user = handle;
  config.state_sink =
      handle->state_ref == LUA_NOREF ? NULL : vectis_audio_lua_state_sink;
  config.state_user = handle;
  result = cpkt_audio_vox_open(&handle->handle.vox, &config);
  if (result != CPKT_AUDIO_OK) {
    lua_pop(lua, 1);
    vectis_audio_lua_unref_segmenter(lua, handle);
    return vectis_audio_lua_push_error(lua, result, "audio vox open");
  }
  return 1;
}

static int vectis_audio_lua_ptt_open(lua_State *lua) {
  cpkt_audio_ptt_config config;
  cpkt_audio_result result;
  vectis_audio_segmenter_lua *handle;

  luaL_checktype(lua, 1, LUA_TTABLE);
  memset(&config, 0, sizeof(config));
  handle = vectis_audio_lua_new_segmenter(lua, VECTIS_AUDIO_PTT);
  handle->segment_ref = vectis_audio_lua_table_function_ref(lua, 1, "segment");
  if (handle->segment_ref == LUA_NOREF) {
    return luaL_error(lua, "audio ptt segment callback is required");
  }
  handle->state_ref = vectis_audio_lua_table_function_ref(lua, 1, "state");
  config.max_segment_ms =
      vectis_audio_lua_table_ulong(lua, 1, "max_segment_ms", 0u);
  config.min_segment_ms =
      vectis_audio_lua_table_ulong(lua, 1, "min_segment_ms", 0u);
  config.memory_spool_bytes =
      vectis_audio_lua_table_ulong(lua, 1, "memory_spool_bytes", 0u);
  config.max_spool_bytes =
      vectis_audio_lua_table_ulong(lua, 1, "max_spool_bytes", 0u);
  config.segment_sink = vectis_audio_lua_segment_sink;
  config.segment_user = handle;
  config.state_sink =
      handle->state_ref == LUA_NOREF ? NULL : vectis_audio_lua_state_sink;
  config.state_user = handle;
  result = cpkt_audio_ptt_open(&handle->handle.ptt, &config);
  if (result != CPKT_AUDIO_OK) {
    lua_pop(lua, 1);
    vectis_audio_lua_unref_segmenter(lua, handle);
    return vectis_audio_lua_push_error(lua, result, "audio ptt open");
  }
  return 1;
}

static int vectis_audio_lua_result_string(lua_State *lua) {
  lua_pushstring(lua,
                 cpkt_audio_result_string((cpkt_audio_result)luaL_checkinteger(
                     lua, 1)));
  return 1;
}

static int vectis_audio_lua_can_decode(lua_State *lua) {
  lua_pushboolean(lua, cpkt_audio_format_can_decode(
                           vectis_audio_lua_format(lua, 1)) != 0);
  return 1;
}

static int vectis_audio_lua_can_encode(lua_State *lua) {
  lua_pushboolean(lua, cpkt_audio_format_can_encode(
                           vectis_audio_lua_format(lua, 1)) != 0);
  return 1;
}

static void vectis_audio_lua_methods(lua_State *lua, const char *metatable,
                                     const luaL_Reg *methods) {
  luaL_newmetatable(lua, metatable);
  lua_pushvalue(lua, -1);
  lua_setfield(lua, -2, "__index");
  luaL_setfuncs(lua, methods, 0);
  lua_pop(lua, 1);
}

static void vectis_audio_lua_set_constants(lua_State *lua) {
  lua_pushinteger(lua, CPKT_AUDIO_OK);
  lua_setfield(lua, -2, "OK");
  lua_pushinteger(lua, CPKT_AUDIO_ERR_ARG);
  lua_setfield(lua, -2, "ERR_ARG");
  lua_pushinteger(lua, CPKT_AUDIO_ERR_ALLOC);
  lua_setfield(lua, -2, "ERR_ALLOC");
  lua_pushinteger(lua, CPKT_AUDIO_ERR_IO);
  lua_setfield(lua, -2, "ERR_IO");
  lua_pushinteger(lua, CPKT_AUDIO_ERR_FORMAT);
  lua_setfield(lua, -2, "ERR_FORMAT");
  lua_pushinteger(lua, CPKT_AUDIO_ERR_UPSTREAM);
  lua_setfield(lua, -2, "ERR_UPSTREAM");
  lua_pushinteger(lua, CPKT_AUDIO_AT_END);
  lua_setfield(lua, -2, "AT_END");
  lua_pushinteger(lua, CPKT_AUDIO_TIMEOUT);
  lua_setfield(lua, -2, "TIMEOUT");
  lua_pushinteger(lua, CPKT_AUDIO_FORMAT_UNKNOWN);
  lua_setfield(lua, -2, "FORMAT_UNKNOWN");
  lua_pushinteger(lua, CPKT_AUDIO_FORMAT_WAV);
  lua_setfield(lua, -2, "FORMAT_WAV");
  lua_pushinteger(lua, CPKT_AUDIO_FORMAT_FLAC);
  lua_setfield(lua, -2, "FORMAT_FLAC");
  lua_pushinteger(lua, CPKT_AUDIO_FORMAT_MP3);
  lua_setfield(lua, -2, "FORMAT_MP3");
  lua_pushinteger(lua, CPKT_AUDIO_DEVICE_BACKEND_AUTO);
  lua_setfield(lua, -2, "DEVICE_BACKEND_AUTO");
  lua_pushinteger(lua, CPKT_AUDIO_DEVICE_BACKEND_PROCESS);
  lua_setfield(lua, -2, "DEVICE_BACKEND_PROCESS");
  lua_pushinteger(lua, CPKT_AUDIO_DEVICE_BACKEND_COREAUDIO);
  lua_setfield(lua, -2, "DEVICE_BACKEND_COREAUDIO");
  lua_pushinteger(lua, CPKT_AUDIO_DEVICE_BACKEND_NATIVE);
  lua_setfield(lua, -2, "DEVICE_BACKEND_NATIVE");
  lua_pushinteger(lua, CPKT_AUDIO_CAPTURE_READY);
  lua_setfield(lua, -2, "CAPTURE_READY");
  lua_pushinteger(lua, CPKT_AUDIO_SEEK_SET);
  lua_setfield(lua, -2, "SEEK_SET");
  lua_pushinteger(lua, CPKT_AUDIO_SEEK_CUR);
  lua_setfield(lua, -2, "SEEK_CUR");
  lua_pushinteger(lua, CPKT_AUDIO_SEEK_END);
  lua_setfield(lua, -2, "SEEK_END");
  lua_pushinteger(lua, CPKT_AUDIO_VOX_TX_ON);
  lua_setfield(lua, -2, "VOX_TX_ON");
  lua_pushinteger(lua, CPKT_AUDIO_VOX_TX_OFF);
  lua_setfield(lua, -2, "VOX_TX_OFF");
  lua_pushinteger(lua, CPKT_AUDIO_VOX_HARD_CUT);
  lua_setfield(lua, -2, "VOX_HARD_CUT");
}

int luaopen_audio(lua_State *lua) {
  static const luaL_Reg decoder_methods[] = {
      {"info", vectis_audio_lua_decoder_info},
      {"read_f32_mono_16k", vectis_audio_lua_decoder_read},
      {"close", vectis_audio_lua_decoder_close},
      {"__gc", vectis_audio_lua_decoder_close},
      {NULL, NULL}};
  static const luaL_Reg encoder_methods[] = {
      {"write_f32", vectis_audio_lua_encoder_write_f32},
      {"close", vectis_audio_lua_encoder_close},
      {"__gc", vectis_audio_lua_encoder_close},
      {NULL, NULL}};
  static const luaL_Reg capture_methods[] = {
      {"start", vectis_audio_lua_capture_start},
      {"read_f32_mono_16k", vectis_audio_lua_capture_read},
      {"wait_ready", vectis_audio_lua_capture_wait_ready},
      {"stop", vectis_audio_lua_capture_stop},
      {"close", vectis_audio_lua_capture_close},
      {"__gc", vectis_audio_lua_capture_close},
      {NULL, NULL}};
  static const luaL_Reg playback_methods[] = {
      {"start", vectis_audio_lua_playback_start},
      {"write_f32_mono_16k", vectis_audio_lua_playback_write},
      {"drain", vectis_audio_lua_playback_drain},
      {"stop", vectis_audio_lua_playback_stop},
      {"close", vectis_audio_lua_playback_close},
      {"__gc", vectis_audio_lua_playback_close},
      {NULL, NULL}};
  static const luaL_Reg vox_methods[] = {
      {"push_f32_mono_16k", vectis_audio_lua_vox_push},
      {"flush", vectis_audio_lua_vox_flush},
      {"close", vectis_audio_lua_vox_close},
      {"__gc", vectis_audio_lua_vox_close},
      {NULL, NULL}};
  static const luaL_Reg ptt_methods[] = {
      {"press", vectis_audio_lua_ptt_press},
      {"push_f32_mono_16k", vectis_audio_lua_ptt_push},
      {"release", vectis_audio_lua_ptt_release},
      {"flush", vectis_audio_lua_ptt_flush},
      {"close", vectis_audio_lua_ptt_close},
      {"__gc", vectis_audio_lua_ptt_close},
      {NULL, NULL}};
  static const luaL_Reg segment_methods[] = {
      {"info", vectis_audio_lua_segment_info},
      {"read_f32_mono_16k", vectis_audio_lua_segment_read},
      {NULL, NULL}};
  static const luaL_Reg module_funcs[] = {
      {"result_string", vectis_audio_lua_result_string},
      {"can_decode", vectis_audio_lua_can_decode},
      {"can_encode", vectis_audio_lua_can_encode},
      {NULL, NULL}};

  vectis_audio_lua_methods(lua, VECTIS_AUDIO_DECODER, decoder_methods);
  vectis_audio_lua_methods(lua, VECTIS_AUDIO_ENCODER, encoder_methods);
  vectis_audio_lua_methods(lua, VECTIS_AUDIO_CAPTURE, capture_methods);
  vectis_audio_lua_methods(lua, VECTIS_AUDIO_PLAYBACK, playback_methods);
  vectis_audio_lua_methods(lua, VECTIS_AUDIO_VOX, vox_methods);
  vectis_audio_lua_methods(lua, VECTIS_AUDIO_PTT, ptt_methods);
  vectis_audio_lua_methods(lua, VECTIS_AUDIO_SEGMENT, segment_methods);

  luaL_newlib(lua, module_funcs);
  vectis_audio_lua_set_constants(lua);
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_audio_lua_decoder_open_file);
  lua_setfield(lua, -2, "open_file");
  lua_pushcfunction(lua, vectis_audio_lua_decoder_open_url);
  lua_setfield(lua, -2, "open_url");
  lua_pushcfunction(lua, vectis_audio_lua_decoder_open_reader);
  lua_setfield(lua, -2, "open_reader");
  lua_setfield(lua, -2, "decoder");
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_audio_lua_encoder_open_file);
  lua_setfield(lua, -2, "open_file");
  lua_pushcfunction(lua, vectis_audio_lua_encoder_open_writer);
  lua_setfield(lua, -2, "open_writer");
  lua_setfield(lua, -2, "encoder");
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_audio_lua_capture_open_default);
  lua_setfield(lua, -2, "open_default");
  lua_setfield(lua, -2, "capture");
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_audio_lua_playback_open_default);
  lua_setfield(lua, -2, "open_default");
  lua_setfield(lua, -2, "playback");
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_audio_lua_vox_open);
  lua_setfield(lua, -2, "open");
  lua_setfield(lua, -2, "vox");
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_audio_lua_ptt_open);
  lua_setfield(lua, -2, "open");
  lua_setfield(lua, -2, "ptt");
  return 1;
}
