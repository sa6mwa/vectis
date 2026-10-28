# Public API inventory and binding work ledger

Status: historical inventory; exhaustive facade coverage is no longer required.
New bindings target useful public APIs of dependencies without upstream Lua
facades, with concrete Vectis or bundled application use cases. Existing upstream Lua
facades are authoritative: pslog, LQL and MDF are consumed unchanged. Their
withdrawn local additions are preserved in per-project handoffs under ignored
`stash/upstream-lua-handoffs-2026-10-28/`. See the current
[boundary and convenience gap assessment](upstream-lua-facades.md).
Counts below are
compiler-derived declaration/signature entries, not implemented Lua API counts
or a completeness approval. Macro APIs, conditional declarations, public value
fields, callback contracts and families outside the initial include probe still
need explicit mapping. No missing operation is considered covered by a module
preload or an unimplemented callable stub.

## Historical inventory baseline and evidence

### Current reconciliation, 2026-10-28

The checkout pins cpkt 0.6.0, cpktdb 0.3.1, cpktmisc 0.2.1,
liblockdc/core and PostgreSQL driver 0.23.0, MDF 0.16.0 and Softline 0.13.0.
The older declaration counts below remain inventory assistance only.

PostgreSQL checked-result retrieval is available and used by the implemented
`vectis.pg` managed XA borrow adapter; it is no longer an upstream blocker.
Standalone PostgreSQL connections/cursors and SQLite standalone/app-owned
receivers are implemented under the
[useful-facade scope](useful-dependency-lua-spec.md), with scoped evidence in
[the current progress record](useful-dependency-lua-progress.md).
App-owned collections, XA and
receipt dispatch are implemented components, not evidence of full lockdc API
coverage. Typed gzip printf/vprintf is historical candidate work; it needs a
concrete application use case before inclusion. The pslog C-parity contract is withdrawn; its omitted native operations
are not outstanding Vectis implementation requirements.

The retained cpkt 0.6.0 native test log records 265/265 passing cases. Separate
sanitizer logs record 88 fast and 138 later passing cases; these scoped runs do
not establish a passing aggregate instrumentation or full-surface gate. Native
SDK/source-rock evidence predating 0.6.0 does not verify final candidate bytes.
No full API, cross-platform or release approval follows from those results.

SSH remote execution is indefinitely deferred to the future roadmap; existing
SSH workflows and public dependency bindings remain distinct concerns. The
libssh2 memory finding awaits an upstream cpkt release and is not under further
investigation here.

This declaration inventory predates the active split-bundle upgrade. Current
versions and semantic evidence are in `lua-upgrade-seam-audit.md` and
`cpkt-bundle-upgrade-spec.md`; these historical counts do not cover the newly
added collections, retention, PostgreSQL XA, MDF or Softline surfaces.

The baseline is `e3687ae` plus the active uncommitted facade work. Resolved native
dependencies are c.pkt.systems 0.11.0, lockdc 0.18.0, LoneJSON 0.45.0, CAI 0.6.0,
liblql 0.3.0, libpslog 0.12.0, libmdf 0.14.0 and Softline 0.9.0. Native syntax
inspection used installed public headers and the selected Bootlin x86_64 GNU
sysroot, with host Clang as an analysis tool. It did not build a cross artifact
or select a host runtime for the product. Scratch inventories live under
ignored `build/lua-api-audit/`; tracked records contain logical library identity
and source references, not workstation paths or addresses.

The initial aggregate probe includes public libvectis headers and the C89 SDK
imports for OpenSSL, SQLite, PostgreSQL, PDF, GSSAPI, SASL/plugin, libssh2,
nghttp2, MQTT, OPC UA, audio, SUS and Lua/runtime. It also includes public curl,
PNG, zlib, selected XML families, lockdc/LQL/LoneJSON/pslog/MDF/Softline and CAI
headers. The OpenSSL-only probe intersects declarations with the released
SSL/crypto exports and includes the C89 adapter namespace. The aggregate probe
uses prefixes for initial grouping. These techniques can miss aliases/macros
and do not decide pointer ownership; they are inventory assistance only.

| Initial group | Declaration/signature entries | Binding investigation |
| --- | ---: | --- |
| libvectis | 603 | Receiver/config/callback/free-function mapping pending beyond existing workflows. |
| OpenSSL native/import plus C89 adapters | 6593 historical entries | Digest/HMAC helpers, reusable private/public/certificate keys and materialized authenticated AES-GCM are implemented. Unused provider/BIO/SSL object graphs are outside the current useful-surface scope. |
| zlib | 88 | Materialized and bounded file helpers; public C89 streaming engine, callback transforms and bounded incremental Lua receivers implemented. Owned gzip FILEs, raw init/copy/end/control operations, retained typed IO, parameter output, native bounds, gzip-header fields and reset/copy lifetimes, bounded back-inflate and independent window locks are tested. Captured Lua allocators, invoking-coroutine/GC teardown and deterministic callback OOM are implemented. All eight caller-buffer transforms, native-width checksum/combine/operator forms and readonly CRC-table receiver mappings have focused C/normal/packed and memory evidence. Typed gz formatting and final semantic/platform/package review remain pending; no full-surface approval. |
| XML selected families | 970 | Mapped parse/write/serialize plus owned DOM editing, element/scalar XPath and incremental SAX are delivered. Unused schema/XSLT and other node graphs are outside the useful slice. |
| SQLite C89 | 278 historical entries | Useful standalone/app-owned open, prepared binding, incremental rows, reset, exec and transactions are implemented; the current useful-surface spec supersedes exhaustive parity. |
| PostgreSQL C89 | 200 historical entries | Public checked retrieval supports the managed XA adapter and implemented standalone connection/cursor/prepared/local-transaction/app ownership. Native/normal/packed live and allocation evidence covers the useful slice; exhaustive direct API parity is superseded. |
| PDF C89 | 290 historical entries | Useful report documents, pages, text/geometry, fonts, PNG/JPEG file/memory images and file/materialized output are implemented. Typed ownership and errors have native/normal/packed and allocation-failure evidence; annotations and exhaustive parity are outside the current useful-surface spec. |
| PNG | 256 historical entries | Useful bounded row I/O and explicitly materialized Adam7 decode/RGB-RGBA-gray8 encode are implemented with metadata, ownership and callback/error evidence. Arbitrary chunks and exhaustive parity are outside current scope. |
| GSSAPI C89 | 136 | Credential/context/buffer/OID/name domains and cleanup mappings pending. |
| SASL C89 | 55 | Connection/property/random/interaction mappings pending; plugin family needs its own inventory. |
| libssh2 C89 | 138 | Existing SSH/SFTP convenience subset; raw native sessions/channels and other families pending. |
| nghttp2 C89 | 181 | Native HTTP/2 state/callback/stream interfaces pending. |
| curl | 100 | Current perform helper is a subset; multi/share/options/info callbacks need complete mapping. |
| MQTT C89 | 44 | Packet/client IO/callback APIs pending beyond current curl MQTT convenience. |
| OPC UA C89 | 307 | Broad existing Lua binding; native-pointer exclusions must be resolved and every field/callback mapped. |
| audio C89 | 12 | Existing binding/service APIs; prove complete config/callback/value domains. |
| SUS C89 | 19 | Existing binding/service APIs; prove complete configs and model/callback domains. |
| Lua/runtime C89 | 202 | Managed independent-runtime and embedding contracts pending. |
| lockdc | 272 | Existing upstream binding and workflow integration; complete symbol/value/stream mapping pending. |
| LoneJSON | 234 | Existing authoritative binding and overlay; full domain/visitor/schema/IO mapping pending. |
| LQL | Historical C declaration inventory | Released 0.3.0 Lua facade is consumed unchanged. Local expansions were archived and removed; deliberate C omissions are not Lua defects. Upstream callback-OOM reports and any concrete workflow request belong in the per-project handoff. |
| CAI selected headers | 346 | Broad upstream binding; source/sink/MCP/agent/session/worker/tools mappings pending. |
| libpslog | Historical C declaration inventory | Released 0.12.0 facade is consumed unchanged, including its official C interop. Vectis logging uses existing Lua constructors and `with`; `withf` and the local native mirrors are withdrawn, not feature gaps. The owner/adoption OOM report remains an upstream issue. |
| libmdf | Historical C declaration inventory | Released 0.16.0 facade is consumed unchanged. Vectis terminal convenience methods use upstream `render` and `render_stream`; local allocator/font/pager/explicit-sink expansions and renames were archived and removed. Additional failure/workspace guarantees require concrete upstream contracts. |
| Softline | 64 | Existing upstream binding; C-only callback/terminal/watch/line domains need full mapping. |

OpenLDAP, iODBC, native backend headers and other supplied application libraries
remain part of the overall scope and are not silently excluded by this initial
table. Additional headers and public constants must be enumerated before any
full-coverage conclusion.

## Binding contract records

Every entry needs its exact public C identifier or macro/value path, Lua path,
input/output domain, owned/borrowed lifetime, callback affinity, EOF/error behavior,
flow classification, app integration where applicable and named behavior tests.
Public receiver fields and their free-function counterparts may share a single
implementation, but both usage contracts need an explicit mapping. Public
configuration fields are not optional coverage merely because no new function
was added to the dependency.

Primitive/value wrappers must represent signed and unsigned 64-bit values
exactly. Buffers are typed owned/borrowed resources, not unchecked numeric
addresses. Variable-output results and mutable pointers need capacity and owner
validation before a call. Retained aliases require independent validity tokens.
Function pointers and callbacks require typed protected thunks with explicit
retention and thread affinity; caller-supplied addresses are not a substitute.

The compiler found variadic examples including BIO_printf/BIO_snprintf,
ERR_set_error, EVP_PKEY_Q_keygen, OSSL_STORE_ctrl, curl option/info setters,
cpkt Lua formatting/GC, MQTT pack helpers and SQLite formatting/logging. Known
command/option dispatch can use typed adapters. Truly dynamic argument lists and
native va_list/function-pointer lifetimes require a reviewed portable call
contract; do not guess types from Lua values or build unsafe platform-specific
stack manipulation. The current inventory does not establish those bindings.

## Landed debug work, not full completion

- Digest/HMAC receiver and explicitly materialized SHA/EVP/HMAC helpers; bounded reader
  and file hashing; exact 64-bit accounting and normal/packed failure probes.
  See [digest contract](digest-streaming-spec.md).
- All 17 typed Pouch app settings, copied selected strings, native shared-writer
  and encryption precedence, typed-over-flat workflow normalization and updated
  examples. See [typed Pouch contract](pouch-typed-settings-spec.md).

These slices remain part of an uncommitted broader implementation. Source-rock,
static/shared SDK and full public-API gates are not approved by their narrow
debug results. Implementation uses focused debug builds/tests only. The full
build waits for the entire surface, followed by the user's review/debug-fix/
review cycle until clean, as specified in the
[complete facade contract](lua-public-api-completeness-spec.md).
