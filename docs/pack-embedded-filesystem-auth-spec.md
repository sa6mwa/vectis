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
- aggregate asset tree hash,
- pack options that affect runtime behavior.

Each asset entry contains:

- logical path, normalized as absolute-within-archive using `/`,
- kind: `file`, `directory`, or `symlink` only if symlink support is explicitly
  enabled,
- byte offset and byte size for files,
- SHA-256 for files,
- mode bits limited to portable read/execute intent,
- content type when known or supplied,
- optional cache metadata such as ETag seed and cache-control.

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

## Runtime Embedded Filesystem

Vectis exposes a read-only embedded filesystem handle over the packed assets.
The handle is available to C and Lua only when the executable contains an asset
manifest.

Required C concepts:

- `vectis_embedded_fs`: read-only handle borrowed from the running executable.
- `vectis_embedded_fs_entry`: file, directory, size, SHA-256, content type,
  mtime equivalent, and ETag-ready metadata.
- lookup, list, open/read, and stream-to-sink operations.
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
- Lua server helpers return C-owned app receiver wrappers. For example,
  `vectis.server.new()` creates a native `vectis_app`, and
  `server:static_embedded()` registers a read-only embedded docroot through
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
- `vectis.cert.validate_bundle(path_or_table)`
- `vectis.cert.validate_pair({certificate_path=..., private_key_path=...,
  ca_bundle_path=...})`
- `vectis.server.new({bind=..., port=..., tls={mode="manual",
  cert_key_bundle_path=..., domain=...}})`
- `vectis.server.new({bind=..., port=..., tls={mode="manual",
  cert_path=..., key_path=..., ca_path=..., domain=...}})`
- `vectis.server.new({bind=..., port=..., tls={mode="acme",
  domains={"example.com", "www.example.com"}, email=...,
  provider=..., cache_dir=...}})`
- `server:static_directory({path_prefix=..., root_dir=..., index_file=...})`
- `server:static_embedded({path_prefix=..., cache_control=...})`
- `server:webdav_embedded_site({path_prefix=..., cache_dir=..., site_id=...,
  extract_policy=..., auth={kind="native", credentials_path=...}})`
- `server:auth_routes({path_prefix=..., credentials_path=..., realm=...,
  login_template_path=..., required_factors={"email_token"}})`
- `server:auth_routes({path_prefix=..., credentials_path=..., realm=...,
  login_template_embedded_path="/templates/login.html"})`
- `server:json({path=..., method=..., status=..., body=...,
  cache_control=...})` for small C-owned unguarded JSON endpoints in service
  scenarios.
- `server:auth_json({path=..., method=..., status=...,
  auth={kind="native", credentials_path=...}, body=...})` for small C-owned
  guarded JSON endpoints in service scenarios.
- `server:consumer_service({ ... })` is the Lua registration point for
  same-process lockd consumer service wiring. It creates a C-owned
  `vectis_consumer_service` receiver around liblockdc and starts it by default.
  The initial built-in handler is `handler.kind = "webdav_marker"`, which
  writes configured marker files into a WebDAV storage cache from the C worker
  callback. Direct Lua `on_message` callbacks remain rejected; a future Lua
  callback implementation needs an explicit C adapter or worker-owned Lua
  state/queue bridge.
- `server:start()`, `server:stop()`, and `server:close()`

Streaming semantics:

- `chunks()` and C sink APIs must stream from the mapped executable or bounded
  file reads. They must not materialize whole files unless the caller requests
  `read()`.
- Extraction streams file-by-file and verifies hashes after writing.
- `verify` extraction policy checks existing files without writing.
- `repair` extraction policy restores missing or hash-mismatched embedded files
  without pruning unrelated files.
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
- extracted docroot initialization before the server starts accepting writes,
- optional repair mode that restores embedded files that are missing or fail
  hash verification,
- no accidental deletion of user-created files unless the caller selected an
  explicit pruning policy.

## Native Auth Flow

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
Password-first browser continuation is backed by locked credentials JSON
`pending_logins` records: when the password is valid but TOTP or email-token
factors are still missing, the native route returns a short-lived
`pending_transaction_id`; a later `/continue` or `/webdav-key` POST can supply
that transaction id plus the remaining factor fields without resending the
password.
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
requests, reads them through both full-read and chunk-iterator Lua embedded
filesystem APIs, verifies bodyless HEAD metadata for the embedded static
docroot, rejects raw literal and encoded traversal attempts against both
read-only mounts, verifies single byte-range and
unsatisfiable byte-range behavior for the C-owned static embedded responder, verifies
`If-None-Match` returns 304 for unchanged embedded assets, verifies matching
and stale `If-Range` validators for embedded range requests, starts a second
packed HTTPS asset server with a generated private self-signed certificate,
verifies content types, ETag and cache metadata, rejects traversal attempts,
rejects writes through the read-only mount, extracts the embedded
assets into the disk WebDAV content tree before accepting WebDAV operations,
verifies generated files are present in that extracted docroot, verifies repair
restores stale embedded files while preserving pre-existing mutable files,
exposes native auth routes, protects a JSON API route through the same native
provider, delivers an email token through a local mock SMTP server, turns
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
docroot, issues a second WebDAV key through the direct all-factor
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
C-owned lockd consumer service from the packed Lua server config, enqueues a
lockd message through the Lua `lockdc` facade, proves the packed consumer writes
WebDAV-visible markers, and proves WebDAV and guarded API routes remain
responsive while the packed consumer service is processing. Focused
packed Lua smoke coverage in
`vectis_lua_pack` also packages and executes a native-auth API service artifact,
packages and executes a lockd consumer-service registration artifact with an
embedded client bundle, proves the raw statically registered `lockdc` Lua
module can open a client from the packed in-memory lockd bundle source without
writing a runtime PEM file, generates a private self-signed certificate bundle,
starts an HTTPS packed asset server with `tls.mode = "manual"`, and fetches the
embedded site over HTTPS. It also extracts packed assets to disk, serves that
extracted tree through the Lua `server:static_directory()` C receiver, and
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
Lua smoke coverage validates ACME-mode server config parsing, Landed-style
`tls.domains`, `tls.email`, `tls.provider`, and `tls.cache_dir` spellings,
duplicate and invalid DNS-domain rejection, non-empty state-dir validation, and
native startup diagnostics for missing domains or email without contacting a
live ACME provider. ACME `cache_dir` is accepted into Vectis runtime config as
`acme_state_dir`, is required for ACME startup, and is wired into Kore's keymgr
and ACME process roots so account keys and certificate state are created under
the configured directory. The deterministic e2e starts ACME mode against a dead
local provider endpoint and verifies state creation without requiring live
Let's Encrypt access. Full mock-provider issuance remains future hardening
work.

Email-token e2e coverage:

- start a local mock SMTP server,
- configure Vectis SMTP delivery,
- request login,
- capture token from the mock mailbox,
- continue login with token within the default 300-second TTL,
- prove expired, replayed, and wrong tokens fail,
- prove an allowlist blocks delivery to unauthorized recipients.

Current coverage: the packed webserver smokes cover mock SMTP startup,
configured delivery, mailbox capture, continued login, wrong token rejection,
email-token attempt-budget exhaustion, expired token rejection and consumption,
replay rejection, recipient allowlist rejection, password+TOTP+email-token
success through the browser `/continue` flow, login-template use of
`continue_action`, packed embedded login-template placeholder expansion,
email-token-only WebDAV-key issuance through both `/continue` and
`/webdav-key`, password-only WebDAV-key issuance for a non-TOTP user,
password-only TOTP continuation for an enrolled user,
password+email-token WebDAV-key issuance for a non-TOTP user through both
`/continue` and `/webdav-key`, WebDAV access and logout revocation for keys
issued by both endpoint styles and by a `require_email_token` auth mount, and
pending-transaction mismatch rejection, unknown-user and missing-username
email-token issuance/finalization rejection in the full packed webserver path,
raw username/password Basic auth rejection for guarded API and WebDAV routes,
no-store headers across native login, email-token, WebDAV-key, and logout auth
responses, and WebDAV use of issued keys. Broader packed auth matrix coverage
remains future hardening work.

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

## Open Decisions

- Whether extraction repair mode should remove files not present in the
  embedded manifest. The safer default is no pruning.
- Whether pending auth state should later move from the main locked credentials
  JSON to a separate state file/directory for high-churn deployments. Current
  native behavior uses the main credentials JSON `pending_logins` array.
- Whether the native login route set belongs in the core C API, the Kore
  integration layer, or both with a shared auth service underneath.
