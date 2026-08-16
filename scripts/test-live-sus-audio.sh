#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

if [ -z "${VECTIS_LUA_SUS_MODEL_PATH:-}" ] &&
   [ "${VECTIS_LUA_SUS_CACHE_ENABLE:-0}" != "1" ]; then
  printf '%s\n' \
    'SKIP: set VECTIS_LUA_SUS_MODEL_PATH=/path/to/ggml-model.bin or VECTIS_LUA_SUS_CACHE_ENABLE=1 to run live SUS/audio loaded-model validation'
  exit 0
fi

vectis_bin=${VECTIS_BIN:-"$repo_root/build/debug/vectis"}
if [ ! -x "$vectis_bin" ]; then
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=tool-discovery
status=failed
class=external-tool-unavailable
reason=vectis-binary-missing
artifact=$vectis_bin
next=run make build-debug or set VECTIS_BIN to a built vectis executable
PKT_DIAGNOSTIC_END
EOF
  exit 2
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/vectis-sus-audio-live.XXXXXX")
cleanup_work() {
  rm -rf "$work"
}
trap cleanup_work EXIT INT TERM

cat >"$work/live-sus-audio.lua" <<'LUA'
local audio = require("audio")
local sus = require("sus")

local work_dir = assert(arg[1])
local wav_path = work_dir .. "/sus-live-input.wav"

local function optional(name)
  local value = os.getenv(name)
  if value == nil or value == "" then
    return nil
  end
  return value
end

local function assert_ok(value, err, context)
  if value == nil then
    error(context .. ": " .. tostring(err and err.message or err), 2)
  end
  return value, err
end

local function open_model()
  local model_path = optional("VECTIS_LUA_SUS_MODEL_PATH")
  if model_path ~= nil then
    local model, err = sus.open_path({
      path = model_path,
      cpu_only = true,
      preserve_initial_space_after_first_transcriber = true,
    })
    return assert_ok(model, err, "sus.open_path")
  end

  local status_events = 0
  local model, err = sus.open_cached({
    model = optional("VECTIS_LUA_SUS_MODEL") or
        assert(sus.model_catalog_default()).name,
    cache_dir = optional("VECTIS_LUA_SUS_CACHE_DIR"),
    sha256 = optional("VECTIS_LUA_SUS_SHA256"),
    source_url = optional("VECTIS_LUA_SUS_SOURCE_URL"),
    insecure_no_checksum = os.getenv("VECTIS_LUA_SUS_INSECURE_NO_CHECKSUM") == "1",
    offline = false,
    cpu_only = true,
    preserve_initial_space_after_first_transcriber = true,
    status = function(event)
      status_events = status_events + 1
      assert(type(event.phase) == "number")
      assert(type(event.model) == "string")
      assert(type(event.cache_path) == "string")
      return 0
    end,
  })
  model = assert_ok(model, err, "sus.open_cached")
  assert(status_events > 0)
  return model
end

local frames = {}
for i = 1, 16000 do
  frames[i] = 0.0
end

local encoder = assert(audio.encoder.open_file({
  path = wav_path,
  format = "wav",
  sample_rate = 16000,
  channels = 1,
}))
assert(encoder:write_f32(frames) == #frames)
assert(encoder:close() == true)

local model = open_model()
local model_info = assert(model:info())
assert(type(model_info.backend_version) == "string")
assert(type(model_info.backend_system_info) == "string")
assert(model_info.cpu_only == true)

local segment_calls = 0
local progress_calls = 0
local transcriber = assert(model:create_transcriber({
  threads = tonumber(os.getenv("VECTIS_LUA_SUS_THREADS") or "1"),
  cpu_only = true,
  language = optional("VECTIS_LUA_SUS_LANGUAGE") or "en",
  timestamps = true,
  segment = function(segment)
    segment_calls = segment_calls + 1
    assert(type(segment.text) == "string")
    assert(type(segment.text_length) == "number")
    assert(type(segment.t0) == "number")
    assert(type(segment.t1) == "number")
    return 0
  end,
  progress = function(percent)
    progress_calls = progress_calls + 1
    assert(type(percent) == "number")
    return 0
  end,
  abort = function()
    return false
  end,
}))
collectgarbage()
collectgarbage()
assert(transcriber:transcribe_f32_mono_16k(frames) == true)
local text = assert(transcriber:transcribe_f32_mono_16k_text(frames))
assert(type(text) == "string")

local abort_calls = 0
local aborting = assert(model:create_transcriber({
  threads = 1,
  cpu_only = true,
  language = optional("VECTIS_LUA_SUS_LANGUAGE") or "en",
  abort = function()
    abort_calls = abort_calls + 1
    return true
  end,
}))
local aborted, aborted_err = aborting:transcribe_f32_mono_16k(frames)
assert(abort_calls > 0)
assert(aborted == nil)
assert(type(aborted_err) == "table")
assert(aborted_err.result == sus.ABORTED)
assert(aborting:close() == true)

local decoder = assert(audio.decoder.open_file({
  path = wav_path,
  encoding = "wav",
}))
local segmented_events = 0
assert(transcriber:transcribe_audio_decoder_segmented(decoder, {
  mode = "simplex",
  read_frames = 2048,
  length_ms = 1000,
  keep_ms = 100,
  threshold = 1.0,
  segmented = function(event)
    segmented_events = segmented_events + 1
    assert(type(event.text) == "string")
    assert(type(event.text_length) == "number")
    assert(type(event.step_index) == "number")
    assert(type(event.is_final) == "boolean")
    return 0
  end,
}) == true)
assert(decoder:close() == true)

local decoder_text = assert(audio.decoder.open_file({
  path = wav_path,
  encoding = "wav",
}))
local segmented_text = assert(transcriber:transcribe_audio_decoder_segmented_text(
  decoder_text,
  {
    mode = "simplex",
    read_frames = 2048,
    length_ms = 1000,
    keep_ms = 100,
    threshold = 1.0,
  }))
assert(type(segmented_text) == "string")
assert(decoder_text:close() == true)

local ptt_segments = 0
local ptt = assert(audio.ptt.open({
  min_segment_ms = 1,
  segment = function(segment)
    ptt_segments = ptt_segments + 1
    assert(transcriber:transcribe_audio_vox_segment(segment, {
      mode = "simplex",
      length_ms = 1000,
      keep_ms = 100,
      threshold = 1.0,
      segmented = function(event)
        assert(type(event.text) == "string")
        return 0
      end,
    }) == true)
  end,
}))
assert(ptt:press() == true)
assert(ptt:push_f32_mono_16k(frames) == true)
assert(ptt:release() == true)
assert(ptt:flush() == true)
assert(ptt_segments == 1)
assert(ptt:close() == true)

local revised = assert(transcriber:revised_text())
assert(type(revised) == "string")
assert(model:reset_transcript_spacing() == true)
assert(transcriber:close() == true)
assert(model:close() == true)

print("sus_audio_loaded_model_live=ok")
print("progress_calls=" .. tostring(progress_calls))
print("segment_calls=" .. tostring(segment_calls))
print("segmented_events=" .. tostring(segmented_events))
LUA

if ! "$vectis_bin" "$work/live-sus-audio.lua" "$work" \
      >"$work/live-sus-audio.out" \
      2>"$work/live-sus-audio.err"; then
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=sus-audio
status=failed
class=test
reason=live-sus-audio-command-failed
artifact=$work/live-sus-audio.err
next=inspect the configured SUS model path/cache variables and whisper backend diagnostics
PKT_DIAGNOSTIC_END
EOF
  cat "$work/live-sus-audio.err" >&2
  exit 1
fi

if ! grep -Fxq 'sus_audio_loaded_model_live=ok' "$work/live-sus-audio.out"; then
  cat >&2 <<'EOF'
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=sus-audio
status=failed
class=test
reason=missing-success-marker
next=inspect live SUS/audio script output
PKT_DIAGNOSTIC_END
EOF
  cat "$work/live-sus-audio.out" >&2
  exit 1
fi

cat "$work/live-sus-audio.out"
