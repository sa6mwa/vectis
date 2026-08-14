# Lua Facade Conventions

Vectis Lua modules follow the C SDK model where that model maps cleanly to Lua.
The raw dependency modules stay thin over their upstream C facades. Vectis-owned
modules provide workflow DX around Vectis concepts such as servers, auth,
packed assets, lockd consumers, file-backed transfers, and certificate
workflows.

## Errors

Expected runtime failures return structured values instead of raising:

- C-backed workflow failures return `nil, err` where `err.status`,
  `err.status_string`, and `err.message` mirror `vectis_status`.
  `err.source` and `err.source_code` mirror `vectis_error_source`.
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

## Payload Shape

Function names must say whether payloads are materialized, file-backed,
spooled, callback-backed, or streaming. `parse()`, `parse_json()`, `download()`,
`upload()`, `parse_spill()`, and `stream_json()` are not interchangeable
terms. Do not describe a materialized or spooled path as streaming.

## Raw And DX Layers

When an upstream Lua facade already exposes the full library, prefer
`require("<raw-module>")` for direct access. Add `vectis.*` helpers only where
they reduce service workflow friction or compose multiple Vectis-owned
concepts.
