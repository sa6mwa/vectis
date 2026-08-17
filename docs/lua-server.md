# Vectis Lua Server

`vectis.server` exposes C-owned server receivers for Lua applications. It
supports fixed routes, buffered Lua request callbacks, static mounts, WebDAV,
auth routes, and lockd consumer services.

## Lifecycle

```lua
local vectis = require("vectis")

local server = assert(vectis.server.new({
  bind = "127.0.0.1",
  port = 8080,
}))

assert(server:run() == true)
server:close()
```

`server:run()` enters the foreground server runtime and returns after `SIGINT`,
`SIGTERM`, or `SIGQUIT`. `server:start()` starts a managed Kore child process
for tests and tools that need the Lua parent to continue; pair it with
`server:wait()` or `server:stop()`. `shutdown_grace_ms` controls the managed
runtime grace period before forced child termination; zero or omission uses the
Vectis default. `supervision_policy` accepts `auto`, `direct`, or `supervised`:
`auto` uses direct foreground Kore unless app-owned services require the managed
supervisor topology, `direct` fails if such services are declared, and
`supervised` forces the managed supervisor topology for route-backed apps.
`service_failure_policy` accepts `fail_closed` or `continue`; the default
`fail_closed` stops the app when a monitored app-owned service fails, while
`continue` leaves the app running and reports the failed service through
`server:consumer_service_states()`.
`quiescence_policy` accepts `strict` or `warn_unavailable`; the default
`strict` fails closed when Vectis cannot prove the declaration process is safe
for Kore startup. `warn_unavailable` only applies on platforms without exact
thread inspection and does not allow known unsafe services or observed extra
threads.

`vectis.server.new({tls = ...})` accepts the same manual and ACME modes as the
C app config. Manual TLS can use paths or in-memory PEM strings:

- `cert_key_bundle_path`, `bundle_path`
- `cert_key_bundle_pem`, `bundle_pem`
- `certificate_path`, `cert_path`
- `certificate_pem`, `cert_pem`
- `private_key_path`, `key_path`
- `private_key_pem`, `key_pem`
- `ca_bundle_path`, `ca_path`
- `ca_bundle_pem`, `ca_pem`
- `client_ca_bundle_path`, `client_ca_path`
- `client_ca_bundle_pem`, `client_ca_pem`
- `require_client_certificate`

## Fixed Routes

- `server:json(opts)` registers a fixed JSON response.
- `server:auth_json(opts)` registers a fixed JSON response guarded by a native
  or callback auth provider.
- `server:text(opts)` registers a fixed text response.
- `server:redirect(opts)` registers a fixed redirect response with a
  `Location` header.

Common route fields:

- `path`
- `method` or `methods`
- `status` or `status_code`
- `body`
- `content_type`
- `cache_control`

`server:redirect` also requires `location` and restricts status codes to
`300..399`.

```lua
assert(server:text({
  path = "/health",
  body = "ok\n",
  cache_control = "no-store",
}) == true)

assert(server:redirect({
  path = "/docs",
  location = "/static/docs/index.html",
  status = 303,
}) == true)
```

## Lua Callback Routes

`server:route(opts)` registers an ordinary route handled by a Lua callback.
The first callback surface is deliberately materialized, not streaming.

Route fields:

- `path`
- `method` or `methods`
- `body`: `nil`, `false`, `"none"`, `true`, `"buffered"`, or
  `{mode = "buffered", max_bytes = ...}`
- `auth`: optional native or callback auth provider config, using the same
  contract as `server:auth_json`
- `before`: optional function receiving the request before the handler
- `after`: optional function receiving the request and handler response value
- `handler`: function receiving a borrowed request table

The request table contains copied scalar fields plus borrowed lookup helpers
that are valid only during the handler call:

- `method`
- `path`
- `body`, `body_size`
- `body_spooled`, `body_path`
- `principal` when an auth provider allowed the request with a principal
- `header(name)`
- `query(name)`
- `param(name)`

Handlers return:

- `nil` for `204`
- a string for a `200 text/plain; charset=utf-8` response
- a table with `status` or `status_code`, optional `content_type`, optional
  `headers`, and one of `body`, `file_path`, `spooled_source`, or
  `stream_source`

`spooled_source` is a file-backed response source, not true response streaming.
Vectis reads chunks from Lua into a temporary response file, then serves that
file through Kore. Use it when the application should avoid constructing one
large Lua string but can accept file-backed materialization. The source table
requires `read(max_bytes)` and accepts optional `reset()` and `close()`
callbacks. `read` returns a string chunk or `nil`/`false` at EOF. `reset` is
called before the first read when present; without it, the source is valid for
the initial response pass only. `close` is called when C closes the callback
source.

`stream_source` is a live response source, not file-backed materialization. The
source table requires `read(max_bytes)` and accepts optional `reset()` and
`close()`. `reset` is called once before streaming starts when present. `read`
returns a string chunk or `nil`/`false` at EOF. Vectis sends the response with
chunked transfer and reads the next Lua chunk only after Kore finishes sending
the previous chunk. It does not write a temporary response file and does not
concatenate the full response body.

`before` hooks run after auth and before the handler. Returning `nil` or
`true` continues into the handler. Returning a response table short-circuits
the route and sends that response.

`after` hooks run after the handler and receive `(request, response)`, where
`response` is the handler's unprocessed `nil`, string, or table response value.
Returning `nil` or `true` keeps the handler response. Returning a response
table replaces it. Hooks are part of the buffered route surface; they do not
make the route streaming.

```lua
assert(server:route({
  path = "/hello/:name",
  methods = {"GET", "POST"},
  body = {mode = "buffered", max_bytes = 4096},
  handler = function(request)
    return {
      status = request.method == "POST" and 201 or 200,
      content_type = "text/plain; charset=utf-8",
      headers = {["cache-control"] = "no-store"},
      body = request.param("name") .. "\n",
    }
  end,
}) == true)

assert(server:route({
  path = "/report.txt",
  handler = function()
    local chunks = {"first\n", "second\n"}
    local index = 1
    return {
      content_type = "text/plain; charset=utf-8",
      spooled_source = {
        reset = function()
          index = 1
          return true
        end,
        read = function()
          local chunk = chunks[index]
          index = index + 1
          return chunk
        end,
      },
    }
  end,
}) == true)

assert(server:route({
  path = "/events.txt",
  handler = function()
    local chunks = {"first\n", "second\n"}
    local index = 1
    return {
      content_type = "text/plain; charset=utf-8",
      stream_source = {
        read = function()
          local chunk = chunks[index]
          index = index + 1
          return chunk
        end,
      },
    }
  end,
}) == true)
```

`server:group(opts)` registers a group of buffered callback routes with shared
defaults. The group accepts `prefix` or `path_prefix`, optional default `auth`,
`before`, `after`, `method`, `methods`, and `body`, plus a non-empty `routes`
array. Each route in `routes` is the same table accepted by `server:route()` and
overrides group defaults. Relative route paths are joined under the group
prefix.

```lua
assert(server:group({
  prefix = "/api",
  auth = auth_provider,
  before = audit_request,
  after = add_common_headers,
  routes = {
    {
      path = "status",
      handler = function()
        return {content_type = "application/json", body = '{"ok":true}\n'}
      end,
    },
  },
}) == true)
```

## DSV Row Routes

`server:dsv(opts)` registers a C-owned streaming upload-reader route for
CSV/TSV/DSV request bodies. It uses the same DSV parser and LoneJSON schema
interop as `vectis.dsv.each()` and invokes Lua once per parsed row; it does not
materialize the full request body.

Route fields:

- `path`
- `method` or `methods`, default `POST`
- `schema`: LoneJSON schema for each row
- `on_row(row_number, row, request)`: required row callback
- `on_complete(request, summary)`: optional response callback
- DSV options such as `format`, `delimiter`, `header`, `columns`,
  `strict_row_width`, `comment_prefix`, and `max_field_bytes`
- `auth`: optional native or callback auth provider config
- `max_body_bytes` and `buffer_bytes`

`request.body` is `nil` for DSV routes because the body is consumed by the
streaming parser. `summary.rows` contains the number of accepted rows. Without
`on_complete`, Vectis returns `204`.

```lua
local schema = lonejson.schema("row", {
  lonejson.field("id", lonejson.string({required = true})),
  lonejson.field("count", lonejson.i64({required = true})),
})

local total = 0
assert(server:dsv({
  path = "/upload.csv",
  schema = schema,
  on_row = function(_, row)
    total = total + row.count
  end,
  on_complete = function(_, summary)
    return {
      status = 200,
      content_type = "application/json",
      body = '{"rows":' .. summary.rows .. ',"total":' .. total .. '}\n',
    }
  end,
}) == true)
```

## OpenAPI

`server:openapi_doc(opts)` attaches OpenAPI metadata to a route path and
methods. `server:openapi(opts)` generates an OpenAPI document from attached
metadata using the C SDK generator.

Route helpers also accept an `openapi` table and attach it after the route is
registered. This works for `server:route()`, `server:group()`, fixed route
helpers, `server:dsv()`, and higher-level wrappers such as `vectis.rest.route()`
because they pass route fields through to `server:route()`.

OpenAPI schemas borrow Lua-owned LoneJSON schema userdata. The server retains
those schema objects until `server:close()` so generated docs remain valid after
normal Lua garbage collection.

OpenAPI fields:

- `summary`
- `operation_id` or `operationId`
- `tags`
- `request = {name = "...", schema = lonejson_schema}`
- `responses = {{status = 200, description = "OK", name = "...", schema = ...}}`

`server:openapi({title = "...", version = "...", format = "json"})` returns a
JSON string. `format = "yaml"` returns YAML.

```lua
local request_schema = lonejson.schema("order-request", {
  lonejson.field("sku", lonejson.string({required = true})),
})
local response_schema = lonejson.schema("order-response", {
  lonejson.field("id", lonejson.string({required = true})),
})

assert(server:route({
  path = "/orders/:id?",
  method = "POST",
  openapi = {
    summary = "Create order",
    operation_id = "createOrder",
    tags = {"orders"},
    request = {name = "OrderRequest", schema = request_schema},
    responses = {
      {
        status = 201,
        description = "Created",
        name = "OrderCreated",
        schema = response_schema,
      },
    },
  },
  handler = function()
    return {status = 201, body = '{"id":"1001"}\n'}
  end,
}) == true)

local spec = assert(server:openapi({
  title = "Orders API",
  version = "1.0.0",
  format = "json",
}))
assert(spec:find('"openapi":"3.1.0"', 1, true))
```

## Mounts

- `server:static_directory(opts)` serves a disk directory.
- `server:static_embedded(opts)` serves packed embedded assets read-only.
- `server:webdav(opts)` serves mutable WebDAV storage, either Vectis-managed
  storage or a direct disk `root_dir`.
- `server:webdav_embedded(opts)` serves packed embedded assets through a
  read-only WebDAV mount without extracting them.
- `server:webdav_embedded_site(opts)` extracts packed assets into mutable
  WebDAV storage and serves them.
- `server:auth_routes(opts)` registers native login/auth/WebDAV-key routes.
- `server:metrics(opts)` registers the opt-in C-owned metrics dashboard and
  JSON snapshot routes.
- `server:consumer_service(opts)` declares a C-owned lockd consumer service.
  The default `start = true` means "start with the selected app runtime"; it
  does not create a consumer pthread while the Lua script is still declaring a
  route-backed server.
- `server:consumer_service_states()` returns diagnostic snapshots for declared
  consumer services, including whether each service is materialized,
  process-local, requested to start, running, monitored, or failed.
- `server:curl_worker_service(opts)` declares a C-owned curl worker service over
  a borrowed `vectis.mailbox` request queue and optional
  `vectis.mailbox.broker` reply adapter. Use `vectis.curl_worker` to build and
  decode the copied HTTP envelopes.
- `server:curl_worker_service_states()` returns managed-service lifecycle
  diagnostics for declared curl worker services.
- `server:cai_worker_service(opts)` declares a C-owned CAI worker service over
  a borrowed `vectis.mailbox` request queue and optional
  `vectis.mailbox.broker` reply adapter. Use `vectis.cai_worker` to build and
  decode copied one-shot text/raw-JSON request envelopes. Lua callbacks are not
  accepted by the managed worker.
- `server:cai_worker_service_states()` returns managed-service lifecycle
  diagnostics for declared CAI worker services.
- `server:audio_worker_service(opts)` declares a C-owned audio worker service
  over a borrowed `vectis.mailbox` request queue and optional
  `vectis.mailbox.broker` reply adapter plus optional `event_mailbox` for VOX
  observations. Use `vectis.audio_worker` to build and decode copied bounded
  file decode/encode envelopes and VOX state/segment events. Lua callbacks are
  not accepted by the managed worker.
- `server:audio_worker_service_states()` returns managed-service lifecycle
  diagnostics for declared audio worker services.
- `server:sus_worker_service(opts)` declares a C-owned SUS worker service over
  a borrowed `vectis.mailbox` request queue and optional
  `vectis.mailbox.broker` reply adapter. Use `vectis.sus_worker` to build and
  decode copied bounded PCM/file transcription envelopes. Lua callbacks are not
  accepted by the managed worker.
- `server:sus_worker_service_states()` returns managed-service lifecycle
  diagnostics for declared SUS worker services.
- `server:mcp(opts)` registers a C-owned CAI Streamable HTTP MCP route with
  Lua-defined raw JSON tools.

`server:wait()` waits for `SIGINT`, `SIGTERM`, or `SIGQUIT` after a
process-backed `server:start()` and then runs the same app shutdown path as
libvectis. If the server has not been started, `server:wait()` enters the same
runtime as `server:run()`. Route-backed servers with declared background
services use the supervised runtime so Kore starts from a thread-clean process
and services materialize in the supervisor. Long-running scripts should use
`server:run()` or `server:wait()`, not shell commands or external sleep loops.

`server:upload(opts)` provides true request-body streaming through a bounded
upload reader and Lua chunk callbacks.
`stream_source` provides true live response streaming for buffered callback
routes. `server:sse(opts)` provides an SSE-specific convenience helper on top
of `stream_source`.
`spooled_source` is explicitly file-backed materialization, not live
producer-to-transport streaming.

## MCP Routes

`server:mcp(opts)` mounts CAI's Streamable HTTP MCP server handler through the
Vectis/Kore route system. The default path is `/mcp`, and the default methods
are `GET`, `POST`, and `DELETE`. Request bodies flow through the upload-reader
path into CAI as a `cai_source`; responses are file-backed through the current
Vectis route model because CAI writes to a sink before Kore pulls the final
response.

Fields:

- `path`: route path, default `/mcp`.
- `methods` or `method`: optional override; normally leave the MCP default.
- `name`, `version`: server metadata advertised by CAI.
- `request_max_bytes`, `response_spool_memory_limit`,
  `tool_output_max_bytes`, `enable_sessions`, `disable_origin_validation`,
  `protocol_version`, and `require_protocol_version`: forwarded to CAI's MCP
  handler config.
- `tools`: non-empty array of Lua tool definitions.

Tool fields:

- `name`: required MCP tool name.
- `description`: optional tool description.
- `schema_json` or `schema`: required JSON schema string for the raw CAI tool.
- `strict`: optional boolean, default `true`.
- `callback(arguments_json)` or `run(arguments_json)`: required Lua callback.
  Return a JSON string or `nil, "message"` on failure.

```lua
assert(server:mcp({
  path = "/mcp",
  tools = {
    {
      name = "echo",
      description = "echo raw arguments",
      schema_json = '{"type":"object","properties":{"text":{"type":"string"}}}',
      callback = function(arguments_json)
        return '{"content":[{"type":"text","text":' ..
            string.format("%q", arguments_json) .. '}]}'
      end,
    },
  },
}))
```

MCP client support is intentionally not implemented in Vectis-owned Lua code;
use the upstream CAI Lua facade through `cai.mcp_client` or
`vectis.cai.mcp_client`.

## Upload Routes

`server:upload(opts)` registers a route whose request body is read through a
bounded streaming reader. It does not populate `request.body`; handlers receive
request metadata plus `body_streaming_upload = true`, and each non-empty chunk
is delivered to `on_chunk(request, chunk, state)` without materializing the
whole body. `path`, `method` or `methods`, `auth`, and `openapi` fields follow
the normal route contract.

Upload fields:

- `on_chunk(request, chunk, state)`: required callback. Return `nil` or `true`
  to continue.
- `open(request)` or `on_open(request)`: optional callback. Its return value is
  the per-request `state`; when omitted, Vectis creates an empty table.
- `on_complete(request, state)`: optional callback returning a normal route
  response table/string/nil after EOF. When omitted, the route returns `204`.
- `close(request, state)` or `on_close(request, state)`: optional cleanup
  callback called after completion or failure.
- `buffer_bytes`: maximum read size delivered to Lua per chunk.
- `max_body_bytes`: optional upload size limit.

```lua
assert(server:upload({
  path = "/upload/:name",
  auth = auth_provider,
  buffer_bytes = 8192,
  open = function(request)
    return { name = request.param("name"), bytes = 0, chunks = {} }
  end,
  on_chunk = function(request, chunk, state)
    assert(request.body == nil)
    assert(request.body_streaming_upload == true)
    state.bytes = state.bytes + #chunk
    state.chunks[#state.chunks + 1] = chunk
    return true
  end,
  on_complete = function(request, state)
    return {
      status = 200,
      content_type = "text/plain",
      body = state.name .. ":" .. tostring(state.bytes) .. "\n",
    }
  end,
}) == true)
```

## SSE Routes

`server:sse(opts)` registers a GET route that returns
`text/event-stream; charset=utf-8` with live chunked transfer. It delegates to
`server:route()`, so `path`, `method` or `methods`, `auth`, `before`, `after`,
and `openapi` fields follow the normal callback route contract.

SSE fields:

- `read(state, max_bytes)`: required callback returning the next event.
- `open(request)`: optional callback called before streaming starts; its return
  value becomes `state`.
- `close(state)`: optional callback called when the stream source closes.
- `headers`: optional response headers; `cache-control = "no-cache"` and
  `x-accel-buffering = "no"` are added when absent.

`read` returns `nil` or `false` at EOF, a string to send as a `data:` event, or
an event table. Event tables accept `id`, `event`, `data`, `retry`, `comment`,
or `frame`. `data` and `comment` values are split into SSE lines on newlines.
`frame` sends a preformatted SSE frame unchanged. Each produced frame must fit
inside the requested `max_bytes`; Vectis does not split one logical SSE event
across multiple reads.

```lua
assert(server:sse({
  path = "/events",
  open = function(request)
    return { topic = request.query("topic") or "default", index = 1 }
  end,
  read = function(state)
    if state.index == 1 then
      state.index = 2
      return {
        id = "1",
        event = "ready",
        data = "topic=" .. state.topic,
      }
    end
    return nil
  end,
}) == true)
```

## Metrics

`server:metrics(opts)` enables metrics collection and registers the dashboard
and JSON snapshot routes. Metrics are disabled until this method is called.

```lua
assert(server:metrics({
  path = "/.metrics",
  json_path = "/.metrics.json",
  title = "admin.example",
  auth = auth_provider,
  persistence_enabled = false,
}))
```

`auth` is optional and uses the same native/callback auth provider contract as
ordinary routes and WebDAV. Persistence is also optional; when enabled, Vectis
writes snapshots through lockdc and defaults to a local `pouch://` store under
XDG state. See [metrics.md](metrics.md) for the full contract.

See [lua-auth.md](lua-auth.md) for native and callback auth providers,
OAuth2/OIDC helpers, email-token flows, and WebDAV-key issuance.
