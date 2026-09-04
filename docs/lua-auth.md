# Vectis Lua Auth

`vectis.auth` is a facade over the C-owned authentication subsystem. Users,
credentials, OAuth flows, email tokens, browser sessions, and native login
workflows are stored through Lockd; no Lua auth API accepts a filesystem
credential store.

## State selection

Every standalone auth operation accepts these fields:

- `app`: an open `vectis.app` which owns the selected Lockd client;
- `namespace`: the auth namespace, defaulting to `vectis.auth`;
- `state_key`: the durable auth record, defaulting to `auth/v1/store`;
- `transient_state_key`: an optional separate record for short-lived auth
  state; and
- `max_record_bytes`.

An app route always uses its app's Lockd configuration. For administration,
provision users from a separate `vectis -a users` process before starting a
route-backed server; opening a Pouch client in a server declaration process
would make the process non-quiescent before Kore starts.

The default is Vectis's encrypted Pouch root. Configure an app's `lockd`
table, or the CLI's Lockd/Pouch options, to select an explicit shared endpoint.
The auth namespace defaults to `vectis.auth`; application profile state belongs
in the separate `vectis.profile` namespace.

```lua
local vectis = require("vectis")

local app = assert(vectis.app.new({
  lockd = {
    endpoints = {"pouch:///var/lib/myapp/vectis?single_writer=false"},
  },
}))

-- Use this form only in a non-server administration/runtime context.
assert(vectis.auth.store_init({app = app, state_key = "auth/v1/store"}))
```

## Users and credentials

`user_add(opts)` creates or updates a user. It accepts `username`, `password`,
optional `email`, `totp`, `totp_secret`, `totp_label`, and `totp_issuer` (or
`issuer`). The result can include a generated password, a TOTP secret and URI,
and an ANSI TOTP QR rendering.

`user_login(opts)` validates `username`, `password`, optional `totp_code`,
`time`, and `window`, returning an authentication result.

`issue(opts)`, `verify(opts)`, and `revoke(opts)` manage machine credentials.
`issue` accepts `subject`, optional `purpose`, `modes`, and
`max_record_bytes`; `verify` accepts `authorization` and `allowed_modes`;
`revoke` accepts `client_id`. Modes use names or the `vectis.auth.BASIC` and
`vectis.auth.BEARER` constants.

`webdav_key(opts)` validates a native login and returns a scoped Basic
credential. `basic_authorization(credential)` formats its Authorization value.
`oauth2_webdav_key(opts)` derives a WebDAV credential from a stored OAuth flow.

## Email tokens and OAuth

`email_token_issue(opts)` accepts `username`, `realm`, optional enrolled
`email`, optional pending-login transaction binding, an optional deterministic
transaction id or token, `now`/`time`, `ttl_seconds`, and `max_attempts`. Its
result has `transaction_id`, `token`, and `expires_at`.

`email_token_verify(opts)` returns `verified`, `expired`, the associated user,
realm, email, pending transaction, and attempt counters. Token records always
remain in the selected Lockd state.

`oidc_authorization`, `oidc_exchange_callback`,
`oauth2_client_credentials`, `oauth2_flow_ensure`, `oauth2_flow_upsert`,
`oauth2_flow_load`, and `oauth2_stored_flow_ensure` provide the native OAuth2
and OIDC operations. A test or custom integration may supply `transport` or
`http_callback`; production defaults to Vectis's HTTP transport.

## Providers and routes

`provider_native(opts)` creates the native provider using the state-selection
fields above plus `purpose`, `realm`, `allowed_modes`, and an optional
`browser_session`. `provider_callback(fn[, opts])` supplies an application
policy callback which returns `allow`, `deny`, `required`, or `redirect`.

`app:auth_routes(opts)` registers an ordered C-owned workflow. It owns its
Lockd client and uses the app's Lockd selection. Set `namespace`, `state_key`,
`transient_state_key`, and `max_record_bytes` only to override the auth-domain
defaults for that route.

```lua
local flow = vectis.auth.workflow({
  state_key = "auth/v1/store",
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
    -- Defaults to this route's auth namespace; set only to isolate sessions.
    namespace = "vectis.auth",
  },
})

assert(flow:mount(app))
assert(app:auth_json({
  path = "/api/status",
  auth = flow:provider({app = app, purpose = "admin-api"}),
  body = '{"ok":true}\n',
}))
```

The JSON start endpoint receives only the current factor and returns either a
terminal credential or an opaque continuation. Browser pages similarly display
only the controls required now. They never expose the number of factors, a
future factor, or workflow progress.

An unauthenticated browser navigation to a protected HTML route redirects to
`<path_prefix>/login?return=<requested-path>`. On success Vectis returns to the
validated protected path; a direct visit without `return` goes to `/`.

Browser workflows require TLS. Their opaque HttpOnly, Secure, SameSite=Strict
cookie, signing key, completed-factor state, and expiry data are all
Lockd-owned. Custom templates are presentation shells with exactly one
`{{content}}` placeholder; Vectis owns controls, errors, and opaque workflow
state.
