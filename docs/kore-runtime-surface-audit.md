# Kore Runtime Surface Audit

This audit records the supported Vectis/Kore runtime surface so broad TODO items
do not regress into repeated re-review. Vectis exposes Kore through Vectis-owned
C and Lua APIs, not by accepting arbitrary Kore configuration files from user
code.

## Completed Runtime Configuration Surface

`vectis_server_config` and `vectis.app.new({ ... })` cover the production
runtime controls Vectis needs to ship:

- connection and request guardrails: `max_connections`, `request_limit`,
  `max_request_header_bytes`, `max_request_body_bytes`,
  `request_header_timeout_ms`, `request_body_idle_timeout_ms`,
  `response_write_idle_timeout_ms`, `idle_timeout_ms`,
  `request_body_min_rate_bytes_per_sec`, and
  `request_body_min_rate_grace_ms`
- worker controls: `worker_count`, `worker_accept_threshold`,
  `worker_rlimit_nofiles`, `worker_cpu_affinity`, `worker_shutdown_timeout_ms`,
  and `worker_death_policy`
- HTTP runtime controls: `keepalive_disabled`, `keepalive_timeout_ms`,
  `keepalive_max_requests`, `socket_backlog`, `request_process_budget_ms`,
  `hsts_max_age_seconds`, `server_header`, `access_log_path`, and
  `pretty_error_pages`
- Kore-owned curl internals for framework workflows such as ACME:
  `kore_curl_timeout_seconds`, `kore_curl_recv_max_bytes`, and `kore_quiet`
- WebSocket runtime controls: `websocket_max_frame_bytes` and
  `websocket_timeout_ms`

The C implementation validates these values before runtime startup when Vectis
can report a structured error. Listener bind/port conflicts, access-log append
access, and request-body spool directory usability are preflighted before Kore
can fail with native fatal setup behavior.

## Completed Route and Handler Surface

Vectis supports practical Kore route features through explicit C receivers and
Lua receiver methods:

- ordinary C routes and Lua `app:get`, `app:post`, `app:put`,
  `app:delete`, `app:head`, `app:options`, and `app:patch`
- JSON routes using LoneJSON-backed C adapters
- `app:upload` and C upload routes with true request-body streaming and
  bounded memory or disk-spool policies
- static disk content through `vectis_register_static_file`,
  `vectis_register_static_directory`, `app:static_file`, and
  `app:static_directory`
- read-only packed content through `app:static_embedded`
- WebSocket routes through `vectis_register_websocket` and `app:websocket`
- WebDAV and auth-guarded route helpers where Vectis owns the higher-level
  workflow above Kore

Low-level C escape hatches are intentionally narrow: C handlers may access the
underlying Kore request through `vectis_request_kore` and can opt into raw
response behavior where that is the correct integration point. Lua keeps the
Vectis-owned request and response model so application scripts do not depend on
Kore internals.

## Intentional Non-Surface

Vectis does not expose every raw Kore capability as a public product contract.
These remain outside the supported surface unless a concrete user-facing need
requires them:

- arbitrary user-supplied Kore config files
- dynamic Kore module loading as an application extension mechanism
- direct mutation of Kore global runtime state after startup
- bypassing Vectis request-body policy validation
- replacing Vectis TLS, auth, metrics, WebDAV, and service lifecycle contracts
  with raw Kore equivalents

The supported rule is: expose Kore controls when they have stable Vectis
semantics, can be validated, and improve libvectis or Lua DX. Keep raw Kore
internals available only through explicit low-level C escape hatches.
