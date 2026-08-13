# Vectis Lua Server

`vectis.server` exposes C-owned server receivers for Lua applications. These
helpers register fixed routes and mounts without Lua request callbacks.

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

## Mounts

- `server:static_directory(opts)` serves a disk directory.
- `server:static_embedded(opts)` serves packed embedded assets read-only.
- `server:webdav(opts)` serves Vectis-managed mutable WebDAV storage.
- `server:webdav_embedded_site(opts)` extracts packed assets into mutable
  WebDAV storage and serves them.
- `server:auth_routes(opts)` registers native login/auth/WebDAV-key routes.
- `server:consumer_service(opts)` registers a C-owned lockd consumer service.

Dynamic request callbacks, middleware-like hooks, request body access, SSE, and
true streaming response helpers are not part of this fixed-route surface.

See [lua-auth.md](lua-auth.md) for native and callback auth providers,
OAuth2/OIDC helpers, email-token flows, and WebDAV-key issuance.
