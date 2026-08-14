# SUS Lua Facade

`require("sus")` exposes the raw cpkt SUS/whisper facade in the embedded
Vectis Lua runtime. It is intentionally separate from any future
`vectis.sus` DX helpers so raw model-cache and backend behavior remain visible.

The current Lua surface covers deterministic metadata, model catalog, path and
cache-open error paths, model handles, and offline cache status callbacks.
Loaded-model transcription and segmented decoder/VOX transcription remain
future work until the project has a committed model fixture/cache policy.

## Metadata And Constants

- `sus.facade_version()` returns the cpkt SUS facade ABI/version string.
- `sus.backend_version()` returns the linked whisper backend version.
- `sus.backend_system_info()` returns backend system information.
- `sus.backend_capabilities()` returns compiled backend capability labels.
- `sus.result_string(code)` returns a stable result string.

Result constants include `OK`, `ERR_ARG`, `ERR_ALLOC`, `ERR_MODEL`,
`ERR_UPSTREAM`, `ERR_CALLBACK`, `ERR_LOOKUP`, `ERR_IO`, `ERR_CHECKSUM`,
`ERR_NETWORK`, and `ABORTED`.

Cache status constants include `CACHE_STATUS_LOOKUP`, `CACHE_STATUS_HIT`, and
`CACHE_STATUS_MISS`.

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

## Model Handles

Successful model opens return a `sus.model` receiver shell.

Methods:

- `model:info()` returns `backend_version`, `backend_system_info`, and
  `cpu_only`.
- `model:reset_transcript_spacing()` resets instance-level segmented transcript
  spacing state.
- `model:close()` releases the loaded model.

Transcriber construction and transcription methods are not exposed yet because
they need deterministic model fixtures and callback lifetime tests.

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
