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
- `totp` to generate a TOTP secret
- `totp_secret` to set a specific TOTP secret
- `totp_label`
- `totp_issuer` or `issuer`

The result may include `username`, generated `password`, `totp_secret`,
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
credential table with `client_id`, `client_secret`, `api_key`, and optional
`claim_json`.

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
- `email`
- `pending_transaction_id`
- `transaction_id`
- `token`
- `now` or `time`
- `ttl_seconds`
- `max_attempts`

The result includes `transaction_id`, `token`, and `expires_at`.

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

The provider has `kind = "native"` and can be called as
`provider:authenticate(request)`.

`provider_callback(fn)` creates a developer-provided Lua provider. The callback
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

`vectis.server` consumes the same provider tables:

- `server:webdav({path_prefix=..., storage_path=..., auth=provider})`
- `server:webdav_embedded_site({path_prefix=..., auth=provider})`
- `server:auth_json({path=..., auth=provider, body=...})`

`server:auth_routes(opts)` registers the native browser login, email-token,
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

The native implementation owns route lifecycle, factor sequencing, pending
transactions, email-token verification, and WebDAV-key issuance. Lua can mount
the routes and can replace auth policy through providers, but the built-in
native flow stays C-owned.

## TOTP And QR Helpers

`vectis.auth.totp.new(secret)` returns a TOTP helper for generating codes,
validating codes, building provisioning URIs, and rendering QR data.

`vectis.auth.qr.new(text)` returns a QR helper for ANSI rendering and size
inspection.
