# Lua zlib Facade

The embedded Vectis runtime preloads `require("zlib")` as a
dependency-native facade over the bundled zlib dependency.

## Entry Points

- `zlib.version()` returns the linked zlib version string.
- `zlib.zlibVersion()` is the same function; `zlib.zlibCompileFlags()` returns
  native build flags, and `zlib.zError(code)` returns the native error string.
  Public `Z_*` flush/status/compression/strategy/data constants and `ZLIB_*`
  version constants retain their native names. These flags describe native
  capabilities; the convenience receiver accepts its documented flush modes.
- `zlib.deflate_materialized(data[, opts])` compresses a string as a zlib-wrapped stream.
- `zlib.inflate_materialized(data[, opts])` decompresses a zlib-wrapped stream.
- `zlib.compress_materialized(data[, opts])` aliases `deflate_materialized`.
- `zlib.decompress_materialized(data[, opts])` auto-detects zlib or gzip input.
- `zlib.gzip_materialized(data[, opts])` compresses a string as gzip.
- `zlib.gunzip_materialized(data[, opts])` decompresses gzip input.
- `zlib.deflate_file(opts)` compresses `input_path` to `output_path` as zlib.
- `zlib.inflate_file(opts)` decompresses zlib `input_path` to `output_path`.
- `zlib.compress_file(opts)` aliases `deflate_file`.
- `zlib.decompress_file(opts)` auto-detects zlib or gzip file input.
- `zlib.gzip_file(opts)` compresses `input_path` to `output_path` as gzip.
- `zlib.gunzip_file(opts)` decompresses gzip `input_path` to `output_path`.

The string APIs are explicitly buffered. They materialize the full input and
output as Lua strings. The file APIs are file-backed and process data through
bounded chunks; they do not materialize the full payload in Lua.

## Streaming helpers

`deflate_stream`, `compress_stream` (alias), `gzip_stream`, `inflate_stream`,
`gunzip_stream`, and `decompress_stream` take `(reader, writer[, opts])`.
Compression produces zlib or gzip; decoding accepts zlib, gzip, or auto-detects
both, respectively. They process input and output incrementally with at most
65536 bytes per chunk, plus native zlib state. They retain no complete payload,
staging file, worker, or asynchronous operation.

`reader(capacity)` returns a nonempty binary string no larger than `capacity`,
or `nil` at EOF. An empty string is a nonprogress error. A second non-nil result
is a callback error. `writer(chunk)` must return `true`; false/nil is a sink
failure, and a second non-nil result is a callback error. Thrown and returned
callback errors propagate with their original Lua identity. Callbacks must not
yield. Chunks reach the writer before EOF when the codec emits output; set
`sync_flush = true` to flush compression after each reader chunk.

Success returns `{ok=true, finished=true, input_bytes=..., output_bytes=...,
members=...}`. Counts are exact Lua integers within the signed range, otherwise
`{high=uint32, low=uint32}`. Input/output totals and member counts are checked
uint64 values independent of native `uLong`. Streaming defaults to `UINT64_MAX`
output, with no arbitrary 64 MiB total cap. An explicit `max_output_bytes` (or
`max_output`) accepts a nonnegative Lua integer or the exact high/low form and
fails before excess output is delivered. A failed chunk may be withheld entirely.
Compression accepts `level = -1..9` and boolean `sync_flush`. Unknown options,
two simultaneous output-limit aliases, and compression options on decoders are
rejected before acquiring native state. Runtime failures return `nil, err` in
the structured shape described below. Earlier sink output remains visible when
a later member or callback fails; these helpers provide no rollback.

`deflate_stream_new`, `gzip_stream_new`, `inflate_stream_new`,
`gunzip_stream_new`, and `decompress_stream_new` take the same options except
`sync_flush` selection. They return an owned incremental receiver:

- `receiver:process(chunk[, flush])` returns one bounded output string,
  the number of input bytes consumed, and a `finished` boolean. `chunk` may be
  empty and must be at most `STREAM_CHUNK_BYTES` (65536). Resubmit unconsumed
  input; if output fills a chunk, drain using empty input. Flush is `"none"`
  (default), `"sync"` (compression only), or `"finish"` (declares EOF).
  Repeat finish with the unconsumed suffix until finished; then repeated empty
  finish produces no more output. No new input is allowed after declaring EOF.
- `receiver:result()` returns the finite count/status metadata; `ok` remains
  false until finished. Member completion alone does not establish input EOF.
- `receiver:close()` is idempotent. `__close` and GC release abandoned state.
  Failed processing closes the receiver. Close/GC during helper callbacks
  cancels the operation and defers native destruction until the active call
  exits. Reentry is rejected.

The reusable C89 engine is public in `<vectis/zlib.h>` through
`vectis_zlib_options_init`, `vectis_zlib_new`, `vectis_zlib_process`,
`vectis_zlib_get_result`, and `vectis_zlib_close`. Its receiver has explicit
self methods, private implementation storage and eight reserve slots. The core
has no Lua dependency; the Lua binding uses only these public functions.

Gzip decoding processes every concatenated member, including empty members.
The output limit applies to their combined decoded size. Invalid or truncated
later members fail the entire transform.

File input and output must identify distinct files. The helpers reject the same
file through identical paths, hard links, or symbolic links before truncating
output, and preserve the input even when `remove_output_on_failure` is true.

## Materialized and file options

- `level`: compression level for the materialized/file compression helpers; default
  `-1`, valid range `-1..9`.
- `max_output_bytes` or `max_output`: decompression and compression output
  safety cap; default `67108864`.
- `input_path` or `path`: file input for `*_file()` helpers.
- `output_path` or `to`: file output for `*_file()` helpers.
- `remove_output_on_failure`: defaults true for `*_file()` helpers.

Expected zlib failures return `nil, err`, where `err.status`,
`err.status_string`, and `err.message` follow the Vectis structured error
shape. Programmer misuse, such as a non-table options value or invalid level,
raises a Lua error.

## Supported and pending surface

`crc32_materialized(data[, seed])` and `adler32_materialized(data[, seed])`
checksum a whole binary string. Initial seeds are zero and one, respectively.
`crc32_update(seed, chunk)` and `adler32_update(seed, chunk)` consume at most
65536 bytes; callers retain only the returned exact uint32 checksum.
`crc32_initial()` and `adler32_initial()` use native NULL initialization;
an empty string instead preserves a supplied seed. Seeds for materialized/update
helpers must be uint32 Lua integers.
`crc32_combine(first, second, second_length)` and
`adler32_combine(...)` combine independently computed checksums. The `*_combine64`
names select the corresponding native offset form. These direct helpers accept
full native uLong checksum arguments as exact integers, unsigned decimal strings
or high/low words. Length accepts the same exact representations within the
selected nonnegative signed offset domain.
`crc32_combine_gen(length)` / `crc32_combine_gen64(length)` produce a reusable
polynomial operator; `crc32_combine_op(first, second, operator)` applies it.
The convenience C checksum is `vectis_zlib_checksum`; native combine/operator
equivalents are `vectis_zlib_raw_checksum_combine`, `vectis_zlib_raw_crc_gen`
and `vectis_zlib_raw_crc_op`. The earlier uint32 C convenience combine/operator
helpers keep their narrower checksum policy.

Supported here: version, explicitly materialized and bounded file transforms,
reader/writer streaming transforms, checksum helpers, constants and the incremental receivers above. The
streaming ownership engine is shared through public libvectis C APIs. There
are no ambiguous whole-string `deflate`, `inflate`, `gzip`, or other old aliases.

The remaining zlib implementation work includes typed gzprintf/gzvprintf varargs
and the final semantic surface review.
The high-level strict decoder fails on dictionary-needed input; raw receivers
provide explicit dictionary negotiation as documented below. This slice does not approve the full public
API goal. The source rock still requires an embedding binary or separately
provided native facade; it does not compile this binding by itself.

## Example

```lua
local zlib = require("zlib")

local body = ("hello\n"):rep(100)
local compressed = assert(zlib.gzip_materialized(body, { level = 9 }))
local restored = assert(zlib.decompress_materialized(compressed, {
  max_output_bytes = 1024 * 1024,
}))

assert(restored == body)
```

```lua
local zlib = require("zlib")
local input = assert(io.open("site.tar", "rb"))
local output = assert(io.open("site.tar.gz", "wb"))
local ok, result, err = pcall(zlib.gzip_stream,
  function(capacity) return input:read(capacity) end,
  function(chunk)
    local written, message = output:write(chunk)
    return written ~= nil, message
  end, { level = 6 })
local input_closed, input_error = input:close()
local output_closed, output_error = output:close()
assert(input_closed, input_error)
assert(output_closed, output_error)
assert(ok, result)
assert(result, err)
```

```lua
local zlib = require("zlib")

local packed = assert(zlib.gzip_file({
  input_path = "site.tar",
  output_path = "site.tar.gz",
  level = 9,
}))
assert(packed.output_bytes > 0)

assert(zlib.gunzip_file({
  input_path = "site.tar.gz",
  output_path = "site-restored.tar",
  max_output_bytes = 1024 * 1024 * 1024,
}).ok)
```

## Owned gzip files

`zlib.gzopen(path, mode)` (`gzopen64` equivalent) and `gzdopen(fd, mode)`
return an owned userdata or `nil, {code, errno, message}`. Paths/modes reject
embedded NUL; descriptors are exact nonnegative OS integers. gzdopen transfers
that descriptor only on success, without duplication; positive invalid FDs may
fail only on IO, as upstream specifies. Native mode options, including `N` and
`e` descriptor flag changes, apply. Handles are caller serialized and blocking
unless native mode says otherwise; GC and Lua `<close>` release them.

Each IO request/payload is bounded to 65536 bytes, with no aggregation:

- `read(capacity)` returns a materialized byte chunk; clean EOF returns nil.
  Zero capacity returns empty bytes. Errors return nil/error or partial
  bytes/error; always inspect the second result. Truncation is `Z_BUF_ERROR`,
  not successful EOF. Already delivered chunks are not rolled back.
- `write(bytes)` returns native bytes accepted; `fwrite(bytes,size,count)`
  returns complete elements. `fread(size,count)` returns bytes, complete element
  count, optional error; bytes include a partial final item. Products are checked.
- `gets(capacity)` returns the native bounded C-string chunk through newline or
  capacity minus one, or nil at EOF. Embedded NUL truncates the Lua result but
  still advances native IO; `puts(bytes)` writes only before the first NUL.
  Use read/write for binary data. `getc()`/`getc_()` return a byte or -1;
  `putc(integer)` applies native unsigned-byte conversion; `ungetc(integer)`
  preserves native pushback, including the -1 pending-seek behavior.
- `buffer(size)` must precede IO or `direct()` detection; late calls return -1.
  `setparams(level,strategy)`, `flush(native_flush_code)`, `rewind()`,
  `eof()`, `direct()`, `clearerr()` preserve native results. `error()` returns
  copied code, message, errno. clearerr permits reading newly appended input.
- `seek(signed_offset,whence)`, `tell()`, `offset()` and their `64` method
  aliases use exact signed Lua integers. Seek/tell count uncompressed bytes;
  offset counts underlying compressed bytes. SEEK_END is unsupported; writer
  seek is forward-only. Relative overflow is rejected before native arithmetic.
- `close()`, `close_r()`, `close_w()` return native zlib status and optional
  error. Wrong form returns Z_STREAM_ERROR without destroying the owner. Actual
  close always invalidates the native file, even on failure; repeated close
  returns its recorded status. Retained userdata safely rejects further IO.

Native transparent plain-file reading, concatenated members and ignored trailing
garbage are preserved. The strict incremental gzip decoder has different
trailing-data policy. Allocating Lua results runs in a protected driver; OOM
following advancement closes the receiver before rethrowing so retry cannot
silently skip consumed bytes. Native provider frames never invoke Lua.

The public Lua-agnostic C89 counterpart is `<vectis/gz.h>`: `vectis_gz_open`,
`vectis_gz_dopen`, matching receiver methods/free functions, copied
`vectis_gz_error`, exact int64 offsets and caller-owned bounded buffers.
`vectis_gz_close(g, form, error)` retains the shell (0 automatic, 1 reader,
2 writer); `vectis_gz_destroy` frees it. `vectis_gz_get_error` copies native
error state. printf/vprintf remain pending typed native-vararg investigation;
Lua formatting is not a substitute.

Current provider limitation: focused Valgrind checks of installed zlib 1.3.2
report an uninitialized branch in writer `gztell64`, also reproduced using only
public native gzopen/gzwrite/gztell/gzclose. Functional offset checks pass, but
writer-offset memory correctness requires an upstream fix before release
approval. The receiver does not alter private provider state or suppress it.

## Raw native stream receivers

`deflateInit(level)`, `inflateInit()`, `deflateInit2(level,method,windowBits,
memLevel,strategy)` and `inflateInit2(windowBits)` create owned raw receivers.
Their underscore forms additionally take the native version string and stream
size; `Z_STREAM_SIZE` is the current native size. Failed constructors return
nil and a structured error containing zlib_status. All argument ints are checked
before narrowing; C strings reject NUL. The public C89 shell and defaults are
in `<vectis/zlib_raw.h>`.

`process_materialized(input_or_nil[,output_capacity,flush])` returns a table
with zlib_status, consumed, produced and one materialized output chunk. Input
and output capacity are at most 65536 bytes. It does not combine gzip members,
collect a whole value or latch recoverable native failures. Repeat using the
unconsumed input suffix; `Z_NEED_DICT` permits dictionary negotiation and
`Z_BUF_ERROR` permits more input/output. `deflate_chunk_materialized` and
`inflate_chunk_materialized` also take a receiver first and validate its kind.
Result-copy OOM closes an advanced owner before propagating the error.

`copy()` returns an independent native state. `end_native()` reports the native
end code and is idempotent; `close()` releases the shell. Native end may return
Z_DATA_ERROR for unfinished deflation while still releasing native state.
`native()` returns the validated receiver capability, never an address.
`fields()` reports native total_in/total_out/adler/data_type/avail_in/avail_out
and optional msg; uLong values use exact decimal strings beyond Lua's signed
range. These native counters are distinct from the checked aggregate counters
of the higher-level streaming engine. Native IO pointers are cleared after a
borrowed chunk call. Private state and reserved fields are not exposed.

The following native identifiers are available both on the module (receiver
first) and as methods: deflateReset, inflateReset, deflateResetKeep,
inflateResetKeep, inflateReset2, deflateSetDictionary, inflateSetDictionary,
deflateTune, deflatePending, deflateUsed, deflatePrime, inflatePrime, inflateSync,
inflateSyncPoint, inflateMark, inflateValidate, inflateUndermine and
inflateCodesUsed. Return codes retain their native meaning. Dictionary getters
are explicitly `deflateGetDictionary_materialized` and
`inflateGetDictionary_materialized`, returning code and a bounded dictionary.
Module deflateCopy/inflateCopy and deflateEnd/inflateEnd validate receiver kind.

All raw/back constructors accept a final optional allocator record after their
normal arguments, including version/size arguments in the underscore forms:
`{zalloc=function(items,size,opaque) ... end,
zfree=function(view,opaque) ... end, opaque=value}`. Supply both hooks or neither.
The record is captured at construction; later edits do not change the hooks.

`zalloc` returns an owned, unleased `vectis.buffer` of at least the checked
`items * size` bytes (one physical byte for a zero product), or nil/error.
Accepted buffers are consumed; rejected buffers remain caller-owned. `zfree`
receives a scoped readonly view that expires on return or error. It observes
retirement; native storage is released even if the hook fails. Copies share the
captured allocator context and retain it independently of their parent. Hooks
run on the invoking Lua thread; close/reentry through any sibling sharing that
context is rejected while a hook runs. The first Lua error is propagated after
native cleanup, preserving its identity, including false.

`fields()` includes captured zalloc/zfree/opaque. Without custom hooks, its
allocator fields are callable typed-buffer allocation/free helpers. An
opaque-only record retains its value while using native default allocation.
These mappings do not claim complete zlib coverage. The strict streaming helpers
retain their member, EOF, error and checked-counter policies.

`set_input(buffer_or_span[,offset,capacity])` and
`set_output(buffer_or_span[,offset,capacity])` retain independent public C
leases. Input is readonly and output must be mutable; default capacity is the
remaining resource size. Native capacities are checked against uInt. Closing
the original Lua buffer/span cannot free a live binding. Passing nil unbinds
it. An invalid replacement leaves the prior binding intact.

`set_avail_in(size)` / `set_avail_out(size)` change only availability at the
current native cursor, within its remaining typed storage. `fields()` also
returns next_in/next_out as owned typed views of those regions, never numeric
addresses. Close the returned views when finished. Copies retain their own IO
leases, and native end releases bindings. Native reset/keep rules retain public
pointer state; no automatic rebinding or hidden source replay is added.

`run([flush])` uses bound buffers and returns zlib_status/consumed/produced.
Output stays in the supplied resource; the call creates no materialized payload.
Read bounded output through the resource explicitly. Overlapping IO is rejected.
`process_materialized` requires retained bindings to be unbound first.

`deflateParams(level,strategy)` uses bound output and can emit bytes. Its result
has the same metrics as run. BUF_ERROR reports insufficient output space;
retry explicitly with more capacity. The change is not applied until the retry
succeeds, and pending output is not silently discarded.
`deflateBound(size)` and `deflateBound_z(size)` return the native bound; exact
decimal sizes are accepted within uLong/size_t. Z_ULONG_MAX and Z_SIZE_MAX expose
those domains. Native unsigned overflow/arithmetic behavior is preserved; these
raw queries do not impose a new output-allocation policy.

## Native gzip headers

`gzip_header(fields[,read_mode])` creates an owned header; read_mode defaults
false for writing. It maps text, time, xflags, os, extra, extra_len, extra_max,
name, name_max, comment, comm_max, hcrc and done. Byte fields accept typed
`vectis.buffer` or spans. Write leases are readonly; name/comment require a
terminating zero in their actual storage. Read destinations require mutable
leases. Omitted write extra_len and read maxima use actual resource sizes;
explicit used lengths/capacities must fit storage. Scalar fields retain native
int/uLong/uInt domains, including fields ignored by native writing. Invalid
constructors release temporary leases before reporting errors.

`deflateSetHeader(header_or_nil)` and `inflateGetHeader(header)` attach typed
headers to correctly configured raw receivers. Nil on deflate restores default
header output. Read/write mode mismatch is rejected before native attachment;
native rejection leaves the previous attachment owned. Header `close()` is
idempotent and releases its Lua ownership only. Native stream attachments and
copies retain the same shared record and independent ownership references until
replacement/discard/end, so closed parent handles cannot leave dangling data.

`header:fields()` returns scalar snapshots and typed byte spans; close returned
spans when finished. Read extra is bounded by min(extra_len,extra_max), while
extra_len preserves the actual native length. Name/comment expose their capture
capacity, including truncation without a terminating zero. Missing native
fields return nil. Present empty fields remain typed spans, keeping NULL and
empty distinct. Native pointer replacement with NULL does not lose the original
cleanup lease. xflags is chosen by native compression, hcrc is validated during
read, and done can be 0, 1 or -1 for auto-detected zlib input.

Pinned zlib 1.3.2 retains a custom header across deflateReset/ResetKeep despite
its public documentation's default-header claim. The managed wrapper preserves
that lifetime; explicitly detach with deflateSetHeader(nil) or end the stream.
Inflate reset/keep/reset2 success discards its capture and releases that stream's
reference. Header owners and sibling copies retain their own references. This
native discrepancy has a self-contained upstream report in stash; the wrapper
uses public APIs and does not inspect private compression state.

## Callback back-inflate

`inflateBackInit(windowBits,window)` creates an owned raw back-inflate receiver.
Its underscore form also accepts version and stream size. The mutable typed
window must provide at least 2^windowBits bytes, with bits 8..15. The native
receiver retains its own window lease; closing the caller's buffer/span is safe.
Only raw deflate data is accepted, without gzip/zlib framing. It uses
inflateBackEnd, and ordinary inflate/reset/copy/header operations cannot
misinterpret its native state.

`inflateBack(receiver,reader,writer)` / `receiver:back(reader,writer)` accept
functions or read/write/userdata records. The reader receives `(65536,userdata)`
and returns a nonempty string of at most that size, nil at EOF or nil/error.
One private 65536-byte input buffer is reused; no complete input is collected.
The writer receives `(chunk,userdata)`, with each chunk at most the window size,
and accepts it with nil/true/zero. Error objects retain their identity, including
false. Callback close/reentry and Lua mutation of any window alias are rejected
until the native frame returns; native window locks are private capabilities.
Locks and callbacks are released after every successful/error/OOM exit.

The result exposes zlib_status, checked uint64 supplied/consumed/output_bytes,
input_failed and unused_length. `unused` is an owned typed span of current
remaining input; close it when finished. `unused_materialized` additionally
contains those bytes when their size is at most 65536. Remaining input is retained
for the next complete raw stream; multiple back calls do not concatenate output
or hide staging. Source EOF before completion returns native Z_BUF_ERROR with
input_failed=true, rather than success. Output failure preserves the first Lua
error after native unwind. OOM after advancement closes the owner before raising.
`inflateBackEnd(receiver)` and generic end_native release native state/window;
close also releases the shell and bounded private input lease.

## Caller-owned native buffers and CRC table

`compress(destination,source)`, `compress2(destination,source,level)`,
`uncompress(destination,source)` and `uncompress2(destination,source)` operate
directly on typed `vectis.buffer` objects/spans. Every name also has an `_z`
variant. Use a span to select an input length or output capacity; nil source
means NULL/zero bytes. The ordinary forms check native uLong capacities and
the `_z` forms use size_t. Output must be mutable and unlocked. Overlapping
nonempty input/output is rejected before writing.

Return values are `(zlib_status,produced)` and, for uncompress2,
`(zlib_status,produced,consumed)`. Native failures remain codes, including
Z_BUF_ERROR for insufficient output and Z_DATA_ERROR for corrupt input. Partial
output stays in the destination; uncompress2 reports consumed input even on
failure. Leases end before return or a Lua exception, and no output string is
created. These operate on whole caller-owned buffers and are not streaming.
`compressBound(length)` / `compressBound_z(length)` return the native bound,
including native unsigned wrap arithmetic at extreme lengths.

`crc32(seed,source[,length])` / `adler32(seed,source[,length])` and their `_z`
variants borrow typed storage directly. Omitted length selects the whole view;
the ordinary form checks uInt, while `_z` checks size_t. Nil source requests
native initialization; a zero-size typed buffer is non-NULL empty input. Seed
arguments accept the exact full native uLong domain. Unsigned arguments and
lengths accept integers, decimal strings or `{high=...,low=...}` words; values
outside the selected native domain fail before narrowing. Results are integers
or unsigned decimal strings beyond Lua's signed range. Native checksum
normalization is preserved, so a wide argument need not produce a wide result.
The zero CRC operator returns zero in pinned 1.3.2. A nonzero operator with
only bits above the polynomial's low 32 bits is rejected to avoid native
nontermination; operators produced by combine_gen satisfy the contract.

`get_crc_table()` creates an independent readonly capability with `size()`
(256), `entry(index)` (0..255), `native()` (the validated capability), and
`close()`. Values stay owned by zlib, cannot be mutated, and are not copied
into a Lua array. Closing one shell leaves others usable. The public C89
equivalents are `vectis_zlib_buffer_transform`, `vectis_zlib_buffer_bound`,
`vectis_zlib_raw_checksum` and the `vectis_zlib_crc_table` receiver/free helpers
in `<vectis/zlib_raw.h>`.

```lua
local z, bytes = require('zlib'), require('vectis.buffer')
local input <close> = assert(bytes.from_materialized('hello'))
local output <close> = assert(bytes.new(z.compressBound(input:size())))
local code, length = z.compress(output, input)
assert(code == z.Z_OK)
local encoded <close> = output:borrow(0, length)
local decoded <close> = assert(bytes.new(5))
local code, produced, consumed = z.uncompress2(decoded, encoded)
assert(code == z.Z_OK and produced == 5 and consumed == length)
```
