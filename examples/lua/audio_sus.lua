local audio = require("audio")
local sus = require("sus")

assert(audio.can_decode("wav") == true)
assert(audio.can_encode("wav") == true)
assert(type(audio.result_string(audio.OK)) == "string")

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

print("lua audio sus example ok")
