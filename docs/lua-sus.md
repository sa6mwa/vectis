# SUS Lua Facade

`require("sus")` exposes the raw cpkt SUS/whisper facade in the embedded
Vectis Lua runtime. It is intentionally separate from any future
`vectis.sus` DX helpers so raw model-cache and backend behavior remain visible.

The current Lua surface covers deterministic metadata, model catalog, path and
cache-open error paths, model handles, transcriber receiver shells, PCM
transcription methods, and offline cache status callbacks. Live model
transcription and segmented decoder/VOX transcription remain opt-in/future work
until the project has a committed model fixture/cache policy and a stable
audio/SUS Lua interop boundary.

## Metadata And Constants

- `sus.facade_version()` returns the cpkt SUS facade ABI/version string.
- `sus.backend_version()` returns the linked whisper backend version.
- `sus.backend_system_info()` returns backend system information.
- `sus.backend_capabilities()` returns compiled backend capability labels.
- `sus.result_string(code)` returns a stable result string.

Result constants include `OK`, `ERR_ARG`, `ERR_ALLOC`, `ERR_MODEL`,
`ERR_UPSTREAM`, `ERR_CALLBACK`, `ERR_LOOKUP`, `ERR_IO`, `ERR_CHECKSUM`,
`ERR_NETWORK`, and `ABORTED`.

Cache status constants include `CACHE_STATUS_LOOKUP`, `CACHE_STATUS_HIT`,
`CACHE_STATUS_MISS`, `CACHE_STATUS_DOWNLOAD_BEGIN`,
`CACHE_STATUS_DOWNLOAD_COMPLETE`, `CACHE_STATUS_VERIFY_BEGIN`,
`CACHE_STATUS_VERIFY_COMPLETE`, and `CACHE_STATUS_LOAD_BEGIN`.

Log level constants include `LOG_NONE`, `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`,
`LOG_ERROR`, and `LOG_CONT`.

## Model Catalog

- `sus.model_catalog_count()` returns the number of curated cache entries.
- `sus.model_catalog_entry(index)` returns the 1-based catalog entry.
- `sus.model_catalog_default()` returns the default curated model entry.
- `sus.model_catalog_find(name)` returns the named entry. Passing nil or an
  empty name selects the default entry.

Catalog entries include:

- `name`
- `provider`
- `source_url`
- `filename`
- `sha256`
- `size_bytes`
- `license`
- `quantization`
- `is_default`

## Opening Models

`sus.open_path(opts)` opens an explicit local model path.

Options:

- `path` or `model_path`
- `cpu_only`
- `preserve_initial_space_after_first_transcriber`

`sus.open_cached(opts)` resolves a model through the explicit cache-backed
resolver. This is the only SUS Lua entry point that may perform model-cache
network fetches.

Options:

- `model`
- `cache_dir`
- `sha256`
- `source_url`
- `insecure_no_checksum`
- `offline`
- `cpu_only`
- `preserve_initial_space_after_first_transcriber`
- `status(event)`

Checksum validation is enabled by default. Disabling it requires
`insecure_no_checksum = true`.

Cache status callbacks receive a Lua-owned event table with:

- `phase`
- `model`
- `cache_path`
- `source_url`

Return zero or true to continue. Return non-zero or false to abort with
`ERR_CALLBACK`.

## Backend Logging

`sus.set_log_sink(callback_or_nil)` installs or clears the process-wide cpkt SUS
backend log sink. Passing a function receives Lua-owned event tables:

- `level`
- `component`
- `message`

Passing `nil` or `false` clears the sink. The sink is process-wide because the
underlying cpkt/whisper logging hook is process-wide; install it deliberately
and clear it when a script no longer wants backend log events.

## Model Handles

Successful model opens return a `sus.model` receiver shell.

Methods:

- `model:info()` returns `backend_version`, `backend_system_info`, and
  `cpu_only`.
- `model:create_transcriber(opts)` returns a `sus.transcriber` receiver bound
  to the loaded model.
- `model:reset_transcript_spacing()` resets instance-level segmented transcript
  spacing state.
- `model:close()` releases the loaded model.

`model:create_transcriber(opts)` accepts:

- `threads`
- `cpu_only`
- `language`
- `translate`
- `timestamps`
- `initial_prompt`
- `segment(segment)` or `on_segment(segment)`
- `progress(percent)` or `on_progress(percent)`
- `abort()` or `should_abort()`

Segment/progress callbacks return `true`, `0`, or `nil` to continue, and
`false` or a non-zero number to fail with `ERR_CALLBACK`. Abort callbacks
return `true` or a non-zero number to request `ABORTED`.

Transcriber methods:

- `transcriber:transcribe_f32_mono_16k(frames)` runs inference over a Lua
  array of float samples at mono 16 kHz and returns `true`.
- `transcriber:transcribe_f32_mono_16k_text(frames)` returns materialized text
  from a Lua array of float samples at mono 16 kHz.
- `transcriber:revised_text()` returns the latest committed segmented
  transcript text.
- `transcriber:close()` releases the transcriber; the model remains open.

Segmented decoder/VOX methods are not exposed yet because they need a stable
Lua interop boundary for borrowing `audio.decoder` and `audio.segment` handles
without duplicating private userdata layouts.

## Errors

Failures return `nil, err`, where `err` includes:

- `result`
- `result_string`
- `message`

```lua
local sus = require("sus")

local default_model = assert(sus.model_catalog_default())
local model, err = sus.open_cached({
  model = default_model.name,
  cache_dir = "models",
  offline = true,
  status = function(event)
    print(event.phase, event.model, event.cache_path)
    return 0
  end,
})

if not model then
  assert(err.result == sus.ERR_IO or err.result == sus.ERR_MODEL)
end
```
