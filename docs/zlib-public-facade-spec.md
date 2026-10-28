# Complete zlib Lua surface and streaming contract

Status: component implementation with typed gzip formatting and final semantic
review still pending. Implemented slices include streaming, raw/header/back
state, protected Lua allocators, caller-owned transforms, native-width checksums
and readonly CRC-table capabilities; focused evidence is recorded below.
This is implementation evidence, not the final semantic review or approval of
the full zlib public surface.

## Pinned API inventory

The installed zlib 1.3.2 public header and compiler inventory identify 88
declaration/signature entries in the initial native probe. Explicitly resolve
macro aliases, large-file variants, deprecated/public compatibility functions
and platform conditions before declaring coverage complete. Required families:

- `zlibVersion`, `zlibCompileFlags`, `zError` and public constants/types.
- `deflateInit`/`deflateInit2` and ABI/version forms; deflate/end, reset/copy,
  dictionary set/get, params/tune, bound/bound_z, pending/used, prime and gzip
  header settings.
- `inflateInit`/`inflateInit2` and ABI/version forms; inflate/end, reset/reset2,
  copy, dictionary set/get, sync/sync-point, prime/mark, gzip header extraction,
  validation/undermine/codes-used and inflateBack initialization/IO/end.
- `compress`/`compress2`, `_z` counterparts, bounds, uncompress/uncompress2 and
  `_z` counterparts. These use caller-owned whole buffers, separate from
  explicitly materialized string conveniences.
- `adler32`, `crc32`, `_z` variants, combine and large-file variants, combine
  generator/operator and the public CRC table.
- gz open/dopen/buffer/params, read/write and fread/fwrite, printf/vprintf,
  puts/gets/putc/getc/ungetc, flush/seek/rewind/tell/offset and 64-bit forms,
  eof/direct/error/clear-error/close and read/write close forms.
- Public z_stream/gz_header fields and allocator/input/output callback types.
  Do not omit allocator or buffer fields solely because they need owned resources.

## Names and value domains

Rename current whole-string transforms `deflate_materialized`,
`inflate_materialized`, `compress_materialized`, `decompress_materialized`,
`gzip_materialized`, `gunzip_materialized`. Remove their ambiguous aliases and
migrate every caller/example/test/manual. A compression format being called a
stream does not make a whole-string helper a streaming Lua operation.

Expose incremental state through owned receivers and reader-to-writer helpers.
Raw native operation identifiers require documented Lua mappings; low-level
buffer operations may return an explicitly materialized chunk, while stream
helpers never aggregate the input/output. Keep separate exact native counters
and checked uint64 aggregate counters; uLong may be only 32 bits. Preserve
native signed offsets and 64-bit large-file variants without Lua float conversion.

The complete facade must expose public flags and return codes as constants.
Data/null distinction
must preserve zlib's initialization semantics for checksum functions. Required
native input lengths are validated before narrowing. Dictionary and header
byte ranges remain owned/pinned for every operation that borrows them.

Checksum bindings use `crc32_materialized(data[, seed])` and
`adler32_materialized(data[, seed])` for whole-string input, and
`crc32_update(seed, chunk)` / `adler32_update(seed, chunk)` for at most 65536
bytes. The `_z` operations provide size_t processing; the narrower native
operations are mapped through the same result semantics on valid lengths.
Expose null-input initialization separately as `crc32_initial` and
`adler32_initial`; an empty string is not replaced by NULL. Checksums and
convenience seeds are exact unsigned 32-bit integers. Native domains are
specified in the completion contract below. Combine helpers take
nonnegative exact lengths and check the native signed offset domain before
narrowing. `crc32_combine_gen` and `crc32_combine_op` preserve native polynomial
semantics. Expose applicable large-file variants with their full signed offset
domain. Version, compile flags, error strings and public constants retain native
names where appropriate. The undocumented CRC-table pointer requires its own
bounded read-only ownership contract; it is not an unchecked numeric address.

## Ownership and progress

Install the Lua cleanup owner before native stream initialization. Copying a
stream creates an independent owned receiver; ending a receiver invalidates its
aliases and must not free caller-owned buffers. Mutable input/output buffers
are validated owned/borrowed resources; immutable Lua strings cannot serve as
writable output memory. Native allocator callbacks require typed allocation
resources and protected error handling, not numeric addresses or private layouts.

Bind callbacks with retained contexts and protected calls. Reader/writer errors,
false sink results, Lua OOM, callback close and reentry never escape through an
unprotected native frame. Preserve the first error, close resources exactly once
and latch terminal failure. Yielding is not silently made asynchronous.

Reader-to-writer helpers use bounded input and output chunks. Handle short input,
output backpressure, NEED_DICT, BUF_ERROR and no-progress states accurately.
Distinguish successful END from truncated input. Gzip decoding accepts complete
concatenated members, including empty members, but rejects trailing corruption
or truncation. Partial output before a later failure is not success or rollback.

The native zlib window/header/dictionary memory is explicitly accounted for.
Do not collect the full compressed or decompressed value behind a callback API.
Streaming helpers default to checked uint64 accounting with no arbitrary total
input/output cap; optional caller-selected output limits fail before excess
delivery. Materialized helpers retain documented allocation limits. File input
and output are explicit paths; reject aliases/hardlinks/symlinks before output
truncation, detect read/write/close errors, and preserve the source on failure.

## Verification and remaining work

Delivered streaming slice: `<vectis/zlib.h>` and `src/vectis_zlib.c` provide a
Lua-agnostic C89 receiver (`process`, `result`, `close`) and exported constructor,
options initializer and free functions. The shell has explicit self, private
impl and eight reserve slots; existing public shells and ABI versions are
unchanged. Each process call borrows at most 64 KiB input/output and retains
neither buffer. Sync-flush output capacity must exceed six bytes to satisfy
native progress requirements. Checked uint64 input/output/member counts default to UINT64_MAX
output with explicit limits. Native zlib state is private to the core.

`src/vectis_lua_zlib_stream.c` uses only that public receiver. A cleanup userdata
precedes acquisition; a protected driver covers reader/writer calls, output
allocation and finite result construction. No provider C frame calls Lua.
Callback errors preserve identity. Close/GC while busy cancels without freeing
in-flight state; reentry fails. Helpers emit bounded chunks without collecting
the payload. Optional owned Lua receivers mirror the public process method.
See [the manual](../manual/lua-zlib.md) for exact names, options and progress/EOF
semantics. Dictionary-needed, corrupt/truncated input and trailing bytes fail;
concatenated gzip includes empty members and validates all members.

Focused Debug evidence is in `tests/unit/test_vectis_zlib_stream.c` and
`tests/lua/zlib_streaming.lua` (the existing normal/packed upgrade-seam harness).
They cover binary/empty/fragmented data, equivalence, generated >1 MiB producer
flow without collecting either payload, decoded output before producer EOF,
limits/counts, member errors, dictionary-needed errors, callbacks, close/GC,
reentry, and deterministic Lua OOM. Counter faults mutate private test state
only. Existing materialized/file regressions remain applicable. These are
slice tests; full/platform/package verification and the final review remain
deferred under the parent goal's sequence.

The subsequent raw/header/back-inflate/allocator and native-buffer component
records supersede the initial gap list for dictionary/reset/copy/header,
params/tune/bound/pending/prime, raw public fields, allocator callbacks and
wider checksum domains. Owned gz receivers and exact large-file offsets are
also implemented. Typed gz printf/vprintf, final semantic inventory reconciliation
and whole-goal verification remain unfinished; no full API completion count is
asserted. The existing source rock consumes an embedding/provided native
facade; no standalone source-rock native build is established by this slice.

Remaining full-surface verification must cover every raw state method and code
family, public constants,
binary/empty/fragmented input, concatenated gzip members, dictionaries, reset/copy,
early close, callbacks/GC/OOM, native/aggregate counter boundaries, checksums and
combine equivalence, file offsets and alias-preservation. Unknown/invalid options
must fail before acquiring native resources. Prove output before input EOF where
the native format permits it; producer retention is not inferred from chunk syntax.

Include normal/packed Lua, source-rock availability and C SDK/libvectis seams
when the convenience engine crosses Vectis ownership. Existing file-regression
tests remain, with renamed materialized helpers. Full platform builds wait until
the whole facade implementation lands, then follow the required review/debug-fix
cycle. The inventory and this specification do not claim all 88 entries bound.

## Owned gzip FILE receiver contract (reviewed before implementation)

Reviewed against `.cache/deps/host-debug/include/zlib.h` (1.3.2), gzip file
section and large-file declarations. `<vectis/gz.h>` owns a native gzFile behind
a C89 shell with explicit self methods, private impl, eight reserve slots and
matching free functions. Caller buffers are borrowed, capped at 65536 bytes;
element products are checked before IO. Close retains the shell, is idempotent,
and invalidates the file even on failure; destroy releases the shell. Wrong
read/write close form fails without invoking the unsafe native close variant.
Native integer results, zlib error code, errno and copied message remain visible.
Native getc function and backward-compatible getc_ are used without private
layout access. Large-file declarations select *64; otherwise conversion to
native z_off_t is checked. Public offsets are exact signed int64 values.

Lua gzopen/gzopen64 and gzdopen validate strings (including NUL), integer FD and
mode before acquisition; gzdopen transfers the actual OS descriptor on native
success only, without duplication. Native gzdopen intentionally defers positive
invalid descriptor detection until IO. Cleanup userdata precedes acquisition.
Every allocating result is protected; allocation failure after native advancement
closes the owner before rethrowing. GC validates the exact userdata type.

read(capacity), write(bytes), fread(size,count), fwrite(bytes,size,count),
gets(capacity), puts(bytes), getc/getc_, putc/ungetc, buffer, setparams, flush,
seek/seek64, tell/tell64, offset/offset64, rewind, eof, direct, error, clearerr
and close/close_r/close_w preserve native meanings. Chunk requests and payloads
are at most 65536 bytes; no aggregation or staging. fread returns complete
items plus a materialized chunk including any partial final item (determined
from tell delta); fwrite returns complete items, not bytes. gets is a bounded
materialized C-string chunk: embedded NUL truncates the returned Lua string even
though native IO consumed it. puts stops at the first NUL. Use read/write for
binary data. Zero-capacity reads return empty bytes, not EOF.

Read returns bytes plus an optional structured error when native partial output
coexists with an error; zero-byte clean EOF returns nil, zero-byte failure returns
nil/error. Truncated input is Z_BUF_ERROR even when gzread returns bytes or zero;
corruption and IO failure are never EOF. clearerr clears native EOF/error and
permits resumption; no permanent EOF latch is added. Native transparent plain
reads, concatenated members and ignored trailing garbage are preserved (unlike
the strict incremental decoder). seek offsets are uncompressed; offset is the
underlying compressed position; SEEK_END is unsupported and writer seek is
forward-only. Buffer must precede IO, including direct detection. Native 'N'/'e'
mode flags may change the transferred descriptor flags as documented upstream.

printf/vprintf remain pending a typed native vararg contract. Raw z_stream,
reset/copy/dictionary/header/allocator/inflateBack/CRC-table and wider checksum
contracts remain pending; this receiver is not full zlib completion.

Focused receiver verification: `cmake --preset debug` and
`cmake --build --preset debug --target vectis_bin vectis_unit_gz -j2` passed.
`ctest --preset debug -j1 --stop-on-failure -R
'^(vectis_unit_gz|vectis_lua_gz)$'` passed both tests (including normal/Linux
packed Lua). The focused Valgrind probe exits 99 with one uninitialized-value
branch in installed zlib 1.3.2 `gztell64`, originating in native `gz_open` malloc.
It reports 501 allocations/frees and zero live bytes; no leaks. The pure public
provider reproducer `vectis_unit_gz --native-tell` (gzopen, gzwrite, gztell,
gzclose; no receiver calls) independently produces that same error, with nine
allocations/frees and zero live bytes. Logs are in ignored
`build/gz-valgrind-origins.log` and `build/gz-native-valgrind.log`.
Writer-offset memory correctness remains an unresolved upstream requirement;
no private-layout initialization, hidden seek or Valgrind suppression is used.
The header/API compiles under the strict project C89 flags; there is no C89
availability blocker. This provider defect blocks clean memory/release evidence,
not declaration coverage. Parent investigation/fix is required before approval.

## Raw receiver foundation and state controls

The reviewed supplemental contract is in zlib-raw-stream-spec.md. Public C89
`vectis/zlib_raw.h` owns native state, exposes a public z_stream capability,
borrowed bounded process, copy, native end and close. Native status codes remain
separate from Vectis argument/lifetime errors. Buffer overlap and active
close/copy reentry are checked; borrowed IO pointers are cleared before return.
Native callback allocation failure and copy failure preserve the source owner.

The Lua raw surface maps all eight basic/extended/version-size constructors,
bounded materialized process chunks, independent copy/end, native scalar fields,
reset/keep/reset2, dictionary set/get and tuning/pending/used/prime/sync/mark/
validation/codes-used controls. Wrong-kind native operations fail rather than
silently selecting the other algorithm. A protected driver contains output-copy
OOM; fields use a guarded protected frame so GC cannot close the native owner
while result allocation reads it. Private state/reserved are excluded by the
public header's ownership rules, not a C-only availability exception.

Focused C and normal/packed Lua behavior, plus four deterministic VM OOM cases,
pass in 0.29 seconds. Separate native/fault Memcheck runs have zero live bytes
and zero errors. C89 builds use the pinned compiler and no SDK/cache changes.
Raw public IO/header/allocator/parameter/bounds/back-inflate, wider checksums,
CRC-table and typed gz formatting remain part of the full scope. Full platform/
package/consumer/final-review gates remain deferred under the whole goal.

## Gzip headers and native reset lifetime

Public C89 gzip-header receivers map every gz_header field and retain independent
byte leases. Native stream attachments/copies share the public header record,
with reference counts independent of shell lifetime. Lua constructors and field
getters are protected; constructor rejection/OOM releases temporary leases,
views adopt native spans before further Lua allocation, and close cannot free
an active getter. Typed read destinations are mutable; writer C strings require
an actual terminator and binary extra bytes remain length-aware.

Normal/packed and direct C tests prove fields, nullable/empty distinctions,
truncation, CRC, copy after parent close, early rejection and exact cleanup.
Inflate reset/keep/reset2 discard capture; deflate reset retains it in pinned
1.3.2 despite the public header's contrary default-header statement. The pure
provider reproducer and upstream report are independent of the adapter. No
private-state inspection or speculative early release is used. At this earlier
slice, raw Lua allocator/back-inflate, CRC/checksum and gz formatting remained;
the following sections supersede allocator/back-inflate status.

## Bounded back-inflate slice

The back-inflate supplemental contract is implemented through public C89 native
init/back/end, typed retained windows and independent buffer write-lock shells.
Lua callback input/output remains bounded and protected, unused input is retained
rather than referencing a stack/string temporary, checked counts and native status
are observed, and source/sink failures preserve error identity after unwind.
Native/C and normal/packed Lua lock/lifetime/EOF/unused/error/OOM checks pass with
clean Memcheck. At this slice custom Lua allocator integration was still required;
the next section supersedes that status. Checksum/CRC-table/gz formatting remains
in scope; this does not approve the full public surface.

## Protected Lua allocator slice

The supplemental raw-stream allocator contract is implemented: constructor
records capture paired callbacks and opaque, allocation consumes checked typed
buffers, and free observers see expiring readonly scopes before physical cleanup.
Copies root the shared allocator context independently; callback activity blocks
sibling reentry/close. Native destruction on GC keeps observers rooted without a
permanent registry cycle. Protected callback frames preserve the first Lua error
after native unwind, including false, and successful error reporting releases
the saved error graph while surviving owners remain usable.

Focused normal/packed Lua and deterministic VM OOM tests cover construction,
copy failure/recovery, cleanup failure, creator-coroutine collection and back
inflation. Wider native checksum domains, CRC-table and typed gzip formatting
remain implementation work. Full platform/package/consumer verification and
the final whole-goal review remain deferred.

## Native buffer and checksum completion contract

Reviewed against the pinned public 1.3.2 header before implementation. Expose
`compress`, `compress2`, `uncompress`, `uncompress2` and all `_z` variants as
caller-buffer operations. Lua arguments are `(destination,source[,level])`;
pass typed buffers/spans to select capacities, with nil source representing
NULL/zero length. Return native code and destination length, plus consumed
source length only for uncompress2. No result bytes are materialized or copied;
these are whole-buffer operations, never described as streaming. Leases are
temporary and protected against Lua setup/result OOM; reject readonly output,
locked output, closed resources and overlapping nonempty ranges before writing.
Ordinary forms check uLong capacities; `_z` forms preserve size_t. Native error
codes and partial output lengths remain observable. C uses the same public
typed-span transform helper and returns native results separately from Vectis
argument/lifetime failures.

Expose `crc32`, `adler32` and their `_z` variants as
`(seed,buffer_or_span_or_nil[,length])`. Preserve uLong seed/results exactly
(integer or unsigned decimal), distinguish NULL from non-NULL empty input,
and check uInt versus size_t lengths without payload copying. Direct combine,
combine64 and operator names preserve uLong checksum/result domains and the
selected signed native offset width. Lengths must be nonnegative per the useful
public checksum contract. The zero operator retains the native 1.3.2 zero
result. Nonzero operators with no low 32-bit polynomial bits violate the native
multiplication termination precondition and are rejected; wider operators with
valid low bits retain native behavior. Generated operators are nonzero uint32.
The existing explicitly materialized/update convenience helpers retain their
documented uint32 checksum policy. New C raw helpers expose the native widths.

`get_crc_table()` returns an owned readonly table capability, never a numeric
address. C receiver size/entry/native/close and Lua size/entry/native/close map
the immutable provider-owned values without cloning them. Indices are 0..255,
with checked exact values and independent shell lifetimes. The provider table
extent/lifetime is established by the pinned
[upstream public accessor implementation](https://github.com/madler/zlib/blob/v1.3.2/crc32.c)
returning its static 256-entry byte table. No private header or state layout is
used. Reconsider this bound when upgrading that accessor implementation.

Fast C and normal/packed tests cover every variant, partial writes, truncation,
trailing input consumption, binary values, NULL/empty, width rejection, overlap,
locks, parent close, table lifetime/bounds and native-reference results. Protected
lease cleanup gets deterministic Lua allocation-failure proof. Full-platform
and final whole-goal checks remain deferred.
