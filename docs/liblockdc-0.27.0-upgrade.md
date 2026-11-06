# liblockdc 0.27.0 upgrade

Historical evidence for 0.27.0. The [0.28.0 upgrade](liblockdc-0.28.0-upgrade.md)
supersedes its current-version and encoded staging/spill descriptions; push and
staged append now stream directly into canonical segments.

## Reviewed specification

Baseline `c10a03e` selects liblockdc 0.24.0. Upgrade the seven core SDKs,
seven matching PostgreSQL providers and released Lua sources together to
[0.27.0 from c89systems/liblockdc](https://github.com/c89systems/liblockdc/releases/tag/v0.27.0).
Reviewed source SHA-256:
`8b3a63fc6a135edc5e440e02f81b135f6de534ccf0399fe27c3cf8bccf913d24`.
Release commit: `be40d56f4459af2f65cae2c980cfe4246d223220`.
Other pins remain unchanged: cpkt 0.6.1, cpktdb 0.3.1, LQL 0.4.0,
LoneJSON 0.45.0 and pslog 0.12.0 meet the released dependency requirements.

Reviewed 0.25/0.26/0.27 notes, all public header differences, native collection,
Lua, compression and provider contracts, docs/examples and upstream tests.
Core ABI advances 7→8→9 across those releases; select released ABI 9, with
provider ABI 0 rebuilt for core 9. Vectis has no published release baseline;
its initial ABI 0 is unchanged, and all downstream consumers are rebuilt.

Expose Pouch collections through both unchanged upstream `lockdc` Lua and
Vectis-owned `app:storage()`: create/open/list/capabilities; direct and staged
append; owned push writers; metadata/retention/status; ordered reads/cursors/LQL;
acquire/takeover/keepalive/release; coherent heads, atomic edits/status and read
head preconditions. Acquire/takeover return app-owned collection proxies.
Release invalidates a proxy only on success. Local close and app stop close
app-owned receivers and unfinished writers; they do not release durable
ownership. App scopes retain their resources until explicit close or stop;
native receiver GC also never releases durable ownership.
Renew/release explicitly; no implicit renewal task, foreign-thread Lua call,
automatic takeover or retry/rebase. Preserve exact decimal fields and native
structured errors, current-head observations and uncertain publication results.

Collection leases may be established atomically at creation. Defaults remain
multiwriter/public reads with no lease; acquired leases default to 30 seconds.
Private reads and stale writer fencing follow upstream. Atomic edits are
collection-local, independent of XA, with at most 64 position ranges and a
1024-byte recovery descriptor. Replacement sources and direct pull/read remain
streaming. Staged append and push writers explicitly stage encoded bytes up to
64 KiB then spill to an anonymous file; encrypted roots spill ciphertext.
Do not describe staged publication as end-to-end streaming. Push commits now
pack shared rolling segments. Remote capabilities remain false/unsupported.

Stop every existing root client before first open: upstream upgrades old
128-byte collection heads to 384 bytes with exclusive ownership, preserving
entry bodies/identity/positions. Busy upgrade reports collection_upgrade_busy.
Do not downgrade or mix older clients afterward. New edits also require all
clients upgraded. No Vectis migration implementation or format bump is added.
Compression changes remain synchronous before open, now replacing one canonical
file at a time with durable recovery metadata. Failed/interrupted conversion
resumes even when compression is omitted; allow largest-file scratch, indexes
and final decompression growth, and never delete recovery metadata to bypass it.
Existing logger, typed-option precedence and post-fork client opening remain.

Implement the complete convenience seam, direct/convenience behavior tests,
native/default-stack coverage, runnable example, manuals, matrix and audit before
broad testing. Prove leases/fencing/private reads, staged sources/cleanup,
streamed edits/head conflicts/retries/recovery markers, stop/restart, existing
conversion/continuations and XA wake delivery. Verify an actual 0.24 root with
the old executable before opening it with the new product. Use focused Debug
checks, then standard native tests, musl defaults/memory, all target builds,
normal/packed workflows, source/header provenance, source rock, embedded manuals
and relocated static/shared consumers. Reuse unaffected prior evidence and
record Darwin/ARM emulation limits precisely. No release/tag/push.

Specification review: complete; no unresolved product choice.

## Delivered integration and review

All 15 lockdc/provider/Lua pins now use verified 0.27.0 artifacts from the new
repository. Build requirements are exact; installed CMake/pkg-config minimums
are 0.27.0. Other dependencies and Vectis public C declarations are unchanged.
The released PostgreSQL header is identical to 0.24.0. Its new provider needs
core SONAME 9, while keeping provider SONAME 0. CAI has no collection imports or
shared lockdc dependency requiring an additional upgrade.

Production changes are confined to Vectis's owned storage method/result maps
and successful collection-release invalidation. Both public collection layers
are usable end to end. Native C factories already return the complete upstream
receiver; no extra C wrapper or provider extension is needed. Upstream Lua
sources are consumed unchanged, including protected callback/result adoption.

Review traced acquisition allocation before ownership publication, credential
fencing at commit, private read admission, immutable edit controls, provisional
replacement/receipt publication, status reconciliation, pinned read visibility,
per-file conversion recovery and exclusive head upgrade. Bounded edit/control
workspaces remain on the owning allocator. Existing XA receipt-to-supervisor
wake and post-fork client materialization remain intact. No new polling,
renewal task, producer thread or local provider patch is introduced.

Review added late staged-source failure beyond the spill threshold, a 256 KiB
streamed edit/readback checked by incremental hashes, exact new-header/default
checks in installed consumers, and the actual released collection Lua binding
inside the default-stack worker. These are test refinements; no production fix
or repeated full platform build followed them. The worker links the same
released binding objects and existing Vectis open bridge as the product.

## Verification

Evidence is under ignored `build/lockdc-0.27.0-review/`; archive acquisition used
the owned `build/useful-lua-facades/dependency-cache`.

| Check | Result |
| --- | --- |
| Debug build and full `ctest --test-dir build/debug --output-on-failure -j1` | **285/286 passed**, 513.24s. The one failure was the existing OPC UA service shutdown in `vectis_lua_upgrade_seams` (`BadInternalError`, line 82); an unchanged isolated rerun **passed**, 5.98s. No OPC UA fix or clean first-pass suite is claimed. PostgreSQL XA/services, storage, wake delivery, fork, packed and cleanup checks passed. |
| Affected Debug checks | **7/7 passed**, 15.32s; direct/convenience collections normal/packed, existing conversion/inheritance, native storage, example, source provenance and default worker. Final large-edit/spill check **1/1 passed**, 2.58s. Final worker/example checks **2/2 passed**, 1.14s. |
| Collection behavior | Atomic creation with lease, acquire/takeover, stale owner/writer rejection, private read denial before bytes, separate renewal, release success/failure, staged single-pass source close/no reset, retries, early/late source faults, streamed cuts/replacement, stale read/edit heads, bounded recovery markers/anchors, persistent receipts, stop/restart and remote unsupported capabilities. Plain and encrypted storage, none/zlib codecs; app-owned roots keep Vectis encryption defaults. |
| Real legacy root | Old relocated executable asserts lockdc 0.24.0 and seeds an encrypted collection, ETag and saved cursor. New Debug executable opens it through app storage, upgrades heads, converts none→zlib, preserves IDs/ETag/cursor/count policy, then leases/edits it and reopens with compression omitted. Manifest reports `collection_head_bytes=384`. Logs and seed/check scripts retained. |
| Strict Valgrind full collection workflow | **50823 allocations/frees**, zero live bytes, **zero errors**, no suppressions. Includes direct and app-owned receivers, late spool failure, encrypted multi-frame edit/read, takeover and stop/restart. |
| Final strict Valgrind combined worker | **117957 allocations/frees**, zero live bytes, **zero errors**, no suppressions, including released collection Lua callbacks. Earlier native-only collection worker also passed. |
| Seven Release targets | CLI/static/shared libraries and C89 public header compilation pass on x86_64 GNU/musl, aarch64 GNU/musl, armhf GNU/musl and arm64 Darwin. Actual binding compilation uses the selected imported SDK headers; native/source/embedded-module provenance checks pass. Darwin is compile/link only. |
| Native musl integration | **7/7 focused checks passed**, 15.85s, including normal/packed direct and app-owned collections and existing conversion/storage. Final Lua-enabled default worker **passed**, 1.68s. |
| Three musl default worker stacks | NULL pthread attributes; x86_64 **133656 B**, aarch64 **133640 B**, armhf **132048 B**. ARM under QEMU. Native large append/read/LQL and atomic edit paths plus encrypted released-Lua edit/read callbacks run under an active native digest reader with nested Vectis buffer callbacks. No enlarged stacks. The native combined lockdc probe frame is **1760 B**; released collection dispatcher **1264 B**. These are measured individual frames, not a universal call-chain bound. |
| GNU Release execution | Final collection workflow passes normal and Linux packed execution on x86_64 and aarch64 GNU; ARM under QEMU. |
| Relocated GNU SDK | Actual archive generated/extracted into a new prefix; static and shared C89/C++/pkg-config consumers pass. New public edit/read/lease types and initializer defaults compile/run with core 0.27.0. All **36 installed C examples per mode** pass runtime contracts. |
| Manuals/examples/source rock | Embedded app/lockd/storage and other current manuals match source bytes. Both collection examples pass. Lua source-rock helper smoke passes using native stubs; actual native behavior is established separately above. |
| Formatting/whitespace | `scripts/check_format.sh` and `git diff --check` pass. |

The isolated OPC UA rerun does not establish the cause or repair of its first
shutdown failure. It remains an intermittent verification limitation outside the
changed lockdc/storage surface. Any Vectis release still needs its fresh release
gate; this upgrade is not release approval.

No new ARM app-emulation claim is made for aarch64 musl or armhf GNU/musl: the
existing QEMU process-shared robust-mutex limitation remains documented in the
0.24 review. Their products build, and both ARM musl library/Lua default-worker
probes execute successfully. Native ARM app deployment and Darwin execution are
not proved here. The downstream tests do not repeat upstream private-hook
power-loss, allocation/publication-fault or controlled maintenance-clock suites.
They prove the promised Vectis seams without adding those hooks to the product.

Final integration review found no actionable defect in the changed surface.
The ownership, streaming/staged distinctions, compatibility commitment and
verification limits above are explicit. No release, tag or push is performed.
