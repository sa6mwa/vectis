# Lua Facade Conventions

Vectis Lua modules follow the C SDK model where that model maps cleanly to Lua.
Dependency-native modules stay thin over their upstream C facades. Vectis-owned
modules provide workflow DX around Vectis concepts such as servers, auth,
packed assets, lockd consumers, file-backed transfers, and certificate
workflows.

## Naming And Options

Vectis-owned Lua helpers use the same public concept names as the C SDK where
they map cleanly to Lua. Handles use method-call syntax such as
`server:run()`, `server:close()`, `logger:close()`, and `session:open_file()`.
Configuration tables prefer explicit nouns over positional arguments when a
workflow has credentials, paths, TLS material, retry policy, body policy, or
ownership-sensitive handles.

Timeout and duration fields use unit-suffixed names. Millisecond values use
`*_ms`, such as `timeout_ms` and `connect_timeout_ms`; second-based protocol
fields keep their protocol name when that name is already established, such as
`visibility_timeout_seconds`.

## Errors

Expected runtime failures return structured values instead of raising:

- C-backed workflow failures return `nil, err` where `err.status`,
  `err.status_string`, and `err.message` mirror `vectis_status`.
  `err.source_code` mirrors `vectis_error_source`, and `err.source` is the
  canonical string for that source code.
  `err.dependency_code`, `err.http_status`, and `err.detail` are present only
  when the C SDK error object carries those diagnostics.
- Curl-backed workflow failures return a result table with `ok = false`,
  `transport_ok`, and `error.kind`. Their nested `error` table also carries
  `status`, `status_string`, `source`, and `source_code`; libcurl failures use
  `source = "curl"` and place the CURLcode in `dependency_code`.
- HTTP status failures use `error.kind = "http_status"` and preserve the
  response status as `error.http_status` plus body fields that are available.
- Vectis-owned pure Lua helper failures use the shared `vectis.status` helper
  for the same `status`, `status_string`, `source`, and `source_code` fields.
  For example, DSV callback stops are sourced from Vectis and DSV generated
  JSON decode failures are sourced from LoneJSON.
- Packed embedded bundle/assets helper failures return `nil, err` with
  `source = "vectis"` and the same structured status fields.
- Dependency-native cpkt-backed facades return `nil, err` with the same Vectis
  envelope and `source = "cpkt"`. They also keep dependency-native diagnostics such as
  `result`, `result_string`, and `dependency`; OPC UA service status details use
  `opcua_status` and `opcua_status_name` so `err.status` remains the Vectis
  status code.
- Lua callback abort/stop paths return `nil, err` when abort is normal workflow
  control.

Programmer misuse raises a Lua error. Examples include missing required
callbacks, missing required request bodies, missing destinations, or arguments
with the wrong shape.

## Ownership

Objects with an explicit `close()` method may also have a finalizer, but Lua
code should close them deterministically in service workflows. Helper functions
that borrow a handle for a callback must release the handle after the callback
returns or raises.

Callback-owned request/event tables contain copied scalar values unless the
module documentation explicitly marks a field as borrowed. Borrowed handles and
Lua-owned records are valid only for the documented callback or receiver
lifetime and must not be retained past that lifetime unless the module exposes a
specific retained object.

## Payload Shape

Function names must say whether payloads are materialized, file-backed,
spooled, callback-backed, or streaming. `parse()`, `parse_json()`, `download()`,
`upload()`, `parse_spill()`, and `stream_json()` are not interchangeable
terms. Do not describe a materialized or spooled path as streaming.

## Native And DX Layers

When an upstream Lua facade already exposes the full library, prefer
direct `require("<module>")` access. Add `vectis.*` helpers only where they
reduce service workflow friction or compose multiple Vectis-owned concepts.
