# Useful dependency facade implementation review

Working candidate: specification `9fb3265`, preserved prerequisite `cf0d818`,
and the useful facade changes in the commit containing this review. The original
264 dirty paths were classified and committed separately without replacing their
bytes with the later feature changes. This is implementation evidence, not a
release approval. Current final verification is recorded below.
The user subsequently authorized LQL 0.4.0; its released facade remains unchanged.
Other upstream Lua facades are unchanged; SSH/LZO remain deferred. The LQL
upgrade review is in `liblql-0.4.0-upgrade.md`.

## Semantic workflow review

- SQLite: typed values retain NULL/text/blob/exact integer distinctions. Prepared
  statements bind/reset/step explicitly; tail authorization does not replace the
  authorizer per prepare. Protected row copies cannot silently skip a lost row.
  Transactions preserve primary/cleanup/outcome and reject caught manual control,
  lifecycle changes, nested transactions and escaped active cursors. Copied app
  declarations open after startup/fork and invalidate on stop/restart/close.
- PostgreSQL: independent connections preserve the existing XA domain and shared
  NULL identity. Checked single-row retrieval requires terminal success and
  checked drainage before EOF/reuse. The first SQLSTATE survives caught later
  errors. Lost COMMIT acknowledgement is unknown; acknowledged deferred failure
  is rolled back. Cursor abandonment closes without draining; inherited live
  connections never send Terminate on the parent's session. App ownership is
  lazy, copied and restart-controlled, without silent reconnect after unknown.
- PDF/PNG: native documents own fonts/images/pages; view close does not remove
  native content. PNG encoded bytes pass directly into PDF without staging and
  remain valid after input release. PDF serialization is explicitly materialized.
  PNG rows remain bounded; Adam7 is explicitly materialized. Metadata, failed
  source/sink calls, finish completeness, ownership and process/thread guards
  have native and normal/packed evidence with original tiny fixtures.
- Compression: Zstd/LZ4-frame/XZ use public incremental codecs and bounded output
  callbacks outside provider stacks. Finish verifies one frame, rejects truncated,
  concatenated/trailing input and latches failure. Level zero, window/memory
  ceilings and source/sink errors have behavior evidence. Materialization is named
  and bounded; close never calls a sink.
- OpenSSL: PEM/file key ownership is independent of inputs. Password hooks copy
  explicit bytes in C without prompting/Lua. Repeated sign/verify uses fresh
  contexts. AES-GCM publishes no plaintext before authentication and clears failed
  output. Known vectors, all key widths, old/new helper interoperability, native
  process/thread guards and Lua allocation failures pass.
- XML: documents own element views; every mutation invalidates prior generations
  before freed nodes could be reused. XPath returns copied scalars or ordered
  element views. Literal text edits validate XML UTF-8; namespace bindings reject
  conflicts/reserved misuse. SAX uses a custom non-DOM handler, per-context
  resource/error handling and protected copied events before EOF. It retains no
  full document and accepts cumulative input beyond the materialized ceiling.
  DTDs and unused node/schema/XSLT graphs are explicit exclusions. Namespace
  registration/sorting uses documented installed public XPath APIs.
- Curl: sessions reset settings while retaining native connection reuse. Multi
  advancement stays caller-driven; the pair-barrier HTTP fixture proves concurrent
  requests. Source failure cannot become EOF; writer delivery omits body storage.
  Options/headers are copied, and native/returned callback failures remain per
  transfer. Completion acknowledgement follows successful copying of a bounded
  selected batch; failed copies preserve queued results. Vectis HTTP sessions own
  defaults, lifetime and status normalization independently of direct bindings.
  MIME/SMTP/schema/retry stay on existing helpers by the user's scope decision.

## Findings and fixes

1. XML's new config initially collided with the existing mapped-XML config.
   Renamed it document_config; both public interfaces and exports are distinct.
   Replaced deprecated character validation with the public current macro.
2. XML namespace mutations now reject reserved bindings and literal invalid XML;
   ordered XPath pointers are refreshed after sorting. UTF-8 SAX chunks do not
   split a multibyte character. Native/normal/packed and 88 Lua fault positions
   pass; strict VM memory checks report zero bytes/zero errors.
3. Caught curl reentry initially contaminated other transfers in the same native
   step. Cleared the per-callback violation after latching its owning transfer;
   the mixed failed/healthy transfer regression passes.
4. Libcurl may invoke progress/header callbacks while cleaning up. The
   [public cleanup contract](https://curl.se/libcurl/c/curl_easy_cleanup.html)
   requires explicit quiescence here. Engine callbacks now stop before removal/
   cleanup and are detached before context release. Native fault injection invokes
   cleanup-time hooks and proves no source/sink/progress callback is entered.
5. Lua finalization may visit request holders before their owning handle.
   Attached holders defer file/header release to the owner after native teardown.
   Deterministic child-first finalization plus file-close/copy fault tests prove
   subsequent transfer correctness, retained errors and idempotent cleanup.
6. Completion-copy/file-close failures must survive retry. The whole selected
   batch remains queued until copied; close failure is retained in its request.
   Sixteen injected copy failures recover every result. Optional 1..64 batch size
   permits draining buffered responses within the caller's Lua memory budget.
7. A failing C configure hook need not supply a diagnostic. The engine now
   supplies a status-matching failure when absent; a native regression proves it.
8. The redirect test initially permitted a redirect before source consumption.
   Its origin now drains a large upload before redirecting, proving one-shot
   rewind failure without hidden collection. Native bounded buffering is not
   mistaken for a facade replay promise.
9. Release optimization diagnosed a PNG setjmp argument lifetime. Post-setjmp
   reads now use the receiver's copied limits; debug PNG/PDF regression passes
   4/4 and native Release compilation succeeds without suppressing the warning.
10. Installed/pkg-config probes previously could pass without linking new engine
    objects, and --static alone could select libvectis.so. Consumers now execute
    all new engines; the pkg-config probe explicitly selects libvectis.a. The
    fast-test build target also depends on the new native/fault/fixture targets.

## Verification checkpoint

The first full debug build passed. The full test run executed 275 tests in
485.90 s: 273 passed initially, including both PostgreSQL live suites, upstream
byte/source checks, metrics performance and normal/packed workflows. The new
redirect test was corrected; its rerun passes. The existing proxy curl-loop
fixture had a connection-reset failure and passes its focused rerun unchanged.
The affected cleanup selection passes 4/4 in 8.38 s; the final curl batch/error
selection passes 5/5 in 2.64 s. No broad repeated test/platform runs are used for
these focused fixes.

Final curl VM memory checks before the batch/default-diagnostic adjustment cover
132 allocation failures plus 16 recovered copy failures with forced file-close
failure and child-first finalization: zero bytes/zero errors across 84317
allocations/frees. Native cleanup/fork memory checks also report zero bytes/
zero errors across 3644 parent allocations/frees. Earlier component evidence and
bounded provider-reachable allocations are recorded in the progress document.
These are historical checkpoint results. The final artifacts, manual, source-rock,
installed/platform and memory checks are recorded below.

Archive remains an explicit upstream blocker: current public archive targets
link LZO, which the user deferred. No archive stub, SDK patch or link stripping
was introduced. Existing first-class Lua facades remain authoritative. Released LQL 0.4.0 is
consumed unchanged; its remaining persistent-OOM cleanup limit is recorded in the
upgrade review and the ignored upstream handoff. SSH remains parked.


## Final implementation pass

The selected executable surfaces, native/Lua registration, root exports, installed
headers and exports, source-rock modules, manuals and examples are implemented.
The 0.4.0 follow-up uses only released upstream APIs. Its example now performs
actual bounded-reader filtering with scoped client/selector cleanup. Fast-test
build dependencies include both new LQL test executables. The matrix and manual
identify 0.4.0 and distinguish its known closer-entry limitation from native
memory cleanup. No archive binding is shipped under the explicit LZO deferral.

The user stopped the premature platform matrix and required a single completed
implementation pass before testing. Six Linux Release builds had passed at that
point, including both ARMhf variants; Darwin was interrupted and is unverified.
Those results may be reused for unchanged executable inputs. Verification resumes
only after this complete implementation pass, with no repeated full matrix for
small review fixes. This record is not release approval.

## Final verification

The selected surface is implemented and verified. Current inputs are the exact
pins in `scripts/dependency-pins.tsv`: cpkt 0.6.0, cpktdb 0.3.1, cpktmisc 0.2.1,
liblql 0.4.0 and the otherwise unchanged provider releases. Released upstream Lua
facades are consumed directly; no local provider extension is reinstated.

- `cmake --build build/debug -j 4` passed. The final serial CTest run passed
  **279/279 in 481.22 seconds**, including normal/packed workflows, both rootless
  PostgreSQL service suites, native callback/ownership/cleanup and Lua allocation
  fault tests, source-byte checks and the updated LQL example.
- Static/shared libraries and the CLI compile/link on all seven targets. Linux
  GNU/musl x86_64, aarch64 and ARMhf execute the LQL behavior test, a combined
  SQLite/codecs/PNG/PDF/XML/AES-GCM/curl/PostgreSQL-error smoke and the updated
  pipeline example. ARM runs use QEMU. The native LQL worker probe passes with
  default and small stacks on all six Linux targets, including musl. Darwin is
  compilation/link evidence only; no macOS runtime is available here.
- A test-only correction makes the small-stack request at least the platform's
  supported minimum: aarch64 GNU rejects 64 KiB and requires 128 KiB. The affected
  two debug tests and Linux worker probes pass after this correction. Product
  code and the six completed Linux builds were not rebuilt for it.
- A fresh no-Kore build passes its runtime contract and combined facade behavior.
  Source-rock helper smoke passes with its documented dependency stubs; actual
  native behavior is established separately by the CLI/native tests. Debug and
  Release embedded manuals match current source bytes.
- GNU installed and relocated-package consumers pass in static and shared modes:
  C89/C++ headers, CMake and pkg-config linking, actual new-engine calls and LQL
  0.4.0 version/selector calls, plus runtime contracts for 36 executables per
  mode. The pkg-config static probe selects the archive explicitly. ELF and
  Darwin shared export allowlists pass. The development GNU archive is under
  `build/useful-lua-facades/dist/`; it is not a tagged release.
- Final curl Lua memory checking covers 132 allocation failures and 16 recovered
  completion-copy failures: 84317 allocations/frees, zero bytes at exit and zero
  errors. Native cleanup/fork checks show zero lost allocations/invalid accesses
  in parent and child; the parent frees all 3644 allocations. The child retains
  inherited reachable state until `_exit`, as required by its fork-refusal test.
  No suppression is used. Initial manual invocations used a relative file URL
  and then incorrectly treated inherited reachable child memory as lost; the
  corrected invocations match the declared fixture/ownership contracts.
- Component memory evidence remains in `useful-dependency-lua-progress.md`.
  PostgreSQL's 4235 bytes/37 reachable provider globals and XML's 41 bytes/two
  reachable diagnostics are distinguished from lost allocations. LQL's four-case
  allocation sweep frees all 82703 allocations with zero errors; its remaining
  persistent-OOM closer-entry limit is prepared for upstream in the local draft
  handoff, not patched or represented as repaired. Nothing was sent upstream.

Logs use `build/useful-lua-facades/final-*` and
`build/useful-lua-facades/lql-0.4.0/`. Formatting, shell syntax, fixture formatting
and final whitespace/readback checks complete the commit gate. Opt-in external
AI/SSH/audio/device workflows are not claimed as live coverage by this goal.

Final review: no unresolved Vectis implementation defect was found in the agreed
surface. Archive/LZO and SSH are deliberate deferrals; the remaining LQL callback
limit is an upstream contract limitation. No push, release, ABI change or local
upstream facade patch is part of this completion.
