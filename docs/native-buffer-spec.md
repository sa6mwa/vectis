# Owned byte buffers and native leases

Status: reviewed supporting contract for the complete public API goal. Public
allocator callbacks and mutable provider buffers need typed storage with stable
ownership. Lua strings cannot be writable memory, and numeric addresses are
not an acceptable substitute. This facility owns bytes only; it adds no FFI
dispatcher, threads, files or whole-message staging.

Expose C89 receiver shells `vectis_buffer` and `vectis_buffer_span` through
`vectis/buffer.h`, with explicit self fields, private impl, eight reserve slots
and matching exported free functions. Buffer allocation is explicit and
materialized; calloc initializes requested bytes to zero. Size uses native
size_t, with exact unsigned input/return domains in Lua. Zero size is valid.

Buffer operations are size, bounded read/write, resize, borrow and close.
Reads/writes take zero-based offset and explicit length; check subtraction-based
bounds before pointer arithmetic. Resize preserves the common prefix and zeros
new bytes. Failed allocation/resize leaves the original storage intact.

A borrow creates an independently owned span with fixed offset/length and
readonly or mutable access. Spans retain the private allocation control rather
than a parent receiver pointer. Subspan borrowing retains an independent lease
within its source range; a readonly source cannot grant mutable access.
Resize and direct buffer mutation are rejected
while any span is live. C spans expose checked const/mutable byte pointers for
provider calls; no private layout access is required. Native callers serialize
access and keep the span live for the complete borrowed-pointer lifetime.
Buffer close frees its shell but existing span leases retain usable storage;
new buffer operations cannot use the freed shell. Span close releases its lease
and frees the allocation after its last owner. All C closes tolerate NULL;
using an already freed C receiver pointer is invalid.

Lua exposes `vectis.buffer.new(size)` and `from_materialized(bytes)`. Buffer and
span methods include exact size, `read_materialized(offset, length)`, write,
borrow and idempotent close/__close/__gc. Copy methods accept at most 65536
bytes per call; this is a bounded copy limit, not a cumulative allocation or
stream-size ceiling. A caller explicitly asks to allocate an entire buffer.
Native allocation failure returns a structured error; wrong types, closed
handles, narrowing, readonly mutation and out-of-range access fail explicitly.

Lua pointer getters return the validated typed capability and exact length,
not an address. Native adapters acquire their own C span leases before giving
bytes to a provider. Closing a Lua handle cannot free memory still leased by
native code. A readonly span cannot become writable through a pointer getter.
Do not mix user-allocator origins or silently transfer ownership. Explicit
provider transfer, adoption and realloc ownership need a separate reviewed
contract before their implementation; this base lease does not pretend to
solve provider take_ownership by casting a pointer.

Install Lua cleanup owners before native acquisition. Early/manual GC is
idempotent, validates the userdata type and cannot destroy unrelated resources.
Native storage has no Lua dependency. Root loading, embedded resources,
source-rock native requirements, CMake static/shared/header/export seams,
manual and coverage must reflect the usable API.

Fast C and normal/packed Lua checks prove zero/empty/binary ranges, exact bounds,
prefix-preserving resize, zero-filled growth, live-lease resize/write rejection,
parent close with live spans, independent mutable/readonly leases, userdata
expiry and GC, allocation-failure cleanup and size-domain rejection without
huge allocations. Full SDK/platform/package checks wait for the entire surface;
this supporting resource alone does not complete allocator/provider integration.
