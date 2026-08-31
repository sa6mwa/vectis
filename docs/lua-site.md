# Serving a Lua Site with Vectis

Vectis can serve a conventional content site from the generic `vectis` binary.
Site-specific behavior lives in Lua; Vectis owns the HTTP runtime, native auth
flow, WebDAV protocol, and protocol clients. This is a composition guide, not a
Landed implementation or a requirement to pack the site into the executable.

The runnable [`examples/lua/site_server.lua`](../examples/lua/site_server.lua)
and its deterministic E2E scenario are the reference shape. The example uses
fixed credentials and unencrypted HTTP only because the E2E harness creates a
fresh isolated environment; do not deploy those values or that TLS setting.

## Reference fixture configuration

The example deliberately requires explicit filesystem paths so an accidental
working-directory default cannot publish or overwrite content. Its environment
contract is:

| Variable | Required | Meaning |
| --- | --- | --- |
| `VECTIS_LUA_SITE_BIND` | no | Bind address; defaults to `127.0.0.1`. |
| `VECTIS_LUA_SITE_PORT` | no | HTTP port; defaults to `28630`. |
| `VECTIS_LUA_SITE_CREDENTIALS` | yes | Native credential-store path. |
| `VECTIS_LUA_SITE_AUTH_STATE` | yes | State path for lower-level auth primitives; the fixture also derives its local Lockd/Pouch root from it. Native workflow and browser-session state live in Lockd. |
| `VECTIS_LUA_SITE_ASSET_ROOT` | yes | Public static asset directory. |
| `VECTIS_LUA_SITE_CONTENT_ROOT` | yes | Direct editable and publicly served content directory. |
| `VECTIS_LUA_SITE_CACHE` | yes | WebDAV cache, lock, and transaction-scratch directory. |

The example creates a fixed `site-admin` user for E2E isolation. Treat it as a
test fixture; production deployment should provision its own users and keep
the credentials and state paths persistent and private.

## Site shape

A site normally has four separate filesystem roots:

- an immutable asset root for CSS, JavaScript, images, and other public files;
- a mutable content root written through authenticated WebDAV and served through
  a separate public static mount;
- a WebDAV cache root for lock and transaction scratch state;
- private, persistent native-auth credentials and Lockd state.

Do not serve the private credential, Lockd, or WebDAV cache roots as static
content. Keep the WebDAV editor prefix separate from the public content prefix.
For example, `/content` is editor-only while `/published` reads the same direct
disk content root publicly.

```lua
local vectis = require("vectis")

local app = assert(vectis.app.new({
  app_name = "my-site",
  bind = "127.0.0.1",
  port = 8080,
  profile = "production_webserver",
  tls = { cert_key_bundle_path = "/etc/my-site/tls.pem" },
  lockd = { endpoints = {"pouch:///var/lib/my-site/lockd?single_writer=false"} },
}))

local credentials_path = "/var/lib/my-site/credentials.json"
local browser_session = { mode = "m2m_and_browser", purpose = "my-site-browser" }
local editor_auth = {
  kind = "native",
  credentials_path = credentials_path,
  realm = "my-site",
  purpose = "webdav",
  browser_session = browser_session,
}

assert(app:static_directory({
  path_prefix = "/assets",
  root_dir = "/srv/my-site/assets",
}) == true)
assert(app:static_directory({
  path_prefix = "/published",
  root_dir = "/srv/my-site/content",
}) == true)
assert(app:auth_routes({
  path_prefix = "/auth",
  credentials_path = credentials_path,
  realm = "my-site",
  credential_purpose = "webdav",
  browser_template_path = "/srv/my-site/templates/login-shell.html",
  steps = {"password"},
  browser_session = browser_session,
}) == true)
assert(app:webdav({
  path_prefix = "/content",
  cache_dir = "/var/cache/my-site/webdav",
  site_id = "my-site",
  root_dir = "/srv/my-site/content",
  conceal_unauthorized = false,
  auth = editor_auth,
}) == true)
assert(app:route({
  path = "/",
  handler = function()
    return {
      content_type = "text/html; charset=utf-8",
      body = "<!doctype html><main id=\"site-home\">Hello</main>\n",
    }
  end,
}) == true)

assert(app:run() == true)
app:close()
```

Provision native users and credential stores as a separate administration step;
do not create fixed development users on each production startup. See
[Lua auth](lua-auth.md) for user enrollment, browser flows, and credential
storage.

`cache_dir` and `site_id` are required even for a direct `root_dir` WebDAV
mount. `root_dir` is the actual editable content tree; `cache_dir` holds
WebDAV-managed metadata and scratch state. See [Lua WebDAV](lua-webdav.md) for
limits, direct-root semantics, and all supported methods.

## Native login pages

`app:auth_routes()` is an ordered C-owned authentication workflow. It always
offers JSON M2M start/continue endpoints, and with
`browser_session.mode = "m2m_and_browser"` it offers the dark responsive
browser form sequence at `<path_prefix>/login`. A browser page presents one
factor only; password authentication completes before TOTP, and email codes
complete before a later password or TOTP step.

For a site editor, configure a browser session and use the same session policy
on the native provider that protects WebDAV. The app also needs a persistent
Lockd endpoint because workflow state, browser-session records, and the
libvectis signing key are stored there.

```lua
local browser_session = {
  mode = "m2m_and_browser",
  purpose = "my-site-browser",
  state_key = "my-site.browser-session",
}

assert(app:auth_routes({
  path_prefix = "/auth",
  credentials_path = credentials_path,
  credential_purpose = "webdav",
  steps = {"password", "totp"},
  browser_template_path = "/srv/my-site/templates/login-shell.html",
  browser_session = browser_session,
}) == true)
```

A custom shell is presentation only: set one browser template source and put
exactly one `{{content}}` in it. Vectis injects the security-owned form and
may also expand escaped `{{title}}`, `{{progress}}`, and `{{error}}` values.
The default shell is usually sufficient and is dark by default, centered, and
mobile-safe. See [Lua auth](lua-auth.md) for JSON continuation contracts and
email-code policy.
## WebDAV editing and publication

An editor first completes the configured native login policy. Vectis issues a
scoped Basic credential for the `webdav` purpose; use that credential against
the editor prefix, never the user password as a substitute. A successful `PUT`
to `/content/article.html` writes `/srv/my-site/content/article.html`; the
public static mount can then serve it at `/published/article.html`.

Keep `auth_required` at its default of `true`. This guide sets
`conceal_unauthorized = false` so editors receive an explicit `401` and Basic
challenge. Sites that intentionally conceal the editor surface can leave the
default enabled, which reports unauthorized requests as not found instead.

The E2E scenario verifies anonymous reads and writes are rejected without
creating content, forged Basic credentials fail, an authenticated editor can
write and list content, the public mount serves that exact write, and logout
revokes the issued credential.

## Form mail and other SMTP

Lua site code can send mail through `vectis.smtp`. Supply a complete RFC 5322
message body and explicit transport limits:

```lua
local smtp = require("vectis.smtp")

local result = smtp.send({
  url = "smtps://mail.example.test:465",
  username = assert(os.getenv("SITE_SMTP_USERNAME")),
  password = assert(os.getenv("SITE_SMTP_PASSWORD")),
  from = "noreply@example.test",
  to = {"editors@example.test"},
  body = "Subject: New site message\r\n" ..
      "Content-Type: text/plain; charset=utf-8\r\n\r\n" ..
      "A visitor submitted the contact form.\r\n",
  timeout_ms = 5000,
  connect_timeout_ms = 2000,
})
assert(result.ok, result.error and result.error.message)
```

`vectis.smtp.send()` is a synchronous curl operation in the Lua code that calls
it. Keep timeouts bounded and do not let a slow mail relay turn an interactive
route into an unbounded wait. Its normalized result preserves structured curl
and Vectis error details. See [Lua SMTP](lua-smtp.md) for the complete option
table and file-backed message bodies.

## Production checklist

- Enable TLS and use the production webserver profile; configure trusted proxy
  addresses only when a reverse proxy is actually in front of the site. See
  [Lua app](lua-app.md).
- Keep credentials, auth state, WebDAV cache, SMTP secrets, and Pouch keys out
  of public directories and source control.
- Use stable writable paths for WebDAV content and auth state. A direct-root
  WebDAV mount does not require the content root to be packed.
- Use the same `VECTIS_POUCH_CRYPTO_KEY` value on every instance that must open
  the same Vectis-owned local Pouch state. Existing roots encrypted with a
  different key, or legacy plaintext roots, fail closed rather than migrating
  or falling back to plaintext.
- Exercise the generic-binary E2E scenario with `make test-e2e` after changing
  the serving, auth, or WebDAV composition.

For packed, self-contained site deployments, see
[Pack embedded filesystem and auth](pack-embedded-filesystem-auth-spec.md).
