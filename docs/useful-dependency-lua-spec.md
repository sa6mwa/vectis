# Useful dependency Lua facades

Status: reviewed for implementation, 2026-10-28; archive linkage is blocked as
described below. Review evidence is in `useful-dependency-lua-review.md`.
This is the implementation
authority for the new goal; it supersedes exhaustive parity requirements in
`lua-public-api-completeness-spec.md` and conflicting historical sequencing in
`sql-database-facades-spec.md`. It does not claim delivered functionality.

## Outcome and boundaries

Applications running ordinary or Linux-packed Vectis can use the useful database,
document, image, archive, compression, transfer, crypto and XML operations below
without writing C. Implement the complete specified scope, then verify it as a
product. Availability of a header or constructor alone is not completion.

Pinned inputs are cpkt 0.6.0, cpktdb 0.3.1 and cpktmisc 0.2.1, as identified by
`scripts/dependency-pins.tsv` and the selected installed package manifests.
Inspect these public headers and package targets, not an adjacent checkout.
No dependency upgrades, upstream patches or upstream messages are authorized by
this specification. Existing first-class upstream Lua facades remain unchanged.
In particular pslog, LQL and MDF await new upstream releases. SSH work stays parked.

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
accept existing documented options plus bounded Lua reader/writer callbacks and
progress cancellation. Callbacks remain on the owner's invoking state; multi
advancement is caller-driven, with poll/readiness and copied completion results.
Do not introduce worker threads or silently register on Kore's event loop.

Streaming callbacks do not populate `result.body`; buffered mode retains its
documented ceiling. Callback source failure cannot become EOF; sink failure
aborts transfer. Reject callback reentry/closure of active handles, reset per-
transfer state, and expose native error/status on each completion. Share-handle
global caches and raw socket callbacks are excluded: easy/multi already support
the specified reuse/concurrency workflow.

## OpenSSL: data encryption and reusable keys

Keep digest/HMAC/cert/sign helpers. Add owned PEM-loaded public/private key
receivers for repeated signing/verification and key metadata. Add explicitly
materialized AES-GCM encrypt/decrypt with binary key, nonce, AAD and separate tag.
Authentication failure returns an error without returning unauthenticated
plaintext. Require explicit nonce input; no implicit key/nonce persistence.
Bounded plaintext/ciphertext sizes and exact algorithm/key/tag constraints are
documented and tested against known vectors. Use public EVP APIs.

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

During implementation run focused debug checks serially. After all specified
surfaces exist, run standard format/tests, affected memory/service tests, Linux
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
