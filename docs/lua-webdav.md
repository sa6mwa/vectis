# Vectis Lua WebDAV

`vectis.webdav` is a WebDAV workflow helper built on the bundled `curl`
facade. It keeps protocol transfer behavior in libcurl while providing
WebDAV-oriented method defaults, header shaping, and the same normalized result
contract as `vectis.http`.

## Loading

```lua
local vectis = require("vectis")
local webdav = require("vectis.webdav")

assert(vectis.webdav == webdav)
```

## Requests

- `webdav.request(opts)` executes a WebDAV/HTTP request through `curl.perform`.
- `webdav.get(opts_or_url[, opts])`
- `webdav.put(opts_or_url[, opts])`
- `webdav.delete(opts_or_url[, opts])`
- `webdav.mkcol(opts_or_url[, opts])`
- `webdav.propfind(opts_or_url[, opts])`
- `webdav.copy(opts_or_url[, opts])`
- `webdav.move(opts_or_url[, opts])`
- `webdav.download(opts)`
- `webdav.upload(opts)`

`opts.url` is required. Unless explicitly overridden, `protocols` defaults to
`http,https`.

Convenience fields map to WebDAV/HTTP headers:

- `depth` sets `Depth`.
- `destination` sets `Destination` and is required for `copy` and `move`.
- `overwrite` sets `Overwrite`.
- `authorization` sets `Authorization`.

Custom headers may also be supplied with `headers = { ... }`.

## File-Backed Transfer

`download_path` streams the response body into a file through libcurl's write
callback. `upload_path` and `body_path` stream request bytes from a file through
libcurl's read callback. These are file-backed transfers; ordinary response
bodies still use the bounded in-memory response buffer from `curl.perform`.

```lua
local uploaded = webdav.upload({
  url = "https://example.test/dav/report.csv",
  upload_path = "report.csv",
  authorization = "Bearer token",
})
assert(uploaded.ok, uploaded.error and uploaded.error.message)

local downloaded = webdav.download({
  url = "https://example.test/dav/report.csv",
  download_path = "downloaded-report.csv",
  authorization = "Bearer token",
})
assert(downloaded.ok, downloaded.error and downloaded.error.message)
```

## Result Contract

Results are normalized with `vectis.http.normalize`.

- `ok = true` means transport succeeded and the HTTP status is below 400.
- `transport_ok = true` means libcurl completed even if the protocol status is
  an application error such as `404`.
- `ok = false, error.kind = "transport"` means libcurl failed.
- `ok = false, error.kind = "http_status"` means the server returned HTTP 400
  or higher.
- nested `error` tables carry Vectis status/source metadata; WebDAV transport
  and HTTP status failures use `source = "curl"`, and protocol status is
  exposed as `error.http_status`.

`PROPFIND` defaults `depth` to `1` when not supplied.

```lua
local listed = webdav.propfind({
  url = "https://example.test/dav/",
  depth = 1,
})
if listed.ok then
  print(listed.body)
end
```

## Server Mounts

Server-side WebDAV stays on `vectis.app`.

- `app:webdav(opts)` registers an ordinary mutable WebDAV storage mount. By
  default it uses Vectis-managed storage; set `root_dir` to serve a direct
  mutable disk docroot.
- `app:webdav_embedded(opts)` registers a read-only WebDAV mount over packed
  embedded assets without extracting them. It supports `OPTIONS`, `PROPFIND`,
  `GET`, and `HEAD`; mutating WebDAV methods return `405`.
- `app:webdav_embedded_site(opts)` extracts packed embedded assets into the
  mutable WebDAV storage tree, then registers a WebDAV mount over that storage.

`app:webdav` requires:

- `path_prefix`, defaulting to `/`.
- `cache_dir`, an absolute cache/storage directory used for managed storage or
  direct-root metadata such as locks and transaction scratch space.
- `site_id`, a stable storage namespace containing letters, digits, `_`, or
  `-`.
- Optional `root_dir`, an absolute directory. When present, WebDAV content
  reads and writes go directly to that directory instead of
  `cache_dir/webdav/<site_id>/content`.

By default WebDAV mounts require auth. Set `auth_required = false` only for
deliberately public mounts. Auth tables accept the same native and callback
provider shapes used by `app:auth_json` and `app:webdav_embedded_site`.
`app:webdav_embedded()` accepts the same auth fields plus optional
`cache_control` and fallback `content_type`; it requires a packed binary with
embedded assets.

```lua
local vectis = require("vectis")
local server = assert(vectis.app.new({bind = "127.0.0.1", port = 8080}))

assert(app:webdav({
  path_prefix = "/dav",
  cache_dir = "/var/lib/myapp",
  site_id = "default",
  auth = {
    kind = "native",
    credentials_path = "/etc/myapp/credentials.json",
    realm = "myapp",
    purpose = "webdav",
  },
}) == true)
```

Direct disk-root mounts use the same WebDAV auth contract:

```lua
assert(app:webdav({
  path_prefix = "/files",
  cache_dir = "/var/cache/myapp",
  site_id = "files",
  root_dir = "/srv/myapp/files",
  auth_required = false,
}) == true)
```

`app:webdav_embedded()` is read-only over the packed asset tree.
