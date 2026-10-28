# Useful dependency Lua specification review

Review date: 2026-10-28. Baseline: `b2aeacb` plus the preserved pending migration
recorded under `build/useful-lua-facades/pre-task.patch`. Review scope: the
specification, selected installed public headers and imported-target contracts.
This is not implementation or release verification.

## Findings and disposition

1. Exhaustive C parity would repeat the earlier scope error. The specification
   selects application operations and lists concrete exclusions. Existing
   upstream Lua facades, SSH and dependency upgrades are outside this goal.
2. Haru save-to-stream buffers a complete document. Corrected the PDF contract
   to file output or explicitly materialized bytes; no streaming wrapper.
3. Native codecs have incompatible framing/lifetimes. Selected standard Zstd,
   LZ4 frame and XZ streams; bounded chunks and single-frame/trailing-byte policy
   are explicit. LZO is deferred by the user's decision, not an implementation
   task. Bzip2, YAML and FFI are not inferred requirements from SDK presence.
4. Public archive targets pull LZO transitively. Checked
   `CpktArchiveFacadeConfig.cmake` and `LibArchiveConfig.cmake` in the selected
   cpktmisc SDK. Both static and shared closure include LZO. Archive binding
   linkage is blocked until an upstream no-LZO archive variant is available.
   Do not strip link dependencies, edit the SDK or expose a stub.
5. A PostgreSQL XA borrow is not a standalone connection. Kept both ownership
   domains distinct. Public checked retrieval in cpktdb 0.3.1 resolves the old
   result/drained/failure ambiguity; single-row async query supports the proposed
   cursor without whole-result materialization. A SQL error after rows must
   remain observable. No native network drainage in finalizers.
6. SQLite preparation can have effects. Reused the provider tail parser with a
   deny authorizer for trailing SQL, retained the first-statement caveat, and
   defined parameter counts, NULL, exact integer conversion and child invalidation.
7. Callback/OOM cleanup and transaction ambiguity were underspecified in a bare
   method list. Defined protected calls, preinstalled owners, latched copy failure,
   active receiver guards, secondary errors and committed/unknown outcomes.
8. PNG interlace cannot be hidden behind one-row storage. Limited the row API to
   noninterlaced input and named interlaced whole-image decoding materialized.
9. Partial curl/OpenSSL/XML bindings are not reasons to wrap every pointer API.
   Selected reusable/concurrent transfers, bounded callbacks, reusable keys,
   materialized authenticated encryption, DOM/XPath and caller-fed SAX. Kept
   existing helpers and excluded global managers/plugins and duplicate TLS.
10. Packaging and process placement need observable evidence. The completion
    contract includes preloads, normal/packed execution, embedded manuals,
    source rock, installed static/shared consumers and fork ownership.

## Public-operation feasibility

| Component | Selected pinned public operations |
| --- | --- |
| SQLite | `cpkt_sqlite_open`, `prepare`, statement bind/step/reset/column/finalize, authorizer, autocommit, exact changes/rowid and error APIs from `cpkt/sqlite.h`. |
| PostgreSQL | `connect_start_params`, `connect_poll`, async parameterized/prepared send, single-row mode, flush/consume/is_busy, checked result retrieval, status/metadata/free from `cpkt/postgres.h`. |
| PDF | `cpkt_pdf_new/free`, page/font/image operations, metadata, save_to_file and explicitly buffered save/read-stream from `cpkt/pdf.h`. |
| PNG | Installed public libpng read/write rows, transforms, text/physical metadata, interlace and native error handling. C contains native setjmp boundaries. |
| Compression | `cpkt_zstd_ZSTD_compressStream2/decompressStream`; `cpkt_lz4_LZ4F_compressBegin/Update/End/decompress`; `cpkt_lzma_lzma_easy_encoder/stream_decoder/code/end`. Public packages supply static/shared facade targets. |
| Archive | Public `cpkt_archive_archive_*` callback/entry APIs support flow, but the target's LZO closure blocks deployment under the user's deferral. |
| Curl | Installed public easy/multi/poll/completion and typed read/write/progress callback APIs; no foreign thread enters Lua. |
| OpenSSL | Public EVP key, digest-sign/verify and AES-GCM control/update/final APIs; output withheld until authentication succeeds. |
| XML | Installed public document/node/XPath and push SAX APIs; context-specific configuration, no global loader replacement. |

## Decision

The independent components are sufficiently specified to implement. Archive is
an explicit upstream dependency blocker and is not counted as implemented.
No implementation proof or full-surface approval is claimed by this review.
Component implementation reviews must still check exact error/ownership behavior,
and record any newly discovered public-contract blocker before substituting a
different behavior.

## Curl scope clarification, 2026-10-28

The initial phrase "existing documented options" included helper-specific MIME,
SMTP, schema and retry policies beyond ordinary transfer ownership. The user
explicitly selected ordinary receivers with existing transport/TLS/proxy and
read/write/progress callbacks, retaining those families on existing helpers.
The specification now states that boundary and the concrete session/multi,
buffering, callback, completion-copy and Vectis HTTP ownership contracts.
