# Lua SUS And Audio Contract

Vectis depends on the cpkt C89 facades for speech and audio:

- `cpkt_sus` over whisper.cpp
- `cpkt_audio` over miniaudio

The Lua facades are partially implemented as dependency-native modules. This
document records the required contract for completing them without weakening
the upstream C ownership, streaming, and model-cache semantics.

Current deterministic coverage includes:

- `require("audio")` constants/result strings, format capability checks,
  decoder file/URL/callback open, encoder file/callback open, callback
  reader/writer error propagation, VOX, PTT, and capture/playback receiver
  shells with opt-in live device tests, documented in `docs/lua-audio.md`;
- `require("sus")` constants/result strings, backend/facade metadata, model
  catalog lookup, path/cache open error handling, model handles, and offline
  cache status callback propagation, model-created transcriber handles,
  PCM table transcription methods, transcriber callback registration, and
  process-wide backend log sink configuration, documented in
  `docs/lua-sus.md`.

The audio/SUS Lua interop boundary lets `sus` borrow `audio.decoder` and
`audio.segment` handles without exposing private userdata layouts. Live model
transcription remains opt-in until a committed model fixture/cache policy
exists.

## Module Names

- `require("audio")` exposes the dependency-native cpkt audio facade.
- `require("sus")` exposes the dependency-native cpkt SUS/whisper facade.
- Vectis-owned DX helpers may later live under `vectis.audio` and `vectis.sus`
  only when they reduce real service workflow friction.

The dependency-native modules must remain available. Vectis helpers must not
hide the direct facades when full API coverage matters.

## Ownership

Lua handles wrap C receiver shells and own the corresponding C handle until
`close()`/`destroy()` or Lua garbage collection:

- `audio.decoder`
- `audio.encoder`
- `audio.capture`
- `audio.playback`
- `audio.vox`
- `audio.ptt`
- `sus.model`
- `sus.transcriber`

Lua must never expose miniaudio, whisper.cpp, ggml, C++ standard library, or
backend-specific pointers. Callback event tables are valid only for the callback
call. If Lua needs to retain event data, the binding must copy strings and
scalar fields into Lua-owned values.

## Streaming Rules

The Lua surface must preserve the upstream streaming behavior:

- decoder URL input streams through libcurl as the decoder pulls bytes;
- callback readers and writers must call Lua incrementally;
- capture and playback use bounded buffers;
- VOX/PTT segments are pullable and valid only during their segment callback;
- SUS segmented decoder transcription must not retranscribe previous audio;
- materialized transcript helpers must be named as materialized or text helpers.

Do not call a helper streaming if it buffers, downloads, spools, or concatenates
the full input or transcript behind the API. Bounded chunk buffers and the
upstream VOX/PTT spool caps are acceptable when documented.

## Audio Dependency-Native Surface

Initial `audio` Lua coverage should expose:

- `audio.version` or equivalent facade/backend version metadata if available;
- result constants and `audio.result_string(code)`;
- format constants for `wav`, `flac`, `mp3`, and `unknown`;
- `audio.can_decode(format)` and `audio.can_encode(format)`;
- `audio.decoder.open_file(opts)`;
- `audio.decoder.open_url(opts)`;
- `audio.decoder.open_reader(opts)` with Lua read/seek callbacks;
- `decoder:info()`;
- `decoder:read_f32_mono_16k(frame_capacity)` returning a Lua-owned PCM chunk;
- `decoder:close()`;
- `audio.encoder.open_file(opts)`;
- `audio.encoder.open_writer(opts)` with Lua write/seek callbacks;
- `encoder:write_f32(frames, opts)`;
- `encoder:close()`;
- `audio.vox.open(opts)` with `segment` and optional `state` callbacks;
- `vox:push_f32_mono_16k(frames)`;
- `vox:flush()`;
- `audio.ptt.open(opts)`, `ptt:press()`, `ptt:push_f32_mono_16k(frames)`,
  `ptt:release()`, and `ptt:flush()`.

Capture/playback device helpers should be added only with opt-in tests because
host audio devices and platform commands are environment-sensitive.

## SUS Dependency-Native Surface

Initial `sus` Lua coverage should expose:

- facade/backend version, system info, and capabilities;
- result constants and `sus.result_string(code)`;
- model catalog count/default/find/entry helpers;
- `sus.open_path(opts)` for explicit local model files;
- `sus.open_cached(opts)` for explicit cache-backed model resolution;
- cache status callbacks that can abort by returning non-zero;
- process-wide log sink configuration;
- `model:info()`;
- `model:create_transcriber(opts)`;
- `model:reset_transcript_spacing()`;
- `model:close()`;
- `transcriber:transcribe_f32_mono_16k(frames, opts)`;
- `transcriber:transcribe_f32_mono_16k_text(frames, opts)`;
- `transcriber:transcribe_audio_decoder_segmented(decoder, opts)`;
- `transcriber:transcribe_audio_decoder_segmented_text(decoder, opts)`;
- `transcriber:transcribe_audio_vox_segment(segment, opts)`;
- `transcriber:revised_text()`;
- `transcriber:close()`.

Segment, progress, abort, and segmented transcript callbacks must cross the
C/Lua boundary explicitly and return contracted values.

## Model Cache Policy

`sus.open_cached(opts)` is the only Lua API that may perform model-cache network
fetches. Ordinary `open_path` and transcriber construction must not perform
implicit network access.

The Lua cache options must map directly to cpkt:

- `model`
- `cache_dir`
- `sha256`
- `source_url`
- `insecure_no_checksum`
- `offline`
- `cpu_only`
- `preserve_initial_space_after_first_transcriber`
- `status` callback

Checksum verification remains enabled by default. Disabling it must require the
explicit `insecure_no_checksum = true` option.

## Deterministic Tests

Initial tests must avoid live microphones, speakers, live external network, and
large model downloads by default.

Required deterministic coverage:

- module preload and constant/result-string smoke;
- format decode/encode capability checks;
- decoder failure for missing or invalid files;
- callback reader contract, including read/seek error propagation;
- encoder writer callback contract for a small WAV output;
- VOX/PTT no-speech and small synthetic PCM behavior;
- SUS model catalog lookup/default metadata;
- cached-model offline miss and checksum error paths without network;
- callback return-value error propagation for cache status, segment, progress,
  abort, and segmented transcript sinks.

Live model transcription, live URL decoding, capture, and playback tests must
remain opt-in.
