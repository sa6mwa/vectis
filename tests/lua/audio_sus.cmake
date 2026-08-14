set(script "${WORK_DIR}/audio-sus-smoke.lua")
set(cache_dir "${WORK_DIR}/sus-offline-cache")

file(REMOVE_RECURSE "${cache_dir}")
file(MAKE_DIRECTORY "${cache_dir}")
file(WRITE "${script}" [[
local audio = require("audio")
local sus = require("sus")

assert(audio.result_string(audio.OK) == "ok")
assert(type(audio.result_string(audio.ERR_IO)) == "string")
assert(audio.can_decode("wav") == true)
assert(audio.can_encode("wav") == true)
assert(audio.can_decode(audio.FORMAT_WAV) == true)
assert(type(audio.can_decode("flac")) == "boolean")

local missing_decoder, missing_decoder_err = audio.decoder.open_file({
  path = "/__vectis_missing_audio_input__.wav",
})
assert(missing_decoder == nil)
assert(type(missing_decoder_err) == "table")
assert(type(missing_decoder_err.message) == "string")

local sink = {
  data = "",
  position = 1,
}
local function sink_seek(offset, origin)
  local next_position
  if origin == audio.SEEK_SET then
    next_position = offset + 1
  elseif origin == audio.SEEK_CUR then
    next_position = sink.position + offset
  elseif origin == audio.SEEK_END then
    next_position = #sink.data + offset + 1
  else
    return false
  end
  if next_position < 1 then
    return false
  end
  sink.position = next_position
  return true
end
local function sink_write(chunk)
  local before = sink.data:sub(1, sink.position - 1)
  local after = sink.data:sub(sink.position + #chunk)
  sink.data = before .. chunk .. after
  sink.position = sink.position + #chunk
  return #chunk
end
local encoder = assert(audio.encoder.open_writer({
  format = "wav",
  sample_rate = 16000,
  channels = 1,
  write = sink_write,
  seek = sink_seek,
}))
local frames = {}
for i = 1, 320 do
  frames[i] = 0.0
end
assert(encoder:write_f32(frames) == 320)
assert(encoder:close() == true)
assert(sink.data:sub(1, 4) == "RIFF")
assert(sink.data:sub(9, 12) == "WAVE")

local source = {
  data = sink.data,
  position = 1,
}
local function source_read(bytes)
  local chunk = source.data:sub(source.position, source.position + bytes - 1)
  source.position = source.position + #chunk
  return chunk
end
local function source_seek(offset, origin)
  local next_position
  if origin == audio.SEEK_SET then
    next_position = offset + 1
  elseif origin == audio.SEEK_CUR then
    next_position = source.position + offset
  elseif origin == audio.SEEK_END then
    next_position = #source.data + offset + 1
  else
    return false
  end
  if next_position < 1 then
    return false
  end
  source.position = next_position
  return true
end
local decoder = assert(audio.decoder.open_reader({
  encoding = "wav",
  read = source_read,
  seek = source_seek,
}))
local info = assert(decoder:info())
assert(info.output_sample_rate == 16000)
assert(info.output_channels == 1)
local decoded, decoded_count = assert(decoder:read_f32_mono_16k(512))
assert(decoded_count > 0)
assert(#decoded == decoded_count)
assert(decoder:close() == true)

local vox_segments = 0
local vox = assert(audio.vox.open({
  threshold = 1.0,
  min_segment_ms = 1,
  segment = function(segment)
    vox_segments = vox_segments + 1
    assert(type(segment:info().frame_count) == "number")
  end,
}))
assert(vox:push_f32_mono_16k(frames) == true)
assert(vox:flush() == true)
assert(vox_segments == 0)
assert(vox:close() == true)

local ptt_segments = 0
local ptt = assert(audio.ptt.open({
  min_segment_ms = 1,
  segment = function(segment)
    local segment_info = segment:info()
    ptt_segments = ptt_segments + 1
    assert(segment_info.frame_count > 0)
    local pulled, pulled_count = assert(segment:read_f32_mono_16k(segment_info.frame_count))
    assert(pulled_count == segment_info.frame_count)
    assert(#pulled == pulled_count)
  end,
}))
assert(ptt:press() == true)
assert(ptt:push_f32_mono_16k(frames) == true)
assert(ptt:release() == true)
assert(ptt:flush() == true)
assert(ptt_segments == 1)
assert(ptt:close() == true)

local bad_ptt = assert(audio.ptt.open({
  min_segment_ms = 1,
  segment = function()
    return 1
  end,
}))
assert(bad_ptt:press() == true)
assert(bad_ptt:push_f32_mono_16k(frames) == true)
local bad_release, bad_release_err = bad_ptt:release()
assert(bad_release == nil)
assert(type(bad_release_err) == "table")
assert(bad_release_err.result == audio.ERR_IO)
bad_ptt:close()

assert(sus.result_string(sus.OK) == "ok")
assert(type(sus.backend_version()) == "string")
assert(type(sus.backend_system_info()) == "string")
assert(type(sus.backend_capabilities()) == "string")
assert(type(sus.facade_version()) == "string")
assert(sus.model_catalog_count() > 0)
local default_model = assert(sus.model_catalog_default())
assert(type(default_model.name) == "string")
assert(default_model.name ~= "")
assert(sus.model_catalog_find(default_model.name).name == default_model.name)
assert(type(sus.model_catalog_entry(1).filename) == "string")
local unknown_model, unknown_model_err = sus.model_catalog_find("__no_such_model__")
assert(unknown_model == nil)
assert(type(unknown_model_err) == "table")
assert(unknown_model_err.result == sus.ERR_LOOKUP)

local cache_dir = assert(arg[1])
local cache_status = {}
local cached_model, cached_err = sus.open_cached({
  model = default_model.name,
  cache_dir = cache_dir,
  offline = true,
  status = function(event)
    cache_status[#cache_status + 1] = event.phase
    assert(type(event.model) == "string")
    assert(type(event.cache_path) == "string")
    return 0
  end,
})
assert(cached_model == nil)
assert(type(cached_err) == "table")
assert(cached_err.result == sus.ERR_IO or cached_err.result == sus.ERR_MODEL)
assert(#cache_status >= 1)

local aborted_model, aborted_err = sus.open_cached({
  model = default_model.name,
  cache_dir = cache_dir,
  offline = true,
  status = function()
    return 1
  end,
})
assert(aborted_model == nil)
assert(type(aborted_err) == "table")
assert(aborted_err.result == sus.ERR_CALLBACK)

local missing_model, missing_model_err = sus.open_path({
  path = "/__vectis_missing_sus_model__.bin",
  cpu_only = true,
})
assert(missing_model == nil)
assert(type(missing_model_err) == "table")

print("lua audio sus smoke ok")
]])

execute_process(COMMAND "${VECTIS_BIN}" "${script}" "${cache_dir}"
                RESULT_VARIABLE result
                OUTPUT_VARIABLE stdout
                ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Lua audio/sus smoke failed: ${stdout}${stderr}")
endif()
if(NOT stdout MATCHES "lua audio sus smoke ok")
  message(FATAL_ERROR "Lua audio/sus smoke missed marker: ${stdout}${stderr}")
endif()
