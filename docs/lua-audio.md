# Audio Lua Facade

`require("audio")` exposes the raw cpkt audio facade in the embedded Vectis Lua
runtime. It is a receiver-shell binding over the bundled `cpkt_audio` C89
facade, not a higher-level Vectis workflow wrapper.

The facade keeps streaming behavior explicit. Callback readers and writers are
called incrementally by the C backend, decoder URL input is pulled through
libcurl by the decoder, and VOX/PTT segments are valid only during their Lua
segment callback.

## Metadata And Constants

- `audio.result_string(code)` returns a stable result string.
- `audio.can_decode(format)` and `audio.can_encode(format)` report bundled
  format support. `format` may be a numeric `FORMAT_*` constant or `"wav"`,
  `"flac"`, `"mp3"`, or `"unknown"`.
- Result constants include `OK`, `ERR_ARG`, `ERR_ALLOC`, `ERR_IO`,
  `ERR_FORMAT`, `ERR_UPSTREAM`, `AT_END`, and `TIMEOUT`.
- Format constants include `FORMAT_UNKNOWN`, `FORMAT_WAV`, `FORMAT_FLAC`, and
  `FORMAT_MP3`.
- Seek constants include `SEEK_SET`, `SEEK_CUR`, and `SEEK_END`.
- Device backend constants include `DEVICE_BACKEND_AUTO`,
  `DEVICE_BACKEND_PROCESS`, `DEVICE_BACKEND_COREAUDIO`, and
  `DEVICE_BACKEND_NATIVE`.

## Decoder

- `audio.decoder.open_file(opts)` opens a filesystem path.
- `audio.decoder.open_url(opts)` opens a libcurl-supported URL.
- `audio.decoder.open_reader(opts)` opens a Lua callback-backed source.

Decoder options:

- `path`: required for `open_file`.
- `url`: required for `open_url`.
- `encoding`: optional input hint, using a format string or encoding constant.
- `read(bytes)`: required callback for `open_reader`; returns the next byte
  chunk as a Lua string.
- `seek(offset, origin)`: optional reader callback; return true/zero for
  success and false/non-zero for failure.

Decoder methods:

- `decoder:info()` returns `source_format`, `output_sample_rate`,
  `output_channels`, and `output_frame_count`.
- `decoder:read_f32_mono_16k(frame_capacity)` returns `frames`, `count`, and
  the audio result code. `frames` is a Lua-owned numeric array.
- `decoder:close()` releases the decoder.

## Encoder

- `audio.encoder.open_file(opts)` opens a filesystem output path.
- `audio.encoder.open_writer(opts)` opens a Lua callback-backed output sink.

Encoder options:

- `path`: required for `open_file`.
- `format`: output format; WAV is the supported deterministic local path.
- `sample_rate`: optional output sample rate, defaulting to 16000.
- `channels`: optional channel count, defaulting to mono.
- `write(chunk)`: required writer callback for `open_writer`; return the byte
  count written, or nil to mean the full chunk was accepted.
- `seek(offset, origin)`: required writer seek callback for formats that patch
  headers on close.

Encoder methods:

- `encoder:write_f32(frames)` writes a Lua numeric array and returns the number
  of frames written.
- `encoder:close()` finalizes and releases the encoder. Some writer failures
  are reported here because formats such as WAV patch headers during close.

## VOX And PTT

`audio.vox.open(opts)` and `audio.ptt.open(opts)` create bounded segmenters for
float32 mono 16 kHz PCM.

Common options:

- `segment(segment)` receives each emitted segment and must not retain the
  segment userdata after the callback returns.
- `state(event)` optionally receives state transitions.
- `min_segment_ms`, `max_segment_ms`, `memory_spool_bytes`, and
  `max_spool_bytes` pass through to the C facade.

VOX-specific options include `threshold`, `release_silence_ms`, and
`prebuffer_ms`.

VOX methods:

- `vox:push_f32_mono_16k(frames)`
- `vox:flush()`
- `vox:close()`

PTT methods:

- `ptt:press()`
- `ptt:push_f32_mono_16k(frames)`
- `ptt:release()`
- `ptt:flush()`
- `ptt:close()`

Segment methods:

- `segment:info()` returns `frame_count`, `t0`, `t1`, `segment_index`,
  `hard_cut`, and `is_final`.
- `segment:read_f32_mono_16k(frame_capacity)` pulls segment frames while the
  callback is active.

## Capture And Playback

Capture/playback helpers expose device receiver shells, but live device tests
are opt-in because microphones, speakers, and platform commands are
environment-sensitive.

- `audio.capture.open_default(opts)` opens the default input device.
- `audio.playback.open_default(opts)` opens the default output device.

Options:

- `backend`: one of the `DEVICE_BACKEND_*` constants.
- `buffer_ms`, `period_ms`: optional bounded-buffer/device timing controls.
- `state(event)`: optional capture state callback.

Capture methods:

- `capture:start()`
- `capture:wait_ready(timeout_ms)`
- `capture:read_f32_mono_16k(frame_capacity)`
- `capture:stop()`
- `capture:close()`

Playback methods:

- `playback:start()`
- `playback:write_f32_mono_16k(frames)`
- `playback:drain()`
- `playback:stop()`
- `playback:close()`

## Errors

Constructor and operation failures return `nil, err`, where `err` includes:

- `result`
- `result_string`
- `message`

Bad callback return values fail closed as ordinary audio I/O errors.

```lua
local audio = require("audio")

local chunks = {}
local encoder = assert(audio.encoder.open_writer({
  format = "wav",
  sample_rate = 16000,
  channels = 1,
  write = function(chunk)
    chunks[#chunks + 1] = chunk
    return #chunk
  end,
  seek = function()
    return true
  end,
}))

local frames = {}
for i = 1, 320 do
  frames[i] = 0.0
end

assert(encoder:write_f32(frames) == 320)
assert(encoder:close() == true)
```
