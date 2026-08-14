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

## Mounts

- `server:static_directory(opts)` serves a disk directory.
- `server:static_embedded(opts)` serves packed embedded assets read-only.
- `server:webdav(opts)` serves Vectis-managed mutable WebDAV storage.
- `server:webdav_embedded_site(opts)` extracts packed assets into mutable
  WebDAV storage and serves them.
- `server:auth_routes(opts)` registers native login/auth/WebDAV-key routes.
- `server:consumer_service(opts)` registers a C-owned lockd consumer service.

Middleware-like hooks, SSE, and true streaming response helpers are separate
future route surfaces; `server:route()` must not be used to describe streaming
behavior.

See [lua-auth.md](lua-auth.md) for native and callback auth providers,
OAuth2/OIDC helpers, email-token flows, and WebDAV-key issuance.
