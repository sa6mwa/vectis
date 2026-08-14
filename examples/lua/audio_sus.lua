local audio = require("audio")
local sus = require("sus")

assert(audio.can_decode("wav") == true)
assert(audio.can_encode("wav") == true)
assert(type(audio.result_string(audio.OK)) == "string")
assert(type(audio.capture.open_default) == "function")
assert(type(audio.playback.open_default) == "function")

local sink = {data = "", position = 1}
local function seek(offset, origin)
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
local function write(chunk)
  sink.data = sink.data:sub(1, sink.position - 1) ..
      chunk .. sink.data:sub(sink.position + #chunk)
  sink.position = sink.position + #chunk
  return #chunk
end

local encoder = assert(audio.encoder.open_writer({
  format = "wav",
  sample_rate = 16000,
  channels = 1,
  write = write,
  seek = seek,
}))
local frames = {}
for i = 1, 160 do
  frames[i] = 0.0
end
assert(encoder:write_f32(frames) == 160)
assert(encoder:close() == true)
assert(sink.data:sub(1, 4) == "RIFF")

local default_model = assert(sus.model_catalog_default())
assert(type(default_model.name) == "string")
assert(default_model.name ~= "")
assert(sus.model_catalog_find(default_model.name).name == default_model.name)
local sus_registry = debug.getregistry()
assert(type(sus_registry["sus.model"].__index.create_transcriber) == "function")
assert(type(sus_registry["sus.transcriber"].__index.transcribe_f32_mono_16k) == "function")
assert(type(sus_registry["sus.transcriber"].__index.transcribe_f32_mono_16k_text) == "function")
assert(type(sus_registry["sus.transcriber"].__index.revised_text) == "function")

print("lua audio sus example ok")
