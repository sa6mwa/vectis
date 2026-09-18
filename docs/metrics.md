# Vectis Metrics Surface

Vectis metrics are disabled by default. No counters, load sampling, worker, or
snapshot persistence is started unless an application explicitly registers the
metrics route surface.

## C API

Use `vectis_metrics_config_init()` and register through the app receiver:

```c
vectis_metrics_config metrics;
vectis_metrics_config_init(&metrics);
metrics.path = "/.metrics";
metrics.json_path = "/.metrics.json";
metrics.auth_provider = &provider; /* optional */
metrics.persistence_enabled = 1;   /* optional */
app->metrics(app, &metrics, &error);
```

The `vectis_app_config` used to construct `app` must set an explicit
`app_name` when persistence is enabled.

`vectis_metrics_snapshot_json(app, &out, &error)` returns the current in-memory
snapshot as JSON. It fails with `VECTIS_ERR_STATE` when metrics were not
registered.

The dashboard presents metrics as HTML and links to `json_path` for the raw
snapshot; it does not embed the raw JSON in the page.

## Lua API

Lua scripts register the same C-owned surface through `app:metrics(opts)`:

```lua
assert(app:metrics({
  path = "/.metrics",
  json_path = "/.metrics.json",
  title = "admin.example",
  auth = auth_provider,
}))
```

`auth` uses the same native or callback auth provider table accepted by
`app:route`, `app:webdav`, and `app:auth_json`. There is no
metrics-specific auth mechanism.

Lua accepts the same persistence fields as `vectis_metrics_config`:
`persistence_enabled` (or `persist`), `storage_endpoint` (or `endpoint`),
`storage_namespace`, `storage_owner`, and `snapshot_interval_seconds`.
`storage_namespace` defaults to `"vectis.metrics"`, `storage_owner` to
`"vectis"`, and the periodic interval to 300 seconds; shorter values are
clamped to five minutes. A non-`nil` `auth` value must be a provider table, so
a configuration typo cannot silently expose the dashboard.

The native authenticated shape is:

- mount `vectis.auth.workflow(...):mount(server)` when browser login routes
  are wanted;
- guard metrics with `auth = flow:provider(...)` or another provider table;
- issue native Bearer credentials with `vectis.auth.issue(..., modes={"bearer"})`
  for M2M-style clients;
- use `vectis.auth.provider_callback(fn)` when an application needs to bridge
  external OAuth2/OIDC validation, token introspection, or custom policy.

Metrics itself does not implement a separate OAuth2/OIDC or M2M adapter. It
only calls the configured auth provider and applies the returned allow, deny,
required, or redirect action.

## Collected Values

The current snapshot includes process and runtime fields, uptime, route count,
lifecycle state, load average where supported, HTTP request/status counters,
route misses, body rejects, metrics auth outcomes, and snapshot persistence
write/error counts. Vectis does not collect latency histograms, per-route
high-cardinality labels, or other request-path expensive metrics in this
surface.

Load average is sampled at most once per minute by the metrics worker. Request
handling only updates cheap in-memory counters when metrics is enabled.

## Persistence

Persistence is opt-in and requires an explicit, non-default `app_name` in the
app configuration. Vectis derives one deterministic current-checkpoint key from
`storage_owner` and `app_name` inside the configured namespace. Apps sharing a
storage endpoint and namespace must therefore use distinct names unless they
intentionally represent the same logical app. Concurrent instances that share
an identity merge each instance's newly observed monotonic counters while
holding the checkpoint lease; one writer cannot overwrite another's progress.

On startup, Vectis reads that exact checkpoint before making the app available.
The HTML metrics URL, JSON metrics endpoint, and in-process JSON snapshots all
immediately include restored cumulative HTTP, auth, and persistence counters.
PID, uptime, lifecycle, route count, and load samples describe the new process
and are not restored. Missing checkpoints are treated as a first start; an
unreadable or invalid checkpoint fails startup rather than silently publishing
zeroed history.

The single-checkpoint lookup does not make local storage recovery constant-time:
opening Pouch can replay its persisted log while Vectis is becoming ready.
For route-backed applications, Vectis opens the checkpoint client only in the
supervisor after the Kore fork boundary and before it marks the child ready. It
then reuses that client for the worker's five-minute checkpoints and the final
shutdown checkpoint. A failed checkpoint discards the client; the next
scheduled checkpoint opens a fresh one. Remote request timeouts do not
establish a CPU/time bound on local Pouch recovery. Startup errors preserve the
underlying lockdc message and dependency error metadata.

`make perf-gate` (also mandatory in `make test-e2e` and hence `make test-all`)
tests the actual binary against encrypted Pouch on Linux. It writes 20,000
checkpoint versions, including acquire/update/release churn, and checks two
fresh-process restarts after clean shutdown and a killed shared writer. A
one-update store provides a small-store comparison. Each successful restart
must serve HTTPS with the restored counters within **5 wall-clock seconds**
and consume less than **2 CPU seconds** across the process tree. Fixture
generation is excluded from these budgets and is separately bounded to 300
seconds. Test state is isolated under `build/`; no deployed state is opened.
The workload crosses the default 64 MiB log-segment size and the gate reports
store size, readiness latency, and CPU consumption. These are regression
budgets for this workload, not a guarantee for arbitrarily large stores or slow
disks. The Python runner accepts `--updates`, `--ready-seconds`, and
`--cpu-seconds` for explicit larger investigations; standard gates use fixed
defaults.

Pouch's default exclusive-writer lease may prevent immediate reopening after
SIGKILL until the lease expires. The gate separately checks that this case
fails promptly with the lockdc exclusive-writer diagnostic. Crash recovery is
measured using an explicit `?single_writer=false` endpoint; Vectis does not
bypass an exclusive lease or silently change the configured writer policy.

The metrics worker updates the checkpoint at least five minutes apart through
its own supervisor-owned liblockdc client. That client is constructed after the
Kore fork boundary from the complete app Lockd configuration: mTLS bundle
path/source/memory material, logger policy, timeout, and Pouch controls are
all retained. It does not share a client instance with request handling or
other Vectis subsystems. `storage_endpoint` may point at a remote lockd
endpoint or a local `pouch://` endpoint. When it is not set, Vectis uses:

```text
${XDG_STATE_HOME:-$HOME/.local/state}/vectis/metrics
```

The ordinary Vectis root is the sibling `vectis/storage`; it contains normal
Vectis state, including ACME under the `vectis.acme` namespace. These are the
only two default Pouch roots. Local Pouch roots are encrypted at rest by
default. They use the same
`lockd.pouch_crypto_key`, `lockd.pouch_crypto_key_file`,
`lockd.pouch_crypto_generate_key_file`, and `lockd.pouch_compression` settings
as other Vectis persistence. With no key setting or exact
`pouch_crypto_key`/`pouch_crypto_key_file` endpoint query option, Vectis
securely generates `${XDG_CONFIG_HOME:-$HOME/.config}/vectis/pouch.key`.
Remote endpoints and separate liblockdc clients do not use these fields.

`VECTIS_POUCH_CRYPTO_KEY` provides a non-empty, deterministic key override
for Vectis-owned local Pouch clients. It takes precedence over the app lockd
key and key-file settings, is passed directly to liblockdc without being
logged or persisted by Vectis, and lets a container reopen the same root on
each start when it receives the same secret. An empty value is a configuration
error.

Vectis never migrates or reinitializes an existing local root. A legacy
plaintext root or a root encrypted with another key fails to open and remains
unchanged; Vectis does not fall back to plaintext or offer a force option.

Snapshots are written under the `vectis.metrics` namespace by default. Runtime
write failures increment the persistence error counter and do not block request
processing.

## Dashboard

The HTML dashboard is served from `path` and the JSON snapshot from `json_path`.
The dashboard uses the same dense dark panel layout pattern as Landed's stats
view, but is generated by Vectis and contains no site-specific assets or
copy. The dashboard title is selected from `title`, then the request `Host`,
then configured server identity, then `vectis`.

## Examples

Runnable Lua examples are available under `examples/lua/`:

- `metrics_persistent.lua`: unauthenticated Hello World plus persistent
  `/.metrics`;
- `metrics_authenticated.lua`: Hello World plus authenticated metrics using
  browser-flow routes, native Basic credentials issued after password+TOTP,
  native issued Bearer credentials, and a callback provider adapter;
- `metrics_ephemeral.lua`: unauthenticated Hello World plus in-memory-only
  metrics.

Before running `metrics_authenticated.lua`, create the browser-flow user through
the operator CLI:

```sh
vectis -a users \
  --lockd-endpoint pouch://vectis-metrics-auth-example-pouch?single_writer=false \
  --add metrics-admin \
  --password metrics-password \
  --totp-secret GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ \
  --label Vectis:metrics-admin \
  --issuer "Vectis Metrics Example"
```

The command prints the TOTP secret and terminal QR code. The Lua example then
validates username, password, and TOTP before issuing the Basic credential used
to read `/.metrics`. For a real deployment, omit `--totp-secret` and use
`--totp` so Vectis generates a fresh secret.
