local audio = require("audio")
local sus = require("sus")

local model_path = os.getenv("VECTIS_LUA_SUS_MODEL_PATH")
if model_path == "" then
  model_path = nil
end
local cache_enabled = os.getenv("VECTIS_LUA_SUS_CACHE_ENABLE") == "1"

if model_path == nil and not cache_enabled then
  print("lua sus loaded model example skipped")
  print("lua sus loaded model example ok")
  return
end

local work_dir = os.getenv("VECTIS_LUA_SUS_EXAMPLE_DIR")
if work_dir == nil or work_dir == "" then
  work_dir = "."
end
local wav_path = work_dir .. "/vectis-sus-example.wav"

local frames = {}
for i = 1, 16000 do
  frames[i] = 0.0
end

local language = os.getenv("VECTIS_LUA_SUS_LANGUAGE")
if language == nil or language == "" then
  language = "en"
end

local encoder = assert(audio.encoder.open_file({
  path = wav_path,
  format = "wav",
  sample_rate = 16000,
  channels = 1,
}))
assert(encoder:write_f32(frames) == #frames)
assert(encoder:close() == true)

local model
if model_path ~= nil then
  model = assert(sus.open_path({
    path = model_path,
    cpu_only = true,
    preserve_initial_space_after_first_transcriber = true,
  }))
else
  local default_model = assert(sus.model_catalog_default())
  local model_name = os.getenv("VECTIS_LUA_SUS_MODEL")
  local cache_dir = os.getenv("VECTIS_LUA_SUS_CACHE_DIR")
  local sha256 = os.getenv("VECTIS_LUA_SUS_SHA256")
  local source_url = os.getenv("VECTIS_LUA_SUS_SOURCE_URL")
  if model_name == "" then
    model_name = nil
  end
  if cache_dir == "" then
    cache_dir = nil
  end
  if sha256 == "" then
    sha256 = nil
  end
  if source_url == "" then
    source_url = nil
  end
  model = assert(sus.open_cached({
    model = model_name or default_model.name,
    cache_dir = cache_dir,
    sha256 = sha256,
    source_url = source_url,
    insecure_no_checksum = os.getenv("VECTIS_LUA_SUS_INSECURE_NO_CHECKSUM") == "1",
    cpu_only = true,
    status = function(event)
      assert(type(event.phase) == "number")
      return 0
    end,
  }))
end

local transcriber = assert(model:create_transcriber({
  threads = tonumber(os.getenv("VECTIS_LUA_SUS_THREADS") or "1"),
  cpu_only = true,
  language = language,
  progress = function(percent)
    assert(type(percent) == "number")
    return 0
  end,
  segment = function(segment)
    assert(type(segment.text) == "string")
    return 0
  end,
}))

local text = assert(transcriber:transcribe_f32_mono_16k_text(frames))
assert(type(text) == "string")

local decoder = assert(audio.decoder.open_file({
  path = wav_path,
  encoding = "wav",
}))
local segmented_text = assert(transcriber:transcribe_audio_decoder_segmented_text(
  decoder,
  {
    mode = "simplex",
    read_frames = 2048,
    length_ms = 1000,
    keep_ms = 100,
    threshold = 1.0,
  }))
assert(type(segmented_text) == "string")
assert(decoder:close() == true)

assert(transcriber:close() == true)
assert(model:close() == true)

print("lua sus loaded model example ok")
