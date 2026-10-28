# Owned byte buffers and spans

`vectis.buffer` binds the public C89 receivers in `<vectis/buffer.h>`.
`new(size)` explicitly allocates zeroed, materialized byte storage.
`from_materialized(bytes)` copies a whole Lua string into owned storage.
Neither operation is a streaming source or an undisclosed staging substitute.
Sizes/offsets accept nonnegative Lua integers or exact unsigned decimal strings
within native size_t. Returned sizes use a Lua integer when possible and an
exact decimal string beyond its signed range.

Buffers provide `size()`, `read_materialized(offset, length)`,
`write(offset, bytes)`, `resize(size)`, `borrow(offset, length[, readonly])`, and
`close()`, plus `data()` and `data_mutable()`. These data getter mappings return
the resource itself and its exact size, never an address. Mutable buffer access
requires no live span leases. Offsets are zero-based. Read/write copies are at most 65536 bytes per
call; this is not a total allocation limit. Resize preserves the common prefix
and zeroes newly added bytes. Failed allocation leaves the original storage
intact. Direct writes and resize are rejected while any span is leased.

Borrow returns an independently owned fixed-range span; readonly defaults true.
Spans provide size/read_materialized/write/borrow/close plus `data()` and
`data_mutable()`. Those pointer-getter mappings return the validated span itself
and its exact length, never an address. Readonly spans reject writes and mutable
data access. A readonly lease restricts its access, not mutations through a
separate mutable lease; access remains caller-serialized.
Subspan borrow uses offsets relative to its source span and retains an
independent lease. A readonly source cannot produce a mutable child.

Closing a buffer makes that Lua handle unusable but existing spans keep their
storage alive. Closing a span releases its lease. `close` is idempotent and
`__close`/GC release abandoned resources. A retained span survives parent GC.
Native adapters must acquire independent C leases for every provider borrow;
closing a Lua handle cannot stand in for releasing the provider's lease.

`lock_writes()` returns an independent owned lock capability from a buffer or
span. While any lock is held, checked mutation and mutable-data access through
every alias fail; reads and parent/span close remain safe. Nested locks release
only their own hold. Lock close is idempotent and supports `__close`/GC. Native
adapters can privately lock provider windows across callbacks without allowing
the caller to release the adapter's lock. C pointer borrowers still owe the
provider's serialization/lifetime contract.

Wrong types, closed resources, readonly mutation and invalid ranges raise Lua
errors. Allocation failures return nil and a Vectis structured error. C callers
must keep span shells alive until their borrowed pointer is no longer used;
using freed C receiver pointers is invalid. All C closes accept NULL.

```lua
local bytes = require('vectis.buffer')
local owner = assert(bytes.from_materialized('a\0bc'))
local view <close> = owner:borrow(0, 4, false)
owner:close()
view:write(2, 'Z')
assert(view:read_materialized(0, 4) == 'a\0Zc')
```

The base facility does not silently transfer storage to a provider or mix
allocator origins. Provider adoption/reallocation needs an explicit owned
contract; the withdrawn local MDF facade extension is not part of the released
MDF API. The Vectis buffer facility remains available independently, including
for Vectis-owned zlib bindings. It creates no threads and carries no file
handles. The source rock requires an embedding native `vectis.buffer.core`.
