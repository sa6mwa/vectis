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
| `VECTIS_LUA_SITE_AUTH_STATE` | yes | Native pending-login and email-token state path. |
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
- private, persistent native-auth credentials and auth-state files.

Do not serve the private credential, auth-state, or WebDAV cache roots as static
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
}))

local credentials_path = "/var/lib/my-site/credentials.json"
local auth_state_path = "/var/lib/my-site/auth-state.json"
local editor_auth = {
  kind = "native",
  credentials_path = credentials_path,
  realm = "my-site",
  purpose = "webdav",
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
  auth_state_path = auth_state_path,
  realm = "my-site",
  login_template_path = "/srv/my-site/templates/login.html",
  required_factors = {"password"},
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

`app:auth_routes()` supplies a minimal browser login, factor continuation,
email-token, WebDAV-key, and logout route group. The default pages are usable,
but a site can provide its own presentation through exactly one of:

- `login_template_html` for inline HTML;
- `login_template_path` (or `template_path`) for a file used by a generic
  binary;
- `login_template_embedded_path` (or `template_embedded_path`) for an asset in
  a packed binary.

Vectis reads the selected file or embedded template when the route group is
registered. Change a template by updating its source and restarting the app;
it is not a live-reloaded template engine.

Templates receive only escaped substitutions:

- `{{login_title}}`, `{{realm}}`, and `{{path_prefix}}`;
- `{{continue_action}}` for normal browser-factor submission;
- `{{email_token_action}}` for email-token issuance;
- `{{webdav_key_action}}` for explicit WebDAV-key finalization or diagnostics.

For example:

```html
<!doctype html>
<title>{{login_title}}</title>
<link rel="stylesheet" href="/assets/site.css">
<form method="post" action="{{continue_action}}" data-realm="{{realm}}">
  <label>User <input name="username" autocomplete="username"></label>
  <label>Password <input name="password" type="password"
                         autocomplete="current-password"></label>
  <button type="submit">Sign in</button>
</form>
```

The template owns layout and branding, not security semantics. It must include
the fields required by its configured `required_factors` policy (for example,
`totp_code`, `email_transaction_id`, and `email_token` when those factors are
used) and post to `{{continue_action}}`. Native code continues to own factor
sequencing, pending transactions, key issuance, and logout revocation. Auth
responses include no-store headers; custom pages should not add credential
caching behavior.

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
