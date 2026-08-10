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
- `vectis.embedded.stat(path)`
- `vectis.embedded.list(path)`
- `vectis.embedded.read(path)`
- `vectis.embedded.chunks(path, chunk_size)`
- `vectis.embedded.default_extract_policy()`
- `vectis.embedded.extract({to=..., policy=...})`
- `vectis.cert.generate_bundle({common_name=..., output_bundle_path=...})`
- `vectis.server.new({bind=..., port=..., tls={mode="manual",
  cert_key_bundle_path=..., domain=...}})`
- `server:static_embedded({path_prefix=..., cache_control=...})`
- `server:webdav_embedded_site({path_prefix=..., cache_dir=..., site_id=...,
  extract_policy=..., auth={kind="native", credentials_path=...}})`
- `server:auth_routes({path_prefix=..., credentials_path=..., realm=...})`
- `server:auth_json({path=..., auth={kind="native", credentials_path=...},
  body=...})` for small C-owned guarded JSON endpoints in service scenarios.
- `server:consumer_service({ ... })` is the reserved Lua registration point for
  same-process lockd consumer service wiring. Until Vectis provides a C-owned
  adapter or worker-owned Lua bridge, it must fail with `ERR_NOT_IMPLEMENTED`
  instead of invoking script callbacks from liblockdc worker threads.
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
   GET, HEAD, content type, ETag, cache-control, range support when implemented,
   and traversal denial. It does not support WebDAV writes.

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
configure the Vectis server and reserved consumer registration surface, but a
Lua script-owned state must not be called directly from liblockdc consumer
worker threads. A future Lua callback implementation needs an explicit
C adapter or worker-owned Lua state/queue bridge that serializes callback
execution and preserves the Kore worker lifecycle. Until then,
`server:consumer_service()` returns a structured `ERR_NOT_IMPLEMENTED` error so
mixed runtime-loop attempts fail predictably.

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

Current native route behavior includes a C-owned `/email-token` issue endpoint
and optional `require_email_token` enforcement on `/webdav-key`: when enabled,
WebDAV key issuance requires password, any configured TOTP, and a verified
single-use email token transaction. C-owned SMTP delivery is available through
the route config's `email_smtp` settings; Lua only passes configuration into the
native route registration and does not own auth or delivery semantics. The
packed service smoke now exercises SMTP delivery through a local mock SMTP
harness and reads the delivered token from a mock mailbox before issuing a
WebDAV app key.

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
  TOTP rejection and success through the native HTTP login route,
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
webserver script plus generated HTML/JavaScript assets, runs the packed
executable, serves the embedded read-only docroot, rejects writes through that
read-only mount, exposes native auth routes, issues a WebDAV key after
deterministic password+TOTP login, protects the WebDAV mount, serves embedded
content through authenticated WebDAV, and accepts mutable WebDAV writes. The
remaining full-contract checks are traversal, CSS/additional asset coverage,
WebDAV list/copy/move/delete, mutation isolation proof, auth-guarded API/page
coverage, SMTP email-token coverage in the same generated packed-service path,
and same-process lockd `startconsumer` coverage in a packed executable.

Email-token e2e coverage:

- start a local mock SMTP server,
- configure Vectis SMTP delivery,
- request login,
- capture token from the mock mailbox,
- continue login with token within the default 300-second TTL,
- prove expired, replayed, and wrong tokens fail,
- prove an allowlist blocks delivery to unauthorized recipients.

Current coverage: the packed webserver smoke covers mock SMTP startup,
configured delivery, mailbox capture, continued login, wrong token rejection,
replay rejection, and WebDAV use of the issued key. Expiry and recipient
allowlist behavior are covered by native auth unit tests; broader packed auth
matrix coverage remains future hardening work.

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
   Run one Vectis process with Kore HTTP/API routes, WebDAV routes, and an
   app-owned liblockdc `startconsumer` client service wired through the C
   receiver-style service handles, enqueue lockd messages, prove the consumer
   receives them, and prove HTTP/WebDAV requests still succeed during consumer
   service activity. Lua may provide route or callback glue in the scenario,
   but must not own the built-in server, WebDAV, auth, pack, or consumer
   service lifecycle semantics.

## Open Decisions

- Whether extraction repair mode should remove files not present in the
  embedded manifest. The safer default is no pruning.
- Whether symlink entries are needed in the first implementation slice. The
  safer default is to reject symlinks.
- Whether pending auth state lives inside the main credentials JSON or a
  separate state file/directory. A separate state path is likely cleaner for
  locking and TTL cleanup.
- Whether the native login route set belongs in the core C API, the Kore
  integration layer, or both with a shared auth service underneath.
