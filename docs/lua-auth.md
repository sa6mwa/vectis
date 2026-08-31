# Vectis Lua Auth

`vectis.auth` exposes the C-owned Vectis auth implementation to Lua. The Lua
surface is intentionally a facade over the same native store, factors, tokens,
providers, issued credentials, and ordered login workflows used by `libvectis`
and the C-owned server receivers.

## Store

Most auth functions accept the same store fields:

- `credentials_path` or `path`
- `state_path` or `auth_state_path`
- `max_store_bytes`

`credentials_path` stores durable users, issued credentials, and OAuth2/OIDC
flows. `state_path` separates the higher-churn records used by lower-level auth
primitives such as directly issued email tokens. Native ordered workflow and
browser-session records are always libvectis-owned Lockd state, not JSON files
at either path.

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

`app:auth_routes(opts)` registers one C-owned ordered authentication workflow. It
always exposes JSON continuations at `<path_prefix>/m2m/start` and
`<path_prefix>/m2m/continue`, plus `<path_prefix>/logout`.

With `browser_session.mode = "m2m_and_browser"`, it additionally exposes
`GET <path_prefix>/login` and `POST <path_prefix>/continue`. There are no
all-fields login, email-token, or WebDAV-key routes. A request supplies only
the current factor; a non-terminal response identifies the next endpoint and
required field.

Set `steps` to one of these ordered policies:

- `{"password"}` or `{"password", "totp"}`
- `{"email_code"}` or `{"email_code", "totp"}`
- `{"email_code", "password"}` or
  `{"email_code", "password", "totp"}`

TOTP is never a first or only factor. Email-code workflows require SMTP.
Vectis matches an address to a unique enrolled recipient before delivery.
Unknown addresses receive the same opaque workflow shape but no message and
can never complete.

```lua
local flow = vectis.auth.workflow({
  credentials_path = "credentials.json",
  path_prefix = "/_vectis/auth",
  credential_purpose = "admin-api",
  steps = {"email_code", "password", "totp"},
  email_smtp = {
    url = "smtps://smtp.example.test",
    mail_from = "security@example.test",
  },
  browser_session = {
    mode = "m2m_and_browser",
    purpose = "admin-browser",
    state_key = "example.auth.browser-session",
  },
})

assert(flow:mount(app))
assert(app:auth_json({
  path = "/api/status",
  auth = flow:provider({purpose = "admin-api"}),
  body = '{"ok":true}\n',
}))
```

The JSON start request contains only the first step, for example
`{"email":"admin@example.test"}` or
`{"username":"admin","password":"secret"}`. It returns either a
terminal `201` with `client_id` and `client_secret`, or a `202` with
`workflow`, `next`, `step`, and `required`. Send the opaque workflow id and
exactly that next field to `next`. These custom M2M continuations are separate
from OAuth/OIDC and existing client-id/client-secret protocols, which remain
terminal M2M protocols and never require interactive factors.

Browser pages are responsive, dark-by-default C-owned forms. Each page contains
one factor. Six-character email codes auto-submit when complete. Vectis issues
a browser cookie only after a same-origin document navigation succeeds;
cross-site form posts receive `403`, never a cookie or M2M credential.

A browser workflow cookie is only an opaque short-lived identifier. The
principal, completed factors, token hash, expiry, and consumed state live in
Lockd. Completed, expired, and attempt-exhausted workflow records are deleted;
the Kore parent performs bounded automatic cleanup once per minute. Browser
session signing keys and session records are likewise Lockd-owned by
libvectis; Lua never receives either secret.

Custom browser shells are presentation-only. Set exactly one of
`browser_template_html`, `browser_template_path`, or
`browser_template_embedded_path`; every shell must contain exactly one
`{{content}}` insertion point. Vectis owns the form and opaque state.
`{{title}}`, `{{progress}}`, and `{{error}}` are escaped substitutions.

`vectis.auth.workflow(opts)` is the Lua facade. It provides `routes(opts)`,
`mount(app, opts)`, and `provider(opts)`; it stores no login state and does
not implement cookies. The matching C API uses `vectis_auth_routes_config`.
`vectis_auth_workflow_cleanup()` and
`vectis_auth_browser_session_cleanup()` are available for explicit bounded
maintenance outside request handlers.
## TOTP And QR Helpers

`vectis.auth.totp.new(secret)` returns a TOTP helper for generating codes,
validating codes, building provisioning URIs, and rendering QR data.

`vectis.auth.qr.new(text)` returns a QR helper for ANSI rendering and size
inspection.
