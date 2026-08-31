# Packed Assets, WebDAV, and Native Authentication

This document describes the shipped Vectis contract for packed services. It is
current product documentation, not a design history. See [Lua embedded
assets](lua-embedded.md), [Lua WebDAV](lua-webdav.md), and [Lua auth](lua-auth.md)
for API-level detail.

## Packed executable

`vectis -a pack` appends one Lua entry script, optional Lockd client material,
and optional application assets to a Linux Vectis executable. The packed
payload is self-describing and hash-verified when read or unpacked.

```sh
vectis -a pack \
  --script app.lua \
  --output my-service \
  --asset-dir site:/srv/my-service/site \
  --asset-dir templates:/srv/my-service/templates \
  --asset /srv/my-service/favicon.ico=/favicon.ico \
  --lockd-bundle lockd-bundle.pem
```

The executable contains the runner bytes, script, optional Lockd bundle, asset
payload, manifest, and a fixed footer. It never records host source paths,
build paths, home paths, or dependency-cache paths in the manifest.

Packing is supported on Linux targets. Vectis rejects it on Darwin before an
artifact is written because changing a Mach-O executable invalidates its code
signature.

## Embedded filesystem

The asset filesystem is read-only and uses logical absolute paths such as
`/index.html` and `/assets/app.css`. It exposes only files and directories that
were present in the verified manifest.

- Paths reject `.` and `..`, empty trailing names, duplicate entries, and file
  ancestor/descendant conflicts.
- The packer rejects symlink sources by default. `--follow-symlinks` packages
  the resolved regular-file content; it does not preserve a live host symlink.
- Reads, listings, stat calls, and extraction validate manifest hashes and do
  not expose host pack-input paths.
- Extraction traverses from trusted directory descriptors and refuses symlink
  components, including in the requested output root.

Lua applications use `vectis.embedded.has_assets()`, `stat()`, `list()`,
`read()`, `chunks()`, and `extract()`. C applications use the corresponding
`vectis_embedded_fs` API.

## Extraction

Extraction writes a selected asset tree to an application-owned directory.
Policies are explicit:

- `verify` compares the target tree without writing anything;
- `fail_exists` refuses an existing target;
- `skip_existing` preserves existing entries;
- `repair` restores missing or mismatched packed entries while preserving
  unrelated files; and
- `overwrite` replaces conflicting packed entries.

Repair is the normal mutable-site policy: it restores product-owned content
without deleting operator or WebDAV-created content that is absent from the
manifest. Verification is non-mutating.

## Static sites and WebDAV

Packed assets can be served directly as a read-only static site:

```lua
assert(app:static_embedded({
  path_prefix = "/",
  cache_control = "max-age=60",
}) == true)
```

For editable content, extract first and mount the resulting directory through
`app:webdav()` or `app:webdav_embedded_site()`. The direct disk-root and
embedded-site WebDAV responders keep file resolution descriptor-bound and do
not follow symlinks outside the configured root. WebDAV authentication is an
auth-provider decision; the storage implementation does not know about users
or browser sessions.

Keep the following roots separate:

- public static assets;
- mutable WebDAV content;
- WebDAV cache and transaction scratch data; and
- private credentials and Lockd/Pouch state.

Never publish any private state root through a static mount.

## Native authentication workflow

Native authentication is an ordered, C-owned workflow. A request can submit
only the current factor; Vectis verifies that factor before revealing the next
one. There is no all-fields form, `/email-token` route, or `/webdav-key`
finalization route in the native workflow surface.

Supported policies are:

- `password`
- `password`, `totp`
- `email_code`
- `email_code`, `password`
- `email_code`, `totp`
- `email_code`, `password`, `totp`

TOTP is an additional factor only; it cannot be the sole first factor.
Email-code policies require SMTP. Vectis sends a code only to a uniquely
enrolled recipient. Unknown or ambiguous addresses receive an opaque pending
workflow but no delivery and cannot complete it.

Every workflow record is bound to the route path, store, ordered factors, and
credential purpose. A workflow started at one route cannot be continued at a
route with another policy, even if both use the default Lockd state namespace.

### M2M

Every workflow mounts JSON endpoints:

```text
POST <path_prefix>/m2m/start
POST <path_prefix>/m2m/continue
POST <path_prefix>/logout
```

`start` carries only the first factor. A non-terminal response is `202` with
an opaque `workflow`, the `next` endpoint, one `step`, and its `required`
field. Send that workflow identifier and exactly the requested next factor to
`continue`. A terminal response is `201` with a newly issued `client_id` and
`client_secret` for the configured `credential_purpose` (default `workflow`).
M2M responses never set browser cookies.

These custom continuations are independent of OAuth2/OIDC and standard
client-id/client-secret protocols. OAuth2 client credentials remain terminal
machine-to-machine authentication and never receive interactive factors.

### Browser

Set `browser_session.mode = "m2m_and_browser"` to add:

```text
GET  <path_prefix>/login
POST <path_prefix>/continue
```

The browser and M2M surfaces execute the same factor policy. The default UI is
dark, centered, responsive, and presents one factor per page; a six-character
email code submits automatically when complete. Vectis only treats a request
as a browser continuation when it is a same-origin document navigation. A
cross-site login POST is rejected before a credential or cookie can be issued.

On terminal browser success Vectis sets an opaque, signed, HttpOnly, Secure,
SameSite=Strict session cookie. It does not issue that cookie to M2M clients.

Custom templates are page shells, not authentication implementations. Configure
exactly one of `browser_template_html`, `browser_template_path`, or
`browser_template_embedded_path`; the shell must contain exactly one
`{{content}}`. Vectis owns the controls, actions, errors, and opaque state, and
escapes `{{title}}`, `{{progress}}`, and `{{error}}` substitutions.

## State and cleanup

Native workflow records, browser-session records, and browser-session signing
keys are libvectis-owned Lockd state. Lua receives neither a signing secret nor
a decoded session record. Workflow state expires after `workflow_ttl_seconds`
(zero selects `VECTIS_AUTH_WORKFLOW_DEFAULT_TTL_SECONDS`); email-code attempts
use `email_code_max_attempts` (zero selects the documented default budget).

Completion, expiry, revocation, and exhausted attempts remove the relevant
record. The Vectis lifecycle performs bounded automatic cleanup; applications
may additionally call `vectis_auth_workflow_cleanup()` and
`vectis_auth_browser_session_cleanup()` outside request handlers.

The optional credentials `state_path` remains available for lower-level auth
primitives such as directly issued email tokens and stored OAuth2/OIDC flows.
It is not the persistence location for native ordered workflow or browser
session state.

## Example composition

```lua
local vectis = require("vectis")

local flow = vectis.auth.workflow({
  credentials_path = "/var/lib/my-service/credentials.json",
  path_prefix = "/auth",
  credential_purpose = "webdav",
  steps = {"email_code", "password", "totp"},
  email_smtp = {
    url = "smtps://mail.example.test",
    mail_from = "auth@example.test",
  },
  browser_session = {
    mode = "m2m_and_browser",
    purpose = "editor-browser",
    state_key = "my-service.browser-session",
  },
})

assert(flow:mount(app))
assert(app:webdav({
  path_prefix = "/content",
  root_dir = "/srv/my-service/content",
  cache_dir = "/var/cache/my-service/webdav",
  site_id = "my-service",
  auth = flow:provider({purpose = "webdav"}),
}) == true)
```

The application must configure a persistent Lockd endpoint for browser or
native workflow state. Keep its encryption key and storage root private.

## Verification

The deterministic suite covers packed script execution, manifest and asset
validation, static serving, extraction policies, direct and embedded WebDAV
mounts, traversal and symlink rejection, native ordered M2M/browser workflows,
email delivery, session logout/revocation, and same-process Lockd consumer
services. Run:

```sh
make test
make test-e2e
make prerelease
```

Use `make prerelease-live` only when the required external OAuth2/OIDC provider
environment has been configured.
