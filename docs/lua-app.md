# Vectis Lua App

`vectis.app` exposes C-owned app receivers for Lua applications. It
supports fixed routes, buffered Lua request callbacks, static mounts, WebDAV,
auth routes, and lockd consumer services.

For the supported generic content-site composition of these receivers, see
[Serving a Lua site](lua-site.md).

The supported Kore runtime and route surface is audited in
[`kore-runtime-surface-audit.md`](kore-runtime-surface-audit.md).

## Lifecycle

```lua
local vectis = require("vectis")

local app = assert(vectis.app.new({
  bind = "127.0.0.1",
  port = 8080,
}))

assert(app:run() == true)
app:close()
```

`app:run()` enters the foreground server runtime and returns after `SIGINT`,
`SIGTERM`, or `SIGQUIT`. `app:start()` starts a managed Kore child process
for tests and tools that need the Lua parent to continue; pair it with
`app:wait()` or `app:stop()`. `app:restart()` runs the same graceful
stop/start sequence for the whole Vectis app, including Kore and app-owned
daemon services. `app:reload()` is an alias for `app:restart()`. This is
the supported certificate rotation primitive for file-backed or source-backed
TLS material: the next managed Kore child rereads the configured certificate,
key, chain, and client CA material. Existing connections can be closed during
the restart. In-memory PEM values are immutable after app construction.
`shutdown_grace_ms` controls the managed runtime grace period before forced
child termination; zero or omission uses the Vectis default. `supervision_policy`
accepts `auto`, `direct`, or `supervised`:
`auto` uses direct foreground Kore unless app-owned services require the managed
supervisor topology, `direct` fails if such services are declared, and
`supervised` forces the managed supervisor topology for route-backed apps.
`service_failure_policy` accepts `fail_closed` or `continue`; the default
`fail_closed` stops the app when a monitored app-owned service fails, while
`continue` leaves the app running and reports the failed service through
`app:consumer_service_states()`.
`quiescence_policy` accepts `strict` or `warn_unavailable`; the default
`strict` fails closed when Vectis cannot prove the declaration process is safe
for Kore startup. `warn_unavailable` only applies on platforms without exact
thread inspection and does not allow known unsafe services or observed extra
threads.
`request_body_spool_dir` controls the directory Kore uses for request-body spill
files when a route enables streaming upload disk offload. Omission uses the
Vectis per-user runtime default under `XDG_RUNTIME_DIR` when available,
otherwise a UID-scoped `/tmp/vectis-http-body-<uid>` path; an empty string is
invalid.
`worker_count` controls the number of Kore HTTP worker processes. Omission or
zero preserves Kore's automatic CPU-count selection; explicit values must be at
most `253`.
The same table can also override server guardrails and worker resource knobs:
`max_connections`, `request_limit`, `worker_accept_threshold`,
`worker_rlimit_nofiles`, `worker_set_affinity`, `worker_shutdown_timeout_ms`,
`max_request_header_bytes`, `max_request_body_bytes`,
`request_header_timeout_ms`, `request_body_idle_timeout_ms`,
`response_write_idle_timeout_ms`, `request_body_min_rate_bytes_per_sec`,
`request_body_min_rate_grace_ms`, `idle_timeout_ms`, `keepalive_disabled`,
`keepalive_timeout_ms`, `keepalive_max_requests`,
`kore_curl_timeout_seconds`, `kore_curl_recv_max_bytes`, `kore_quiet`,
`worker_death_policy`, `socket_backlog`, `request_process_budget_ms`,
`hsts_max_age_seconds`, `websocket_max_frame_bytes`,
`websocket_timeout_ms`, `server_header`, `access_log_path`, and
`pretty_error_pages`. Zero uses the C default for most guardrails,
`hsts_max_age_seconds = 0` disables HSTS, `pretty_error_pages = false` keeps
bodyless framework 4xx/5xx responses minimal, `request_limit = 0` keeps Kore's
active request-object limit matched to `max_connections`, and
`max_request_body_bytes = 0` keeps the route-derived global body ceiling
behavior. `server_header` must be
omitted or non-empty because Kore does not expose a true Server-header
suppression switch. `access_log_path` is optional and disabled by default; when
set, Vectis preflights append access and applies the same Kore access log path
to every configured domain. `kore_curl_timeout_seconds = 0` and
`kore_curl_recv_max_bytes = 0` preserve Kore's compiled defaults for
Kore-owned curl operations such as ACME and do not affect Vectis application
curl/http clients. `kore_quiet = true` suppresses native Kore lifecycle
chatter; Vectis logging, metrics, and route responses are unaffected.
`worker_death_policy` accepts `"restart"` and `"terminate"`; omission keeps
Kore's restart behavior.

`client_ip = {trusted_proxies = {"203.0.113.10", "2001:db8::10"}}` enables
trusted reverse-proxy identity resolution. A route request's `ip` is otherwise
the accepted TCP peer. When that peer is trusted, Vectis resolves
`X-Forwarded-For` from right to left, stopping at the first untrusted address;
it uses a valid `X-Real-IP` only when no valid forwarded chain is present.
Trusted proxy entries must be numeric IPv4 or IPv6 addresses. Do not add a
public address merely because it appears in a header: doing so would let an
untrusted client select its own identity.

`profile = "production_webserver"` applies the same C-owned production webserver
profile as `vectis_app_config_init_production_webserver()`: strict quiescence,
fail-closed service failure handling, a longer graceful shutdown window,
explicit request-body guardrails, and conservative autoblock rules for repeated
401, 403, 404, 429, TCP-stall, and TLS-failure events. Explicit fields in
`vectis.app.new()` still override the profile. Metrics, auth routes, WebDAV
mounts, static mounts, and TLS material remain separate opt-in registrations.

Managed app-owned services inherit the app logger for lifecycle events
such as start, stop, and monitored failure. Lua service registration helpers
accept `logger_disabled = true` to suppress service lifecycle logging and
dependency logger inheritance for that service. C embedders can additionally
provide a per-service `pslog_logger *` override on managed service configs.

`vectis.app.new({tls = ...})` accepts the same manual and ACME modes as the
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

TLS servers also accept `version = "default" | "both" | "1.2" | "1.3"` and
`cipher_list` (or `cipher`) for Kore/OpenSSL protocol and cipher selection.
`default` and `both` use TLS 1.2 plus TLS 1.3. Invalid cipher lists fail before
Kore starts.

ACME uses `domains`, `email` (or `acme_email`), and `provider` (or
`acme_directory_url`). Its durable account and certificate state is a lockd
object with attachments. By default it uses `pouch://` under XDG state; an app
lockd endpoint is used when configured. `acme_storage_endpoint`,
`acme_storage_namespace`, and `acme_storage_key` select an explicit store.
`acme_state_dir` (or the compatibility alias `cache_dir`) is only Kore's
private derived runtime directory; omission creates an isolated mode-0700
directory under XDG cache. Vectis removes that generated runtime directory
after Kore stops; a caller-provided `acme_state_dir` remains caller-owned.

Every local Pouch client uses encrypted at-rest storage by default. Configure
the normal liblockdc key controls on the app `lockd` table when a specific key
file or provisioned key is required; remote lockd endpoints ignore them.

```lua
lockd = {
  endpoints = {"pouch:///srv/vectis/state"},
  pouch_crypto_key_file = "/etc/vectis/pouch.key",
  pouch_crypto_generate_key_file = false,
}
```

With no key setting or exact `pouch_crypto_key`/`pouch_crypto_key_file`
endpoint query option, Vectis creates
`${XDG_CONFIG_HOME:-$HOME/.config}/vectis/pouch.key` with mode 0600. This is a
Vectis key used only by Vectis-owned local Pouch persistence; separate
liblockdc clients retain their own configuration.

For deterministic container or other short-lived deployments, set
`VECTIS_POUCH_CRYPTO_KEY` to a non-empty liblockdc Pouch key string (for
example, `lc-pouch-key-v1:<base64url>`). It takes precedence over the app
`lockd` key and key-file settings for every Vectis-owned local Pouch client,
including metrics and ACME state. Vectis passes the value directly to
liblockdc, never logs it, and does not create a key file. Supply the same key
on every start that must reopen the same root; an empty environment value is a
configuration error.

Existing Pouch roots are never migrated or reinitialized by Vectis. A legacy
plaintext root, or a root encrypted with a different key, fails to open with
the configured encryption and is left intact. For pre-release data that may be
discarded, deliberately provision a new empty root; for retained data, restore
the original key. Vectis never falls back to plaintext and provides no force or
automatic-overwrite option.

To add a canonical cleartext-to-HTTPS redirect listener without registering a
route, enable it explicitly inside `tls`. `http_redirect` defaults to false;
when enabled it binds the TLS address and local port `8080` unless overridden.
It always uses a `308` response and preserves the request path and query while
removing the cleartext listener port from `Host`.

```lua
local app = assert(vectis.app.new({
  bind = "0.0.0.0",
  port = 8443,
  tls = {
    mode = "acme",
    domains = {"api.example.com"},
    email = "ops@example.com",
    http_redirect = true,
    http_redirect_port = 8080,
  },
}))
```

The redirect listener is independent of ACME validation. In particular, the
live ACME gate does not enable it: its existing public `:80` forwarding is not
treated as a redirect endpoint.

`make test-acme-live` is an explicit production Let's Encrypt E2E gate. It
defaults to `vectisdemo.c89.systems`, binds `0.0.0.0:8443`, probes the public
HTTPS URL, then starts a fresh Vectis app to prove hydration from the same
durable state. Run it only after the public `:443 -> :8443` forwarding is in
place:

```sh
VECTIS_LIVE_ACME_ENABLE=1 VECTIS_LIVE_ACME_EMAIL=ops@example.com \
  make test-acme-live
```

By default the test intentionally leaves storage selection to Vectis, so the
certificate state is retained in its default XDG-state Pouch store across test
runs. `VECTIS_LIVE_ACME_STORAGE_ENDPOINT`,
`VECTIS_LIVE_ACME_STORAGE_NAMESPACE`, and `VECTIS_LIVE_ACME_STORAGE_KEY` can
select another lockd endpoint or object.

## Fixed Routes

- `app:json(opts)` registers a fixed JSON response.
- `app:auth_json(opts)` registers a fixed JSON response guarded by a native
  or callback auth provider.
- `app:text(opts)` registers a fixed text response.
- `app:redirect(opts)` registers a fixed redirect response with a
  `Location` header.

Common route fields:

- `path`
- `method` or `methods`
- `status` or `status_code`
- `body`
- `content_type`
- `cache_control`

`app:redirect` also requires `location` and restricts status codes to
`300..399`.

```lua
assert(app:text({
  path = "/health",
  body = "ok\n",
  cache_control = "no-store",
}) == true)

assert(app:redirect({
  path = "/docs",
  location = "/static/docs/index.html",
  status = 303,
}) == true)
```

## Lua Callback Routes

`app:route(opts)` registers an ordinary route handled by a Lua callback.
The first callback surface is deliberately materialized, not streaming.

Route fields:

- `path`
- `method` or `methods`
- `body`: `nil`, `false`, `"none"`, `true`, `"buffered"`, or
  `{mode = "buffered", max_bytes = ...}`
- `auth`: optional native or callback auth provider config, using the same
  contract as `app:auth_json`
- `before`: optional function receiving the request before the handler
- `after`: optional function receiving the request and handler response value
- `handler`: function receiving a borrowed request table

The request table contains copied scalar fields plus borrowed lookup helpers
that are valid only during the handler call:

- `method`
- `path`
- `ip`: the effective client address under the app's `client_ip` policy
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
assert(app:route({
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

assert(app:route({
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

assert(app:route({
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

`app:group(opts)` registers a group of buffered callback routes with shared
defaults. The group accepts `prefix` or `path_prefix`, optional default `auth`,
`before`, `after`, `method`, `methods`, and `body`, plus a non-empty `routes`
array. Each route in `routes` is the same table accepted by `app:route()` and
overrides group defaults. Relative route paths are joined under the group
prefix.

```lua
assert(app:group({
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

`app:dsv(opts)` registers a C-owned streaming upload-reader route for
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
assert(app:dsv({
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

`app:openapi_doc(opts)` attaches OpenAPI metadata to a route path and
methods. `app:openapi(opts)` generates an OpenAPI document from attached
metadata using the C SDK generator.

Route helpers also accept an `openapi` table and attach it after the route is
registered. This works for `app:route()`, `app:group()`, fixed route
helpers, `app:dsv()`, and higher-level wrappers such as `vectis.rest.route()`
because they pass route fields through to `app:route()`.

OpenAPI schemas borrow Lua-owned LoneJSON schema userdata. The app retains
those schema objects until `app:close()` so generated docs remain valid after
normal Lua garbage collection.

OpenAPI fields:

- `summary`
- `operation_id` or `operationId`
- `tags`
- `request = {name = "...", schema = lonejson_schema}`
- `responses = {{status = 200, description = "OK", name = "...", schema = ...}}`

`app:openapi({title = "...", version = "...", format = "json"})` returns a
JSON string. `format = "yaml"` returns YAML.

```lua
local request_schema = lonejson.schema("order-request", {
  lonejson.field("sku", lonejson.string({required = true})),
})
local response_schema = lonejson.schema("order-response", {
  lonejson.field("id", lonejson.string({required = true})),
})

assert(app:route({
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

local spec = assert(app:openapi({
  title = "Orders API",
  version = "1.0.0",
  format = "json",
}))
assert(spec:find('"openapi":"3.1.0"', 1, true))
```

## Mounts

- `app:static_directory(opts)` serves a disk directory.
  - Vectis rejects symlinked root components and entries, so requests cannot
    escape `root_dir`. Publish copied files or real directories instead of
    symlink aliases.
  - `content_type` is optional. When omitted, Vectis infers the response type
    from the selected disk file extension and falls back to
    `application/octet-stream` for unknown extensions. When provided, it
    overrides inference for every file in the mount.
- `app:static_file(opts)` serves one disk file.
  - `content_type` is optional. When omitted, Vectis infers the response type
    from `file_path` and falls back to `application/octet-stream` for unknown
    extensions.
- `app:static_embedded(opts)` serves packed embedded assets read-only.
- `app:webdav(opts)` serves mutable WebDAV storage, either Vectis-managed
  storage or a direct disk `root_dir`.
- `app:webdav_embedded(opts)` serves packed embedded assets through a
  read-only WebDAV mount without extracting them.
- `app:webdav_embedded_site(opts)` extracts packed assets into mutable
  WebDAV storage and serves them.
- `app:auth_routes(opts)` registers native login/auth/WebDAV-key routes.
- `app:metrics(opts)` registers the opt-in C-owned metrics dashboard and
  JSON snapshot routes.
- `app:consumer_service(opts)` declares a C-owned lockd consumer service.
  The default `start = true` means "start with the selected app runtime"; it
  does not create a consumer pthread while the Lua script is still declaring a
  route-backed server.
- `app:consumer_service_states()` returns diagnostic snapshots for declared
  consumer services, including whether each service is materialized,
  process-local, requested to start, running, monitored, or failed.
- `app:curl_worker_service(opts)` declares a C-owned curl worker service over
  a borrowed `vectis.mailbox` request queue and optional
  `vectis.mailbox.broker` reply adapter. Use `vectis.curl_worker` to build and
  decode the copied HTTP envelopes.
- `app:curl_worker_service_states()` returns managed-service lifecycle
  diagnostics for declared curl worker services.
- `app:cai_worker_service(opts)` declares a C-owned CAI worker service over
  a borrowed `vectis.mailbox` request queue and optional
  `vectis.mailbox.broker` reply adapter. Use `vectis.cai_worker` to build and
  decode copied one-shot text/raw-JSON request envelopes. Lua callbacks are not
  accepted by the managed worker.
- `app:cai_worker_service_states()` returns managed-service lifecycle
  diagnostics for declared CAI worker services.
- `app:audio_worker_service(opts)` declares a C-owned audio worker service
  over a borrowed `vectis.mailbox` request queue and optional
  `vectis.mailbox.broker` reply adapter plus optional `event_mailbox` for VOX
  observations. Use `vectis.audio_worker` to build and decode copied bounded
  file decode/encode envelopes and VOX state/segment events. Lua callbacks are
  not accepted by the managed worker.
- `app:audio_worker_service_states()` returns managed-service lifecycle
  diagnostics for declared audio worker services.
- `app:sus_worker_service(opts)` declares a C-owned SUS worker service over
  a borrowed `vectis.mailbox` request queue and optional
  `vectis.mailbox.broker` reply adapter. Use `vectis.sus_worker` to build and
  decode copied bounded PCM/file transcription envelopes. Lua callbacks are not
  accepted by the managed worker.
- `app:sus_worker_service_states()` returns managed-service lifecycle
  diagnostics for declared SUS worker services.
- `app:mcp(opts)` registers a C-owned CAI Streamable HTTP MCP route with
  Lua-defined raw JSON tools.

`app:wait()` waits for `SIGINT`, `SIGTERM`, or `SIGQUIT` after a
process-backed `app:start()` and then runs the same app shutdown path as
libvectis. If the app has not been started, `app:wait()` enters the same
runtime as `app:run()`. Route-backed servers with declared background
services use the supervised runtime so Kore starts from a thread-clean process
and services materialize in the supervisor. Long-running scripts should use
`app:run()` or `app:wait()`, not shell commands or external sleep loops.

`app:upload(opts)` provides true request-body streaming through a bounded
upload reader and Lua chunk callbacks.
`stream_source` provides true live response streaming for buffered callback
routes. `app:sse(opts)` provides an SSE-specific convenience helper on top
of `stream_source`.
`spooled_source` is explicitly file-backed materialization, not live
producer-to-transport streaming.

## WebSocket Routes

`app:websocket(opts)` registers a Kore-backed WebSocket route. The route
callbacks receive borrowed connection handles owned by the active Kore callback;
do not retain those handles after the callback returns.

Fields:

- `path`: required WebSocket route path.
- `message(ws, opcode, payload)` or `handler(ws, opcode, payload)`: required
  message callback.
- `connect(ws)`: optional callback after handshake setup.
- `disconnect(ws)`: optional callback when Kore reports disconnect.

The `ws` handle exposes `send(opcode, payload)`, `send_text(text)`,
`send_binary(data)`, and `close()`. Opcode constants are available under
`require("vectis").websocket`: `CONTINUATION`, `TEXT`, `BINARY`, `CLOSE`,
`PING`, and `PONG`.

```lua
local vectis = require("vectis")

assert(app:websocket({
  path = "/ws",
  message = function(ws, opcode, payload)
    if opcode == vectis.websocket.TEXT then
      assert(ws:send_text("echo:" .. payload) == true)
    end
  end,
}) == true)
```

## MCP Routes

`app:mcp(opts)` mounts CAI's Streamable HTTP MCP server handler through the
Vectis/Kore route system. The default path is `/mcp`, and the default methods
are `GET`, `POST`, and `DELETE`. Lua MCP routes use a buffered normal route so
Lua tool callbacks execute in the Kore route domain. The C
`vectis_register_cai_mcp_route()` adapter preserves the streaming upload-reader
path for C-owned handlers; the Lua helper deliberately uses buffered request
and response materialization to keep Lua state ownership simple and local.

Fields:

- `path`: route path, default `/mcp`.
- `methods` or `method`: optional override; normally leave the MCP default.
- `name`, `version`: server metadata advertised by CAI.
- `request_max_bytes`, `response_spool_memory_limit`,
  `tool_output_max_bytes`, `disable_origin_validation`, `protocol_version`, and
  `require_protocol_version`: forwarded to CAI's MCP handler config.
- `response_max_bytes`: buffered Lua-route response ceiling.
- `enable_sessions`: rejected by the Lua helper until CAI session persistence
  callbacks are implemented for this surface.
- `tools`: non-empty array of Lua tool definitions.

Tool fields:

- `name`: required MCP tool name.
- `description`: optional tool description.
- `schema_json` or `schema`: required JSON schema string for the raw CAI tool.
- `strict`: optional boolean, default `true`.
- `callback(arguments_json)` or `run(arguments_json)`: required Lua callback.
  Return a JSON string or `nil, "message"` on failure.

```lua
assert(app:mcp({
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

`app:upload(opts)` registers a route whose request body is read through a
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
assert(app:upload({
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

`app:sse(opts)` registers a GET route that returns
`text/event-stream; charset=utf-8` with live chunked transfer. It delegates to
`app:route()`, so `path`, `method` or `methods`, `auth`, `before`, `after`,
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
assert(app:sse({
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

`app:metrics(opts)` enables metrics collection and registers the dashboard
and JSON snapshot routes. Metrics are disabled until this method is called.

```lua
assert(app:metrics({
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
XDG state. Local Pouch snapshots use the same encrypted default and `lockd`
key settings as ACME. See [metrics.md](metrics.md) for the full contract.

See [lua-auth.md](lua-auth.md) for native and callback auth providers,
OAuth2/OIDC helpers, email-token flows, and WebDAV-key issuance.
