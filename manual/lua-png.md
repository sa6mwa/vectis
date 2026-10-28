# PNG images

`require('vectis.png')` (also `vectis.png`) provides bounded noninterlaced row
receivers and explicitly materialized complete images. Each receiver belongs to
its creating process/thread and starts no threads. Native errors latch.

```lua
local png = require('vectis.png')
local image <close> = assert(png.writer('pixels.png',{
  width=2,height=1,format='rgba8',text={Title='Two pixels'},
}))
assert(image:write_row(string.char(255,0,0,255,0,0,255,127)))
assert(image:finish())
```

`reader(path_or_read_callback,opts)` reads a file or calls
`read(capacity) -> bytes | nil[,error]`. Returned chunks must contain
1–capacity bytes; capacity never exceeds 65536. `next_row()` returns one packed
RGBA8 row, top to bottom. Palette/transparency, low-depth grayscale and 16-bit
channels are converted to RGBA8. `nil,nil` means checked EOF; `nil,error` means
failure. The receiver checks completion through `finish()` or the first
`next_row()` after the final row. Explicit finish before all rows is an error.
Row readers reject interlaced input before producing pixels.

`writer(path_or_write_callback,image,opts)` accepts `width`, `height`, `format`
(`rgba8` default, `rgb8` or `gray8`), optional `text` map and optional `physical`
record `{x,y,unit='meter'|'unknown'}`. `write_row(bytes)` requires exactly one
packed row without padding; `finish()` requires all rows and writes the trailer.
The sink receives chunks <=65536 bytes and returns `true` or `nil,error`.
Metadata is copied during construction. A row writer rejects an image containing
`pixels`; use the materialized encoder for complete pixels.

`info()` returns width/height, packed `row_bytes`, output `format`, original
`bit_depth`/`color_type`/`interlaced`, available `text` and optional `physical`.
Text keys/values are strings without NUL; at most 16 entries and 64 KiB aggregate
are accepted. PNG keyword and native metadata rules also apply. Only selected
text/resolution metadata is exposed; unknown chunks are not retained. Native
ancillary-chunk allocation/cache ceilings may omit oversized optional chunks.
Metadata after IDAT is available after checked completion. Color-management
profiles and arbitrary chunk editing are outside this surface.

Options `max_width`, `max_height` default to 16384 and `max_row_bytes` to 65536.
They must be positive integers; unknown options raise. File open/read/write,
corruption and resource-limit failures return `nil,error` with operation and
diagnostic fields. Callback exceptions preserve their original error object;
returned failures preserve their error value. Allocation failure while copying
a row or metadata closes that receiver, preventing iteration past a lost row.
An active receiver rejects callback reentry/close, including caught attempts.
Independent resources remain usable.

`close`, `__close` and `__gc` release resources. Close is idempotent in Lua and
never finishes a partial image or calls a user callback. Call `finish` explicitly
to observe trailer/output errors. Already delivered bytes cannot be recalled;
discard partial output after failure.

`decode_materialized(bytes,opts)` also supports Adam7 interlaced input. It returns
an image table containing packed RGBA8 `pixels` plus the `info` fields.
`encode_materialized(image,opts)` consumes packed `pixels` matching the selected
format and returns complete PNG bytes. Derived read-info fields are accepted
when re-encoding; width/height/format/pixels determine output geometry.
Both accept `max_bytes` (default 16 MiB) for complete pixel/output allocations,
in addition to the dimension/row limits. Provider allocations and input already
owned by the caller are separate. These APIs explicitly materialize whole
images; the row APIs retain only bounded rows and provider buffers.

Generated PNG bytes can feed `doc:load_image_materialized(bytes)` in
`vectis.pdf` directly. Native APIs are declared in `vectis/png.h`; native
callbacks must not longjmp. The engine contains libpng's setjmp boundaries,
and Lua callbacks always run under protected calls.
