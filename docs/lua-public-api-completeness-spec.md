# Complete public API exposure in Lua

Status: historical specification; its exhaustive coverage requirement is superseded.
The user's 2026-10-28 correction scopes new dependency bindings to useful public
APIs of libraries without upstream Lua facades, with concrete Vectis or bundled
application use cases. Consume existing upstream facades unchanged, including
pslog, LQL and MDF. Omitted C APIs and unused surfaces are not automatic gaps.
Vectis-owned convenience APIs remain a separate boundary. See
[the current scope and restoration](upstream-lua-facades.md). The historical
requirements below do not authorize exhaustive coverage or provider extensions.

Historical scope and implementation specification:
This document does not claim that the listed gaps are implemented. The existing
coverage matrix describes present behavior; its historical C-only exclusions
are not exclusions from this goal.

## Scope and authority

Expose every public operation, constant, relevant public value/config field and
callback contract in libvectis and its bundled application dependencies. Include
the installed C89 SDK facades, imported upstream public surfaces where those
facades declare them, and the authoritative upstream Lua modules. This includes
SQLite, PostgreSQL, PDF/libHaru, PNG, OpenSSL object APIs, XML DOM/SAX, raw libssh2,
GSSAPI/SASL/plugin APIs and transport controls previously excluded from Lua.
The prior decision to keep four library facades in a separate task is superseded
for public API exposure. Future upstream APIs, private implementation functions,
compiler implementation details and test-only frameworks are not public product
APIs. Supplied-but-unused application libraries must still be inventoried; they
cannot be silently omitted because no Vectis convenience wrapper exists today.

Use exact pinned installed headers, released sources, exported ABI metadata,
upstream documentation and actual implementation behavior as evidence. A facade
module's presence or a search for a method name proves neither completeness nor
correctness. Record each public C operation and its Lua mapping, value domain,
ownership, callback/thread behavior, flow classification and behavioral proof.
Compiler-derived declaration inventories assist this work; the responsible
agent performs the semantic review and resolves aliases and macro APIs.

## API naming and flow

Whole-value operations must say materialized. Callback, iterator, file or chunk
syntax alone does not establish streaming. Use streaming names only when input
and output move incrementally through the native and Lua path without collecting
the whole value. Tiny bounded transport/parser chunks are acceptable. Do not
concatenate input, serialize a whole result and slice it afterward, hide a spool
or temporary file, or make a buffered provider result appear producer-backed.

Preserve public dependency access and provide Vectis convenience integration
where ownership, app lifecycle, diagnostics, routing or common workflows require
it. Map native names explicitly where the materialized qualifier changes them.
For pre-1.0 Lua changes, migrate all callers and remove ambiguous aliases; do not
retain a compatibility layer that perpetuates an unclear flow contract.

Some public APIs are intrinsically materialized. For example, the installed
PDF header explicitly describes save-to-stream/read-from-stream as a saved
memory buffer. Expose that public functionality as buffered/materialized, not
as a true callback serializer. DOM objects and already-owned result/value buffers
are also explicit materialized representations. Where a true native stream is
available, expose it faithfully rather than replacing it with those helpers.
If a required stream is unavailable through the public C89 API, record the exact
missing operation and upstream handoff; no private/native bypass or invented
implementation is allowed.

## Shared binding rules

- The C SDK remains Lua-agnostic. Vectis-owned stateful functionality uses
  public receiver shells and exported free functions. Lua uses public C APIs.
- Retain parent/borrow ownership in private tokens that outlive every referring
  shell. Invalidate aliases before native destruction. Never inspect an upstream
  private Lua userdata layout to obtain a native pointer; use public interop or
  a maintained owner-side adaptation with explicit acquisition provenance.
- Install cleanup owners before native allocation. Lua OOM, callback exceptions,
  early abandonment, double close and VM shutdown cannot leak or access freed
  resources. Metamethods validate their userdata type. Never unwind a Lua error
  through an unprotected provider frame or libpng-style native error boundary.
- Preserve complete signed/unsigned domains, especially uint64 values, without
  floating-point conversion or narrowing. Use explicit exact representations for
  values outside Lua's signed integer range. Check sizes before allocation,
  arithmetic and conversion to native int/long/size_t fields.
- Copy provider errors before freeing their owners. Distinguish EOF, pending,
  failure, cancellation and unknown outcome where the public contract permits.
  Do not turn a callback/provider failure into successful stream completion.
- Bind callbacks with protected calls and retained contexts. Foreign threads
  cannot enter arbitrary Lua states. Define owner-thread restrictions, bounded
  handoff and backpressure for each asynchronous callback interface; a queue is
  not permission to retain an expired borrowed provider pointer.
- Global setters remain explicit global operations. Specify owner lifetime,
  restoration and interaction between Lua states/apps. Do not silently replace
  process-wide logging, allocation, authentication or diagnostic hooks.
- Capture app declarations before start; materialize thread-bearing resources
  after fork in the using process. Cover stop, restart, close and retained shells.
  Blocking methods remain visibly blocking. No hidden worker, retry or reconnect.
- Do not invent an unsafe generic function/pointer invocation layer as a symbol
  coverage substitute. Variadic operations, callback types, native object graphs
  and allocator APIs need reviewed typed contracts and actual usable lifetimes.

## Required component specifications

| Component | Required inventory and gap work |
| --- | --- |
| libvectis | Every public header/free function/receiver, configs, defaults, callbacks, worker/service adapters, stream readers/writers, diagnostics and process lifecycle; map to app methods or owned Lua receivers. |
| OpenSSL | Digest/HMAC/codecs and signing; incremental EVP operations; SSL/BIO, key/cert/ASN.1/provider/store/RAND/error and other public imported families; typed owned/borrowed objects, callbacks and global hooks. |
| zlib | Entire public stream/config/callback/file surface, exact counters and options; rename whole-string helpers and prove file/reader/writer paths do not buffer whole payloads. |
| curl/nghttp2 | Every public option/info/multi/share/callback and HTTP/2 API; true request/response flow, pause/readiness, option domains, handle ownership and owner-loop integration. |
| libssh2 | Every C89 session/channel/auth/agent/known-host/SCP/SFTP operation and constant, socket/resource ownership, nonblocking progress and PTY flow; retain existing Vectis SSH workflows. |
| XML | Entire public SAX/reader/writer/DOM/schema/XPath and imported families; distinguish incremental parsing/writing from materialized DOM/results and preserve native error boundaries. |
| SQLite/PostgreSQL | Full public receivers/functions, bind/value/result/callback/readiness/config interfaces and exact numeric values. Retain the stronger SQL convenience spec and its checked PG retrieval prerequisite rather than silently weakening it. |
| PDF/PNG | All document/page/font/image/config and callback APIs; explicit native buffered forms versus genuine IO; row/image ownership, protected error jumps and borrowed callback data. |
| GSSAPI/SASL | All context, credential, mechanism, property, plugin and callback APIs, exact token/flag values, owned cleanup and global/provider configuration. |
| lockdc/lql/LoneJSON | Compare the full public C surface with authoritative Lua modules, including streaming readers/writers, visitor/schema/query/state/outbox/runtime and interop/lifecycle seams. Public future APIs are not invented here. |
| CAI | Complete public client/response/source/sink/agent/MCP/session/runtime/worker surface; true producer flow for native stream adapters, explicit buffered request/result helpers and protected Lua route integration. |
| libmdf/Softline | Complete renderer/editor/options/allocation/trace/sink/source/token/document/watch APIs; explicit buffered layout boundaries, native owner thread, callback close/GC and bounded presentation. |
| audio/SUS/OPC UA | Every C89 operation, record/config/constant/callback and process-owned service seam; exact arrays/numeric values, models/media ownership, async cancellation and borrowed data lifetimes. |
| Lua runtime/remaining SDK product APIs | Inventory installed public APIs and intentional runtime capabilities; bind managed independent runtimes and supported embedding contracts safely rather than exposing unchecked arbitrary host-state pointers. |

Each detailed specification must identify actual symbols and fields, not just
repeat these family labels. Track not-yet-reviewed entries as pending. Existing
upstream tests support provider correctness but cannot replace Lua boundary
tests. Prior exclusions require implementation or an exact capability blocker,
not a rationale saying a current workflow does not need them.

## Implementation and verification sequence

1. Inventory and investigate a component's complete public contract; write and
   review its specification before implementation. Keep an overall ledger so
   small completed slices do not disguise remaining public APIs.
2. Implement all gaps across native binding, direct/convenience Lua, app/service
   lifecycle, normal/packed execution, source-rock and C SDK consumers. Maintain
   dependency archive/patch provenance for owner-side Lua adaptations.
3. During implementation, build and test only affected debug targets. Use tiny
   fixtures, deterministic allocation/IO/callback failures and explicit
   readiness/handshakes. No deliberate waits or multi-exabyte size fixtures.
4. Once the entire surface has landed, run the full build and applicable full
   native/platform/packaging verification once. Do not cross-build each change.
5. Review the complete implementation. Fix findings using focused debug tests
   only, then rerun review. Repeat until clean. Record any full-build/platform
   proof made stale by fixes; do not claim final-byte cross evidence from an
   earlier build. Do not silently repeat full cross builds inside this loop.
6. Record exact component/API mappings, test evidence, exclusions resolved and
   remaining upstream blockers against the final candidate. Commit verified
   task-owned changes; no push or release is authorized by this goal.

This full-build-then-review sequence is the user's explicit override of the
ordinary per-slice lifecycle gate. Agent semantic review remains a reasoning
gate; automated inventories and symbol checks cannot approve completeness.

## First implementation slice

The [digest specification](digest-streaming-spec.md) covers the immediate hashing
gap. Native C and normal/packed Lua debug probes exercise >1 MiB input, bounded
reader/file flow, callback errors and uint64 overflow using private test state.
This is a slice of the goal; it is not evidence that all dependency APIs have
been facaded or that final cross/release gates passed.

The [zlib component specification](zlib-public-facade-spec.md) records the
delivered streaming compression slice: public C89 engine, protected Lua
reader/writer helpers and incremental receivers. Focused C and normal/packed
Lua tests prove bounded generated producer flow, member/EOF validation, errors,
limits, counters and cleanup. The remaining zlib families, broader dependency
work and final whole-goal review are still pending.
