# Documentation status and navigation

Use the [API index](../manual/api.md) and [Lua manual](../manual/lua.md) for
available methods, options and runnable examples. The executable embeds these
manual files at build time; `vectis -a docs` describes that binary's build,
not a newer checkout. See [self-description](../manual/self-description.md).

Documentation status was reconciled with the public headers, Lua facades and
behavioral tests after the supervisor integration in `e20ab55`. Implemented
work has been removed from forward implementation lists. Remaining requirements
are still explicit; a design sketch or an old successful gate is not proof that
an option is available or a release is approved.

## Current contracts and operations

| Document | Scope |
| --- | --- |
| [Split cpkt bundle upgrade](cpkt-bundle-upgrade-spec.md) | Current independent SDK, cache, lifecycle and integration contract. |
| [PostgreSQL XA adapter](postgres-xa-adapter-spec.md) | Managed exact-branch application SQL through public C89 receivers and Lua. |
| [Fork-exit cleanup](fork-exit-cleanup-spec.md) | C/Lua post-teardown callbacks and child runtime ownership. |
| [Concurrency DX](concurrency-dx.md) | Process-local mailboxes, public one-way supervisor IPC and typed adapters. |
| [Service runtime lifecycle](service-runtime-lifecycle-spec.md) | Implemented topology, fork safety, delayed materialization and shutdown; concrete remaining worker extensions. |
| [Supervisor/workflow specification](supervisor-workflow-spec.md) | Implemented serial scopes, channels and receipt dispatch; separately listed stronger remaining contracts. |
| [Lua coverage](lua-coverage-matrix.md) | Supported native/convenience surfaces, behavior evidence and deliberate exclusions. |
| [Musl worker-stack review](musl-stack-review.md) | Compiler frame inspection, owned workspaces and default pthread-stack regression. |
| [liblockdc 0.28.0 upgrade](liblockdc-0.28.0-upgrade.md) | Streaming canonical appends, shared-root compatibility, aligned LoneJSON/pslog and verification. |
| [liblockdc 0.27.0 upgrade](liblockdc-0.27.0-upgrade.md) | Historical collection leases/edits, staged append and resumable conversion evidence. |
| [liblockdc 0.24.0 upgrade](liblockdc-0.24.0-upgrade.md) | Historical append-writer/count-retention upgrade evidence. |
| [MDF 0.19.0 upgrade](libmdf-0.19.0-upgrade.md) | C89 systems repository migration and natural terminal URL wrapping. |
| [MDF 0.18.0 upgrade](libmdf-0.18.0-upgrade.md) | Historical callback/ownership and palette upgrade evidence. |
| [LQL 0.4.1 upgrade](liblql-0.4.1-upgrade.md) | Repository/lifecycle alignment with unchanged runtime and Lua surface. |
| [LQL 0.4.0 upgrade](liblql-0.4.0-upgrade.md) | Historical cleanup upgrade evidence and retained OOM limit. |
| [Upstream Lua facade boundary](upstream-lua-facades.md) | Unmodified pslog/LQL/MDF, archived handoffs, preserved Vectis convenience APIs and exact upstream failure-contract gaps. |
| [Useful dependency Lua specification](useful-dependency-lua-spec.md) | Reviewed scope for dependencies without upstream Lua; [review findings](useful-dependency-lua-review.md) and [implementation progress](useful-dependency-lua-progress.md). |
| [SUS/audio contract](lua-sus-audio-contract.md) | Implemented receiver/worker surfaces, streaming ownership and opt-in model/device gates. |
| [CAI/Smith integration](cai-agent-vectis-integration.md) | Implemented host ownership, bounded terminal presentation and durable sessions. |
| [Packed assets/auth](pack-embedded-filesystem-auth-spec.md) | Implemented Linux pack, WebDAV and ordered native authentication. |
| [Pack platform limits](pack-platform-operability.md) | Linux payload behavior and Darwin's explicit unsupported pack action. |
| [Production webserver](production-webserver-hardening.md) | Implemented opt-in profile, guardrails and remaining platform/release evidence. |
| [Reverse proxy](reverse-proxy-spec.md) | Implemented HTTP/SSE/WebSocket transport and hooks, resource policy and platform gates. |
| [Kore surface](kore-runtime-surface-audit.md) | Supported routes/runtime knobs and deliberate raw-Kore exclusions. |
| [Kore packaging](kore-runtime-packaging.md) | Private vendored runtime and SDK boundary. |
| [Local release matrix](release-matrix.md) | Local artifacts/gates, osxcross Darwin proof and optional hosted diagnostics. |
| [Local test runtime](test-runtime.md) | Pinned Bootlin execution policy; dated measurements below it are historical. |
| [Fast failures](testing-failure-modes.md) | Current fast target closure and deterministic failure coverage. |

The [example index](../examples/README.md) maps runnable source files; the
[benchmark guide](../bench/README.md) describes its opt-in workload. The
[TODO ledger](../TODO.md) separates checked deliveries from remaining work.

## Remaining design work

[Auth/onboarding](auth-lockd-onboarding-spec.md) plans self-service invitations,
profiles and additional state-domain selection. Lockd-backed auth storage and
synchronous login email are already implemented. Removed JSON file stores and
superseded `storage` table sketches are not current API.

The [remaining supervisor contracts](supervisor-workflow-spec.md#remaining-implementation-requirements)
include pull payloads, automatic blocking-handler renewal, kind-filtered intake,
isolated lanes, quotas/restart policy,
shared-runtime leadership and managed cross-process replies. Their existence
does not undo the delivered receipt/wake path. The standalone/app-owned SQLite and PostgreSQL receivers, PDF, PNG,
Zstd/LZ4-frame/XZ, curl sessions/multi, reusable OpenSSL keys/AES-GCM and XML
DOM/XPath/SAX are implemented under the
[useful dependency scope](useful-dependency-lua-spec.md); see its
[implementation and verification evidence](useful-dependency-lua-progress.md).
The older exhaustive public-API goal and database design are historical.
Archive remains deferred because the selected public target requires LZO, which
the user deferred. The proposed SSH remote-execution adapter remains parked on
the [future roadmap](../TODO.md#future-roadmap-ssh-remote-execution).

## Historical evidence and superseded plans

| Record | How to read it |
| --- | --- |
| [Supervisor integration audit](supervisor-workflow-audit.md) | Evidence for `e20ab55`; final verification supersedes earlier scoped suite counts. |
| [Dependency seam audit](lua-upgrade-seam-audit.md) | Active cpkt/cpktdb/cpktmisc, lockdc collections/PostgreSQL XA, MDF and Softline upgrade evidence, outstanding gates, and scoped historical checks. |
| [Packed behavior audit](pack-embedded-filesystem-auth-audit.md) | Requirement-to-test mapping; use the packed contract/manual for APIs. |
| [Reverse-proxy feasibility](reverse-proxy-feasibility-audit.md) | Historical probes preceding the production implementation; its old open list is superseded for delivered features. |
| [Lifecycle migration ledger](lifecycle-migration.md) | Completed cutovers; old version numbers identify those cutovers. |

Keep dated evidence rather than rewriting historical failures into successes.
Mark its candidate and replacement when current behavior changes. Current
documentation must not inherit an old pending status, test count or provisional
blocker without checking the implementation and final evidence.
