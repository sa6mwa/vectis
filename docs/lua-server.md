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

assert(server:start() == true)
assert(server:stop() == true)
server:close()
```

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
  `headers`, and either `body` or `file_path`

`before` hooks run after auth and before the handler. Returning `nil` or
`true` continues into the handler. Returning a response table short-circuits
the route and sends that response.

`after` hooks run after the handler and receive `(request, response)`, where
`response` is the handler's raw `nil`, string, or table response value.
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
- `server:consumer_service(opts)` registers a C-owned lockd consumer service.

SSE and true streaming response helpers are separate future route surfaces;
`server:route()` and `server:group()` must not be used to describe streaming
behavior.

See [lua-auth.md](lua-auth.md) for native and callback auth providers,
OAuth2/OIDC helpers, email-token flows, and WebDAV-key issuance.
