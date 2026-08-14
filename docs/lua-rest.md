# Lua REST Helpers

`require("vectis").rest` is a Vectis-owned helper layer over
`vectis.server`, `vectis.http`, LoneJSON, and `vectis.status`. It is for
buffered JSON API workflows. It is not a streaming route surface.

## Server Routes

- `rest.json(value[, opts])` returns a JSON route response table.
- `rest.error_response(err[, opts])` returns a structured JSON error response.
  If `err` itself contains a value LoneJSON cannot encode, `error_response`
  returns a minimal `500` JSON response describing the encode failure instead
  of raising a Lua assertion error.
- `rest.route(server, opts)` wraps `server:route()` with buffered JSON request
  decoding, optional validation, handler error mapping, and JSON response
  serialization.
- `rest.group(server, opts)` wraps `server:group()` for grouped buffered JSON
  routes with shared defaults.

`rest.route` accepts ordinary `server:route` fields such as `path`, `method`,
`methods`, `auth`, `before`, `after`, and `body`. Additional REST fields:

- `decode_json = false` disables request body decoding.
- `max_body_bytes` sets the default buffered JSON body limit when `body` is not
  provided.
- `validate(json, request)` can return `nil`/`true` to continue, a response
  table to short-circuit, or `false, err` to return a `400` JSON error.
- `handler(request)` receives `request.json` when a JSON body was decoded.
  Returning a route response table sends it directly. Returning another Lua
  value serializes that value as JSON. Returning `value, response_opts` lets the
  handler set status, headers, content type, or newline behavior.
- `error_handler(error, request)` can convert handler exceptions into a custom
  response table.

```lua
local rest = require("vectis.rest")

assert(rest.route(server, {
  path = "/orders/:id",
  method = "POST",
  validate = function(json)
    if type(json) ~= "table" or type(json.sku) ~= "string" then
      return false, {kind = "validation", message = "sku is required"}
    end
  end,
  handler = function(request)
    return {
      id = request.param("id"),
      sku = request.json.sku,
    }, {
      status = 201,
      headers = {["cache-control"] = "no-store"},
    }
  end,
}) == true)
```

REST routes are buffered. Use `server:dsv()` for DSV request-body streaming and
future dedicated route surfaces for SSE or true streaming responses.

## Client

`rest.client(defaults)` returns a base-URL JSON client. It supports `request`,
`get`, `post`, `put`, `patch`, and `delete`. Per-request `json = value` or a
table `body` is JSON-encoded and sent with `Content-Type:
application/json; charset=utf-8` unless a content type is already provided.
Responses use `vectis.http.request_json()` result normalization.
If request JSON cannot be encoded, the client returns the same result envelope
without attempting transport: `ok = false`, `transport_ok = false`, and a
structured `error` with `ERR_INVALID` and `ERROR_SOURCE_LONEJSON`.

```lua
local api = rest.client({
  base_url = "https://api.example.test",
  protocols = "https",
  timeout_ms = 5000,
})

local created = api.post("/orders", {
  json = {sku = "VX-100", qty = 2},
})
assert(created.ok == true)
assert(created.json.sku == "VX-100")
```
