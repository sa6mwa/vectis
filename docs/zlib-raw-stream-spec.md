# Raw zlib C89 and Lua receiver contract

Status: reviewed full raw-stream contract, with state, retained IO, parameter
output, bounds, gzip-header ownership, bounded back-inflate and protected Lua
allocator ownership implemented and covered by focused debug tests.
The pinned public zlib 1.3.2 zlib.h is the authority. This supplements
zlib-public-facade-spec.md; it does not narrow its remaining surface.

A new libvectis receiver owns an initialized public z_stream and exposes native
initialization (including version/stream-size forms), step/end, independent copy,
reset/keep/reset2, dictionary set/get, deflate params/tune/bounds/pending/used/
prime, inflate sync/sync-point/prime/mark/validate/undermine/codes-used, headers
and inflateBack. A raw operation preserves its zlib status separately from
Vectis argument/ownership errors. NEED_DICT and BUF_ERROR remain recoverable;
there is no implicit member reset, whole-input collection or failure latch that
removes native recovery options. End reports the native result and closes state;
resource destruction frees the receiver even after an end error.

The receiver shell has explicit self, private impl and reserved slots. C callers
can borrow the initialized public native z_stream for direct public operations
and documented fields. Lua maps this capability to its owned typed resource,
never an address. z_stream.state and reserved are private per the public header;
they are not caller-owned fields. Native unsigned counters preserve uLong width
and exact unsigned values; checked uint64 cumulative totals remain separate.

Raw IO uses explicit typed input/output buffer leases and accurate next/avail
fields, or bounded borrowed chunks for a single step. Input is readonly; output
requires mutable storage. Processing never secretly stages or concatenates
input/output. Chunk-returning calls are explicitly materialized; reader/writer
helpers transfer chunks immediately. Validate capacities before narrowing to
uInt. Caller-supplied large buffers are explicit materialized resources, not a
hidden streaming fallback. Reset/copy/end reconcile public IO and header leases.
Copied streams have independent native state and retain every resource needed
by copied borrowed pointers, including native gzip-header references.

Retained IO maps to `set_input(resource[,offset,capacity])`,
`set_output(resource[,offset,capacity])`, `set_avail_in(size)` and
`set_avail_out(size)`. NIL unbind requires zero offset/capacity. `run(flush)`
uses these pointers and returns native status plus consumed/produced counts,
without collecting output. `fields().next_in/next_out` are independently owned
typed spans of the current native next/avail region; callers close their views.
Borrowed materialized chunk processing cannot replace retained bindings.
`deflateParams(level,strategy)` uses real bound output capacity and preserves
BUF_ERROR retry semantics. `deflateBound` and `deflateBound_z` preserve native
unsigned arithmetic and width; no float conversion or guessed arch size.

Dictionary writes copy through native APIs without retaining caller bytes.
Dictionary reads use a bounded 32768-byte destination, with exact native length;
these are explicit materialized dictionary values. gzip header configuration
exposes every public field and its native read/write meanings. Retain immutable
write bytes and mutable read destinations through header consumption, reset,
copy and end; expose NULL distinctly from empty C strings. Never use immutable
Lua strings for writable header destinations.

Native alloc/free callbacks map to typed owned buffers and protected callback
frames, preserving item/size multiplication, arbitrary captured userdata,
exact-once physical cleanup and first error. A copied native stream's allocator
opaque remains valid until every native owner closes; foreign threads cannot
enter Lua states. Constructor/copy failure restores caller resources and never
leaves a VM-only or native allocation without a cleanup owner. Active close and
reentry are rejected or defer physical cleanup until the native operation ends.

Back-inflate uses its explicitly supplied window and public input/output
callbacks. Its callback bytes are borrowed only for their documented native
lifetime. Input/output remain bounded and protected across Lua OOM and failure;
no hidden whole-value collection is acceptable. Native unused input, return
codes, errors and consumed counts remain observable.

Fast regression coverage must prove dictionary negotiation, byte-identical copy
branches, reset/keep variants, parameter changes with pending output, bounds,
prime/sync/mark/validation, every public header field, custom allocators,
back-inflate, exact native/aggregate counters, successful and failed cleanup,
normal/packed Lua and native C89 consumer behavior. Use tiny deterministic
fixtures and no waits. Full platform/package/review gates remain deferred until
the entire dependency/API implementation has landed.

## Gzip header ownership contract (reviewed before implementation)

A dedicated C89 header receiver owns a public gz_header record and independent
leases for extra/name/comment. A read-mode header requires mutable destinations;
a write-mode header uses readonly leases and validates a terminating zero in
name/comment before native binding. Scalar fields preserve native int/uLong/uInt
domains. Capacities and extra_len must fit the relevant storage when used; zero
and NULL keep their distinct native meanings. Every public field is observable.

Stream attachment increments a shared header-control reference. Native copies
retain that same public record, because native copy preserves its header pointer;
they must not invent independently mutable header records. Closing the Lua/C
header shell releases only its ownership reference. Streams keep it and its
leases live until replacement, detach, native discard or end. Native mutation
of read-header pointers to NULL does not lose the separately retained leases.
Reading fields returns scalar snapshots and typed bounded spans, never addresses.
Truncated extra output exposes native actual extra_len plus its bounded stored
bytes; a missing NUL in a name/comment remains observable rather than fabricated.

Pinned zlib 1.3.2 deflateReset retains gzhead despite the public documentation's
claim. A pure public C89 probe emits flags=8/name=retained-name after reset.
Therefore deflate reset/keep retain the managed header until explicit
`deflateSetHeader(nil)` or end. This discrepancy is reported in the standalone
stash/zlib-1.3.2-deflatereset-gzip-header-bugreport.md. Inflate reset/keep/reset2
successfully discard the native capture and release its stream reference; failed
reset preserves it. Shared copies continue to hold their own references.

Lua `gzip_header(fields[,read_mode])` accepts typed resources for the three byte
fields and maps all numeric fields. No immutable Lua string becomes a writable
native destination. Header fields are captured before native acquisition and
remain strongly owned without a registry cycle. `deflateSetHeader` and
`inflateGetHeader` accept the typed header, with kind/mode validation. Protected
field getters own any newly returned spans before further Lua allocation. Tiny
normal/packed/native tests prove full fields, nullable/empty distinctions,
truncation, CRC, copy/close/reset/replacement, rejected capacities, constructor
and result OOM, and exact cleanup.

## Back-inflate implementation contract (reviewed before implementation)

`inflateBackInit(bits,window)` and its version/size form create an owned raw
receiver with an independent mutable window lease of exactly 2^bits bytes.
Bits are 8..15; invalid size/readonly windows fail before native acquisition.
The caller's parent may close because the receiver retains storage. During the
native back call, an owned write-lock capability protects the entire underlying
buffer from mutation through any Lua buffer/span alias; read/close remain safe.
No caller can release a lock owned privately by the native driver.

C `back` borrows the public in_func/out_func callbacks and descriptors only for
its call, preserves native unused input and distinguishes source versus sink
BUF_ERROR through the native next_in result. Lua maps functions or raw
read/write/userdata records to protected trampolines. A single private 65536-byte
input buffer is reused chunk by chunk. Latest unused input is rebound as typed
retained input after the call; no pointer to a C stack or a transient Lua string
survives. Native output chunks are at most the window size and go immediately
to the writer. Returning an empty input string is no progress, nil is EOF,
nil/error is failure, and over-capacity/wrong-type data is rejected.

The result includes native code, source/sink failure distinction and checked
uint64 supplied/consumed/output byte counts. Unused bytes are explicitly named
materialized when returned as a bounded value. Native callbacks never unwind
Lua exceptions through provider frames. Root callback graphs and in-flight
bytes for the entire call, preserve the first Lua error identity, reject owner
reentry/close, and restore counters/locks after error and OOM. Back state permits
multiple complete raw streams and uses inflateBackEnd, not inflateEnd. Standard
inflate/reset/copy/header methods cannot misinterpret back state.

The buffer write-lock is a separate C89 owned receiver/capability with Lua
mapping. Nested locks are independent, retain storage and release only their
own freeze. Native library writes occur through its previously acquired public
pointer; Lua mutation APIs consult the lock count. C pointer borrowers still
owe the provider's caller-serialization contract.

Fast tests cover parent close, full raw streams, callback fragmentation/output
before EOF, unused input/second stream, source/sink/corruption failure, window
alias mutation/reentry, undersized/readonly/version errors, nested lock cleanup,
constructor/callback/result OOM and zero native/VM leaks. No sleeps or large
fixtures; full-platform/final whole-goal gates remain deferred.

## Lua allocator mapping (reviewed before implementation)

Native raw/back constructors accept an optional final allocator record after
normal arguments or version/stream-size arguments. The record is
`{zalloc=fn,zfree=fn,opaque=value}`; alloc/free are a required pair when present.
`zalloc(items,size,opaque)` returns an owned unleased vectis.buffer of at least
the checked item-size product, or nil/error. Accepted handles are consumed;
rejected handles remain usable. `zfree(scoped_view,opaque)` observes retirement
with a readonly view that expires on all callback exits. Free failure cannot
skip physical native-buffer release. No pointer becomes an integer address.

A native copy retains the same allocator context because zlib copies opaque.
Every Lua native owner roots that context without permanent registry cycles;
GC teardown has temporary preallocated operation pins so cleared weak values do
not suppress callbacks. Shared-context callback activity prevents conflicting
entry/close through any sibling owner. Calling coroutines refresh context state
only when inactive; no foreign thread enters Lua. Default allocator fields are
also callable typed operations, and fields exposes exact zalloc/zfree/opaque.

All Lua argument setup, result interpretation and scoped-view creation occurs
inside protected native-to-Lua frames. Native alloc returns NULL on callback
failure and free returns normally after cleanup. Preserve the first Lua error
identity; report it only after native unwind, including constructor/copy/reset/
end and OOM/result paths. Necessary frees run after earlier failure. Temporary
views are rooted before acquiring a C lease and are closed without allocating
when an OOM escapes the frame. End/close release context ownership only after
native destruction; copies keep their independent native ownership.

Fast tests cover multiplication/range, accepted/rejected buffers, constructor/
copy allocation failure, free error, graph collection, parent GC/coroutine loss,
sibling close/reentry, scoped-view expiry, standard field/default functions,
back-inflate hooks and deterministic callback OOM. Full platform/package/whole-
review gates remain deferred under the unchanged goal.
