# Vectis Lua Auth

`vectis.auth` exposes the C-owned Vectis auth implementation to Lua. The Lua
surface is intentionally a facade over the same native store, factor, token,
provider, and WebDAV-key workflows used by `libvectis` and the C-owned server
receivers.

## Store

Most auth functions accept the same store fields:

- `credentials_path` or `path`
- `state_path` or `auth_state_path`
- `max_store_bytes`

`credentials_path` stores durable users, issued credentials, and OAuth2/OIDC
flows. `state_path` stores higher-churn auth state when the application wants
that separated from durable credentials.

```lua
local vectis = require("vectis")

assert(vectis.auth.store_init({
  credentials_path = "credentials.json",
  state_path = "auth-state.json",
}) == true)
```

## Users And Credentials

`user_add(opts)` creates or updates a user:

- `username`
- `password`; if omitted, Vectis generates one
- `email` to enroll the recipient for email-token authentication; it must be
  non-empty and at most 319 bytes
- `totp` to generate a TOTP secret
- `totp_secret` to set a specific TOTP secret
- `totp_label`
- `totp_issuer` or `issuer`

The result may include `username`, generated `password`, enrolled `email`, `totp_secret`,
`totp_uri`, and ANSI `totp_qr`.

`user_login(opts)` validates username/password and optional TOTP:

- `username`
- `password`
- `totp_code`
- `time`
- `window`

The result is `{authenticated=..., auth_mode=..., client_id=..., claim_json=...}`.

```lua
local user = assert(vectis.auth.user_add({
  credentials_path = "credentials.json",
  username = "admin",
  totp = true,
  issuer = "vectis",
}))

local login = assert(vectis.auth.user_login({
  credentials_path = "credentials.json",
  username = "admin",
  password = user.password,
  totp_code = "123456",
}))
```

`issue(opts)`, `verify(opts)`, and `revoke(opts)` operate on issued client
credentials:

- `issue`: `subject`, optional `purpose`, `modes`, `max_record_bytes`
- `verify`: `authorization`, optional `allowed_modes`
- `revoke`: `client_id`

`modes` and `allowed_modes` accept the same auth mode names or constants exposed
as `vectis.auth.BASIC` and `vectis.auth.BEARER`.

## WebDAV Keys

WebDAV clients commonly support Basic auth but not browser OAuth2 refresh flows.
Vectis therefore issues application keys after the configured auth flow succeeds.

`webdav_key(opts)` validates a native user login and returns an issued Basic
credential table with `client_id`, `client_secret`, and optional `claim_json`.
Bearer credentials from `issue(opts)` expose `api_key`; Basic credentials do
not.

`basic_authorization(credential)` or
`basic_authorization(client_id, client_secret)` formats the HTTP
`Authorization` header value for issued Basic credentials:

```lua
local key = assert(vectis.auth.webdav_key({
  credentials_path = "credentials.json",
  username = "admin",
  password = "secret",
}))
local authorization = assert(vectis.auth.basic_authorization(key))
```

`oauth2_webdav_key(opts)` issues a WebDAV credential linked to a stored
OAuth2/OIDC flow:

- `flow_id`
- `subject`
- `max_record_bytes`

If a stored OAuth2/OIDC flow later fails refresh policy and
`revoke_webdav_keys_on_failure` is enabled, linked WebDAV keys are revoked by the
native C implementation.

## Email Tokens

`email_token_issue(opts)` creates a short-lived token:

- `username`
- `realm`
- optional `email`, which must exactly match the user's enrolled recipient
- `pending_transaction_id`
- `transaction_id`
- `token`
- `now` or `time`
- `ttl_seconds`
- `max_attempts`

Delivery and the token record always use the enrolled recipient. The result
includes `transaction_id`, `token`, and `expires_at`.

`email_token_verify(opts)` validates a token and returns:

- `verified`
- `expired`
- `username`
- `realm`
- `email`
- `pending_transaction_id`
- `failed_attempts`
- `max_attempts`

## OAuth2 And OIDC

`oidc_authorization(opts)` starts a browser authorization request and returns
`authorization_url`, `code_verifier`, `code_challenge`, `state`, and `nonce`.

Supported request fields include:

- `authorization_endpoint`
- `client_id`
- `redirect_uri`
- `scope`
- `state`
- `nonce`
- `code_verifier`
- `code_challenge`
- `audience`
- `resource`
- `verifier_bytes`
- `max_url_bytes`

`oidc_exchange_callback(opts)` validates an authorization callback query and
exchanges the code for tokens. It returns `code`, `state`, `token`, and `flow`.

`oauth2_client_credentials(opts)` runs a machine-to-machine token request and
returns the token response fields.

`oauth2_flow_ensure(opts)` refreshes or validates a supplied token flow.
`oauth2_flow_upsert(opts)` stores a flow, `oauth2_flow_load(opts)` loads one,
and `oauth2_stored_flow_ensure(opts)` loads and ensures a stored flow.

OAuth2/OIDC requests use the native HTTP transport by default. Tests and custom
integrations may provide `transport` or `http_callback`, a Lua function that
receives `{method, url, content_type, authorization, user_agent, body,
max_response_bytes}` and returns `{status_code, content_type, body}`.

## Providers

WebDAV and guarded routes do not hard-code the native user database. They call
an auth provider.

`provider_native(opts)` creates a native credentials-store provider:

- `credentials_path` or `path`
- `state_path` or `auth_state_path`
- `max_store_bytes`
- `purpose`
- `realm`
- `allowed_modes`
- `browser_session` (described below)

The provider has `kind = "native"` and can be called as
`provider:authenticate(request)`.

`provider_callback(fn[, opts])` creates a developer-provided Lua provider. Its
optional `opts.browser_session` is retained for app binding; the callback
receives a request table and must return:

- `action = "allow" | "deny" | "required" | "redirect"`
- optional `status_code`
- optional `location`
- optional `www_authenticate`
- optional `content_type`
- optional `body`
- optional `principal`

This is the Lua contract for plugging external auth, OIDC/OAuth2 integrations,
or application-owned policy into C-owned Vectis receivers.

```lua
local provider = vectis.auth.provider_callback(function(request)
  if request.authorization == "Bearer internal" then
    return { action = "allow", principal = "system" }
  end
  return {
    action = "redirect",
    status_code = 302,
    location = "/_vectis/auth/login",
  }
end)
```

## Server Integration

`vectis.app` consumes the same provider tables:

- `app:webdav({path_prefix=..., storage_path=..., auth=provider})`
- `app:webdav_embedded_site({path_prefix=..., auth=provider})`
- `app:auth_json({path=..., auth=provider, body=...})`

`app:auth_routes(opts)` registers the native browser login, email-token,
logout, and WebDAV-key endpoints. It accepts store fields plus:

- `path_prefix` or `prefix`
- `realm`
- `login_title`
- `login_template_html`
- `login_template_path` or `template_path`
- `login_template_embedded_path` or `template_embedded_path`
- `required_factors`
- `require_email_token`
- `email_token_ttl_seconds`
- `email_token_max_attempts`
- `email_token = {ttl_seconds=..., max_attempts=...}`
- `smtp` or `email_smtp` delivery configuration
- `browser_session`

The native implementation owns route lifecycle, factor sequencing, pending
transactions, email-token verification, and WebDAV-key issuance. Lua can mount
the routes and can replace auth policy through providers, but the built-in
native flow stays C-owned.

## Browser sessions

Browser sessions are optional and are owned entirely by `libvectis`: Lua only
declares policy and scope. Vectis creates the signing key, persists it and the
opaque server-side session records through the app's configured Lockd store,
and never exposes that key to Lua or application callbacks.

Use the same `browser_session` table on `app:auth_routes` and on every native
or callback provider that should accept the session. `browser_flow` forwards
it to both `routes()` and `provider()` automatically.

```lua
browser_session = {
  mode = "m2m_and_browser",
  cookie_name = "vectis_session",       -- default
  cookie_path = "/",                     -- default
  purpose = "browser",                   -- default session audience
  state_key = "auth.browser_session.v1", -- Lockd key prefix
  ttl_seconds = 30 * 24 * 60 * 60,        -- default: 30 days
}
```

`mode = "m2m_only"` is the default. It never issues or accepts session
cookies, preserving an M2M-only endpoint. `mode = "m2m_and_browser"` requires
an explicit app `lockd` configuration; it accepts valid sessions before native
or callback authentication. A valid callback-provider session therefore does
not invoke the Lua callback or expose a cookie for Lua to parse.
The standalone `provider:authenticate(request)` convenience call has no app
or Lockd owner, so it remains M2M-only even when its provider table also
declares `browser_session` for later app binding.

Vectis issues a cookie only after a successful native login/factor completion
when the request looks like a same-origin browser navigation: `Accept` contains
`text/html`, `Sec-Fetch-Mode` is `navigate`, `Sec-Fetch-Site` is
`same-origin`, and `Sec-Fetch-Dest`, when sent, is `document`. Missing or
mismatched signals, including cross-site form submissions, are treated as M2M
and receive no cookie. OAuth/OIDC and other M2M authentications do not issue
this cookie.

Issued and cleared cookies are `HttpOnly`, `Secure`, and `SameSite=Strict`.
Deploy browser sessions behind HTTPS; normal browsers do not send a `Secure`
cookie over plain HTTP. `cookie_path` can narrow the cookie's browser scope,
but it must include every protected route expected to accept the session.
`POST /logout` accepts a valid session cookie, revokes its Lockd record, and
returns a clearing cookie. It continues to revoke normal WebDAV credentials
when authenticated with `Authorization`.

The `lockd` table belongs on `vectis.app.new`; see [Vectis Lua App](lua-app.md)
for encrypted local Pouch and remote Lockd configuration. Its persistence must
outlive the server process when browser sessions must survive a restart.

[Serving a Lua site](lua-site.md) shows how a generic Lua site supplies a
branded native login template while retaining this C-owned flow.

`browser_flow(opts)` is a small Lua DX helper for that native route group. It
does not implement a separate auth mechanism. It preserves the shared store and
route configuration once and delegates to the C-owned functions:

```lua
local flow = vectis.auth.browser_flow({
  credentials_path = "credentials.json",
  state_path = "auth-state.json",
  path_prefix = "/_vectis/auth",
  realm = "admin",
  purpose = "webdav",
  allowed_modes = { vectis.auth.BASIC },
  required_factors = { "password", "totp", "email_token" },
  browser_session = { mode = "m2m_and_browser" },
})

assert(flow:mount(server))

assert(app:auth_json({
  path = "/api/status",
  auth = flow:provider(),
  body = '{"ok":true}\n',
}))

local authorization = assert(flow:webdav_authorization({
  username = "admin",
  password = "secret",
  totp_code = "123456",
}))
```

The helper methods are:

- `routes(opts)`: merge and return the `app:auth_routes` option table.
- `mount(server, opts)`: call `app:auth_routes(flow:routes(opts))`.
- `provider(opts)`: create a native provider for guarded routes and WebDAV.
- `webdav_key(opts)`: issue a WebDAV Basic credential after native login.
- `webdav_authorization(opts)`: issue a WebDAV credential and format the Basic
  `Authorization` header.

`vectis.auth.core` is the C-owned auth facade. `vectis.auth` re-exports the
same native functions and constants, adds `browser_flow`, and exposes the core
table as `vectis.auth.core`.

## TOTP And QR Helpers

`vectis.auth.totp.new(secret)` returns a TOTP helper for generating codes,
validating codes, building provisioning URIs, and rendering QR data.

`vectis.auth.qr.new(text)` returns a QR helper for ANSI rendering and size
inspection.
