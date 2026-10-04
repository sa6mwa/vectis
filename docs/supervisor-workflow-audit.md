# Supervisor workflow implementation audit

This is a scoped semantic integration review for the serial default, performed
against the working tree based on `b1fe538013c6f9e6bfceee03cf9ee006b0f6f48a`
on branch `init`. No new pin changes are included in this implementation.
The [complete upgrade inventory](lua-upgrade-seam-audit.md) records dependency
seams and deliberate exclusions. Required upstream contracts remain unresolved;
this record does not approve a release.

## Authority and artifact identity

Read `supervisor-workflow-spec.md`, the service lifecycle ownership contracts,
installed `include/lc/lc.h`, and the installed liblockdc Lua C binding and
`lua/lockdc/init.lua`. Adjacent liblockdc core source was read only to diagnose
notification behavior; no private structures or entry points are used.
The installed host-debug manifest records:

| Artifact | Version | SHA-256 |
| --- | --- | --- |
| c.pkt.systems Linux GNU SDK | 0.11.0 | `fbd3e3f357c22dc26b94cc7e5e200651b59ecb39357ce7bb8c4cb3d939a2b557` |
| liblockdc Linux GNU SDK | 0.18.0 | `1e949887d613824716645dd5e9a85a1b89ad2d8d66431838c0a28c999b0e23da` |
| liblockdc Lua source | 0.18.0 | `94852adfc713299d748d808cd4ff5cbe3e4d66f0bc6ce5be6e80d97cd981cc89` |

Compared with this task's initial pinned state, these artifacts are unchanged.
No new external or vendored dependency is introduced. The four SQLite,
Postgres, libharu, and libpng Lua facades are expressly a separate task.

## Implemented end-to-end paths

`app:workflow` captures canonical client/outbox settings and registers one
hidden host-owned scope per workflow identity. Native scope declarations are
threadless. App startup prepares channels before fork, closes receive ends in
producer descendants, then materializes owner clients and one persistent
dispatcher after the fork boundary. Producer clients/outboxes open lazily after
that boundary with copies of the same settings. The existing native app userdata,
auth checks, worker descriptors, and VRC1 service bus remain intact.

The transaction calls upstream threadless `outbox:transaction`, returning its
actual commit result. Rejected kinds poison the enclosing transaction even when
caught, including staged state changes and later valid appends. Every successful
commit receipt is automatically forwarded as copied bytes into the bounded
workflow inbox. The owner wait loops poll this FD together with the private
service-control FD and monotonic timer deadlines. Direct notification is
synchronous upstream; `notify_outbox_key` followed by zero-wait `pump` can claim
the notified work immediately. This path does not wait for namespace recovery.

Inbox overflow emits one finite, separately coalesced repair signal. Frames
alternate with round-robin signals to prevent repair starvation. Non-full
forwarding failures also schedule repair where the endpoint is open, with
`failed_wakes` diagnostics; closed wakes are counted separately. Upstream
reconciliation is explicit at startup and on repair. Upstream asynchronous
candidate readiness has no public FD, so a timer probes existing candidates;
it does not replace direct-key IPC with periodic namespace scanning. Source
continuations are process-local coalesced owner work.

All Lua handlers enter on the main caller in the owner process. The dependency
dispatcher thread never invokes these handlers. One dispatcher routes all kinds
registered for a workflow. Handler completion/retry/dead-letter selection is a
typed token; only that returned token requests deferred terminalization.
Exceptions and absent outcomes become retries. Payload and renewal methods reject
use after handler return. Native pump closes job handles on its error/terminal
paths. Push payload streaming uses public `write_payload` directly, without
materialization, staging files, or a private Lua userdata cast.

Generic receivers, timers, and queue consumers share scope lifecycle. Queue
delivery uses public zero-wait `dequeue_batch` with page size one, typed outcomes,
handler-scoped handles and cleanup on terminal errors. This avoids upstream's
empty single-dequeue userdata inconsistency. A failed scope can stop independently
under explicit stop policy; recursive pump/stop/close and recursive materializing
start are rejected. App stop closes ingress before dispatcher shutdown, uses the
remaining app deadline, and closes clients after dispatchers. Restart recreates
runtime clients/dispatchers; declaration metadata remains immutable.

Memcheck exposed an upstream batch-message ownership mismatch: native batches
keep message handles batch-owned, while the Lua `message:ack/nack` methods clear
the native pointer on success. Since batch results have already transferred
their handles to Lua, those methods leak the handle and retain its client. The
consumer uses public client `queue_ack(message)` / `queue_nack({message=...})`
operations and then explicit message close, preserving public ownership rules.
No dependency source is patched and no private layout is accessed. Memory
verification below distinguishes the initial failing run from the repaired path.

## Behavior evidence

The checked-in tests distinguish API availability from behavior:

| Test | Observable contract |
| --- | --- |
| `vectis_unit_supervisor` | Exact capacity/frame bounds; copied bytes and frame metadata; finite signal coalescing under full capacity; unknown kind rejection; fairness under replenishment; immediate zero-wait FD readiness after fork; producer receive denial; close-on-exec; lifecycle transitions, startup failure cleanup, restart and no descriptor count growth. |
| `vectis_lua_supervisor_workflow` | Real encrypted local Pouch: actual receipts traverse IPC and notify/pump in the first zero-wait dispatch on a fresh direct path; durable completed/retry/dead-letter envelopes; duplicate suppression; caught unregistered-kind state rollback; streaming source/sink chunks; explicit renewal; handler lifetime; separate workflows/kinds/scopes; bounded overflow repair; startup recovery; logical failure isolation; guarded reentry; actual Vectis storage defaults within the test directory; queue ack and partial startup failure cleanup. Normal and packed Linux runs. |
| `vectis_lua_supervisor_route` | Two real Kore worker processes commit from HTTP routes; route responses assert zero producer effects; both kinds run in the owner main Lua caller through one workflow dispatcher; duplicate request suppression; signal-driven owner wait and graceful shutdown. Normal and packed each run a direct sequential scenario with recovery probes set to 1,000,000 ms and discovery disabled: eight sequential commits trigger owner handlers through received keys without timer recovery or a repair signal. The one-shot SUS fixture is drained and disabled before the first HTTP commit, so it cannot supply an unrelated wake during direct-receipt proof. Each also runs an eight-request saturation scenario which may use explicit overflow repair and candidate probes. |
| `vectis_example_lua_supervisor_workflow` | Runnable receipt-to-owner streaming output example in normal and packed execution. |

Initial and expanded selected runs pass. The full debug build uses the configured
Bootlin compiler, C89 warning/error policy and unchanged host-debug dependencies.
The following earlier implementation checks establish the existing product
closure; fresh remediation checks are recorded in the upgrade audit. All operational commands were serialized.

| Command/check | Result |
| --- | --- |
| `cmake --build build/debug -j2` | Full configured debug build passed with Bootlin and strict C89 warnings. Subsequent CLI/unit builds incorporated the final Lua declarations and tests. |
| `ctest --test-dir build/debug -j1 -R 'supervisor' --output-on-failure` | Earlier 5/5 passed: `vectis_unit_supervisor`, `vectis_lua_supervisor_workflow`, `vectis_lua_supervisor_route`, `vectis_example_lua_supervisor_workflow`, and `vectis_lua_supervisor_upstream_stateful`. The last is an upstream renewal blocker probe, distinct from the successful short stateful delivery test. All four Lua scenarios run normal and packed Linux binaries. The expanded route test separately passed in 1.90 s with both direct-wake and saturation cases. |
| Selected runtime/auth/worker/pack regressions | 51/51 passed in 60.95 s; exact selection below. Includes native app auth/cleanup, failure modes, restart, signal shutdown, existing managed worker services, lockd helpers, pack, SDK headers, and runtime contracts. |
| `valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible --errors-for-leak-kinds=definite,indirect,possible --track-origins=yes --error-exitcode=99 build/debug/tests/unit/vectis_unit_supervisor` | Earlier run: zero errors, all heap blocks freed. Logs: `build/supervisor-memcheck-c-latest.log`. |
| Same Memcheck options on `build/debug/vectis tests/lua/supervisor_workflow.lua` | Earlier run: zero errors; zero definite, indirect or possible lost bytes. 4,955,634 bytes in nine blocks remain reachable. Tests use fresh stores and XDG paths inside `build/supervisor-memory/`. Logs: `build/supervisor-memcheck-lua-latest.log`. Initial failure and repaired runs are retained separately; the leak was not suppressed. |
| Shared/static SDK build and dynamic export verification | `cmake --preset debug -B build/supervisor-shared -DVECTIS_BUILD_SHARED=ON -DVECTIS_BUILD_BINARY=OFF -DVECTIS_BUILD_TESTS=OFF -DVECTIS_BUILD_EXAMPLES=OFF` and build passed. `cmake/vectis_export_policy.cmake` verified the canonical allowlist, including the additive supervisor functions. Existing app ABI layout and SOVERSION remain unchanged. |
| `cmake --install build/supervisor-shared --prefix build/supervisor-shared/install` and `bash scripts/verify_installed_sdk.sh build/supervisor-shared/install static .cache/deps/host-debug` / `shared` | Both passed. Each mode verified installed C89, C++98 and pkg-config consumers plus 36 example runtime closures against the supplied pinned dependencies. C consumer exercises new channel send/receive; shared privacy sentinel remains unexported. Logs: `build/supervisor-sdk-verify-final.log`. No release archive assembly or publication is authorized. |
| `VECTIS_VERSION=0.0.0 VECTIS_DIST_DIR=$PWD/build/supervisor-lua-dist bash scripts/stage_lua_rock_sources.sh` | LuaRocks artifact validator passed for source tarball, rendered rockspec and source rock. Archive readback confirms both new modules and the manual. This checks source packaging, not a fresh LuaRocks installation with fetched dependencies. |
| `bash scripts/check_format.sh`; `git diff --check` | Passed. |
| `bash scripts/test_lua_facade_matrix_contracts.sh`; `bash scripts/test_lua_facade_behavior_coverage.sh`; `bash scripts/test_service_runtime_lifecycle_audit.sh` | Passed, including new module documentation/alias/behavior entries. |
| `bash scripts/test_vendor_kore_lifecycle.sh`; `bash scripts/test_release_privacy_contracts.sh` | Passed. |
| `bash scripts/test_lifecycle_contracts.sh` | The earlier blanket `.github` ban conflicted with the already adopted native Darwin lane. The remediation narrows the policy to that declared workflow; the adopted workflow is extended with serialized supervisor/Lua/lifetime coverage; no hosted run is claimed. Fresh local gate results are recorded in the upgrade audit. |

Exact broader regression selection:

```sh
ctest --test-dir build/debug -j1 -R 'vectis_(lua_app_|server_signal_shutdown|unit_(supervisor|runtime|config|auth|mailbox|sdk_surface|header_cpp)|runtime_|example_lua_(mailbox_pipeline|curl_worker_service|cai_worker_service|audio_worker_service|sus_worker_service|supervisor_workflow)|lua_(pack|browser_sessions|lockd_helpers|lockd_examples|supervisor|smoke|top_level_module|preload_artifact_contract|facade_behavior_coverage))' --output-on-failure
```

The broad run preceded the dedicated stateful blocker probe and subsequent
remediation. Fresh failure-fast, repeated workflow, native sanitizer, memory,
source-rock and installed-consumer checks are recorded in the upgrade audit.
Those earlier scoped checks did not run the entire repository suite; the final
verification update below records the subsequent complete standard gate.

## Task file inventory

This inventory describes the reviewed working candidate before commitment.
`AGENTS.md` supplies the mandatory
agent reasoning gate. Scratch, logs, SDK installs and test stores are ignored
under `build/`; self-contained upstream reports are ignored under `stash/`.

| Area | Files |
| --- | --- |
| Core/public boundary | `include/vectis/supervisor.h`, `src/vectis_supervisor.c`, `src/vectis_supervisor_internal.h`, `src/vectis_lua_supervisor.c`, `src/vectis_lua_supervisor.h`, `src/vectis.c`, `src/vectis_cli.c` |
| Lua | `lua/vectis.lua`, `lua/vectis/supervisor.lua`, `lua/vectis/workflow.lua` |
| Build/package/test registration | `CMakeLists.txt`, `cmake/vectis.exports`, `vectis.rockspec.in`, `tests/CMakeLists.txt`, `tests/unit/CMakeLists.txt` |
| Behavior and consumer tests | `tests/unit/test_vectis_supervisor.c`, `tests/unit/test_vectis_header_cpp.cpp`, `tests/install/consumer.c`, `tests/install/consumer_cpp.cpp`, `tests/lua/smoke.lua`, `tests/lua/supervisor_workflow.lua`, `tests/lua/supervisor_workflow.cmake`, `tests/lua/supervisor_route.lua`, `tests/lua/supervisor_route.py`, `tests/lua/supervisor_upstream_stateful.lua` |
| Example | `examples/lua/supervisor_workflow.lua` |
| Documentation | `docs/supervisor-workflow-audit.md`, `docs/supervisor-workflow-spec.md`, `docs/service-runtime-lifecycle-spec.md`, `docs/lua-coverage-matrix.md`, `manual/lua-supervisor.md`, `manual/lua.md`, `manual/lua-app.md`, `manual/lua-lockd.md`, `manual/api.md` |
| Coverage contracts | `scripts/test_lua_facade_matrix_contracts.sh`, `scripts/test_lua_facade_behavior_coverage.sh` |

## Concrete blockers and deliberate limits

1. **Pull payload source:** `lc_outbox_job` has public `write_payload(lc_sink)`;
   it has no public pull `lc_source` or chunk reader. The Lua facade mirrors push
   streaming and provides no supported native job interop accessor. A pull
   `job:payload_source()` usable by existing HTTP clients cannot be implemented
   honestly without upstream support. Buffering/spooling and private userdata
   aliasing were excluded. Reconsider when upstream supplies a handler-scoped
   pull source or an interoperable bounded producer-to-consumer bridge.
2. **Automatic long-handler renewal:** public jobs expose explicit `renew`, but
   the dispatcher does not renew an active host-owned claim automatically.
   Lua blocks inside synchronous foreign operations; its owner timer cannot
   execute then. There is no public supported accessor that would let a Vectis
   native helper safely retain the Lua-owned job for renewal. Explicit stateless
   handler renewal is implemented, but does not prove lease retention
   through arbitrary blocking work. Reconsider with upstream automatic renewal
   under dispatcher ownership, with error/cancellation and terminalization races
   defined. This remains an unmet specification requirement.
3. **Kind-local pause:** one shared dispatcher has no public kind-filtered claim
   operation. While a registered scope is stopped, the entire workflow pauses
   intake to avoid claiming work for a stopped owner. Other workflow identities
   continue. Reconsider when claims can select runnable kinds without retrying
   or retaining stopped-kind jobs.
4. **Cross-runtime leadership:** leader/configuration guards are not implemented;
   shared runtime mode is rejected. This is future Vectis work: public client
   leases and transactions can implement canonical identity/leader guards.
   An upstream reusable guard would be optional, not required to make it possible.
5. **Other planned execution modes:** isolated/module Lua lanes, automatic restart
   backoff and request/reply helpers remain future work. Both C and Lua frames expose
   correlation/flags metadata but no request/reply abstraction is claimed. Queue
   forced dead-letter is not wrapped because no public operation exists;
   upstream queue retry/max-attempt policy owns that transition.
6. **Shutdown of blocking callbacks:** serial callbacks cannot be preempted;
   upstream synchronous client close can join dependency threads. Tests prove
   orderly shutdown, not a hard deadline for arbitrary blocked network handlers.
7. **Stateful claims plus renewal:** a direct public upstream probe demonstrates
   successful stateful read/update/completion, but `job:renew(30)` followed by a
   successful checkpoint update fails at completion with `pouch state version precondition
   failed`. `tests/lua/supervisor_upstream_stateful.lua` reproduces all three
   cases without timing faults or Vectis wrappers. Short `with_state=true`
   handlers support checkpoint read/update/completion and prohibit release/close
   or access after handler return. Their renewal is rejected until upstream fixes
   the version contract. Stateless handlers expose manual renewal; no initial
   renewal or automatic blocked-handler renewal is claimed.

The pull-source, automatic-renewal, and stateful-renewal requirements remain blockers for declaring
the entire specification complete. Remote Lockd/provider execution, Darwin IPC,
hardware/live services, and prolonged claim-expiry behavior are not established
by these local Linux tests. This task does not release or approve a release.

Parent review remediation additionally proves the coalesced wake race with an
actual-send pthread/condition handshake: B arms the FD and the owner receives
while A remains paused, then A's late token produces no duplicate retained signal.
Every accepted signal now attempts a nonblocking wake, and post-stop frame sends
return closed before trying a contended mutex. Current exact commands/results and
the separate Kore Memcheck limitation are recorded in
[the dependency seam audit](lua-upgrade-seam-audit.md#prior-parent-review-remediation).

## Final local verification update

After the last source allocation-error fix, the final serial `finalize-slice`
passed 35/35 fast checks (16.05s) and 176/176 standard checks (248.39s), with zero
failures/CTest skips; opt-in omissions are
separately recorded. Final installed static/shared core checks passed 36 example
runtime closures each, source-rock ordinary Lua loading passed, and targeted
ASan/UBSan and native Memcheck passed. Strict real-service Memcheck remains
non-clean, including Kore padding and fork-exit Lua/LoneJSON possible losses.
The [current semantic audit](lua-upgrade-seam-audit.md#completed-independent-review-and-final-verification)
records exact final logs, the completed independent review/fix loop, the
standalone Kore baseline report and remaining limitations. The final independent
source re-review is clean, with no actionable findings. This update
does not remove any unmet upstream contract or claim a future local phase is
implemented.
