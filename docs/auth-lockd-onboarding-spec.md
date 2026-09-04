# Lockd-Backed Authentication and Self-Service Onboarding

Status: implementation authority; unblocked by liblockdc 0.14.0, which
provides transactional inbox/outbox operations.

## Purpose

Vectis authentication currently has two persistence models: durable users,
credentials, email tokens, pending logins, and OAuth flows use local JSON files
with filesystem locks, while browser sessions and native workflow records use
Lockd. This is an unacceptable split. Authentication data is security-critical
state and must never fall back to an unencrypted or independently synchronized
file store.

This specification replaces that model with one Lockd-backed persistence model
and adds an administrator-created, self-service user onboarding workflow.

The resulting system has these properties:

- all Vectis-managed authentication state is stored through Lockd;
- a single encrypted local Pouch root is the default for libvectis, the Vectis
  CLI, and Lua applications;
- any auth state domain may explicitly use another Lockd or Pouch backend;
- a deployment may deliberately place every Vectis state domain in one Lockd
  endpoint/root while retaining distinct namespaces and keys;
- invitation, email-code state mutation, and email dispatch enqueue use
  liblockdc transactional outbox operations; and
- onboarding creates a normal user and profile, but does not authenticate that
  user or change an endpoint's independently configured login policy.

This is a pre-1.0 clean cutover. Local JSON auth stores, file locks,
`credentials_path`, and `state_path` are removed. There is no compatibility
reader, implicit import, migration, dual write, or fallback to plaintext
storage. Operators export/recreate development data before upgrading.

## Scope and non-goals

The cutover covers every Vectis-owned authentication persistence object:

- native users and their password hashes, enrolled email addresses, and TOTP
  secrets;
- issued Basic and Bearer credentials and their revocation state;
- OAuth2/OIDC stored flows and linked WebDAV credentials;
- direct email-token and pending-login records;
- ordered M2M and browser login workflows;
- browser-session signing material and session records;
- onboarding invitations, progress records, profile records, TOTP setup
  material, and delivery jobs; and
- durable Vectis-owned auth policy/configuration records, where introduced.

It does not make arbitrary application configuration, static files, WebDAV
content, or third-party identity-provider state part of the auth store. An app
still supplies its route and deployment configuration at startup. The one
necessary bootstrap input is the Lockd connection configuration itself; it is
not an auth data file and must not contain a second credentials database.

This change does not add a browser or HTTP administrator API. Administration is
CLI-first. An authenticated administrator endpoint can be added later on the
same store contract without changing onboarding records or browser behavior.

## Shared state topology

### Default backend

When no endpoint is configured, Vectis opens exactly one local endpoint:

```text
pouch://${XDG_STATE_HOME:-$HOME/.local/state}/vectis/storage?single_writer=false
```

It uses the existing Vectis Pouch encryption policy:

- Pouch encryption is enabled by default.
- `VECTIS_POUCH_CRYPTO_KEY`, when non-empty, takes precedence.
- Otherwise Vectis uses or securely creates
  `${XDG_CONFIG_HOME:-$HOME/.config}/vectis/pouch.key` with private directory
  and file permissions.
- A plaintext root or a root encrypted with a different key is an error. It is
  never reinitialized, migrated, or opened without encryption.

The default endpoint is materialized by libvectis itself, not separately by the
CLI or Lua bindings. `vectis_app_config_init()`, `vectis.auth` store defaults,
and `vectis -a` action receivers therefore resolve to the same root and key
policy. Metrics, ACME, and future Vectis persistence surfaces use this same
root unless their explicit storage override selects another one.

### State domains

Every persisted Vectis subsystem names a state domain. A domain contains a
Lockd connection selection, namespace, owner, and key prefix. A domain defaults
to the application's primary `lockd` configuration and inherits its endpoint,
client bundle, Unix socket, timeout, Pouch key controls, and default namespace.
An override replaces the complete backend selection for only that domain.

The standard domains are:

| Domain | Default namespace | Purpose |
| --- | --- | --- |
| `auth.credentials` | `vectis.auth` | users, issued credentials, revocations, OAuth flows |
| `auth.transient` | `vectis.auth` | email tokens and pending logins |
| `auth.workflow` | `vectis.auth` | ordered M2M/browser login workflows |
| `auth.browser_session` | `vectis.auth` | session signing keys and sessions |
| `auth.onboarding` | `vectis.auth` | invitations and onboarding progress |
| `auth.profile` | `vectis.profile` | completed user profiles |
| `auth.delivery` | `vectis.auth` | transactional outbox records and dispatcher cursor/state |

The default namespace names a storage boundary, not an authorization boundary.
Keys are still typed and namespaced. A deployment that needs different access
control or retention can point, for example, `auth.profile` at a dedicated
remote Lockd endpoint while the remaining domains retain the primary backend.

No code may construct an unaudited Pouch endpoint for one auth object. All
client opening, encryption-key resolution, owner naming, and Lockd error
translation flow through one shared libvectis state-client factory.

### Keys, ownership, and retention

Keys are stable, versioned logical paths under their domain. They do not embed
filesystem paths, raw email addresses, passwords, TOTP secrets, or browser
cookie values. Keys that require an identity component use a SHA-256 digest of
the normalized identity, encoded as lowercase hex.

Representative keys are:

```text
auth/v1/users/<username-digest>
auth/v1/email-index/<normalized-email-digest>
auth/v1/credentials/<client-id-digest>
auth/v1/oauth2/<flow-id-digest>
auth/v1/transient/email-token/<transaction-id>
auth/v1/workflow/<workflow-id>
auth/v1/browser-session/key/<purpose-digest>
auth/v1/browser-session/session/<session-id>
auth/v1/onboarding/invite/<email-digest>
auth/v1/onboarding/progress/<workflow-id>
auth/v1/profile/<principal-digest>
```

The Lockd owner is a stable Vectis service identity, not an end-user identity.
Leases use bounded TTLs appropriate to the operation. Expiring workflow,
session, token, invitation, and setup records are deleted on completion and
also pruned in bounded batches. Durable user, profile, credential, and
revocation records do not receive expiry leases.

All state mutation uses a Lockd acquire-for-update transaction. Read-modify-
write through separate requests is prohibited.

## Auth store API cutover

### C API

`vectis_auth_store_config` becomes a Lockd-state selection, not a JSON path
container. It has:

- an optional borrowed app/client context for route-owned operations;
- a primary state-domain selection, inheriting the app/default backend when
  unset;
- optional domain overrides for credentials, transient auth state, profiles,
  and onboarding; and
- bounded record-size and operation-timeout controls.

It has no `credentials_path`, `state_path`, or filesystem-lock fields.
`vectis_auth_store_config_init()` selects the shared default state backend and
the standard auth namespaces/keys. Public auth functions keep their semantic
names where their behavior remains coherent, but receive Lockd-backed store
configuration only.

`vectis_auth_store_init()` becomes an idempotent schema/bootstrap operation. It
creates no filesystem file. It validates that the selected backend is reachable
and that required typed root records can be initialized transactionally.

The shared state-client factory is public enough for auth and other libvectis
subsystems, but raw Lockd handles remain an implementation detail of normal
auth callers. A route-owned auth store borrows the owning app's configured
client lazily after the route process boundary is safe. A standalone C or CLI
call opens a short-lived client from the same domain configuration and closes
it on return.

### Lua API

`vectis.auth.store_init`, `user_add`, `user_login`, `issue`, `verify`,
`revoke`, `workflow`, and all auth helpers accept a common `storage` table.
The table has the same shape and defaults as `app.new({lockd = ...})` and may
contain per-domain overrides. Path aliases such as `credentials_path`, `path`,
`state_path`, and `auth_state_path` are removed.

An application normally needs no duplicated auth storage configuration:

```lua
local server = assert(vectis.app.new({
  lockd = {
    endpoints = {"pouch:///srv/vectis/state?single_writer=false"},
    default_namespace = "example",
  },
}))

local flow = assert(vectis.auth.workflow({
  path_prefix = "/auth",
  storage = { inherit_app = true },
  steps = {"password", "totp"},
}))
assert(flow:mount(server))
```

When no `lockd` table and no explicit auth storage override are supplied,
`app.new`, the workflow, and direct `vectis.auth` calls resolve to the shared
default Pouch root. A separate backend is explicit and complete:

```lua
storage = {
  profile = {
    endpoints = {"https://profiles-lockd.example.test"},
    client_bundle_path = "/run/secrets/profiles-lockd.pem",
    namespace = "customer-profiles",
  },
}
```

### CLI action receivers

`vectis -a` action receivers use the same common state-client configuration as
libvectis and Lua. With no storage arguments they open the shared default
Pouch root. They do not create a JSON credential file.

The receiver shell accepts common Lockd options before its action-specific
arguments: endpoint(s), Unix socket, client bundle, namespace, timeout, and
the established Pouch encryption controls. The names and meanings match the
Lua `lockd` table and C `vectis_lockd_config`; they are parsed by one common
receiver helper rather than reimplemented by `users`, `credentials`, and
`oauth2`.

`--store FILE` is removed. It must not be silently reinterpreted as a Pouch
directory because that would conceal a breaking storage change. The help text
and error diagnostics direct operators to the common Lockd/Pouch options.

## Transactional delivery dependency

The implementation requires liblockdc 0.14.0 or later for transactional
inbox/outbox operations. It must use that API directly; Vectis must not
implement an equivalent queue, lease convention, polling format, or retry
protocol on top of ordinary Lockd records.

Each auth delivery workflow opens `lc_client_new_workflow()` for its selected
auth state domain. A fresh issuance starts an `lc_workflow_transaction` through
`lc_workflow_append_outbox()` or `lc_workflow_accept_command()`, acquires the
affected typed auth records as transaction participants, stages their state,
adds the immutable `lc_outbox_entry` with its streamed encrypted payload, and
commits once. Duplicate operation identities must use liblockdc's durable
receipt/outbox semantics; they must not regenerate a code or enqueue another
delivery.

The Vectis SMTP worker pulls owned `lc_outbox_job` values through
`lc_workflow_next()`, streams the payload with `lc_outbox_job_write_payload()`,
and records exactly one upstream terminal action: `complete`, `retry`, or
`dead_letter`. Its component-owned liblockdc dispatcher, claims, recovery, and
dead-letter lifecycle remain authoritative. Vectis never reads a workflow
payload into a hidden full-message buffer or creates another durable queue.

An email token issuance transaction does all of the following atomically:

1. creates or advances the relevant workflow/onboarding record;
2. stores only the token hash in the verification record;
3. stores the delivery payload, including the plaintext token, only in the
   encrypted transactional outbox payload; and
4. commits the outbox message with a stable message id and delivery policy.

The dispatcher consumes the liblockdc outbox, sends SMTP with a stable
Message-ID/idempotency identity, and acknowledges the outbox message only
after the SMTP handoff succeeds. A retry can produce at-least-once SMTP
delivery; it must never produce a different token for the same active
transaction. Raw codes, provisioning URIs, password values, and TOTP secrets
are never logged, returned in dispatcher diagnostics, included in metrics, or
stored in an unencrypted side file.

Invitation creation itself does not send email. The administrator registers an
address; the user starts onboarding from the login surface, and that action
causes email-code issuance and outbox enqueue. Resend is a configured,
rate-limited operation that atomically invalidates the previous code and
enqueues exactly one replacement delivery.

The future inbox primitive is used for external/admin command ingestion when
that surface is added. The CLI in this specification writes directly through
the state transaction API; it does not create an HTTP administration endpoint.

## Onboarding model

### Invitation lifecycle

An invitation is an email-only, single-use authorization to begin onboarding.
It is not a user, credential, browser session, or issued client credential.
Its record includes at minimum:

- normalized email digest and encrypted/display-safe canonical email;
- creation, expiry, revocation, consumption, and last-delivery timestamps;
- selected onboarding policy identity/digest;
- resend and attempt budgets;
- an opaque invitation identifier; and
- audit-safe administrative metadata, if supplied.

An address has at most one active invitation per onboarding policy. Creating a
new invitation for an active address is a deliberate update/resend operation,
not a second parallel invitation. An existing enrolled email cannot receive an
onboarding invitation unless an explicit future replacement policy authorizes
it.

The CLI contract is:

```sh
vectis -a users --onboard -i user@example.com
```

`-i` means insert/invite and is reserved for the address supplied to
`--onboard`; `-o` remains available for file output and is not overloaded.
The command prints an opaque invitation id, normalized email, expiry, and
policy identity. It never prints a login code, password, TOTP secret, or QR
payload. It also supports configured expiry, revoke, and resend actions.

### Onboarding policy

Onboarding is enabled explicitly on an auth workflow route. It is rejected at
registration when SMTP is absent, browser support is disabled, an invalid
profile field policy is supplied, or its state domain cannot be initialized.

The policy config contains:

- invitation TTL, code TTL, retry/resend limits, and cleanup policy;
- an explicit completion path. It must be root-relative, validated with the
  same open-redirect protections as login return paths, and defaults to the
  route's login path;
- profile field definitions in display order; each is `required`, `optional`,
  or `absent`;
- username rules and uniqueness behavior;
- password policy and whether password enrollment is required;
- TOTP policy: `absent`, `optional`, or `required`, plus issuer and label
  rules; and
- profile-store domain selection and an optional external profile callback.

Supported built-in profile fields are:

```text
username
password
password_confirmation
given_name
family_name
address_line1
address_line2
city
region
postal_code
country
phone
homepage_url
linkedin_url
```

`password_confirmation` is valid only when `password` is present and required
whenever password is required. `username` is required whenever any configured
post-onboarding login policy needs a username; it is independently configurable
because Vectis does not infer endpoint policy from onboarding policy. URL,
email, phone, length, Unicode normalization, and country validation are
performed by named C-owned validators. Profile values are never accepted as
arbitrary undeclared form fields.

The optional profile callback is a typed write/read integration point, not a
Lua free-form callback inside an auth transaction. The default Vectis profile
store is always written transactionally first. An external callback receives a
versioned, validated profile projection through the inbox/outbox contract and
is retryable/idempotent. Failure to project externally does not roll back a
completed local user unless the policy explicitly marks that projection as a
completion gate.

### Entry and enumeration resistance

On an onboarding-enabled login route, a visitor may start with an email
address. If the normal first factor is password, the visitor may submit an
email-shaped value in the username field with any password or no password.
The browser form must permit that password to be empty; normal username
password authentication still requires a password server-side.

If the normalized email has an active invitation, the normal password flow is
not evaluated and onboarding starts. The next current factor is email-code
verification. If no active invitation exists, Vectis follows the normal login
failure path. Browser and M2M responses are deliberately indistinguishable in
status, timing budget, wording, redirects, and workflow shape to prevent
invitation/user enumeration. Unknown, expired, revoked, already-consumed, and
rate-limited invitations never enqueue delivery and cannot complete.

An onboarding browser page names only the current input requirement. It never
reveals step counts, remaining profile fields, whether TOTP will be required,
or whether an address has an invitation. The ordered internal state and M2M
continuation metadata may carry the current stage, but no browser response may
describe a later stage.

### Browser state machine

The browser state machine is C-owned and uses opaque, expiring Lockd records:

```text
invited email entry
  -> email code issuance + transactional outbox enqueue
  -> email code verification
  -> configured profile/credential form
  -> optional or required TOTP provisioning and confirmation
  -> completed; redirect to completion path without a browser session
```

The profile form contains exactly the policy-selected fields. Password entry
uses password controls and is never retained in HTML, query strings, cookies,
logs, telemetry, or profile state. Server validation is authoritative; browser
validation exists only for immediate user feedback.

On a TOTP-required or user-selected TOTP step, Vectis generates a fresh secret
inside the onboarding transaction. It stores the secret only in encrypted,
expiring onboarding state until the first valid code is confirmed. The browser
page renders a standard `otpauth://` provisioning URI as deterministic inline
SVG using Vectis's existing QR matrix encoder:

- the SVG has a fixed quiet zone, integer module geometry, `viewBox`, and no
  external references, scripts, fonts, or raster image dependency;
- it is generated for the specific in-progress secret and is not cacheable;
- the provisioning URI is also presented as an accessible copyable value only
  when the policy permits; and
- the same masked six-cell numeric input used by login TOTP verifies the first
  authenticator code before the secret is promoted into the user record.

Completion atomically creates the user credential record, email index, profile
record, and (when confirmed) TOTP enrollment; consumes the invitation; deletes
or marks the progress record terminal; and enqueues any configured external
profile projection. A conflict—such as a username or email claimed while the
user was onboarding—fails closed with a generic retry-safe message and leaves
no partially enrolled user.

Completion redirects to the configured completion path, normally the login
page. It never issues a browser session, an M2M credential, a WebDAV key, or a
credential for a protected endpoint. The newly created account subsequently
uses whatever authentication policy each endpoint independently requires.

### M2M contract

Onboarding has matching C-owned JSON start/continue routes under the configured
auth prefix. They use opaque workflow ids and accept only the current stage.
The M2M representation can identify the current required field because it is a
programmatic continuation contract; it never exposes browser-only templates,
session cookies, or secrets beyond the current TOTP provisioning material
needed by an interactive M2M client.

The terminal onboarding response is a completion result and completion URL,
not a client credential. It has the same no-auto-login behavior as browser
completion. Replayed, expired, consumed, mismatched-policy, or foreign-route
workflow ids fail without changing durable user/profile state.

## Security requirements

- Every browser form POST is a same-origin document navigation. Cross-site
  submission is rejected before state mutation.
- Browser cookies remain HttpOnly, Secure, and SameSite=Strict. Onboarding
  progress cookies, if used, are opaque identifiers only.
- Invitation and login entry responses are enumeration-resistant.
- Password hashing keeps the existing memory-safe, salted, iterated KDF
  contract or replaces it only with a deliberate documented stronger policy.
- All state keys and records are bounded; user-controlled values are validated
  before being used in a key, SMTP header, redirect, SVG, or JSON projection.
- Email messages use configured SMTP recipient allowlists/domain controls and
  stable message identities. SMTP failures are actionable to operators but
  opaque to visitors.
- Browser pages send `Cache-Control: no-store`; provisioning pages must also
  prevent intermediary caching.
- TOTP secrets and provisioning URIs are redacted from errors, logs, metrics,
  crash diagnostics, CLI output, and audit events.
- Profile fields are sensitive data. The default profile store is encrypted
  Pouch/Lockd state, profile reads are explicit APIs, and auth-provider claims
  do not implicitly contain profile data.
- Cleanup is bounded and idempotent. Expiry/revocation cannot resurrect an
  invitation, token, session, or TOTP setup secret.

## Verification and acceptance criteria

The implementation is complete only when automated tests prove all of the
following.

### Storage cutover

- No auth operation creates, reads, locks, renames, or accepts a local JSON
  credential/state file.
- Default C, Lua, and `vectis -a credentials/users/oauth2` calls resolve to
  the same encrypted Pouch root and can observe one another's state.
- A primary remote Lockd configuration is inherited by auth workflow,
  sessions, CLI actions, credentials, and profiles.
- Each supported per-domain override routes only that domain to the override.
- Encryption failures, missing client material, malformed endpoints, and
  unavailable Lockd endpoints fail closed with actionable diagnostics.
- Concurrent create/update/revoke/login operations preserve indexes and never
  lose records.

### Delivery

- Token record update and outbox enqueue are one transaction.
- Simulated interruption before/after commit produces neither a usable token
  without an outbox job nor an outbox job without matching verification state.
- Dispatcher retry preserves token and message identity, redacts secrets, and
  acknowledges only after successful SMTP handoff.

### Onboarding

- `vectis -a users --onboard -i` creates, expires, revokes, resends, and
  consumes invitations against the selected Lockd domain.
- Invited and non-invited email entry have indistinguishable public failure
  responses while only an invited address receives delivery.
- Browser pages disclose only current controls and never a step count or later
  factor/profile requirement.
- Required, optional, and absent profile fields render, validate, persist, and
  reject undeclared values correctly.
- Username/email races fail atomically without partial account/profile state.
- SVG QR output is deterministic for a known URI, contains a valid QR matrix,
  has no external dependency, and is not cached.
- TOTP secrets are unusable until first-code confirmation and are discarded on
  expiry/cancellation.
- Completion issues no authentication session or M2M credential and redirects
  only to a validated configured completion path.
- M2M and browser flows have equivalent state transitions and expiry/replay
  protections.

Run the complete debug, release, Lua facade, CLI action, Lockd/Pouch, browser
workflow, and SMTP integration suites. Add failure-injection tests for Lockd
transaction/outbox boundaries; happy-path SMTP tests alone are insufficient.

## Implementation order

1. Upgrade to the liblockdc release with transactional inbox/outbox and add
   focused wrapper tests for the upstream API.
2. Introduce the common Vectis state-client factory and shared default Pouch
   root; migrate existing persistence users such as metrics/ACME to it.
3. Replace the file-backed auth store with typed Lockd records and transactional
   indexes. Remove file-store API fields, tests, docs, and CLI options in the
   same change.
4. Migrate direct auth primitives, issued credentials, OAuth flows, ordered
   workflows, and browser sessions to the unified domain contract.
5. Add transactional SMTP delivery dispatch and migrate email-code issuance.
6. Add invitation administration, profile storage/projection, onboarding M2M
   routes, and browser pages.
7. Add SVG QR rendering and TOTP-confirmation enrollment.
8. Run the acceptance suite, inspect the worktree, and publish updated C, Lua,
   CLI, and operational documentation together.
