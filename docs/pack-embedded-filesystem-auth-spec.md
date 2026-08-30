# Pack Embedded Filesystem And Auth Spec

## Goal

Vectis must support a self-contained packed executable that carries:

- One Lua entry script.
- Optional lockd client bundle material.
- Arbitrary application assets such as site files, templates, CSS, JavaScript,
  images, migrations, fixtures, and static configuration.

The packed asset set must be usable without extraction for read-only operations,
including as a static site docroot. It must also be possible to extract the
embedded asset set into an application-selected output directory and then use
that directory as a mutable WebDAV docroot. This is the path needed for a
future Landed migration: ship a complete site in one Vectis binary, initialize
or refresh a writable docroot from embedded assets, then let authenticated users
modify the extracted files through WebDAV.

This feature must not bake Landed-specific site content into Vectis. Vectis
ships the generic machinery. Tests generate a generic fixture site.

The packed service topology must also support a Kore-backed API or web server,
authenticated WebDAV, and an app-owned liblockdc `startconsumer` service running
simultaneously in one Vectis process. This is a required Landed migration shape:
HTTP/WebDAV must remain responsive while the lockd consumer receives and handles
messages, and scenario coverage must keep proving that behavior.

## Non-Goals

- Vectis will not embed HTML/CSS/site assets at build time as first-party
  Vectis content.
- Vectis will not copy Landed's production assets for tests.
- WebDAV write operations do not mutate the embedded payload in the executable.
- The packed Lua script is executable payload, not part of the readable embedded
  asset filesystem.
- The native login UI is minimal and replaceable. It is not a full admin
  application.

## Pack Format

Use a clean current `VECTIS_PACK` format. Vectis has not shipped, so the packed
executable format can be finalized without compatibility branches.
The format and pack action are supported on Linux; Darwin rejects pack before
creating an artifact.

The packed executable layout is:

```text
host vectis executable bytes
script payload
lockd bundle payload, optional
asset payload bytes
pack manifest JSON
fixed footer
```

The fixed footer records:

- magic and fixed footer layout,
- script offset, size, and SHA-256,
- lockd bundle offset, size, and SHA-256 when present,
- asset payload offset and size,
- manifest offset, size, and SHA-256,
- footer SHA-256 coverage rules.

The manifest JSON is parsed by LoneJSON and contains:

- format identifier,
- creation metadata needed for diagnostics,
- script metadata,
- lockd bundle metadata when present,
- asset entries,
- aggregate asset tree SHA-256 over logical paths, file SHA-256 values, sizes,
  and content-type metadata, excluding all source/build/home/cache paths,
- pack options that affect runtime behavior.

Each asset entry contains:

- logical path, normalized as absolute-within-archive using `/`,
- kind: `file`, `directory`, or `symlink` only if symlink support is explicitly
  enabled,
- byte offset and byte size for files,
- SHA-256 for files,
- mode bits limited to portable read/execute intent,
- content type when known or supplied,
- strong ETag as the quoted SHA-256 file validator,
- optional cache metadata such as cache-control.

Path rules:

- paths must be UTF-8 byte strings accepted by Vectis path normalization,
- no empty path components except root,
- no `.` or `..`,
- no absolute host paths,
- no Windows drive prefixes,
- no NUL bytes,
- no duplicate normalized paths,
- directory entries must precede or be inferable for file parents,
- extraction must refuse to follow symlinks unless symlink support is enabled
  and tested.

The manifest must not record source repository paths, build paths, user home
paths, or dependency cache paths. It may record logical pack source labels such
as `site`, `templates`, or `assets`.

## Pack CLI

The action surface remains under `-a/--action`:

```sh
vectis -a pack \
  --script app.lua \
  --output app \
  --asset-dir site:/path/to/site \
  --asset-dir templates:/path/to/templates \
  --asset file:/path/to/favicon.ico=/favicon.ico \
  --lockd-bundle bundle.pem
```

Required:

- `--script <path>`
- `--output <path>`

Optional:

- `--asset-dir <mount>:<path>` recursively embeds a directory under a logical
  mount or root path.
- `--asset <source>=<logical-path>` embeds one file.
- `--asset-manifest <path>` reads a LoneJSON manifest describing multiple
  assets:

  ```json
  {
    "assets": [
      {
        "source": "/absolute/or/relative/file",
        "path": "/templates/login.html",
        "content_type": "text/html; charset=utf-8"
      }
    ]
  }
  ```

  `source` and `path` are required. `content_type` is optional; when omitted,
  Vectis infers the content type from the logical `path` extension when it can.
- `--content-type-map <path>` supplies extension to content-type mappings.
  The map is LoneJSON and may be repeated:

  ```json
  {
    "types": [
      {
        "extension": ".avif",
        "content_type": "image/avif"
      }
    ]
  }
  ```

  `extension` may include or omit the leading dot. Later mappings for the same
  extension take precedence. Explicit per-asset `content_type` values still
  take precedence over the map.
- `--follow-symlinks` enables symlink traversal during packaging. Default is
  refuse.
- `--extract-mode <fail-exists|skip-existing|overwrite|verify|repair>`
  records the default extraction policy for application helpers as manifest
  `extract_mode`. Hyphenated CLI spellings are accepted and stored in canonical
  underscore form.
- `--lockd-bundle <path>` keeps the current lockd bundle behavior.

`--asset-dir` and `--asset` may be repeated. Logical asset destinations must
normalize under `/`. Collisions are errors.

Platform-specific pack limits and operational requirements are defined in
[pack-platform-operability.md](pack-platform-operability.md). Linux packaging
uses the tool-free copy-and-append format. Pack is unavailable on Darwin
because modifying the Mach-O would invalidate its required code signature.

## Runtime Embedded Filesystem

Vectis exposes a read-only embedded filesystem handle over the packed assets.
The handle is available to C and Lua only when the executable contains an asset
manifest.

Required C concepts:

- `vectis_embedded_fs`: read-only handle borrowed from the running executable.
- `vectis_embedded_fs_entry`: file, directory, size, SHA-256, content type,
  mtime equivalent, and a strong ETag borrowed from the embedded filesystem
  entry metadata.
- lookup, list, borrowed read, owned source open, and stream-to-sink
  operations.
- extraction operations from embedded fs to disk with explicit policy.

API ownership:

- Core pack, embedded filesystem, WebDAV, auth, SMTP/email-token, Kore serving,
  and lockd consumer integration surfaces are C-owned receiver-style handles
  where they have stateful lifecycle.
- Lua exposes thin adapters over those C surfaces. Lua may register
  application callbacks and route glue, but it must not own the native
  implementation semantics for the built-in pack/auth/WebDAV/server services.
- Developer-provided Lua auth or route callbacks cross the C/Lua boundary
  through explicit callback contracts; the native implementation remains usable
  from C without Lua translation.
- Lua app helpers return C-owned app receiver wrappers. For example,
  `vectis.app.new()` creates a native `vectis_app`, and
  `app:static_embedded()` registers a read-only embedded docroot through
  `app->static_embedded`; Lua does not implement the server dispatch path.

Required Lua concepts:

- `vectis.embedded.has_assets()`
- `vectis.embedded.stat(path)`, returning path, size, content type, SHA-256,
  and the strong ETag used by the C static embedded responder when available.
- `vectis.embedded.list(path)`
- `vectis.embedded.read(path)`
- `vectis.embedded.chunks(path, chunk_size)`
- `vectis.embedded.default_extract_policy()`
- `vectis.embedded.extract({to=..., policy=...})`
- `vectis.cert.generate_bundle({common_name=..., output_bundle_path=...})`
- `vectis.cert.inspect_bundle(path_or_table)`, returning subject, issuer,
  validity, serial, public-key metadata, CA flag, and SAN entries.
- `vectis.cert.validate_bundle(path_or_table)`
- `vectis.cert.validate_pair({certificate_path=..., private_key_path=...,
  ca_bundle_path=...})`
- `vectis.app.new({bind=..., port=..., tls={mode="manual",
  cert_key_bundle_path=..., domain=...}})`
- `vectis.app.new({bind=..., port=..., tls={mode="manual",
  cert_path=..., key_path=..., ca_path=..., domain=...}})`
- `vectis.app.new({bind=..., port=..., tls={mode="acme",
  domains={"example.com", "www.example.com"}, email=...,
  provider=..., acme_storage_endpoint=...}})`
- `app:static_directory({path_prefix=..., root_dir=..., index_file=...})`
- `app:static_embedded({path_prefix=..., cache_control=...})`
- `app:webdav_embedded_site({path_prefix=..., cache_dir=..., site_id=...,
  extract_policy=..., auth={kind="native", credentials_path=...}})`
- `app:webdav_embedded_site({path_prefix=..., cache_dir=..., site_id=...,
  auth={provider=vectis.auth.provider_callback(fn), purpose=...}})` for
  developer-provided auth adapters wired into the C-owned WebDAV receiver.
- `app:webdav_embedded_site({path_prefix=..., auth=provider})` where
  `provider` is a `vectis.auth.provider_native(...)` or
  `vectis.auth.provider_callback(...)` result.
- `app:auth_routes({path_prefix=..., credentials_path=...,
  auth_state_path=..., realm=..., login_template_path=...,
  required_factors={"email_token"}})`
- `app:auth_routes({path_prefix=..., credentials_path=...,
  auth_state_path=..., realm=...,
  login_template_embedded_path="/templates/login.html"})`
- `app:json({path=..., method=..., status=..., body=...,
  cache_control=...})` for small C-owned unguarded JSON endpoints in service
  scenarios.
- `app:text({path=..., method=..., status=..., body=...,
  cache_control=...})` for small C-owned unguarded text endpoints in service
  scenarios.
- `app:redirect({path=..., location=..., status=..., body=...,
  cache_control=...})` for small C-owned redirect endpoints in service
  scenarios.
- `app:auth_json({path=..., method=..., status=...,
  auth={kind="native", credentials_path=...}, body=...})` for small C-owned
  guarded JSON endpoints in service scenarios.
- `app:auth_json({path=..., auth={provider=...}, body=...})` for guarded
  service endpoints backed by a native or developer-provided auth provider.
- `app:auth_json({path=..., auth=provider, body=...})` for the same
  provider-object shorthand accepted by WebDAV mounts.
- `app:consumer_service({ ... })` is the Lua registration point for
  same-process lockd consumer service wiring. It creates a C-owned
  `vectis_consumer_service` receiver around liblockdc and starts it by default.
  The initial built-in handler is `handler.kind = "webdav_marker"`, which
  writes configured marker files into a WebDAV storage cache from the C worker
  callback. Direct Lua `on_message` callbacks remain rejected; a future Lua
  callback implementation needs an explicit C adapter or worker-owned Lua
  state/queue bridge.
- `app:run()` for foreground serving, plus process-backed `app:start()`,
  `app:wait()`, `app:stop()`, and `app:close()` for managed test/tool
  flows

Streaming semantics:

- `chunks()` and C sink APIs must stream from the mapped executable or bounded
  file reads. They must not materialize whole files unless the caller requests
  `read()`.
- Extraction streams file-by-file and verifies hashes after writing.
- `verify` extraction policy checks existing files without writing.
- `repair` extraction policy restores missing or hash-mismatched embedded files
  without pruning unrelated files.
- Disk extraction preserves embedded read/execute intent but adds owner write
  permission so the extracted docroot remains mutable through WebDAV.
- A failed extraction must leave actionable diagnostics and must not silently
  publish partial files as complete.

## Static Site And Docroot Modes

There are two supported serving modes:

1. Embedded read-only docroot.
   The site is served directly from the packed asset filesystem. It supports
   GET, HEAD, content type, ETag, `If-None-Match`, ETag-based `If-Range`,
   cache-control, single `bytes=` range requests, and traversal denial. It does
   not support WebDAV writes.

2. Extracted mutable docroot.
   The application calls an extraction helper to create or verify a filesystem
   docroot, then registers WebDAV over that directory. WebDAV writes operate on
   the extracted directory only.

Kore HTTP/API serving, WebDAV serving, and liblockdc `startconsumer` client
services must be composable in one Vectis process. Starting the web server must
not preclude an application-owned lockd consumer service from running, and
starting the lockd consumer must not monopolize the process such that Kore
routes or WebDAV requests stop making progress.

The implementation boundary for this model is C-first. Consumer-service
receivers are C-owned shells around liblockdc lifecycle objects. Lua may
configure the Vectis server and supported C-owned consumer handlers, but Lua
script-owned state must not be called directly from liblockdc consumer worker
threads. A future Lua callback implementation needs an explicit C adapter or
worker-owned Lua state/queue bridge that serializes callback execution and
preserves the Kore worker lifecycle. Direct Lua callbacks are rejected with a
structured validation error so mixed runtime-loop attempts fail predictably.

The WebDAV overlay model must support:

- protected WebDAV mounts backed by `vectis_webdav_auth_fn`,
- native auth provider or developer-provided auth provider,
- Lua developer auth providers crossing the C/Lua boundary through explicit
  provider request/response tables, while WebDAV remains C-owned,
- extracted docroot initialization before the server starts accepting writes,
- optional repair mode that restores embedded files that are missing or fail
  hash verification,
- no accidental deletion of user-created files unless the caller selected an
  explicit pruning policy.

## Native Authentication Workflows

Native authentication is an ordered, C-owned workflow. It has no all-fields
form and no `/email-token` or `/webdav-key` finalization endpoints. Every
factor is presented and verified before the next factor is disclosed.

The supported step sequences are:

- `password`
- `password`, `totp`
- `email_code`
- `email_code`, `password`
- `email_code`, `totp`
- `email_code`, `password`, `totp`

TOTP cannot be the sole step. Email-code sequences require SMTP and resolve a
recipient only when it maps uniquely to an enrolled user; unknown or ambiguous
recipients receive an opaque, non-completable workflow and no email delivery.

`POST <prefix>/m2m/start` and `POST <prefix>/m2m/continue` accept JSON. A
non-terminal response is `202` with an opaque `workflow`, the one next `step`,
and its required field. A terminal response is `201` with `client_id` and
`client_secret`. M2M responses never set cookies.

When `browser_session.mode` is `m2m_and_browser`, `GET <prefix>/login` and
same-origin document `POST <prefix>/continue` render the same ordered flow as
a responsive dark-mode page. Browser continuations require
`Sec-Fetch-Site: same-origin`, `Sec-Fetch-Mode: navigate`, and
`Sec-Fetch-Dest: document`; cross-site login submission is rejected before
credential or cookie issuance. The terminal browser result is a persistent,
signed, HttpOnly, Secure, SameSite=Strict session cookie. M2M-only is the
default and does not mount these browser routes.

Workflow records, browser-session records, and the browser-session signing key
are private libvectis data in the configured Lockd namespace. They are opaque
to Lua, expire by configured TTL, are deleted at completion/revocation, and
are pruned by the Vectis parent lifecycle timer. `credential_purpose` selects
the issued credential purpose (default `workflow`) so native providers can
align their accepted purpose with a WebDAV or API mount.

Browser customization is a shell only: `browser_template_html`,
`browser_template_path`, or `browser_template_embedded_path` must contain
exactly one `{{content}}`. Vectis owns all controls, fields, error messages,
workflow state, and form actions.

Lua config mirrors the C route config through `vectis.auth.workflow(opts)`:

```lua
local flow = vectis.auth.workflow({
  credentials_path = "credentials.json",
  path_prefix = "/_vectis/auth",
  steps = {"email_code", "password", "totp"},
  credential_purpose = "webdav",
  email_smtp = {url = "smtps://mail.example", mail_from = "auth@example"},
  browser_session = {mode = "m2m_and_browser"},
})
assert(flow:mount(app))
```

## Removed Pre-Workflow Contract

The following historical design is retained only as migration context. Its
route names, `required_factors` policy, pending JSON-file state, and template
placeholders are not supported by the current Vectis runtime.

The native auth mechanism remains independent of WebDAV. WebDAV asks an auth
adapter whether to allow, deny, challenge, or redirect. The native auth provider
may issue WebDAV app keys only after the configured authentication policy has
completed.

Supported factors:

- username and password,
- TOTP,
- email token,
- OAuth2/OIDC browser or machine flow where configured,
- externally registered C or Lua auth callbacks.

Factor policy is configurable per auth realm:

```json
{
  "realm": "vectis",
  "required_factors": ["password", "totp", "email_token"],
  "webdav_key_purpose": "webdav",
  "email_token": {
    "ttl_seconds": 300,
    "max_attempts": 5
  }
}
```

Allowed examples:

- email only: `["email_token"]`
- password only: `["password"]`
- password plus email token: `["password", "email_token"]`
- password plus TOTP: `["password", "totp"]`
- password plus TOTP plus email token:
  `["password", "totp", "email_token"]`

Multi-step behavior:

- The first request may submit any available credentials.
- If more factors are required, the auth provider returns
  `VECTIS_AUTH_REQUIRED` with a structured challenge describing the next
  required factor.
- The browser flow stores a short-lived pending auth transaction in the
  credentials store or configured auth state store.
- The next request supplies the pending transaction id plus the next credential.
- On successful completion, Vectis can issue a WebDAV app key for Basic auth
  clients. The user's password is not the WebDAV password when more factors are
  configured.

Email token behavior:

- Default token TTL is 300 seconds.
- Tokens are single-use.
- Tokens are stored hashed, not plaintext.
- Tokens are scoped to pending auth transaction id, username/principal, realm,
  purpose, and optional client fingerprint metadata.
- Expired, replayed, or failed tokens produce a generic auth failure.
- Token delivery uses the Lua curl facade or C libcurl SMTP support through an
  explicit SMTP configuration.
- SMTP configuration includes URL, sender, TLS settings, credentials, allowed
  recipient domains or explicit recipient allowlist, timeout, and optional CA
  settings.

## Native Login Endpoints And Templates

Vectis should provide a minimal native login handler set that applications can
mount. Applications may replace every template through config.

Default C-side templates:

- login form: username, password, TOTP code, email token when required,
- pending factor form: next factor only,
- WebDAV key result page,
- generic auth failure page.

Template rules:

- defaults are compiled C strings, not embedded Vectis site files,
- custom templates are loaded from configured paths or the packed asset fs,
- templates must support escaped substitutions only,
- browser forms should post to `{{continue_action}}`; `{{webdav_key_action}}`
  remains available for explicit lower-level WebDAV-key finalization links or
  diagnostics,
- the built-in default form renders email transaction/token inputs and an
  email-token request form only when the route policy requires the email-token
  factor,
- no bespoke JSON or HTML parser is introduced,
- responses include cache-control headers preventing credential caching.

Suggested route contract:

- `GET /_vectis/auth/login`
- `POST /_vectis/auth/login`
- `POST /_vectis/auth/email-token`
- `POST /_vectis/auth/continue`
- `POST /_vectis/auth/webdav-key`
- `POST /_vectis/auth/logout`

Applications may mount the handlers elsewhere or implement equivalent Lua
routes against the same C auth/session APIs.

Current native route behavior includes a C-owned `/email-token` issue endpoint,
`/login`, `/continue`, and `/webdav-key` credential finalization endpoints
using the same factor policy, plus `/logout` to revoke the credential presented
in the Authorization header. Native login templates render `continue_action` as
the default browser form action, while `webdav_key_action` remains a supported
alias placeholder for explicit lower-level flows. Routes default to the password
factor, which also enforces enrolled-user TOTP. `required_factors` can also
name an explicit `totp` factor together with `password`; that policy fails
closed for users without TOTP enrollment instead of silently accepting password
only.
Password-first browser continuation is backed by locked `pending_logins`
records in the configured auth state store: when the password is valid but TOTP
or email-token factors are still missing, the native route returns a
short-lived `pending_transaction_id`; a later `/continue` or `/webdav-key` POST
can supply that transaction id plus the remaining factor fields without
resending the password. `state_path`/`auth_state_path` stores high-churn
pending-login and email-token records separately from durable credentials when
configured, while deployments that want one JSON file can omit it and use the
credentials JSON fallback.
`required_factors={"email_token"}` allows an email-token-only WebDAV-key flow,
while `require_email_token=true` is a convenience alias that adds the
email-token factor to the default password flow. C-owned SMTP delivery is
available through the route config's
`email_smtp` settings; Lua only passes configuration into the native route
registration and does not own auth or delivery semantics. The packed service
smoke now exercises SMTP delivery through a local mock SMTP harness and reads
the delivered token from a mock mailbox before issuing a WebDAV app key. Email
token issue accepts `pending_transaction_id` when a browser flow has already
created one, stores that scope with the hashed token record, and rejects
verification attempts whose pending transaction id does not match. Email-token
records also carry a failed-attempt counter and max-attempt budget, defaulting
to five attempts; wrong token submissions increment the counter and consume the
token once the budget is reached. The Lua auth facade accepts the same pending
transaction and max-attempt fields and returns pending transaction and attempt
state from token verification.

## Configuration Model

Configuration must be explicit and application-owned. Defaults may choose
XDG-style locations, but applications can set their own config and state dirs.

Suggested top-level concepts:

- `credentials_path`: user and credential database.
- `auth_state_path` or `auth_state_dir`: pending login transactions and email
  token state.
- `packed_assets`: runtime options for embedded docroot and extraction.
- `webdav`: mount path, extracted docroot path, auth requirement, limits.
- `smtp`: delivery configuration for email-token auth.
- `tls`: self-signed cert, provided cert paths, ACME configuration.
- `acme`: account/state directory, directory URL, email, domains, staging flag.

ACME and self-signed certificate support remain separate from pack. The packed
site scenario must be able to run with generated private self-signed certs for
local deterministic tests and with ACME/Let's Encrypt-style configuration in
application deployments.

## E2E Scenario Contract

The executable contract for current native auth is the Lockd-backed workflow
suite: it verifies M2M password and password+TOTP continuations, browser
same-origin enforcement and signed-session revocation, and the packed service
email-code → password → TOTP progression. References below to the removed
endpoint family describe the former implementation only and must not be used
as an integration contract.

Do not use Landed assets. Generate a generic test site during the test:

```text
site/
  index.html
  app.css
  app.js
  templates/login.html
  assets/logo.txt
```

Required deterministic e2e coverage:

- pack Lua script plus generated site assets,
- run packed executable as an HTTP server,
- serve `/`, CSS, JavaScript, and asset files from the embedded read-only
  docroot,
- verify traversal attempts fail,
- verify ETag or SHA-256 based cache metadata,
- extract embedded assets to a temp docroot,
- serve extracted docroot,
- protect WebDAV with native auth,
- deny anonymous WebDAV writes,
- complete login with the configured factor policy, including deterministic
  TOTP continuation, rejection, and success through the native HTTP login route,
- issue a WebDAV app key after successful auth,
- write, list, copy, move, and delete files through WebDAV with the app key,
- verify user mutations affect only the extracted docroot, not embedded assets,
- expose one auth-guarded routed page or JSON endpoint through the same auth
  provider,
- run Kore HTTP/API routes, WebDAV, and a liblockdc `startconsumer` client
  service simultaneously in the same Vectis process, proving the web server
  remains responsive while the consumer service receives and handles lockd
  messages,
- run HTTPS using a generated private self-signed certificate,
- run HTTP listener behavior expected by the app, such as direct HTTP serving or
  redirect to HTTPS depending on config,
- exercise ACME configuration parsing and state path behavior with a local mock
  ACME service or fixture. Live Let's Encrypt checks must be explicit opt-in.

Current generated packed-service coverage in `make test-e2e` packages a Lua
webserver script plus generated HTML/CSS/JavaScript files, asset files, and
template files, runs the packed executable, serves the embedded read-only
docroot both at `/` and under `/site` including non-index assets from the root
mount, rejects writes through the root read-only mount without changing the
embedded index, lists packed assets,
serves generated CSS, JavaScript, asset, and template files through ordinary GET
requests, reads them through both full-read and source-backed chunk-iterator
Lua embedded filesystem APIs, verifies bodyless HEAD metadata and HEAD
byte-range metadata for the embedded static docroot, rejects literal and
encoded traversal attempts against both read-only mounts, verifies single
byte-range and
unsatisfiable byte-range behavior for the C-owned static embedded responder,
rejects multi-range requests so the implementation remains a single-range
responder, verifies `If-None-Match` returns bodyless 304 responses for GET and
HEAD requests against unchanged embedded assets, verifies `If-None-Match`
precedes `Range` for both GET and HEAD conditional requests without emitting
partial-content metadata, verifies matching and stale `If-Range` validators for
embedded range requests, starts a second
packed HTTPS asset server with a generated private self-signed certificate,
verifies content types, ETag and cache metadata, rejects traversal attempts,
rejects writes through the read-only mount, extracts the embedded
assets into the disk WebDAV content tree before accepting WebDAV operations,
verifies generated files are present in that extracted docroot, verifies repair
restores stale embedded files while preserving pre-existing mutable files,
exposes native auth routes, protects JSON API routes through native and
Lua callback auth providers, protects WebDAV mounts through nested and direct
Lua callback auth providers, including required, redirect, and allow adapter
responses through the packed WebDAV receiver, delivers an email token through a
local mock SMTP server, turns
password-only login for a TOTP/email-token route into a pending transaction,
loads a native auth login template from the packed embedded asset filesystem
and expands its placeholders through the running packed service, rejects
wrong-token and replayed-token WebDAV key requests, rejects expired email-token
transactions, rejects SMTP delivery to non-allowlisted recipients, requires
TOTP continuation for a missing TOTP code with an otherwise valid email-token
transaction, rejects a wrong TOTP code, issues a WebDAV key after deterministic
password-only login for a non-TOTP user, requires TOTP continuation for a
password-only route when the user has TOTP enrollment, issues a WebDAV key after
deterministic password+email-token login for a non-TOTP user, issues a WebDAV
key after deterministic password+TOTP login for a TOTP user, rejects explicit
password+TOTP auth for a non-TOTP user, issues a WebDAV key after deterministic
password-first pending continuation with TOTP+email-token login for a TOTP
user, protects the WebDAV mount,
rejects anonymous WebDAV reads and writes without mutating the extracted
docroot, exercises the native `POST /login` route for a password-only realm,
uses the issued key against an auth-guarded API route, revokes it through
logout, issues a second WebDAV key through the direct all-factor
password+TOTP+email-token `/webdav-key` path and proves it works for both the
guarded API and WebDAV, rejects authenticated WebDAV request-path and
Destination-header traversal without creating escaped files, serves embedded
content through authenticated WebDAV, accepts mutable WebDAV writes, verifies
those writes land in the extracted docroot, deletes an
extracted embedded asset through WebDAV while leaving the packed read-only mount
unchanged and suppressing the deleted asset from WebDAV collection listings,
exercises
WebDAV PROPFIND/MKCOL/COPY/MOVE/DELETE, exercises WebDAV reads with issued
email-only, password-only, password+email-token, password+TOTP, browser-flow,
and direct all-factor WebDAV keys, revokes those keys through their native
logout routes where they are no longer needed, proves revoked keys are rejected
by both guarded API routes and WebDAV, and proves WebDAV mutations do not change
embedded read-only assets. It also embeds the lockd client bundle, starts a
C-owned lockd consumer service from the packed Lua app config, enqueues a
lockd message through the Lua `lockdc` facade, proves the packed consumer writes
WebDAV-visible markers, and proves WebDAV and guarded API routes remain
responsive while the packed consumer service is processing. Focused
packed Lua smoke coverage in
`vectis_lua_pack` also packages and executes a native-auth API service artifact,
packages and executes a lockd consumer-service registration artifact with an
embedded client bundle, proves the direct statically registered `lockdc` Lua
module can open a client from the packed in-memory lockd bundle source without
writing a runtime PEM file, generates a private self-signed certificate bundle,
starts an HTTPS packed asset server with `tls.mode = "manual"`, and fetches the
embedded site over HTTPS. It also extracts packed assets to disk, serves that
extracted tree through the Lua `app:static_directory()` C receiver, and
fetches both the generated index and asset through the disk docroot mount. It
also proves extraction verify, skip-existing, repair, and overwrite policies do
not prune unrelated user-created files. Pack
smoke coverage also proves symlink asset sources
are rejected by default for directory, single-file, and manifest inputs, then
opt-in `--follow-symlinks` packages followed file content as ordinary embedded
read-only assets. It rejects invalid embedded logical paths from both `--asset`
and asset-manifest inputs and rejects invalid content-type map entries before
packaging. Native auth route smoke coverage proves custom login
templates can be loaded from the filesystem and from packed embedded assets, and
that supported login-template placeholders are expanded through escaped
substitution before serving.
Lua smoke coverage validates ACME-mode server config parsing, including the
legacy `tls.domains`, `tls.email`, `tls.provider`, and `tls.cache_dir`
spellings, plus duplicate and invalid DNS-domain rejection and native startup
diagnostics for missing domains or email without contacting a live ACME
provider. ACME account keys and certificate chains are stored transactionally
as attachments on a lockd state object. The default is local `pouch://` state
under XDG state; an explicit lockd endpoint can be selected with
`acme_storage_endpoint`. Kore is hydrated from that object into a private
runtime directory before startup, and commits the updated object before it
activates an issued certificate. The deterministic e2e starts ACME mode against
a local mock ACMEv2 provider, drives directory, nonce, account, order,
authorization, tls-alpn-01 challenge, finalize, and certificate endpoints,
signs Kore's generated CSR with a test CA, and reaches an HTTPS probe through
the ACME-issued certificate without requiring live Let's Encrypt access.

Email-token e2e coverage:

- start a local mock SMTP server,
- configure Vectis SMTP delivery,
- request login,
- capture token from the mock mailbox,
- continue login with token within the default 300-second TTL,
- prove expired, replayed, and wrong tokens fail,
- prove an allowlist blocks delivery to unauthorized recipients.

Current coverage: the packed webserver smokes cover mock SMTP startup,
direct Lua curl-facade SMTP delivery against the mock server, configured native
auth delivery, mailbox capture, continued login, wrong token rejection,
email-token attempt-budget exhaustion, expired token rejection and consumption,
replay rejection, recipient allowlist rejection, password+TOTP+email-token
success through the browser `/continue` flow, login-template use of
`continue_action`, packed embedded login-template placeholder expansion,
default C-side login form rendering of password/TOTP controls only for
password-backed routes and email-token controls only on routes whose policy
requires the email-token factor,
email-token-only WebDAV-key issuance through both `/continue` and
`/webdav-key`, rejects mismatched usernames for valid email-token transactions
on both endpoint styles, password-only WebDAV-key issuance for a non-TOTP user,
password+TOTP WebDAV-key issuance through both `/continue` and `/webdav-key`,
password+email-token WebDAV-key issuance for a non-TOTP user through both
`/continue` and `/webdav-key`, WebDAV access and logout revocation for keys
issued by both endpoint styles and by a `require_email_token` auth mount, and
pending-transaction mismatch rejection, unknown-user and missing-username
email-token issuance/finalization rejection in the full packed webserver path,
direct username/password Basic auth rejection for guarded API and WebDAV routes,
no-store headers across native login, email-token, WebDAV-key, and logout auth
responses, split durable credentials versus transient auth-state files for
pending login and email-token records, anonymous logout challenges on native
auth routes, and WebDAV use of issued keys. Focused C auth and Lua smoke
coverage prove OIDC browser callback
token exchanges can be stored and converted into OAuth2-linked WebDAV app keys
through both native C and Lua facade paths, and OAuth2/OIDC-linked WebDAV app
keys are revoked on stored token flow failure by default and are preserved when
`revoke_webdav_keys_on_failure=false` is explicitly configured. Focused CLI
admin coverage proves OAuth2 WebDAV-key issuance requires an existing stored
flow and that the issued Basic credentials authenticate through the public
credentials verification path while preserving the linked OAuth2 flow claim.
External OIDC/OAuth2 provider interoperability is covered by `make
prerelease-live` when `VECTIS_LIVE_OAUTH2_ENABLE=1` and provider environment is
configured; it is skipped by default because it depends on provider-specific
runtime configuration and credentials.

## Implementation Slices

1. Pack manifest and asset writer.
   Add asset CLI flags, manifest generation, hash validation, and tests for
   path normalization and corrupt payload detection.

2. Runtime embedded filesystem.
   Add C and Lua read-only asset APIs, streaming reads, and extraction with
   hash verification.

3. Embedded static docroot.
   Add C helper registration and Lua access for serving packed assets directly.

4. Extracted WebDAV docroot workflow.
   Add extraction-to-docroot helpers and WebDAV scenario tests.

5. Native browser auth endpoints.
   Add default C templates, mountable handlers, pending auth sessions, and
   WebDAV key issuance flow.

6. Email-token factor and SMTP delivery.
   Add token storage, delivery through configured SMTP, mock SMTP tests, and
   factor policy combinations.

7. Full packed webserver e2e.
   Generate a generic site, pack it, run HTTP/HTTPS, auth-guard a route,
   extract docroot, and run WebDAV operations.

8. Kore plus lockd `startconsumer` e2e.
   Run one Vectis process with Kore as an API or web server, WebDAV routes, and
   an app-owned liblockdc `startconsumer` client service wired through the C
   receiver-style service handles, enqueue lockd messages, prove the consumer
   receives them, and prove HTTP/WebDAV requests still succeed during consumer
   service activity. Lua may provide route or callback glue in the scenario, but
   must not own the built-in server, WebDAV, auth, pack, or consumer service
   lifecycle semantics. Treat Kore and the lockd consumer as simultaneous
   C-hosted receiver shells, not mutually exclusive process modes.

   Keep this as a required scenario family: one variant covers Kore API/web
   serving plus the consumer service, and another covers Kore WebDAV/fileserver
   serving plus the consumer service. Current coverage includes both a direct C
   example and a packed Lua executable that configures the C-owned consumer
   adapter with an embedded lockd client bundle.

## Decisions

- Extraction repair mode does not remove files that are not present in the
  embedded manifest. Repair restores missing or stale embedded files, but
  unrelated user-created files in the extracted docroot are preserved. This is
  the required default for the Landed migration path, where WebDAV edits may
  create operational content beside generated site assets.
- The native login route set belongs in the C-owned Vectis app/server surface,
  with shared auth-service logic underneath. The public C receiver surface is
  `app->auth_routes(app, ...)`; Lua configures that C-owned route set through
  `app:auth_routes(...)`. Lua may provide application routing glue, but it
  does not own the built-in auth route lifecycle or WebDAV key issuance
  semantics.
- Pending auth state uses the same LoneJSON-backed store format as credentials,
  but may be split into an application-selected `state_path`/`auth_state_path`.
  The split state store owns `pending_logins` and `email_tokens`; durable users,
  issued WebDAV/API credentials, and OAuth2/OIDC token flows remain in the
  credentials store. Omitting the state path preserves the single-file JSON
  deployment shape.
