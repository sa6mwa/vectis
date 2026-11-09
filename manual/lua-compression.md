# Zstandard, LZ4 frame, XZ and zlib-ng

`require('vectis.compression')` (also `vectis.compression`) supports `zstd`,
`lz4`, `xz` and `zlib-ng`. Each receiver handles one standard frame/container; decoders
reject trailing bytes, concatenated frames and incomplete input. Existing zlib
bindings are separate. LZO is deferred.

`zlib-ng` uses cpkt's public C89 facade over zlib-ng 2.3.3. It produces and
accepts one zlib-wrapped DEFLATE stream, interoperable with the existing zlib
bindings. Select it explicitly; the other codecs and zlib/gzip APIs keep their
existing implementations. Native gzip files, dictionaries and engine-specific
controls remain accessible in the C SDK rather than duplicated Lua APIs.

```lua
local codec = require('vectis.compression')
local out <close> = assert(io.open('report.xz','wb'))
local encoder <close> = assert(codec.new('xz','compress',{level=3}))
local function write(chunk)
  local ok,err = out:write(chunk)
  if not ok then return nil,err end
  return true
end
assert(encoder:process('first record\n',write))
assert(encoder:process('second record\n',write))
assert(encoder:finish(write))
```

`new(algorithm,direction,opts)` accepts `compress` or `decompress`.
`process(bytes,sink)` consumes the input incrementally and delivers output
chunks of at most 65536 bytes. `finish(sink)` completes the frame or reports
truncation; success is idempotent. Processing after finish is an error. A sink
returns `true` or `nil,error`. Exceptions preserve their original error object;
returned errors are passed through. Sink failures and codec failures latch the
receiver. Output already delivered cannot be recalled; callers must discard
partial output after failure. No callback runs from `close` or GC.

Options reject unknown/inapplicable keys:

- `level` applies only to compression. Zstandard uses its pinned native range;
  LZ4 selects 0 through the native maximum high-compression level; XZ accepts
  0–9. zlib-ng accepts -1 (native default) through 9. Omission uses native
  defaults (Zstandard 3, LZ4 0, XZ 6, zlib-ng -1). XZ/zlib-ng level zero
  is distinct from omission. LZ4 acceleration selection is outside this slice.
- `max_window_log` applies only to Zstandard decompression, 10–31 (subject to
  native platform limits), default 26 (64 MiB).
- `memory_limit` applies only to XZ decompression, a positive byte count,
  default 67108864. The provider reports insufficient memory limits as errors.

LZ4 output uses 64 KiB blocks, content checksums and native bounded autoflush.
XZ uses CRC64. Chunk boundaries do not imply a Zstandard, XZ or zlib-ng flush. Each codec
may keep its bounded internal buffers/dictionary; the facade never collects a
whole stream. Receivers are confined to their creating process/thread, start
no worker threads and reject callback reentry/close on the active receiver.
Independent receivers can be composed synchronously in a sink.

`stream(algorithm,direction,reader,sink,opts)` connects a reader to a sink using
the same bounded receiver. `reader(65536)` returns 1–65536 bytes or `nil[,error]`
at EOF/failure. Empty strings are rejected to prevent a no-progress loop.
It closes the receiver on every exit, including exceptions.

`materialized(algorithm,direction,bytes,opts)` explicitly collects complete
output and returns bytes or `nil,error`. `opts.max_bytes` defaults to 16 MiB;
other options retain the constructor meanings. The limit bounds collected
output, not input already owned by the caller or provider dictionaries. Chunk
storage plus the final concatenated string may coexist during materialization.
Use `stream` or the receiver methods for producer-to-consumer flow.

The native API is in `vectis/compression.h`. It uses caller-owned sink context;
borrowed output is valid only during that call. Native sink callbacks must not
longjmp. Public SDK C89 adapters own codec internals; no private layouts are used.
