# Useful dependency Lua facades

Status: reviewed for implementation, 2026-10-28; archive linkage is blocked as
described below. Review evidence is in `useful-dependency-lua-review.md`.
This is the implementation
authority for the new goal; it supersedes exhaustive parity requirements in
`lua-public-api-completeness-spec.md` and conflicting historical sequencing in
`sql-database-facades-spec.md`. Implementation status is recorded separately in `useful-dependency-lua-progress.md`.

## Outcome and boundaries

Applications running ordinary or Linux-packed Vectis can use the useful database,
document, image, archive, compression, transfer, crypto and XML operations below
without writing C. Implement the complete specified scope, then verify it as a
product. Availability of a header or constructor alone is not completion.

Pinned inputs are cpkt 0.6.0, cpktdb 0.3.1 and cpktmisc 0.2.1, as identified by
`scripts/dependency-pins.tsv` and the selected installed package manifests.
Inspect these public headers and package targets, not an adjacent checkout.
The user additionally authorized the liblql 0.3.0 → 0.4.0 upgrade; its released
C and Lua facades are consumed unchanged. The upgrade specification and seam
review are in `liblql-0.4.0-upgrade.md`. No other dependency upgrade, upstream
patch or upstream message is authorized by this specification. Existing upstream
Lua facades remain authoritative. SSH work stays parked.

The existing Vectis convenience APIs and managed PostgreSQL XA adapter remain
supported. New direct bindings do not change their policy or ownership.
No exhaustive translation of native declarations, C pointer escape hatches,
provider plugin machinery or unused global controls is required.

## Common execution contract

Native engines live in separate language-independent libvectis components with
public C headers, receiver shells and matching exported functions. Lua cores bind
these components; small Lua modules provide language conveniences. Do not expand
`vectis_cli.c` into another implementation container. Binding registration and
resources may remain there. Use installed public C89 adapters where supplied;
PNG, curl, OpenSSL and XML may use their installed supported public headers.
Never use SDK private objects, unchecked numeric addresses or guessed struct layouts.

Receivers are caller-serialized and confined to the creating process/thread.
No constructor starts a thread. Open files/connections explicitly after the
applicable fork boundary. Reject inherited live receivers; do not reconnect them.
Standalone resources are not implicitly app-owned. App-owned database declarations
are separately defined below and materialize lazily after app startup/fork.

Lua resources are full userdata with idempotent `close`, `__close` and `__gc`.
Install an owner before acquisition. Parent closure invalidates children;
borrowed child views cannot outlive parent state. C shells retain their parent
state until closed; C close frees a shell and tolerates NULL. Deferred teardown
protects owners used by active callbacks. Callback close/reentry on the active
receiver is an error; independent receivers remain usable.

Expected execution failures return `nil, error`; EOF returns `nil, nil`. Errors
carry Vectis status, source, operation and native code/message, plus SQLSTATE or
secondary cleanup detail where applicable. Invalid argument types/options raise
before side effects where possible. Never truncate, silently coerce out-of-range
integers, convert execution failure into EOF, or automatically log credentials/data.
Preserve exact signed Lua integers, explicit binary lengths and SQL NULL.

Lua callbacks execute on their invoking state under protected calls. Native
callbacks retain/copy any borrowed chunk required across the call. Never let a
Lua longjmp cross a provider stack. After callback failure, clean native state
before propagating the original error object. Allocation failures must not lose
native ownership; failures copying a row/entry latch that iteration as failed.
Closing or GC never invokes arbitrary Lua callbacks or waits on the network.

Streaming consumes and emits bounded chunks on demand. Default transport/codec
chunks are at most 64 KiB; image rows and SQL rows are bounded separately. No
whole-message collection, temporary-file staging or serialize-once readback may
be named streaming. Explicitly materialized APIs may allocate a complete result
within configured limits. Limits are checked for overflow before narrowing.

Default materialized byte limit is 16 MiB, SQL limit 1 MiB, parameter count 256,
parameter aggregate/cell/row limit 16 MiB and column limit 1024. Image dimensions
and row bytes are validated before allocation with configurable limits. These
bound facade allocations, not all work or memory inside a provider.

## SQLite: embedded local application data

Public C: `vectis/sql.h`, `vectis/sqlite.h`. Lua: `vectis.sql`, `vectis.sqlite`.

- `sqlite.open(path, opts)`: mode `readwrite_create` (default), `readwrite` or
  `readonly`; URI opt-in; busy timeout default zero. No extension loading,
  implicit WAL changes or global callback changes.
- `db:prepare(sql)`, `stmt:bind(params)`, `stmt:next()`, `stmt:columns()`,
  `stmt:reset()`, `stmt:close()` provide reusable prepared statements.
- `db:query(sql, params)` produces an owned incremental cursor;
  `db:exec(sql, params)` rejects row results and returns changes/last-rowid.
- `db:transaction(fn, opts)` supports DEFERRED, IMMEDIATE and EXCLUSIVE.
- `db:transaction_diagnostic()` preserves outcome and secondary rollback failure
  details, including after catching a thrown callback error.
- `sql.NULL`/`sqlite.NULL` distinguish NULL from empty text/blob;
  `sqlite.blob(bytes)` explicitly selects a BLOB. Rows are dense arrays;
  column metadata is ordered, preserving duplicate names and storage types.

All convenience execution uses prepared statements and an explicit dense
parameter array, including `{}` for no parameters. Reject missing/extra bindings,
NUL SQL, empty statements and trailing executable SQL before stepping. Use the
provider tail parser with a deny authorizer during tail preparation; restore it
on every exit. Preparation can have native first-statement effects; this is not
a SQL sandbox. Preserve INTEGER values through exact signed 64-bit conversions.
Bindings copy borrowed input. Statement reset clears all bindings. Cursor EOF
releases its native statement and remains EOF; prepared statements require reset.
Parent close finalizes native children and leaves retained child shells invalid.

Use only `cpkt/sqlite.h`. Capture failed-open diagnostics before closing even
when open returns a non-NULL failed receiver. Exclude virtual filesystems,
extensions, FTS tokenizer callbacks, change sessions and global allocators.

## PostgreSQL: standalone incremental SQL

Extend `vectis.pg` with independently owned `connect(opts)` and
`connect_start(opts)`; retain `from_borrow` and its materialized XA contract.
Public C additions belong in `vectis/pg.h` without changing existing shell layout.

- Keyword/value connection parameters are copied; no connection-string assembly.
  Insert `sslmode=verify-full` when absent. Document native service/passfile/env
  inheritance. A local Unix socket has its native transport behavior.
- Connect progression/readiness exposes FD and read/write interests. Blocking
  connect defaults to 5 s of polling; DNS/auth plugins may block outside that
  budget and are not claimed to have a hard deadline.
- `db:query(sql, params, opts)` uses parameterized async send, immediately selects
  single-row mode, and returns a cursor. Keep at most one row result and native
  transport buffering. `cursor:next_ready()` distinguishes row, pending and EOF;
  readiness supports an external loop. `next()` is explicitly blocking.
- `db:prepare(name, sql, opts)` and prepared execution reuse server statements.
  Result columns retain OID/format; initial results are text strings/NULL so
  bigint, numeric and timestamps are not rounded. Binary parameters retain
  explicit OIDs. No implicit binary-result codec is promised.
- `db:exec` returns command tag and exact decimal affected count;
  `db:transaction(fn, opts)` owns a local transaction.

One operation owns a connection. Use checked retrieval and require terminal
success plus drainage before EOF/reuse. SQL errors remain latched even after
earlier rows. Nonblocking flush/consume paths honor read/write pressure and
EINTR; active I/O budget defaults to 30 s, excluding time spent in application
code. An abandoned cursor, deadline or transport/protocol failure closes its
owned connection without network drainage in GC. Unsupported COPY/pipelines are
errors, not successful EOF. Exclude replication, large objects, COPY and plugins.

PostgreSQL interface detail: connection options are `{parameters={keyword=value},
connect_timeout_ms=5000,io_timeout_ms=30000}` with string keyword/value pairs.
Native environment/service/passfile inheritance is retained; missing sslmode
becomes verify-full. `connect_start` returns an owned pending connection;
`db:poll()` advances it and returns `true` when ready or `false` while pending.
`db:readiness()` returns `{fd,read,write}` for the next external wait.
`connect` drives the same state with blocking polling. Per-query `timeout_ms`
overrides the active blocking I/O budget; zero performs an immediate probe.
Nonblocking readiness consumers own their external wait/deadline policy; time
in application code is not charged to the blocking wait budget.

`db:query(sql,params,opts)` / `stmt:query(params,opts)` return cursors.
`next_ready()` returns `row,'row'`, `nil,'pending'`, `nil,nil` at checked EOF,
or `nil,error`; `next()` waits for row/EOF/error. Rows are dense text/NULL arrays,
with `cursor:columns()` available after the first row or terminal result and
returning `{name,oid,format}` records. `db:exec` drives command completion and
returns `{command,affected}` with affected as the exact native decimal string.
`db:prepare(name,sql,{parameter_types={oid,...},timeout_ms=...})` acknowledges
server preparation before returning a reusable statement view. Closing the
view releases its client shell; the caller-named server statement belongs to
the connection until explicit SQL DEALLOCATE or connection close. GC performs
no hidden network deallocation. Existing `pg.binary(bytes,oid)` remains valid;
integers bind as exact decimal text and SQL NULL shares the existing identity.

Local transactions accept `isolation='read_committed'|'repeatable_read'|
'serializable'` and `read_only=false`. Transaction-control statements, including
previously prepared ones, are rejected inside the owned callback. Active escaped
cursors invalidate their connection rather than causing network drainage during
cleanup; transaction outcome is unknown if rollback cannot be acknowledged.
`app:pg({name=...,parameters=...,connect_timeout_ms=...,io_timeout_ms=...})`
captures the same copied configuration without opening a connection before start.

## Transaction and app database ownership

Protect callbacks and check begin, callback, commit and rollback separately.
Successful return commits; `nil, error` requests rollback; thrown errors rollback
then rethrow the original object. Return success plus callback values. Reject
nesting, manual changes to helper-owned transaction state and escaped active
cursors. Preserve the primary error if cleanup also fails. Outcomes distinguish
not-started, committed, rolled-back and unknown; lost commit acknowledgement is
never reported as a confirmed rollback. The existing XA coordinator remains
upstream-owned and is not duplicated.

`app:sqlite(opts)` and `app:pg(opts)` capture copied named declarations before
startup. Each using process opens lazily after startup/fork. Stop/restart closes
materialized handles and invalidates children; declarations rematerialize after
restart. App close detaches declarations. Active transaction callbacks forbid app
lifecycle changes, including caught attempts. No live connection crosses fork.

## PDF: reports and document output

Public C: `vectis/pdf.h`; Lua: `vectis.pdf`. `pdf.new(opts)` owns a document;
`doc:add_page(opts)`, page text/font/position/color/line/rectangle/image operations,
standard fonts, file-loaded TrueType fonts, PNG/JPEG image loading and document
title/author/subject metadata cover ordinary reports. Child fonts/images/pages
retain their owning document and reject cross-document use.

`doc:save(path)` writes an explicitly selected file. `doc:bytes_materialized(opts)`
returns bounded complete PDF bytes. The pinned public Haru API buffers save-to-
stream internally: no streaming PDF API is specified. Do not conceal this with
chunked readback or a spool. Validate document errors/status after each operation;
Haru callbacks record native diagnostics and never enter Lua. Exclude forms,
JavaScript, encryption, embedded files and custom font internals.

PDF interface detail: `doc:font(name,{encoding=...})` selects a standard font;
`doc:load_font(path,{encoding=...,embed=true})` loads TrueType;
`doc:load_image(path,{format="png"|"jpeg"})` returns image dimensions and an owned
view. The document retains all native objects; closing a view releases only its
shell. Native errors latch until `doc:reset_error()` or close. Reset acknowledges
the native error and does not roll back partial page output. Text defaults to
Helvetica, 12 points and black; shape options select RGB color, line width
(including native hairline zero), fill/stroke. Native float geometry precision
and selected font encoding apply. Materialized output defaults to a 16 MiB
facade allocation limit, configurable per call; native Haru buffers remain
outside that limit. Positive font/PNG/JPEG checks use original tiny fixtures.

`doc:load_image_materialized(bytes,{format=...})` accepts bounded complete
PNG/JPEG bytes through the public memory loaders. This lets generated PNG data
feed a PDF without a staging file. Input is borrowed only during the call;
the document owns the resulting image. The 16 MiB encoded-input ceiling is
distinct from native decoder memory. Memory-loader lifetime is tested by
discarding input before PDF serialization.

## PNG: image interchange

Public C: `vectis/png.h`; Lua: `vectis.png`. Reader/writer receivers support files
and protected bounded read/write callbacks. `reader:info()` exposes dimensions,
original color/depth, selected text and physical-resolution metadata.
`reader:next_row()` transforms ordinary noninterlaced input to RGBA8 and returns
one bounded row. `writer:write_row(bytes)` accepts configured RGB8/RGBA8/gray8;
`finish()` checks row count and native completion. Native PNG setjmp/error state
stays wholly inside C; no longjmp crosses Lua callbacks.

`png.decode_materialized(bytes, opts)` / `encode_materialized(image, opts)` are
explicit whole-image alternatives, including interlaced input. Row receivers
reject interlaced input rather than secretly collecting the image. Preserve
alpha, validate strides/dimensions and report truncated/corrupt input. No palette
editing, arbitrary chunk callbacks or ICC color-management engine is specified.

PNG interface detail: `png.reader(path_or_read_callback,opts)` and
`png.writer(path_or_write_callback,image,opts)` construct owned receivers.
Callbacks use `read(capacity) -> bytes | nil[,error]` (1..capacity bytes) and
`write(bytes) -> true | nil,error`. A reader's `next_row` returns RGBA8 bytes
or `nil,nil` after checked completion. `finish` verifies all expected rows;
close never completes a partially written image or invokes a user callback.
Writer image fields are width/height, format (`rgba8`, `rgb8`, `gray8`),
optional text map and physical `{x,y,unit='meter'|'unknown'}`. Reader info includes
original depth/color type/interlace and RGBA8 row size, available text and physical
metadata. Limits are `max_width`, `max_height`, `max_row_bytes`; materialized
operations also accept `max_bytes` (16 MiB default) for complete pixels/output.
Metadata is bounded to 16 text entries/64 KiB aggregate; unknown chunks are not
retained. The native bounded ancillary-chunk cache may omit oversized optional
chunks; selected text/resolution metadata is exposed when available. Materialized
decode returns the image fields plus packed RGBA8 pixels;
encode accepts packed pixels matching the declared format, with no padding.

## Archive: bundles and import/export

Blocked dependency: the selected public `cpkt::archive` target imports
`cpkt::libarchive_static`, whose declared closure includes
`cpkt::lzo_native_static`; the shared equivalent also requires LZO. The user
explicitly deferred LZO. Do not link an archive binding or ship an unverified
stub. This component awaits an upstream cpktmisc variant with a public archive
target and implementation that do not require LZO. Other components proceed.

Public C: `vectis/archive.h`; Lua: `vectis.archive`. Readers accept explicit paths
or bounded callbacks; iterate copied entry metadata and read entry bytes in
bounded chunks. Each entry is valid until advancement/close; reject stale entry
use. Writers support explicit tar/pax and zip formats, selectable built-in
filters, entry metadata and incremental entry data. Explicit finish reports
trailer/close failures; GC frees without calling user sinks.

No implicit disk extraction, path creation, external filter process or global
lookup callback is included. Callers decide where entry data goes. Reject
filters needing external programs before starting. Exercise multi-entry archives,
empty entries, metadata, truncated input, source/sink failure and early close.

## Compression: interchangeable data formats

Public C: `vectis/compression.h`; Lua: `vectis.compression` with algorithm names
`zstd`, `lz4` and `xz`.

Zstandard, LZ4 frame and XZ use incremental compressor/decompressor receivers:
`new(algorithm, direction, opts)`, `process(input, sink)`, `finish(sink)`, `close()`.
Provide bounded reader-to-writer helpers and explicitly named materialized
conveniences. Compression levels and decompression memory/window limits pass
through public contracts; reject unknown/inapplicable options. LZ4 means the
standard frame format, XZ means the standard XZ container, not invented framing.
Finish distinguishes end-of-stream, pending output and truncated input; document
and test concatenated/trailing-data policy. Flush is not implied by every chunk.

Existing zlib bindings stay intact. Native vararg gzip formatting adds no needed
workflow and remains excluded. LZO is deferred by the user's explicit decision
on 2026-10-28; SDK inclusion alone is not an application requirement. No LZO
binding, linkage or invented framing is part of this goal. Bzip2/libyaml/FFI and
codec internals are not added merely because inspected SDK headers expose them.

## Curl: reusable transfers and concurrent requests

Keep `curl.perform/json/stream_json` behavior. Add owned easy/session handles for
connection reuse and a multi receiver for concurrent ordinary transfers. Methods
accept existing transport/TLS/proxy options plus bounded Lua reader/writer callbacks and
progress cancellation. Callbacks remain on the owner's invoking state; multi
advancement is caller-driven, with poll/readiness and copied completion results.
Do not introduce worker threads or silently register on Kore's event loop.

Streaming callbacks do not populate `result.body`; buffered mode retains its
documented ceiling. Callback source failure cannot become EOF; sink failure
aborts transfer. Reject callback reentry/closure of active handles, reset per-
transfer state, and expose native error/status on each completion. Share-handle
global caches and raw socket callbacks are excluded: easy/multi already support
the specified reuse/concurrency workflow.

The user confirmed on 2026-10-28 that these receivers target ordinary transfers.
MIME, SMTP, schema mapping and automatic retry remain on existing helpers;
unsupported receiver options fail explicitly before transfer initiation. This
supersedes the initial ambiguous phrase "existing documented options". New
receivers keep buffered/file body sources, bounded callbacks, protocol/method,
headers and transport/TLS/proxy options. No implicit retry or callback replay is
added. Each request captures its options/headers; settings reset before reuse,
while the native connection cache survives until close. Responses retain the
8 MiB buffered body and 64 KiB header ceilings; callback output has no cumulative
body ceiling and does not publish `result.body`.

`curl.session()` exposes perform/reset/close. `curl.multi()` exposes
add/step/poll/completions/close. Add returns a transfer ID; step returns running
count; poll accepts 0..60000 ms and returns readiness count (zero when queued
completions need draining). Multi admits at most 64 combined active/queued
requests. `completions(limit)` copies up to limit results (default 64, 1..64),
then acknowledges that selected batch. A small limit permits bounded draining
within the Lua memory budget;
failed Lua copies leave every completion queued for retry. Individual failures
remain per transfer. Callback error objects survive in result.callback_error
and result.error; configuration failures return nil,error. Progress receives
{download_total,downloaded,upload_total,uploaded}. Callbacks cannot yield.
File sinks report close failures. Handles and callbacks belong to their creating
process/thread and calling state; no GC/close user callbacks or network drain.
`vectis.http.session(defaults)` owns a native session, applies copied defaults,
merges headers and normalizes HTTP/transport status for request/get/post/put/
patch/delete/head/options, with explicit reset/close. Callers construct it after
their process/fork boundary and own its lifetime. Existing stateless HTTP/JSON,
MIME, SMTP and schema workflows remain available independently.

## OpenSSL: data encryption and reusable keys

Keep digest/HMAC/cert/sign helpers. Add owned PEM-loaded public/private key
receivers for repeated signing/verification and key metadata. Add explicitly
materialized AES-GCM encrypt/decrypt with binary key, nonce, AAD and separate tag.
Authentication failure returns an error without returning unauthenticated
plaintext. Require explicit nonce input; no implicit key/nonce persistence.
Bounded plaintext/ciphertext sizes and exact algorithm/key/tag constraints are
documented and tested against known vectors. Use public EVP APIs.

OpenSSL interface detail: `openssl.load_key(opts)` takes exactly one of the
existing private/public key or certificate `_pem`/`_path` source fields, plus
optional `passphrase`. It returns an owned process/thread-confined key with
`sign(data,opts)`, `verify(data,signature,opts)`, `info()` and `close()`.
Digest-based signatures keep the existing `algorithm`/`digest` options and
SHA-256 default; unsupported key/digest combinations report native errors.
Info reports native type/bits/signature-size and whether a private key was
loaded. File/PEM input is materialized and bounded to 16 MiB; password callbacks
copy explicit bytes in C and never prompt or enter Lua. Key generation and
provider management remain outside the facade.

`openssl.aes_gcm_encrypt_materialized({key,nonce,data,aad=...})` returns
`{ciphertext,tag}`; `aes_gcm_decrypt_materialized({key,nonce,ciphertext,tag,aad=...})`
returns authenticated bytes or `nil,error`. Keys are exactly 16/24/32 bytes,
nonce 12 bytes and tag 16 bytes. Data/ciphertext and AAD each have a 16 MiB
ceiling. No output is published on authentication failure. Existing digest,
certificate/sign and random helpers retain their interfaces.

No generic cipher enum mirror, provider lifecycle manager, BIO object graph or
second TLS stack is specified. Existing curl/app TLS handles those workflows.
Encryption results are materialized deliberately, particularly to withhold
unauthenticated plaintext. This is not a streaming encryption promise.

## XML: document queries and incremental input

Keep XML/LoneJSON mapping and bounded output. Add owned documents, selected
node attribute/text/child editing, XPath queries with explicit namespace maps,
and explicitly materialized serialization. Query node views retain document
ownership; mutation invalidates affected views predictably instead of exposing
borrowed native pointers. XPath scalar and ordered node-set results are distinct.

Add incremental SAX-style parsing with caller-fed chunks and protected start,
end and text callbacks; copy callback payloads and preserve namespace/attribute
values. Do not build a DOM behind this API. External entity loading is disabled;
no XInclude, XSLT, schema compiler, global loader or arbitrary native callbacks.

XML interface detail: `xml.document(bytes,{max_bytes=16777216,max_xpath_nodes=4096})`
owns a materialized DOM. XPath returns number/boolean/string scalars or ordered
arrays of element views; non-element node sets are outside this selected view
domain (use scalar functions or element attribute/text accessors). Namespace
bindings are an explicit prefix-to-URI map. `node:name()` returns local name,
namespace URI and prefix. `node:attribute(name,namespace_uri)` returns a string
or nil for absence. `set_attribute(name,value,{namespace=...,prefix=...})`
requires a prefix for namespaced attributes and rejects conflicting bindings.
`append_child(name,{namespace=...,prefix=...,text=...})` returns a new child view;
omitting namespace inherits the parent element's namespace. Text is literal,
not entity markup. Root removal is rejected. Each mutation invalidates all prior
element views, including its receiver; reacquire through root/XPath.

`xml.sax(callbacks,{max_metadata_bytes=16777216})` exposes feed/finish/close.
Each feed is <=65536 bytes; no cumulative stream-size ceiling or DOM collection
is added. Start/end callbacks receive a namespace record `{uri,prefix}` when
applicable; start records also retain namespace declarations. Attributes are
arrays of `{name,value,namespace,prefix}` records, preserving duplicate local
names across namespaces. A start event has at most 256 attributes/declarations
and bounded aggregate copied metadata. Text events are <=65536-byte UTF-8 chunks.
Callback exceptions and explicit `nil,error` fail the parser; other returns
are ignored and never imply pause. Finish verifies completeness and is idempotent.
DTDs/entity declarations are deliberately excluded: ordinary data interchange
needs predefined entities, not retained declaration state. Reconsider when a
concrete document workflow needs those declarations or non-element XPath views.
Use context-specific public resource/error handlers; never replace global loaders.

## Verification and completion

For every component, supply native public-API tests and Lua normal/packed tests
of successful output, invalid input, native errors, limits, early close,
parent-child lifetime, GC/allocation failures, callback error/reentry where used,
and process ownership. Use tiny deterministic fixtures and zero-wait contention
probes. Add examples using real work rather than constructor-presence assertions.
Database tests cover exact integers, NULL/empty/binary distinctions, duplicate
columns, reset/rebind, rollback and transaction outcome. PostgreSQL uses the
existing rootless Podman Kube fixture for normal/packed SQL and transport faults.

Spec review must trace every promised workflow to pinned public operations,
close ownership/error/streaming ambiguities, identify genuine upstream blockers,
and record fixes before implementation. Required contracts blocked by upstream
remain explicit blockers, not stubs or hidden buffers.

The user clarified the execution order: implement the entire agreed surface,
integration, documentation and examples in one pass before starting verification.
Do not interleave component implementation with test/build cycles. After that
implementation pass, run standard format/tests, affected memory/service tests, Linux
packed execution, source-rock checks, static/shared installed consumers and
dependency/link closure checks. Update the manual, embedded manual, examples,
coverage matrix and current semantic audit. Review the resulting implementation,
fix actionable findings with focused tests and repeat until clean. Cross-target
verification follows the project's promised targets; report actual results and
any environmental limitation without claiming a release. No push/tag/release.

Commit only verified coherent task-owned changes on the existing non-default
branch. Preserve/classify the pre-existing migration edits; the initial status
and patch are saved under ignored `build/useful-lua-facades/`. Record component
progress and final evidence separately from this specification.

## Fixed interface choices from spec review

New modules are available through `require` and the matching `vectis` root
member. SQL modules share one per-state `NULL` sentinel. Native and Lua names
preserve existing conventions without duplicate aliases solely for C parity.

- SQL options reject unknown keys. SQLite options are `mode`, `uri` and
  `busy_timeout_ms`; query/prepared rows are dense arrays. Columns are arrays
  of `{name, declared_type}` records, with per-row values preserving their
  actual types. `exec` returns `{changes, last_insert_rowid}`. `query` creates
  a statement, binds once and finalizes at EOF. A reusable statement must be
  bound, even to `{}`, before stepping; after EOF it stays EOF until reset.
  Errors include `operation`, `dependency_code` and copied `message`.
- SQLite C exposes borrowed column/value views until next/reset/close;
  Lua copies them inside a protected operation. A copy failure invalidates the
  cursor rather than skipping the lost row. C parameter arrays have explicit
  type, length, signed int64 and double fields. No runtime types enter C headers.
- Compression direction is `compress`/`decompress`. Default levels are native
  defaults; accepted level ranges come from pinned codec APIs. XZ memory limit
  defaults to 64 MiB, Zstandard maximum window to 64 MiB. LZ4 uses 64 KiB blocks.
  Decoders accept exactly one frame/container and reject trailing bytes or a
  second frame; applications wanting concatenation create a new receiver at an
  explicit boundary. `finish` is idempotent after success, errors are latched.
  `level` omission is distinct from XZ level zero. LZ4 exposes 0 through its
  native high-compression maximum; negative acceleration selection is outside
  this useful slice. Lua `stream(algorithm,direction,reader,sink,opts)` connects
  bounded callbacks; `materialized(algorithm,direction,bytes,opts)` explicitly
  collects output under `max_bytes` (default 16 MiB). The public C engine has
  process/finish sink callbacks, invoked only after native codec calls return.
- PDF page methods are `text(x,y,text,opts)`, `line(x1,y1,x2,y2,opts)`,
  `rectangle(x,y,width,height,opts)` and `image(image,x,y,width,height)`.
  Page sizes default to A4 portrait; explicit positive width/height are points.
  Text options select an owned font, positive size and RGB color. Strings use
  the selected Haru font encoding; no Unicode shaping promise is invented.
- PNG row order is top to bottom. Defaults cap dimensions to 16384 and row
  bytes to 65536, with explicit configurable ceilings. Materialized images use
  `{width,height,format,pixels}` and packed rows without padding. Metadata is
  copied; arbitrary unknown chunks are not retained. `finish` reports incomplete
  rows and writer callbacks are never invoked from GC.
- Curl callbacks use `read(capacity) -> bytes | nil[,error]`,
  `write(bytes) -> true | nil,error` and `progress(record) -> true | false`.
  Callback exceptions retain identity. Only write callback delivery avoids the
  existing body buffer; headers retain their documented bounded capture.
  A session exposes `perform`, `reset`, `close`; multi exposes `add`, `step`,
  `poll`, `completions`, `close`. Transfer IDs correlate completion with requests.
- AES-GCM accepts only 128/192/256-bit keys, a 12-byte nonce and 16-byte tag.
  Encryption returns `{ciphertext,tag}`; decryption returns authenticated bytes.
  Reusable key methods are `sign`, `verify`, `info`, `close`; existing signing
  option semantics apply. Key generation is outside this slice.
- XML documents expose `root`, `xpath`, `serialize_materialized`, `close`.
  Nodes expose `name`, `text`, `attribute`, `set_text`, `set_attribute`,
  `append_child`, `remove`. A document generation changes after every mutation;
  previously acquired views become invalid, including XPath result node views.
  Reacquire a view to continue. SAX uses `feed`/`finish`/`close` with
  `start_element(name,attrs,namespace)`, `end_element(name,namespace)` and
  `text(bytes)`. No event callback return value is treated as an implicit pause.
