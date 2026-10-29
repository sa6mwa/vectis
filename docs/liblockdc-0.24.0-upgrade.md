# liblockdc 0.24.0 upgrade

## Specification and semantic review

Baseline: `60bd3a3`, liblockdc/core and PostgreSQL provider 0.23.0,
cpkt 0.6.0 and liblql 0.4.0. Released lockdc tag commit is
`7ed2c2a5fc1db9620123abdd3845132b818114de`. Upgrade core, separate provider and released Lua
sources together to 0.24.0 on all seven targets. Select cpkt 0.6.1 as required
by this release; retain cpktdb 0.3.1 and liblql 0.4.0. The existing
[LQL upgrade](liblql-0.4.0-upgrade.md) is already delivered.

Reviewed the [released source and notes](https://github.com/sa6mwa/liblockdc/releases/tag/v0.24.0),
public core/provider headers, Lua bindings/modules, collection/writer/compression
contracts and examples against the selected 0.23.0 inputs. Core ABI advances
6→7 for collection request/result/capability and receiver layouts. Provider
ABI remains 0 but must be rebuilt against core 7. No libvectis public declaration
changes. Its collection adapter was introduced in unreleased work (`cf0d818`);
Vectis has no published release/tag baseline, and its initial SOVERSION stays 0.
All consumers in this verification are rebuilt against the dependency layouts.

- Count retention: creation and collection metadata accept decimal-string
  `max_entries`; describe/list return it. Open and entry metadata reject it.
  Maintenance trims oldest live entries lazily, independently of age expiry;
  append may exceed the limit and disabling background compaction disables
  automatic trimming. Zero disables; omission preserves. No disk-cap promise.
- Incremental append: consume upstream `append_begin`, writer `write`, `commit`,
  `abort`, `close` directly, without a local dependency facade extension.
  App storage must own the returned writer, guard its calls, and close it on
  stop before older resources. A writer retains its collection independently.
  Commit is terminal; close/abort never publish. Propagate structured errors,
  uncertain publication/status reconciliation and exact decimal result fields.
  No whole-value buffering, producer thread or staging bridge is needed.
- Both append forms accept atomic `collection_retention_seconds`. Only new
  membership applies it; retries expose the original deadline, not the current
  parent deadline. Creation defaults remain unlimited/no expiry.
- Compression: omitted mode inherits an existing root; explicit `none` or
  `zlib` converts synchronously before open returns. Writer conflicts warn and
  adopt stored mode; conversion errors fail open. Existing typed/legacy/query
  precedence and app configuration copying remain applicable. Document disk
  headroom/startup latency, peer-writer refusal and post-fork opening. Do not
  invent a background migration or assert conversion despite a peer conflict.
- Native copy/mutation/query workspaces move to bounded heap scratch; implicit
  retention transactions are retired once fully applied. XA outcomes and wake
  notification contracts do not change. Exercise existing storage/XA/renewal
  paths and a default-stack musl worker using the new collection operations.
- [cpkt 0.6.1](https://github.com/c89systems/cpkt/releases/tag/v0.6.1)
  changes only the libssh2 AES-GCM unused-key cleanup, its regression/build
  registration and documentation. The complete release comparison has no
  public-header, Lua, option, callback, ABI or default changes. Relink static
  consumers; no SSH execution feature or local vendor patch is involved.

Implementation: update verified archive digests and exact package requirements;
add only the Vectis-owned writer lifetime seam, behavior tests, example/manual
updates and audit/index links. Existing upstream Lua facades remain unchanged.
The reviewed specification is sufficient; no unresolved product decision.

## Verification requirements

Finish the implementation before broad verification. Prove fragmented bounded
push delivery, success/error/abort/close, owner close and app stop/restart,
retry/conflict and parent-renewal observations; count option propagation and
rejection; compression conversion/inheritance in both directions with encrypted
state/attachments/collections and continuation preservation. Verify normal and
packed execution, selected released compilation/header inputs and embedded
Lua bytes, native/default-musl-stack operation, existing XA/PostgreSQL workflows,
source-rock and installed static/shared consumer closure. Compile supported
targets; distinguish cross compilation from target execution. Record actual
commands, results and limitations below. No release or push is authorized.

## Implementation and verification

Delivered: 22 archive pins (seven core SDKs, seven lockdc SDKs, seven PostgreSQL
providers and one released Lua source archive), exact build requirements and
installed CMake/pkg-config minimums. LQL's eight existing 0.4.0 pins are retained.
The lockdc source archive reviewed has SHA-256
`1da0adc674f334ed41861e6209abf663548840ce2470e67125a1de830d2de13c`.
The separate PostgreSQL public header is byte-identical to 0.23.0.
cpkt release commit is `5afb8f58c66201aaf12e7863fbbbf84ab5e8e6b6`;
the selected SDK manifest includes the AES-GCM cleanup recipe input.

Production runtime changes are confined to the Vectis-owned storage method and
result-kind maps: `append_begin` returns an owned `collection_writer`, with
guarded write/commit/abort and existing reverse-order stop cleanup. Upstream Lua
sources are unchanged. Count and parent-retention options already pass through;
native open already preserves compression omission and copies explicit settings.
No additional configuration policy, local provider patch or wrapper is needed.

The review traced native writer entry/leave/close, incremental encoder writes,
canonical pending-inode publication by rename, retry comparison before parent
renewal, bounded lazy count maintenance, and compression exclusion/recovery.
These are implemented native paths, not only upstream design text. Vectis's
existing storage/XA receipt-to-supervisor wake path is unchanged and exercised
by the live PostgreSQL/storage regressions.

Evidence under ignored `build/lockdc-0.24.0-review/`:

| Check | Result |
| --- | --- |
| `cmake --build build/debug -j2`; `ctest --test-dir build/debug --output-on-failure -j1` | **283/283 passed**, 457.12s, including PostgreSQL XA/standalone services, fork/lifecycle, packed execution, existing renewal/storage, trace and failure regressions. |
| `vectis_lua_lockdc_upgrade` | Normal/packed: bounded fragmented push, invisibility before commit, duplicate/conflict, abort/close/invalid JSON, closed owners, app stop/restart, canonical uint64 count validation and lazy-policy propagation, atomic parent clear/renewal and retry observations. |
| Compression integration in that test | Encrypted state, attachment, collection identity/ETag/count and saved continuation survive none→zlib→inherited→none; peer refusal preserves stored mode; a wrong-key conversion request fails startup and leaves the original root usable. |
| `vectis_lua_upstream_sources` and actual compile/link readback | Released lockdc collection binding compiled directly, selected provider include roots, exported embedded lockdc/pslog/LQL/MDF bytes equal selected modules. Manifest dependency invalidation rebuilds native sources despite archive mtimes. Core SONAME 7; provider SONAME 0 with NEEDED core 7. |
| Seven Release targets | Static/shared library, CLI and public-header compilation pass on x86_64 GNU/musl, aarch64 GNU/musl, armhf GNU/musl and arm64 Darwin. Darwin is compile/link evidence only. |
| Native x86_64 musl focused checks | **7/7 passed** including normal/packed upgrade/storage, released sources/facades, LQL default/small worker and combined native/Lua worker. |
| Final review additions | Wrong-key startup/root preservation, parent-clear assertions and collection LQL scan-path probe pass focused GNU **2/2** and x86_64 musl **2/2**. No production-code defect found; no repeated full matrix for test-only additions. |
| Musl default worker stacks | `pthread_create(..., NULL, ...)`: x86_64 **133656 B**, aarch64 **133640 B**, armhf **132048 B**. ARM runs under QEMU. New probe pushes a 256 KiB compressed collection item, reads it unfiltered and through LQL's scan path, and performs 64 KiB bulk copy alongside existing library/Lua exercises. Measured added probe frame **864 B**; no enlarged stacks. |
| Final strict Valgrind worker probe | **72318 allocations/frees**, zero bytes/blocks at exit, **zero errors**, no suppressions; includes the added LQL scan path. |
| Relocated GNU SDK archive | `scripts/verify_installed_sdk.sh PREFIX static` and `shared` pass C89/C++/pkg-config, actual lockdc **0.24.0** and LQL **0.4.0** calls, private-export/runtime closure and **36 installed C examples per mode**. |
| Source rock/manual/example | `scripts/test_lua_rock.sh` passes helper packaging with native stubs; native behavior is proved separately above. Embedded manual byte readback includes lockd/storage/supervisor. `examples/lua/storage_writer.lua` runs with owned state/config directories. |

ARM app execution limitation: aarch64 GNU's complete upgrade workflow passes
under QEMU. aarch64 musl and both armhf variants stop at the existing supervisor
inbox mutex initialization, before storage client materialization. QEMU returns
`ENOSYS` for `get_robust_list` (aarch64 musl) or `set_robust_list` (armhf GNU);
the supervisor requires process-shared robust mutexes. Those app runs are not
passing evidence. Their builds and the two ARM musl native/default-stack probes
pass. Native ARM app execution remains unverified here; no runtime fallback or
weakened synchronization is introduced. Traces and failed-run logs are retained.

This downstream slice does not repeat upstream crash-in-publication, injected
disk/allocation-fault or controlled maintenance-clock tests. Count coverage here
proves option propagation/validation, persistence and absence of synchronous
trimming; it does not claim a forced janitor pass through a private test hook.
Existing LQL persistent-OOM closer-entry and zlib `gztell64` findings remain
documented in their own reviews; neither is patched or claimed fixed here.

Final review: the requested upgrade and required Vectis ownership seam are
implemented. Verification limits above are explicit. SSH execution roadmap,
LZO/archive deferral and other workflow gaps are unchanged. No release, tag or
push is performed.
