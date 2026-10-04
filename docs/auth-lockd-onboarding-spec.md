# Lockd-Backed Authentication and Self-Service Onboarding

Status: Lockd-backed authentication is implemented; self-service onboarding and
additional domain-selection requirements below remain an implementation plan.
The earlier file-store cutover/API sketches are superseded by the current
[Lua auth manual](../manual/lua-auth.md), [packed auth contract](pack-embedded-filesystem-auth-spec.md)
and public `include/vectis/auth.h`. Examples in the remaining onboarding
sections describe intended behavior, not currently available commands/routes.

## Implemented authentication substrate

Users, credentials, OAuth2/OIDC flows, email tokens, pending logins, ordered
login workflows, browser sessions and session signing keys are Lockd-backed.
Local JSON credential/state stores and file-lock APIs are removed. There is no
compatibility reader, implicit import, dual write or plaintext fallback.
Operators must recreate or explicitly export development data when adopting
that cutover; the implementation does not migrate it.

C `vectis_auth_store_config` accepts a borrowed `app`, or a standalone `lockd`
selection, plus `ns`, `state_key`, `transient_state_key` and `max_record_bytes`.
An app and standalone selection cannot be supplied together. Lua standalone
operations accept `app`, `namespace`, `state_key`, `transient_state_key` and
`max_record_bytes`; app routes use the app's `lockd` configuration. Auth defaults
are namespace `vectis.auth` and durable record `auth/v1/store`; transient state
shares that record unless explicitly separated. There is no current `storage`
table or `inherit_app` option as proposed in the superseded sketch.

The default encrypted local endpoint is:

```text
pouch://${XDG_STATE_HOME:-$HOME/.local/state}/vectis/storage?single_writer=false
```

Libvectis, Lua app-owned auth and CLI `users`/`credentials`/`oauth2` actions use
this ordinary root unless configured otherwise. `VECTIS_POUCH_CRYPTO_KEY`, when
nonempty, overrides Vectis-owned Pouch key selection; otherwise Vectis securely
creates or uses `${XDG_CONFIG_HOME:-$HOME/.config}/vectis/pouch.key`. Plaintext
or differently encrypted existing roots fail unchanged. ACME also uses the
ordinary root by default. Metrics uses the separate sibling `vectis/metrics`
root, as documented in [Metrics](../manual/metrics.md).

Auth CLI actions accept the common Lockd/Pouch connection options. Removed
file-store options are rejected rather than reinterpreted. Provision users in a
separate administration process before route-backed serving starts; opening a
client during server declaration would violate the Kore quiescence boundary.

## Synchronous login email and asynchronous effects

Email-code login makes one synchronous SMTP attempt. SMTP failure leaves only
an ineligible decoy verifier with the same public response shape as an unknown
address. It creates no email outbox job, delayed retry or Vectis-owned retry
queue. If SMTP accepts a code but the following state commit fails, that code
cannot authenticate; the user starts again.

Other asynchronous application effects can use the implemented
`app:workflow()` transaction and outbox facade. Committed receipt keys reach the
persistent supervisor dispatcher through bounded FD-waking IPC automatically.
See [Lua supervisors and workflows](../manual/lua-supervisor.md). Raw liblockdc
access remains available; Vectis does not recreate durable claims or retries.
This delivered integration does not implement onboarding invitations or their
administration routes.

## Remaining state-domain requirements

Onboarding still requires invitation/progress/profile records, enrollment
material, bounded cleanup and atomic username/email uniqueness. Additional
per-domain backend overrides are an intended extension; the current API selects
auth records by namespace/key within the selected app or standalone backend.
Do not present the old proposed `storage.profile`/`storage.onboarding` tables
as accepted configuration.

Profile state belongs in `vectis.profile`; auth state stays in `vectis.auth` by
default. Any future domain override must inherit the full configured transport,
client material, timeout, logger and Pouch policy, replace the complete backend
selection for that domain, and preserve transactional mutation and encryption.
Keys must not embed raw email addresses, passwords, TOTP secrets or cookie
values; normalize and hash identity components. No onboarding code may create
an unaudited secondary Pouch root or restore file-backed auth storage.

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
  -> one synchronous email-code send; activate verifier on SMTP success
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

- A failed SMTP attempt leaves no usable verifier and schedules no retry.
- A known address with failed SMTP and an unknown address have identical public
  response shapes; diagnostics never disclose the code or recipient.
- A successful SMTP handoff followed by a failed verifier commit does not leave
  an active code.

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
workflow, and SMTP integration suites. Test failure boundaries as well as
successful delivery; do not infer activation merely from a 202 response.

## Remaining implementation work

1. Add invitation administration, profile storage/projection, onboarding M2M
   routes and browser pages on the existing Lockd-backed auth substrate.
2. Add SVG QR rendering and TOTP-confirmation enrollment.
3. Define and implement any additional per-domain backend selection needed by
   onboarding without creating a second auth store or changing current defaults.
4. Prove the remaining acceptance criteria and update the C, Lua, CLI and
   operational manuals together.

The liblockdc upgrade, removal of file-backed auth APIs and synchronous login
email delivery are implemented prerequisites, not pending steps. Metrics keeps
its documented separate default root; this plan does not mandate a persistence
migration merely to introduce onboarding.
